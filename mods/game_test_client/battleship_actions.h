#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"

// Drives the shipped battleship tutorial against manager-owned popup objects.
// The adapter opens the same named tutorial object as the original lobby,
// invokes its real callbacks with the real controls and control-name objects,
// and observes the original reply consumer. It never creates a reply, edits
// profile data, supplements inventory, or constructs a battleship deck.
class BattleShipActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForLobby = 1,
        kWaitingForTutorial = 2,
        kTutorialOpened = 3,
        kPaging = 4,
        kRequestSent = 5,
        kAccepted = 6,
        kConfirmed = 7,

        kProfileUnavailable = 0x100,
        kNotEligible = 0x101,
        kAlreadyCompleted = 0x102,
        kOpenFailed = 0x103,
        kNativeObjectMismatch = 0x104,
        kControlMissing = 0x105,
        kControlNameInvalid = 0x106,
        kPageStateInvalid = 0x107,
        kPageDidNotMove = 0x108,
        kNativeRequestPending = 0x109,
        kRequestNotSent = 0x10A,
        kReplyRejected = 0x10B,
        kReplyNotConsumed = 0x10C,
        kNativeStateMismatch = 0x10D,
        kResultPopupUnavailable = 0x10E,
        kNativeFault = 0x10F,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t native_app_state;
        uint32_t profile_pilot_id;
        uint32_t tutorial_level;
        uint32_t request_transport_sequence;
        uint32_t reply_sequence;
        uint32_t confirm_sequence;
        uint32_t ship_uid_low;
        uint32_t ship_uid_high;
        uint32_t ship_id;
        uint32_t leader_uid_low;
        uint32_t leader_uid_high;
        uint16_t reply_opcode;
        uint16_t reply_error;
        uint8_t page;
        uint8_t page_count;
        uint8_t request_sent;
        uint8_t reply_consumed;
        uint8_t tutorial_popup_ready;
        uint8_t result_popup_ready;
        uint8_t native_fault;
        uint8_t reserved;
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;
        profilePointer_ =
            Symbol<uint8_t**>("flow.profile_pointer");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>("flow.native_wstring_dtor");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>("flow.native_ui_find_control");
        tutorialCtor_ =
            Symbol<TutorialCtorFn>("flow.battleship_tutorial_ctor");
        tutorialDtor_ =
            Symbol<TutorialDtorFn>("flow.battleship_tutorial_dtor");
        namedUiEnqueue_ =
            Symbol<NamedUiEnqueueFn>("flow.named_ui_enqueue");
        tutorialClone_ =
            Symbol<CloneFn>("flow.battleship_tutorial_clone");
        tutorialAction_ =
            Symbol<PopupActionFn>("flow.battleship_tutorial_action");
        tutorialDispatch_ =
            Symbol<PacketDispatchFn>(
                "flow.battleship_tutorial_dispatch");
        tutorialVtable_ =
            Symbol<void**>("flow.battleship_tutorial_vtable");
        resultPopupClone_ =
            Symbol<CloneFn>("flow.battleship_get_popup_clone");
        resultPopupAction_ =
            Symbol<PopupActionFn>("flow.battleship_get_popup_action");
        resultPopupVtable_ =
            Symbol<void**>("flow.battleship_get_popup_vtable");

        if (!api_ || !mod_ || !native_ || !profilePointer_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_ ||
            !nativeUiFindControl_ || !tutorialCtor_ || !tutorialDtor_ ||
            !namedUiEnqueue_ || !tutorialClone_ || !tutorialAction_ ||
            !tutorialDispatch_ || !tutorialVtable_ ||
            !resultPopupClone_ || !resultPopupAction_ ||
            !resultPopupVtable_ || !ValidBindings())
            return false;
        if (active_ && active_ != this)
            return false;

        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.battleship_tutorial_clone",
                reinterpret_cast<void*>(&ObserveTutorialClone),
                reinterpret_cast<void**>(&tutorialClone_)) ||
            !api_->HookCode(
                mod_,
                "flow.battleship_tutorial_dispatch",
                reinterpret_cast<void*>(&ObserveTutorialDispatch),
                reinterpret_cast<void**>(&tutorialDispatch_)) ||
            !api_->HookCode(
                mod_,
                "flow.battleship_get_popup_clone",
                reinterpret_cast<void*>(&ObserveResultPopupClone),
                reinterpret_cast<void**>(&resultPopupClone_))) {
            active_ = nullptr;
            return false;
        }

        bound_ = true;
        Reset();
        return true;
    }

    void Reset()
    {
        InterlockedExchangePointer(&tutorialPopup_, nullptr);
        InterlockedExchangePointer(&resultPopup_, nullptr);
        InterlockedExchange(&lastResult_, bound_ ? kWaitingForLobby : kUnbound);
        InterlockedExchange(&openAttempted_, 0);
        InterlockedExchange(&requestAttempted_, 0);
        InterlockedExchange(&requestSent_, 0);
        InterlockedExchange(&requestTransportSequence_, 0);
        InterlockedExchange(&replyWriteVersion_, 0);
        InterlockedExchange(&replySequence_, 0);
        InterlockedExchange(&replyOpcodeError_, 0);
        InterlockedExchange(&replyConsumed_, 0);
        InterlockedExchange(&replyShipUidLow_, 0);
        InterlockedExchange(&replyShipUidHigh_, 0);
        InterlockedExchange(&replyShipId_, 0);
        InterlockedExchange(&replyLeaderUidLow_, 0);
        InterlockedExchange(&replyLeaderUidHigh_, 0);
        InterlockedExchange(&confirmSequence_, 0);
        InterlockedExchange(&nativeFault_, 0);
    }

    Result OpenTutorial()
    {
        const Result eligibility = Eligibility();
        if (eligibility != kAccepted)
            return Publish(eligibility);

        void* existing =
            InterlockedCompareExchangePointer(
                &tutorialPopup_, nullptr, nullptr);
        if (existing) {
            if (!ValidObject(existing, tutorialVtable_))
                return Publish(kNativeObjectMismatch);
            return Publish(
                PopupRoot(existing)
                    ? kTutorialOpened
                    : kWaitingForTutorial);
        }
        if (InterlockedCompareExchange(&openAttempted_, 1, 0) != 0)
            return Publish(kWaitingForTutorial);

        alignas(4) uint8_t prototype[kTutorialObjectSize] = {};
        alignas(4) uint8_t name[kNativeWStringSize] = {};
        bool prototypeConstructed = false;
        bool nameConstructed = false;
        bool invoked = false;
        __try {
            tutorialCtor_(prototype);
            prototypeConstructed = true;
            if (*reinterpret_cast<void***>(prototype) != tutorialVtable_)
                __leave;
            nativeWStringCtor_(name, L"BattleShipTutorial");
            nameConstructed = true;
            namedUiEnqueue_(prototype, name, 1);
            invoked = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&nativeFault_, 1);
        }
        if (nameConstructed) {
            __try {
                nativeWStringDtor_(name);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedExchange(&nativeFault_, 1);
                invoked = false;
            }
        }
        if (prototypeConstructed) {
            __try {
                tutorialDtor_(prototype);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedExchange(&nativeFault_, 1);
                invoked = false;
            }
        }
        if (!invoked)
            return Publish(
                InterlockedCompareExchange(&nativeFault_, 0, 0)
                    ? kNativeFault
                    : kOpenFailed);

        existing = InterlockedCompareExchangePointer(
            &tutorialPopup_, nullptr, nullptr);
        if (!existing)
            return Publish(kWaitingForTutorial);
        if (!ValidObject(existing, tutorialVtable_))
            return Publish(kNativeObjectMismatch);
        return Publish(
            PopupRoot(existing)
                ? kTutorialOpened
                : kWaitingForTutorial);
    }

    // Advances one real tutorial page per call. On the final page it invokes
    // CloseBtn through the shipped callback, using the manager-owned control,
    // its embedded native name, and the current UI thread.
    Result DriveTutorial()
    {
        const Result eligibility = Eligibility();
        if (eligibility != kAccepted)
            return Publish(eligibility);
        if (InterlockedCompareExchange(&requestAttempted_, 0, 0) != 0)
            return Publish(
                InterlockedCompareExchange(&requestSent_, 0, 0)
                    ? kRequestSent
                    : kNativeRequestPending);

        void* popup = InterlockedCompareExchangePointer(
            &tutorialPopup_, nullptr, nullptr);
        if (!popup)
            return Publish(kWaitingForTutorial);
        if (!ValidObject(popup, tutorialVtable_))
            return Publish(kNativeObjectMismatch);

        uint8_t page = 0;
        uint32_t pageCount = 0;
        __try {
            page = *reinterpret_cast<const uint8_t*>(
                static_cast<const uint8_t*>(popup) + kPopupPage);
            pageCount = *reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(popup) + kPopupPageCount);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&nativeFault_, 1);
            return Publish(kNativeFault);
        }
        if (!pageCount || pageCount > UINT8_MAX || page >= pageCount)
            return Publish(kPageStateInvalid);

        if (static_cast<uint32_t>(page) + 1u < pageCount) {
            const Result invoked =
                InvokeControl(popup, L"NextBtn");
            if (invoked != kAccepted)
                return Publish(invoked);
            uint8_t after = 0;
            __try {
                after = *reinterpret_cast<const uint8_t*>(
                    static_cast<const uint8_t*>(popup) + kPopupPage);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedExchange(&nativeFault_, 1);
                return Publish(kNativeFault);
            }
            if (after != static_cast<uint8_t>(page + 1))
                return Publish(kPageDidNotMove);
            return Publish(kPaging);
        }

        if (InterlockedCompareExchange(&requestAttempted_, 1, 0) != 0)
            return Publish(kNativeRequestPending);
        const NativeActions::TransportObservation before =
            native_->TransportState();
        const Result invoked =
            InvokeControl(popup, L"CloseBtn");
        if (invoked != kAccepted)
            return Publish(invoked);
        const NativeActions::TransportObservation after =
            native_->TransportState();
        if (after.send_sequence == before.send_sequence ||
            after.opcode != kTutorialRequestOpcode ||
            after.packet_size != kTutorialRequestSize ||
            after.result == 0)
            return Publish(kRequestNotSent);

        InterlockedExchange(
            &requestTransportSequence_,
            static_cast<LONG>(after.send_sequence));
        InterlockedExchange(&requestSent_, 1);
        return Publish(kRequestSent);
    }

    Result ConfirmAcquisitionPopup()
    {
        const Observation observed = State();
        if (observed.result == kConfirmed)
            return kConfirmed;
        if (!observed.reply_sequence ||
            observed.reply_opcode != kTutorialAcceptedOpcode ||
            !observed.reply_consumed)
            return Publish(
                observed.reply_sequence
                    ? static_cast<Result>(observed.result)
                    : kResultPopupUnavailable);

        void* popup = InterlockedCompareExchangePointer(
            &resultPopup_, nullptr, nullptr);
        if (!popup)
            return kResultPopupUnavailable;
        if (!ValidObject(popup, resultPopupVtable_))
            return Publish(kNativeObjectMismatch);

        void* root = nullptr;
        __try {
            root = PopupRoot(popup);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&nativeFault_, 1);
            return Publish(kNativeFault);
        }
        if (!root)
            return kResultPopupUnavailable;

        const Result invoked =
            InvokeControl(
                popup,
                L"OkBtn",
                resultPopupAction_,
                resultPopupVtable_);
        if (invoked != kAccepted)
            return Publish(invoked);
        // The callback has now closed the manager-owned popup. Retain only the
        // pointer value for the atomic comparison; never dereference it again.
        InterlockedCompareExchangePointer(
            &resultPopup_, nullptr, popup);

        const ProfileSnapshot profile = ReadProfile();
        if (!profile.available ||
            profile.tutorial_level < kCompletedTutorialLevel ||
            (!profile.ship_uid_low && !profile.ship_uid_high))
            return Publish(kNativeStateMismatch);
        InterlockedIncrement(&confirmSequence_);
        return Publish(kConfirmed);
    }

    Observation State() const
    {
        Observation observed = {};
        observed.result = static_cast<uint32_t>(
            InterlockedCompareExchange(&lastResult_, 0, 0));
        observed.native_app_state =
            native_ ? native_->AppState() : UINT32_MAX;
        observed.request_transport_sequence = static_cast<uint32_t>(
            InterlockedCompareExchange(
                &requestTransportSequence_, 0, 0));
        observed.reply_sequence = static_cast<uint32_t>(
            InterlockedCompareExchange(&replySequence_, 0, 0));
        observed.confirm_sequence = static_cast<uint32_t>(
            InterlockedCompareExchange(&confirmSequence_, 0, 0));
        observed.request_sent = static_cast<uint8_t>(
            InterlockedCompareExchange(&requestSent_, 0, 0) != 0);
        observed.reply_consumed = static_cast<uint8_t>(
            InterlockedCompareExchange(&replyConsumed_, 0, 0) != 0);
        observed.native_fault = static_cast<uint8_t>(
            InterlockedCompareExchange(&nativeFault_, 0, 0) != 0);

        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before =
                InterlockedCompareExchange(&replyWriteVersion_, 0, 0);
            if (before & 1)
                continue;
            const uint32_t reply = static_cast<uint32_t>(
                InterlockedCompareExchange(&replyOpcodeError_, 0, 0));
            observed.reply_opcode =
                static_cast<uint16_t>(reply & 0xFFFFu);
            observed.reply_error =
                static_cast<uint16_t>(reply >> 16);
            observed.ship_uid_low = static_cast<uint32_t>(
                InterlockedCompareExchange(&replyShipUidLow_, 0, 0));
            observed.ship_uid_high = static_cast<uint32_t>(
                InterlockedCompareExchange(&replyShipUidHigh_, 0, 0));
            observed.ship_id = static_cast<uint32_t>(
                InterlockedCompareExchange(&replyShipId_, 0, 0));
            observed.leader_uid_low = static_cast<uint32_t>(
                InterlockedCompareExchange(&replyLeaderUidLow_, 0, 0));
            observed.leader_uid_high = static_cast<uint32_t>(
                InterlockedCompareExchange(&replyLeaderUidHigh_, 0, 0));
            const LONG after =
                InterlockedCompareExchange(&replyWriteVersion_, 0, 0);
            if (before == after && !(after & 1))
                break;
        }

        const ProfileSnapshot profile = ReadProfile();
        if (profile.available) {
            observed.profile_pilot_id = profile.pilot_id;
            observed.tutorial_level = profile.tutorial_level;
            if (!observed.ship_uid_low && !observed.ship_uid_high) {
                observed.ship_uid_low = profile.ship_uid_low;
                observed.ship_uid_high = profile.ship_uid_high;
                observed.ship_id = profile.ship_id;
                observed.leader_uid_low = profile.leader_uid_low;
                observed.leader_uid_high = profile.leader_uid_high;
            }
        }

        void* tutorial = InterlockedCompareExchangePointer(
            &tutorialPopup_, nullptr, nullptr);
        if (tutorial && ValidObject(tutorial, tutorialVtable_)) {
            __try {
                observed.tutorial_popup_ready =
                    PopupRoot(tutorial) != nullptr;
                observed.page = *reinterpret_cast<const uint8_t*>(
                    static_cast<const uint8_t*>(tutorial) + kPopupPage);
                const uint32_t pageCount =
                    *reinterpret_cast<const uint32_t*>(
                        static_cast<const uint8_t*>(tutorial) +
                        kPopupPageCount);
                observed.page_count = pageCount <= UINT8_MAX
                    ? static_cast<uint8_t>(pageCount)
                    : 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                observed.native_fault = 1;
            }
        }
        void* result = InterlockedCompareExchangePointer(
            &resultPopup_, nullptr, nullptr);
        if (result && ValidObject(result, resultPopupVtable_)) {
            __try {
                observed.result_popup_ready =
                    PopupRoot(result) != nullptr;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                observed.native_fault = 1;
            }
        }
        return observed;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound: return "unbound";
        case kWaitingForLobby: return "waiting-for-lobby";
        case kWaitingForTutorial: return "waiting-for-tutorial";
        case kTutorialOpened: return "tutorial-opened";
        case kPaging: return "paging";
        case kRequestSent: return "request-sent";
        case kAccepted: return "accepted";
        case kConfirmed: return "confirmed";
        case kProfileUnavailable: return "profile-unavailable";
        case kNotEligible: return "not-eligible";
        case kAlreadyCompleted: return "already-completed";
        case kOpenFailed: return "open-failed";
        case kNativeObjectMismatch: return "native-object-mismatch";
        case kControlMissing: return "control-missing";
        case kControlNameInvalid: return "control-name-invalid";
        case kPageStateInvalid: return "page-state-invalid";
        case kPageDidNotMove: return "page-did-not-move";
        case kNativeRequestPending: return "native-request-pending";
        case kRequestNotSent: return "request-not-sent";
        case kReplyRejected: return "reply-rejected";
        case kReplyNotConsumed: return "reply-not-consumed";
        case kNativeStateMismatch: return "native-state-mismatch";
        case kResultPopupUnavailable: return "result-popup-unavailable";
        case kNativeFault: return "native-fault";
        default: return "unknown";
        }
    }

private:
    static constexpr uint32_t kLobbyAppState = 6;
    static constexpr uint32_t kRequiredTutorialLevel = 2;
    static constexpr uint32_t kCompletedTutorialLevel = 4;
    static constexpr uint16_t kTutorialRequestOpcode = 0x04FC;
    static constexpr uint32_t kTutorialRequestSize = 0x0A;
    static constexpr uint16_t kTutorialAcceptedOpcode = 0x08EF;
    static constexpr uint16_t kTutorialRejectedOpcode = 0x09E1;
    static constexpr uint32_t kUiButtonPressed = 0x0101;
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kTutorialObjectSize = 0x58;
    static constexpr size_t kPopupRoot = 0x4C;
    static constexpr size_t kPopupPage = 0x50;
    static constexpr size_t kPopupPageCount = 0x54;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kNativeStringSize = 0x10;
    static constexpr size_t kNativeStringCapacity = 0x14;
    static constexpr size_t kProfileTutorialLevel = 0xBC;
    static constexpr size_t kProfilePilotId = 0x138;
    static constexpr size_t kProfileBattleShip = 0x2A6;
    static constexpr size_t kBattleShipPayloadSize = 0x2BC;
    static constexpr size_t kBattleShipUidLow = 0x00;
    static constexpr size_t kBattleShipUidHigh = 0x04;
    static constexpr size_t kBattleShipId = 0x0C;
    static constexpr size_t kBattleShipLeaderUidLow = 0x1C;
    static constexpr size_t kBattleShipLeaderUidHigh = 0x20;
    static constexpr size_t kReplyPayload = 0x0A;
    static constexpr size_t kReplyError = 0x06;
    static constexpr size_t kReplyDispatchVtableSlot = 0x28 / sizeof(void*);
    static constexpr size_t kActionVtableSlot = 0x38 / sizeof(void*);
    static constexpr size_t kCloneVtableSlot = 0x54 / sizeof(void*);

    struct ProfileSnapshot
    {
        bool available;
        uint32_t pilot_id;
        uint32_t tutorial_level;
        uint32_t ship_uid_low;
        uint32_t ship_uid_high;
        uint32_t ship_id;
        uint32_t leader_uid_low;
        uint32_t leader_uid_high;
    };

    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn = void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(void*, const void*, uint32_t);
    using TutorialCtorFn = void*(__thiscall*)(void*);
    using TutorialDtorFn = void(__thiscall*)(void*);
    using NamedUiEnqueueFn =
        void(__cdecl*)(void*, const void*, int);
    using CloneFn = void*(__thiscall*)(void*);
    using PopupActionFn =
        void(__stdcall*)(void*, uint32_t, const void*, void*);
    using PacketDispatchFn =
        int(__thiscall*)(void*, int, int, int, int, void*);

    bool ValidBindings() const
    {
        __try {
            return
                tutorialVtable_[kReplyDispatchVtableSlot] ==
                    reinterpret_cast<void*>(tutorialDispatch_) &&
                tutorialVtable_[kActionVtableSlot] ==
                    reinterpret_cast<void*>(tutorialAction_) &&
                tutorialVtable_[kCloneVtableSlot] ==
                    reinterpret_cast<void*>(tutorialClone_) &&
                resultPopupVtable_[kActionVtableSlot] ==
                    reinterpret_cast<void*>(resultPopupAction_) &&
                resultPopupVtable_[kCloneVtableSlot] ==
                    reinterpret_cast<void*>(resultPopupClone_);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool ValidObject(const void* object, const void* vtable) const
    {
        if (!object || !vtable)
            return false;
        __try {
            return *reinterpret_cast<void* const*>(object) == vtable;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void* PopupRoot(void* popup) const
    {
        return *reinterpret_cast<void**>(
            static_cast<uint8_t*>(popup) + kPopupRoot);
    }

    ProfileSnapshot ReadProfile() const
    {
        ProfileSnapshot observed = {};
        if (!profilePointer_)
            return observed;
        __try {
            const uint8_t* profile = *profilePointer_;
            if (!profile)
                return observed;
            observed.available = true;
            observed.pilot_id =
                *reinterpret_cast<const uint32_t*>(
                    profile + kProfilePilotId);
            observed.tutorial_level =
                *reinterpret_cast<const uint32_t*>(
                    profile + kProfileTutorialLevel);
            observed.ship_uid_low =
                *reinterpret_cast<const uint32_t*>(
                    profile + kProfileBattleShip +
                    kBattleShipUidLow);
            observed.ship_uid_high =
                *reinterpret_cast<const uint32_t*>(
                    profile + kProfileBattleShip +
                    kBattleShipUidHigh);
            observed.ship_id =
                *reinterpret_cast<const uint32_t*>(
                    profile + kProfileBattleShip +
                    kBattleShipId);
            observed.leader_uid_low =
                *reinterpret_cast<const uint32_t*>(
                    profile + kProfileBattleShip +
                    kBattleShipLeaderUidLow);
            observed.leader_uid_high =
                *reinterpret_cast<const uint32_t*>(
                    profile + kProfileBattleShip +
                    kBattleShipLeaderUidHigh);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
        }
        return observed;
    }

    Result Eligibility() const
    {
        if (!bound_ || !native_)
            return kUnbound;
        if (native_->AppState() != kLobbyAppState)
            return kWaitingForLobby;
        const ProfileSnapshot profile = ReadProfile();
        if (!profile.available || !profile.pilot_id)
            return kProfileUnavailable;
        if (profile.tutorial_level >= kCompletedTutorialLevel ||
            profile.ship_uid_low || profile.ship_uid_high)
            return kAlreadyCompleted;
        if (profile.tutorial_level != kRequiredTutorialLevel)
            return kNotEligible;
        return kAccepted;
    }

    void* FindControl(void* root, const wchar_t* name) const
    {
        if (!root || !name || !nativeUiFindControl_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_)
            return nullptr;
        alignas(4) uint8_t nativeName[kNativeWStringSize] = {};
        bool constructed = false;
        void* control = nullptr;
        __try {
            nativeWStringCtor_(nativeName, name);
            constructed = true;
            control = nativeUiFindControl_(root, nativeName, 4);
        } __finally {
            if (constructed)
                nativeWStringDtor_(nativeName);
        }
        return control;
    }

    bool ControlHasName(
        const void* control,
        const wchar_t* expected) const
    {
        if (!control || !expected)
            return false;
        __try {
            const uint8_t* nativeName =
                static_cast<const uint8_t*>(control) + kControlName;
            const uint32_t size =
                *reinterpret_cast<const uint32_t*>(
                    nativeName + kNativeStringSize);
            const uint32_t capacity =
                *reinterpret_cast<const uint32_t*>(
                    nativeName + kNativeStringCapacity);
            const size_t expectedSize = wcslen(expected);
            if (size != expectedSize || size > 63)
                return false;
            const wchar_t* value =
                capacity < 8
                    ? reinterpret_cast<const wchar_t*>(nativeName)
                    : *reinterpret_cast<wchar_t* const*>(nativeName);
            return value &&
                wmemcmp(value, expected, expectedSize) == 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    Result InvokeControl(
        void* popup,
        const wchar_t* name,
        PopupActionFn action = nullptr,
        const void* expectedVtable = nullptr)
    {
        if (!action)
            action = tutorialAction_;
        if (!expectedVtable)
            expectedVtable = tutorialVtable_;
        if (!ValidObject(popup, expectedVtable))
            return kNativeObjectMismatch;

        void* root = nullptr;
        __try {
            root = PopupRoot(popup);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&nativeFault_, 1);
            return kNativeFault;
        }
        if (!root)
            return kWaitingForTutorial;
        void* control = nullptr;
        __try {
            control = FindControl(root, name);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&nativeFault_, 1);
            return kNativeFault;
        }
        if (!control)
            return kControlMissing;
        if (!ControlHasName(control, name))
            return kControlNameInvalid;

        __try {
            const void* nativeName =
                static_cast<const uint8_t*>(control) + kControlName;
            action(
                popup,
                kUiButtonPressed,
                nativeName,
                control);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedExchange(&nativeFault_, 1);
            return kNativeFault;
        }
        return kAccepted;
    }

    Result Publish(Result result) const
    {
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(result));
        return result;
    }

    static void* __fastcall ObserveTutorialClone(
        void* self,
        void*)
    {
        BattleShipActions* actions = active_;
        if (!actions || !actions->tutorialClone_)
            return nullptr;
        void* live = actions->tutorialClone_(self);
        if (live &&
            actions->ValidObject(live, actions->tutorialVtable_))
            InterlockedExchangePointer(
                &actions->tutorialPopup_, live);
        return live;
    }

    static void* __fastcall ObserveResultPopupClone(
        void* self,
        void*)
    {
        BattleShipActions* actions = active_;
        if (!actions || !actions->resultPopupClone_)
            return nullptr;
        void* live = actions->resultPopupClone_(self);
        if (live &&
            actions->ValidObject(live, actions->resultPopupVtable_))
            InterlockedExchangePointer(
                &actions->resultPopup_, live);
        return live;
    }

    static int __fastcall ObserveTutorialDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        BattleShipActions* actions = active_;
        if (!actions || !actions->tutorialDispatch_)
            return 0;

        const uint16_t normalizedOpcode =
            static_cast<uint16_t>(opcode & 0xFFFF);
        const bool accepted =
            normalizedOpcode == kTutorialAcceptedOpcode;
        const bool rejected =
            normalizedOpcode == kTutorialRejectedOpcode;
        alignas(4) uint8_t payload[kBattleShipPayloadSize] = {};
        uint16_t error = 0;
        bool copied = false;
        if (packet && (accepted || rejected)) {
            __try {
                const uint8_t* bytes =
                    static_cast<const uint8_t*>(packet);
                if (accepted) {
                    memcpy(
                        payload,
                        bytes + kReplyPayload,
                        sizeof(payload));
                    copied = true;
                } else {
                    error = *reinterpret_cast<const uint16_t*>(
                        bytes + kReplyError);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                InterlockedExchange(&actions->nativeFault_, 1);
                copied = false;
                error = UINT16_MAX;
            }
            InterlockedCompareExchangePointer(
                &actions->tutorialPopup_, nullptr, self);
        }

        // The shipped handler owns the packet and performs the authoritative
        // profile copy, popup creation, error display, and tutorial close.
        const int result =
            actions->tutorialDispatch_(
                self, a, b, c, opcode, packet);
        if (!accepted && !rejected)
            return result;

        bool consumed = false;
        ProfileSnapshot profile = {};
        if (accepted && copied) {
            profile = actions->ReadProfile();
            __try {
                const uint8_t* live =
                    actions->profilePointer_
                        ? *actions->profilePointer_
                        : nullptr;
                consumed = live &&
                    memcmp(
                        live + kProfileBattleShip,
                        payload,
                        sizeof(payload)) == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                consumed = false;
                InterlockedExchange(&actions->nativeFault_, 1);
            }
        }

        InterlockedIncrement(&actions->replyWriteVersion_);
        InterlockedExchange(
            &actions->replyOpcodeError_,
            static_cast<LONG>(
                static_cast<uint32_t>(normalizedOpcode) |
                (static_cast<uint32_t>(error) << 16)));
        InterlockedExchange(
            &actions->replyConsumed_,
            consumed ? 1 : 0);
        if (accepted && copied) {
            InterlockedExchange(
                &actions->replyShipUidLow_,
                *reinterpret_cast<const LONG*>(
                    payload + kBattleShipUidLow));
            InterlockedExchange(
                &actions->replyShipUidHigh_,
                *reinterpret_cast<const LONG*>(
                    payload + kBattleShipUidHigh));
            InterlockedExchange(
                &actions->replyShipId_,
                *reinterpret_cast<const LONG*>(
                    payload + kBattleShipId));
            InterlockedExchange(
                &actions->replyLeaderUidLow_,
                *reinterpret_cast<const LONG*>(
                    payload + kBattleShipLeaderUidLow));
            InterlockedExchange(
                &actions->replyLeaderUidHigh_,
                *reinterpret_cast<const LONG*>(
                    payload + kBattleShipLeaderUidHigh));
        }
        InterlockedIncrement(&actions->replySequence_);
        InterlockedIncrement(&actions->replyWriteVersion_);

        Result observedResult = kReplyRejected;
        if (accepted) {
            if (!copied || !consumed) {
                observedResult = kReplyNotConsumed;
            } else if ((!profile.ship_uid_low &&
                        !profile.ship_uid_high) ||
                       !profile.ship_id) {
                observedResult = kNativeStateMismatch;
            } else {
                observedResult = kAccepted;
            }
        }
        actions->Publish(observedResult);
        return result;
    }

    template <typename Fn>
    Fn Symbol(const char* name) const
    {
        return api_
            ? reinterpret_cast<Fn>(api_->Symbol(mod_, name))
            : nullptr;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    TutorialCtorFn tutorialCtor_ = nullptr;
    TutorialDtorFn tutorialDtor_ = nullptr;
    NamedUiEnqueueFn namedUiEnqueue_ = nullptr;
    CloneFn tutorialClone_ = nullptr;
    PopupActionFn tutorialAction_ = nullptr;
    PacketDispatchFn tutorialDispatch_ = nullptr;
    void** tutorialVtable_ = nullptr;
    CloneFn resultPopupClone_ = nullptr;
    PopupActionFn resultPopupAction_ = nullptr;
    void** resultPopupVtable_ = nullptr;
    bool bound_ = false;

    mutable PVOID volatile tutorialPopup_ = nullptr;
    mutable PVOID volatile resultPopup_ = nullptr;
    mutable volatile LONG lastResult_ = kUnbound;
    mutable volatile LONG openAttempted_ = 0;
    mutable volatile LONG requestAttempted_ = 0;
    mutable volatile LONG requestSent_ = 0;
    mutable volatile LONG requestTransportSequence_ = 0;
    mutable volatile LONG replyWriteVersion_ = 0;
    mutable volatile LONG replySequence_ = 0;
    mutable volatile LONG replyOpcodeError_ = 0;
    mutable volatile LONG replyConsumed_ = 0;
    mutable volatile LONG replyShipUidLow_ = 0;
    mutable volatile LONG replyShipUidHigh_ = 0;
    mutable volatile LONG replyShipId_ = 0;
    mutable volatile LONG replyLeaderUidLow_ = 0;
    mutable volatile LONG replyLeaderUidHigh_ = 0;
    mutable volatile LONG confirmSequence_ = 0;
    mutable volatile LONG nativeFault_ = 0;

    inline static BattleShipActions* active_ = nullptr;
};
