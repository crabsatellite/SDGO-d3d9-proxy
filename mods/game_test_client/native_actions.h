#pragma once

#include <windows.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/sdgo_api.h"

// In-process driver for the original client's request path. Every action calls
// original constructors/request methods and leaves replies and state changes to
// the original service handlers. It does not synthesize a reply, room, profile,
// participant, or application state.
class NativeActions
{
public:
    enum AuthResult : uint32_t
    {
        kAuthNone = 0,
        kAuthRejected = 1,
        kAuthAccepted = 2,
    };

    struct AuthObservation
    {
        uint32_t reply_sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t native_login_state;
        uint32_t profile_pilot_id;
        uint16_t reply_opcode;
        uint16_t reply_status;
    };

    enum NicknameResult : uint32_t
    {
        kNicknameNone = 0,
        kNicknameRejected = 1,
        kNicknameAccepted = 2,
    };

    struct NicknameObservation
    {
        uint32_t reply_sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t native_login_state;
        uint32_t profile_pilot_id;
        uint16_t reply_opcode;
        uint16_t reply_status;
    };

    struct OnboardingObservation
    {
        uint32_t native_app_state;
        uint32_t native_login_state;
        uint32_t profile_pilot_id;
        uint32_t tutorial_level;
        uint32_t login_path_state;
        uint32_t pending_login_event_count;
        uint16_t channel_id;
        uint16_t previous_channel_id;
        uint32_t channel_group;
        uint8_t starter_draw_busy;
        uint8_t reserved[3];
    };

    struct TransportObservation
    {
        uint32_t send_sequence;
        uint32_t packet_size;
        uint32_t result;
        uint16_t opcode;
        uint16_t reserved;
    };

    struct Config
    {
        const char* username;
        const char* password;
        const char* room_title;
        uint16_t room_mode;
        uint32_t map_id;
    };

    bool Bind(const SdgoApi* api, SdgoMod* mod, const Config& config)
    {
        api_ = api;
        mod_ = mod;
        config_ = config;
        gameApp_ = Symbol<GameAppFn>("game.app");
        loginPacketCtor_ = Symbol<LoginPacketCtorFn>("flow.login_packet_ctor");
        passwordMd5Hex_ = Symbol<PasswordMd5HexFn>("flow.password_md5_hex");
        network_ = Symbol<NetworkFn>("flow.network");
        networkSend_ = Symbol<NetworkSendFn>("flow.network_send");
        loginServiceDispatch_ =
            Symbol<LoginServiceDispatchFn>("flow.login_service_dispatch");
        profilePointer_ = Symbol<uint8_t**>("flow.profile_pointer");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nicknameDialogCtor_ =
            Symbol<NicknameDialogCtorFn>("flow.nickname_dialog_ctor");
        enqueueUiObject_ =
            Symbol<EnqueueUiObjectFn>("flow.enqueue_ui_object");
        nicknameDialogDtor_ =
            Symbol<NicknameDialogDtorFn>("flow.nickname_dialog_dtor");
        nicknameDialogConfirm_ =
            Symbol<NicknameDialogConfirmFn>("flow.nickname_dialog_confirm");
        if (!api_ || !mod_ || !gameApp_ || !loginPacketCtor_ || !passwordMd5Hex_ ||
            !network_ || !networkSend_ || !loginServiceDispatch_ ||
            !profilePointer_ || !nativeWStringCtor_ || !nicknameDialogCtor_ ||
            !enqueueUiObject_ || !nicknameDialogDtor_ ||
            !nicknameDialogConfirm_ || !ValidCredentials())
            return false;
        if (active_ && active_ != this)
            return false;
        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.login_service_dispatch",
                reinterpret_cast<void*>(&ObserveLoginServiceDispatch),
                reinterpret_cast<void**>(&loginServiceDispatch_))) {
            active_ = nullptr;
            return false;
        }
        // The observer is optional for login, but nickname confirmation fails
        // closed without it: CreateNickname reports success only after this
        // hook sees the original business method reach the real wire sender.
        networkObserverInstalled_ = api_->HookCode(
            mod_,
            "flow.network_send",
            reinterpret_cast<void*>(&ObserveNetworkSend),
            reinterpret_cast<void**>(&networkSend_)) != 0;
        return true;
    }

    // Sends the same 0x4D-byte local-login request as CServiceGateLogin's
    // LoginBtn handler. For a new account the real backend's first failure and
    // second registration/login attempt are two calls to this method, separated
    // by the original reply/state observation in the GameTest runner.
    bool Login() const
    {
        if (!ValidCredentials())
            return false;
        __try {
            char passwordHash[33] = {};
            passwordMd5Hex_(
                config_.password,
                passwordHash,
                static_cast<int>(strlen(config_.password)));
            passwordHash[32] = '\0';

            uint8_t packet[0x4D] = {};
            loginPacketCtor_(packet, config_.username, passwordHash);
            return SendRequest(packet, sizeof(packet));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // Completes the original first-account nickname gate. The account name is
    // deliberately reused as the test nickname: private GameTest accounts are
    // generated as short ASCII alphanumeric identifiers, so the runner does
    // not need a UI textbox or a second piece of private configuration.
    //
    // This follows the normal Nickname OK path without fabricating a callback:
    // the original dialog constructor receives the native by-value wstring,
    // the original UI manager clones and owns it, and the original confirm
    // business method runs on that live managed clone. That method performs
    // the REQ_CREATE_NICKNAME gate, serialization, transport and close.
    bool CreateNickname() const
    {
        if (!ValidNickname() || !nativeWStringCtor_ || !nicknameDialogCtor_ ||
            !enqueueUiObject_ || !nicknameDialogDtor_ ||
            !nicknameDialogConfirm_ || !networkObserverInstalled_)
            return false;

        wchar_t nickname[15] = {};
        const size_t length = strlen(config_.username);
        for (size_t i = 0; i < length; ++i)
            nickname[i] = static_cast<unsigned char>(config_.username[i]);

        alignas(4) uint8_t sourceDialog[0x70] = {};
        bool constructed = false;
        void* liveDialog = nullptr;
        __try {
            constructed = ConstructNicknameDialog(sourceDialog, nickname);
            if (constructed)
                liveDialog = enqueueUiObject_(sourceDialog, 1);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            liveDialog = nullptr;
        }
        if (constructed) {
            __try {
                nicknameDialogDtor_(sourceDialog);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                liveDialog = nullptr;
            }
        }
        if (!liveDialog)
            return false;

        const uint32_t before = TransportState().send_sequence;
        __try {
            nicknameDialogConfirm_(liveDialog);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        const TransportObservation sent = TransportState();
        return sent.send_sequence != before &&
               sent.opcode == kNicknameRequestOpcode &&
               sent.packet_size == kNicknameRequestSize &&
               sent.result != 0;
    }

    // Sends a packet already built by an original request constructor through
    // the original network object. Room/channel actions use this so their
    // request accounting and transport stay identical to the login path.
    bool SendRequest(const void* packet, size_t size) const
    {
        if (!packet || size == 0 || size > static_cast<size_t>(INT_MAX) ||
            !network_ || !networkSend_)
            return false;
        __try {
            uint8_t* network = network_();
            if (!network)
                return false;
            void* stream = *reinterpret_cast<void**>(network + 0x14);
            if (!stream)
                return false;

            ++*reinterpret_cast<uint32_t*>(network + 0x58);
            *reinterpret_cast<uint32_t*>(network + 0x5C) +=
                static_cast<uint32_t>(size);
            return networkSend_(
                       network,
                       stream,
                       packet,
                       static_cast<int>(size),
                       0) != 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    uint32_t AppState() const
    {
        __try {
            uint8_t* app = gameApp_ ? gameApp_() : nullptr;
            return app ? *reinterpret_cast<const uint32_t*>(app + 0x4A0) : UINT32_MAX;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return UINT32_MAX;
        }
    }

    AuthObservation LoginState() const
    {
        AuthObservation observed = {};
        __try {
            // The hook makes authWriteVersion_ odd while publishing and even
            // after it finishes. A matching even version prevents a caller
            // from consuming a mixture of two replies.
            for (int attempt = 0; attempt < 3; ++attempt) {
                const LONG before =
                    InterlockedCompareExchange(&authWriteVersion_, 0, 0);
                if (before & 1)
                    continue;
                observed.reply_sequence = static_cast<uint32_t>(
                    InterlockedCompareExchange(&authReplySequence_, 0, 0));
                observed.result = static_cast<uint32_t>(
                    InterlockedCompareExchange(&authResult_, 0, 0));
                observed.native_app_state = AppState();
                observed.native_login_state = static_cast<uint32_t>(
                    InterlockedCompareExchange(&nativeLoginState_, 0, 0));
                observed.profile_pilot_id = static_cast<uint32_t>(
                    InterlockedCompareExchange(&profilePilotId_, 0, 0));
                const uint32_t reply = static_cast<uint32_t>(
                    InterlockedCompareExchange(&authReply_, 0, 0));
                observed.reply_opcode = static_cast<uint16_t>(reply & 0xFFFFu);
                observed.reply_status = static_cast<uint16_t>(reply >> 16);
                const LONG after =
                    InterlockedCompareExchange(&authWriteVersion_, 0, 0);
                if (before == after && !(after & 1))
                    return observed;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
            observed.native_app_state = UINT32_MAX;
        }
        return observed;
    }

    NicknameObservation NicknameState() const
    {
        NicknameObservation observed = {};
        __try {
            for (int attempt = 0; attempt < 3; ++attempt) {
                const LONG before =
                    InterlockedCompareExchange(&nicknameWriteVersion_, 0, 0);
                if (before & 1)
                    continue;
                observed.reply_sequence = static_cast<uint32_t>(
                    InterlockedCompareExchange(&nicknameReplySequence_, 0, 0));
                observed.result = static_cast<uint32_t>(
                    InterlockedCompareExchange(&nicknameResult_, 0, 0));
                observed.native_app_state = AppState();
                observed.native_login_state = static_cast<uint32_t>(
                    InterlockedCompareExchange(&nicknameLoginState_, 0, 0));
                observed.profile_pilot_id = static_cast<uint32_t>(
                    InterlockedCompareExchange(&nicknameProfilePilotId_, 0, 0));
                const uint32_t reply = static_cast<uint32_t>(
                    InterlockedCompareExchange(&nicknameReply_, 0, 0));
                observed.reply_opcode = static_cast<uint16_t>(reply & 0xFFFFu);
                observed.reply_status = static_cast<uint16_t>(reply >> 16);
                const LONG after =
                    InterlockedCompareExchange(&nicknameWriteVersion_, 0, 0);
                if (before == after && !(after & 1))
                    return observed;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
            observed.native_app_state = UINT32_MAX;
        }
        return observed;
    }

    void* LoginService() const
    {
        const uint32_t appState = AppState();
        // CServiceGate is released when the original client leaves the gate.
        // The dispatch hook intentionally keeps only an observational pointer,
        // so never expose it after its two owning application states end.
        if (appState != kLoginServiceAppState &&
            appState != kLoginServiceTransitionAppState)
            return nullptr;
        return InterlockedCompareExchangePointer(&loginService_, nullptr, nullptr);
    }

    // Read-only progress markers used to decide which real onboarding action is
    // currently legal. In particular, profile+0xBC is the original tutorial
    // level checked by both the login transition and the first capsule path:
    // values below 2 enter beginner onboarding; the real 0x072E/0x054A starter
    // draw advances it to 2.
    OnboardingObservation OnboardingState() const
    {
        OnboardingObservation observed = {};
        observed.native_app_state = AppState();
        __try {
            const uint8_t* service = static_cast<const uint8_t*>(LoginService());
            if (service) {
                observed.native_login_state =
                    *reinterpret_cast<const uint32_t*>(service + kLoginServiceState);
                observed.pending_login_event_count =
                    *reinterpret_cast<const uint32_t*>(service + 0x2068);
            }
            const uint8_t* profile =
                profilePointer_ ? *profilePointer_ : nullptr;
            if (profile) {
                observed.profile_pilot_id =
                    *reinterpret_cast<const uint32_t*>(profile + kProfilePilotId);
                observed.tutorial_level =
                    *reinterpret_cast<const uint32_t*>(profile + 0xBC);
                observed.login_path_state =
                    *reinterpret_cast<const uint32_t*>(profile + 0x364C);
                observed.channel_id =
                    *reinterpret_cast<const uint16_t*>(profile + 0x5D0);
                observed.previous_channel_id =
                    *reinterpret_cast<const uint16_t*>(profile + 0x5DC);
                observed.channel_group =
                    *reinterpret_cast<const uint32_t*>(profile + 0x5D4);
                observed.starter_draw_busy = profile[0x62E];
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
            observed.native_app_state = UINT32_MAX;
        }
        return observed;
    }

    TransportObservation TransportState() const
    {
        TransportObservation observed = {};
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before =
                InterlockedCompareExchange(&transportWriteVersion_, 0, 0);
            if (before & 1)
                continue;
            observed.send_sequence = static_cast<uint32_t>(
                InterlockedCompareExchange(&transportSequence_, 0, 0));
            observed.packet_size = static_cast<uint32_t>(
                InterlockedCompareExchange(&transportSize_, 0, 0));
            observed.result = static_cast<uint32_t>(
                InterlockedCompareExchange(&transportResult_, 0, 0));
            observed.opcode = static_cast<uint16_t>(
                InterlockedCompareExchange(&transportOpcode_, 0, 0));
            const LONG after =
                InterlockedCompareExchange(&transportWriteVersion_, 0, 0);
            if (before == after && !(after & 1))
                return observed;
        }
        memset(&observed, 0, sizeof(observed));
        return observed;
    }

    const Config& GetConfig() const { return config_; }

private:
    static constexpr int kLoginRejectedOpcode = 0x232A;
    static constexpr int kLoginAcceptedOpcode = 0x232B;
    static constexpr int kNicknameRejectedOpcode = 0x232D;
    static constexpr int kNicknameAcceptedOpcode = 0x232E;
    static constexpr uint16_t kNicknameRequestOpcode = 0x232C;
    static constexpr uint32_t kNicknameRequestSize = 0x63;
    static constexpr size_t kLoginReplyStatus = 0x06;
    static constexpr size_t kLoginServiceState = 0x2058;
    static constexpr size_t kProfilePilotId = 0x138;
    static constexpr uint32_t kLoginServiceAppState = 4;
    static constexpr uint32_t kLoginServiceTransitionAppState = 25;

    using GameAppFn = uint8_t*(__cdecl*)();
    using LoginPacketCtorFn = void*(__thiscall*)(void*, const char*, const char*);
    using PasswordMd5HexFn = char*(__cdecl*)(const void*, char*, int);
    using NetworkFn = uint8_t*(__cdecl*)();
    using NetworkSendFn = int(__thiscall*)(void*, void*, const void*, int, char);
    using LoginServiceDispatchFn =
        int(__thiscall*)(void*, int, int, int, int, void*);
    using NativeWStringCtorFn = void*(__thiscall*)(void*, const wchar_t*);
    using NicknameDialogCtorFn = void*(__thiscall*)(void*);
    using EnqueueUiObjectFn = void*(__cdecl*)(void*, int);
    using NicknameDialogDtorFn = void(__thiscall*)(void*);
    using NicknameDialogConfirmFn = void(__thiscall*)(void*);

    static int __fastcall ObserveLoginServiceDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        NativeActions* actions = active_;
        if (!actions || !actions->loginServiceDispatch_)
            return 0;

        uint16_t status = 0;
        const int normalizedOpcode = opcode & 0xFFFF;
        if ((normalizedOpcode == kLoginRejectedOpcode ||
             normalizedOpcode == kNicknameRejectedOpcode) &&
            packet) {
            __try {
                status = *reinterpret_cast<const uint16_t*>(
                    static_cast<const uint8_t*>(packet) + kLoginReplyStatus);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                status = UINT16_MAX;
            }
        }

        InterlockedExchangePointer(&actions->loginService_, self);

        // Preserve the real handler and all of its profile, UI and state work.
        const int result =
            actions->loginServiceDispatch_(self, a, b, c, opcode, packet);

        const bool authReply =
            normalizedOpcode == kLoginRejectedOpcode ||
            normalizedOpcode == kLoginAcceptedOpcode;
        const bool nicknameReply =
            normalizedOpcode == kNicknameRejectedOpcode ||
            normalizedOpcode == kNicknameAcceptedOpcode;
        if (!authReply && !nicknameReply)
            return result;

        LONG loginState = 0;
        LONG pilotId = 0;
        __try {
            if (self)
                loginState = *reinterpret_cast<const LONG*>(
                    static_cast<const uint8_t*>(self) + kLoginServiceState);
            // This reads the singleton pointer directly. It never invokes the
            // lazy profile accessor while login is still in progress.
            uint8_t* profile =
                actions->profilePointer_ ? *actions->profilePointer_ : nullptr;
            if (profile)
                pilotId = *reinterpret_cast<const LONG*>(
                    profile + kProfilePilotId);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            loginState = 0;
            pilotId = 0;
        }

        const uint32_t packedReply =
            static_cast<uint32_t>(normalizedOpcode) |
            (static_cast<uint32_t>(status) << 16);
        if (authReply) {
            InterlockedIncrement(&actions->authWriteVersion_);
            InterlockedExchange(
                &actions->authReply_,
                static_cast<LONG>(packedReply));
            InterlockedExchange(
                &actions->authResult_,
                normalizedOpcode == kLoginAcceptedOpcode
                    ? static_cast<LONG>(kAuthAccepted)
                    : static_cast<LONG>(kAuthRejected));
            InterlockedExchange(&actions->nativeLoginState_, loginState);
            InterlockedExchange(&actions->profilePilotId_, pilotId);
            InterlockedIncrement(&actions->authReplySequence_);
            InterlockedIncrement(&actions->authWriteVersion_);
        } else {
            InterlockedIncrement(&actions->nicknameWriteVersion_);
            InterlockedExchange(
                &actions->nicknameReply_,
                static_cast<LONG>(packedReply));
            InterlockedExchange(
                &actions->nicknameResult_,
                normalizedOpcode == kNicknameAcceptedOpcode
                    ? static_cast<LONG>(kNicknameAccepted)
                    : static_cast<LONG>(kNicknameRejected));
            InterlockedExchange(&actions->nicknameLoginState_, loginState);
            InterlockedExchange(&actions->nicknameProfilePilotId_, pilotId);
            InterlockedIncrement(&actions->nicknameReplySequence_);
            InterlockedIncrement(&actions->nicknameWriteVersion_);
        }
        return result;
    }

    static int __fastcall ObserveNetworkSend(
        void* network,
        void*,
        void* stream,
        const void* packet,
        int size,
        char flag)
    {
        NativeActions* actions = active_;
        if (!actions || !actions->networkSend_)
            return 0;

        LONG opcode = 0;
        if (packet && size >= static_cast<int>(sizeof(uint16_t))) {
            __try {
                opcode = *reinterpret_cast<const uint16_t*>(packet);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                opcode = -1;
            }
        }

        // Never delay, suppress or rewrite the original transport call.
        const int result =
            actions->networkSend_(network, stream, packet, size, flag);
        InterlockedIncrement(&actions->transportWriteVersion_);
        InterlockedExchange(&actions->transportOpcode_, opcode);
        InterlockedExchange(&actions->transportSize_, size);
        InterlockedExchange(&actions->transportResult_, result);
        InterlockedIncrement(&actions->transportSequence_);
        InterlockedIncrement(&actions->transportWriteVersion_);
        return result;
    }

    bool ConstructNicknameDialog(void* dialog, const wchar_t* nickname) const
    {
#if defined(_M_IX86)
        void* result = nullptr;
        NativeWStringCtorFn nativeWStringCtor = nativeWStringCtor_;
        NicknameDialogCtorFn nicknameDialogCtor = nicknameDialogCtor_;
        __asm {
            // 0x004027E0 constructs the shipped 0x1C-byte wstring directly in
            // the compiler-reserved by-value argument. 0x008EBD80 consumes and
            // destroys that argument, including its heap buffer when needed.
            sub esp, 0x1c
            mov ecx, esp
            push nickname
            mov eax, nativeWStringCtor
            call eax
            mov ecx, dialog
            mov eax, nicknameDialogCtor
            call eax
            mov result, eax
        }
        return result == dialog;
#else
        (void)dialog;
        (void)nickname;
        return false;
#endif
    }

    template <typename Fn>
    Fn Symbol(const char* name) const
    {
        return api_ ? reinterpret_cast<Fn>(api_->Symbol(mod_, name)) : nullptr;
    }

    bool ValidCredentials() const
    {
        if (!config_.username || !config_.password)
            return false;
        const size_t usernameLength = strlen(config_.username);
        const size_t passwordLength = strlen(config_.password);
        // The original packet owns 15 bytes for the account including NUL and
        // hashes any nonempty password to a fixed 32-byte MD5 hex string.
        return usernameLength > 0 && usernameLength <= 14 && passwordLength > 0;
    }

    bool ValidNickname() const
    {
        if (!config_.username)
            return false;
        const size_t length = strlen(config_.username);
        if (length == 0 || length > 14)
            return false;
        for (size_t i = 0; i < length; ++i) {
            const unsigned char value =
                static_cast<unsigned char>(config_.username[i]);
            if (!((value >= '0' && value <= '9') ||
                  (value >= 'A' && value <= 'Z') ||
                  (value >= 'a' && value <= 'z')))
                return false;
        }
        return true;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    Config config_ = {};
    GameAppFn gameApp_ = nullptr;
    LoginPacketCtorFn loginPacketCtor_ = nullptr;
    PasswordMd5HexFn passwordMd5Hex_ = nullptr;
    NetworkFn network_ = nullptr;
    NetworkSendFn networkSend_ = nullptr;
    LoginServiceDispatchFn loginServiceDispatch_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NicknameDialogCtorFn nicknameDialogCtor_ = nullptr;
    EnqueueUiObjectFn enqueueUiObject_ = nullptr;
    NicknameDialogDtorFn nicknameDialogDtor_ = nullptr;
    NicknameDialogConfirmFn nicknameDialogConfirm_ = nullptr;
    bool networkObserverInstalled_ = false;
    mutable PVOID volatile loginService_ = nullptr;
    mutable volatile LONG authWriteVersion_ = 0;
    mutable volatile LONG authReplySequence_ = 0;
    mutable volatile LONG authReply_ = 0;
    mutable volatile LONG authResult_ = kAuthNone;
    mutable volatile LONG nativeLoginState_ = 0;
    mutable volatile LONG profilePilotId_ = 0;
    mutable volatile LONG nicknameWriteVersion_ = 0;
    mutable volatile LONG nicknameReplySequence_ = 0;
    mutable volatile LONG nicknameReply_ = 0;
    mutable volatile LONG nicknameResult_ = kNicknameNone;
    mutable volatile LONG nicknameLoginState_ = 0;
    mutable volatile LONG nicknameProfilePilotId_ = 0;
    mutable volatile LONG transportWriteVersion_ = 0;
    mutable volatile LONG transportSequence_ = 0;
    mutable volatile LONG transportOpcode_ = 0;
    mutable volatile LONG transportSize_ = 0;
    mutable volatile LONG transportResult_ = 0;
    inline static NativeActions* active_ = nullptr;
};
