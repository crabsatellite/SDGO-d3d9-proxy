#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"

// Enters the shipped My Room / hangar scene and makes its normal UnitRadio
// selection available to later GameTest actions.
//
// This adapter never writes the application state or registers a service. In
// app 6 it invokes the real downmenu MyRoomRadio callback and accepts that
// action only when NativeActions observes its original 10-byte 0x0621 request.
// In app 5 it first waits for the client's default unitmgr construction. If
// that service is still absent after mainframe has a real UI root, it invokes
// the real UnitRadio callback once and then waits for the original service
// manager to expose the constructed unitmgr.
class HangarNavigation
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForLobby = 1,
        kWaitingForLobbyUi = 2,
        kLobbyBusy = 3,
        kOpeningHangar = 4,
        kWaitingForHangar = 5,
        kWaitingForMainframe = 6,
        kOpeningUnitManager = 7,
        kWaitingForUnitManager = 8,
        kReady = 9,
        kOpeningGameRoom = 10,
        kWaitingForGameRoom = 11,
        kOpeningUnitManagement = 12,
        kWaitingForUnitManagement = 13,

        kDownmenuServiceMismatch = 0x100,
        kDownmenuControlMismatch = 0x101,
        kHangarRequestRejected = 0x102,
        kMainframeServiceMismatch = 0x103,
        kUnitRadioControlMismatch = 0x104,
        kUnitManagerServiceMismatch = 0x105,
        kNativeFault = 0x106,
        kOperatorMenuMismatch = 0x107,
        kOperatorMenuControlMismatch = 0x108,
        kUnitManagementMismatch = 0x109,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t native_app_state;
        uint32_t request_transport_sequence;
        uint32_t unit_radio_invocations;
        uint16_t request_opcode;
        uint16_t reserved;
        uint32_t request_size;
        uint32_t request_result;
        uint8_t downmenu_present;
        uint8_t mainframe_present;
        uint8_t unit_manager_present;
        uint8_t mainframe_root_ready;
        uint8_t unit_manager_root_ready;
        uint8_t lobby_busy;
        uint8_t reserved_bytes[2];
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;

        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>(
                "flow.service_find_by_name");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>(
                "flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>(
                "flow.native_wstring_dtor");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>(
                "flow.native_ui_find_control");

        downmenuVtable_ =
            Symbol<void*>("flow.hangar_downmenu_vtable");
        downmenuCallback_ =
            Symbol<UiCallbackFn>(
                "flow.hangar_downmenu_callback");
        mainframeVtable_ =
            Symbol<void*>("flow.hangar_mainframe_vtable");
        mainframeCallback_ =
            Symbol<UiCallbackFn>(
                "flow.hangar_mainframe_callback");
        unitManagerVtable_ =
            Symbol<void*>(
                "flow.battleship_lobby_manager_vtable");
        unitManagerCallback_ =
            Symbol<UiCallbackFn>(
                "flow.battleship_lobby_select_business");

        if (!api_ || !mod_ || !native_ ||
            !serviceManagedPointer_ || !serviceFindByName_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_ ||
            !nativeUiFindControl_ || !downmenuVtable_ ||
            !downmenuCallback_ || !mainframeVtable_ ||
            !mainframeCallback_ || !unitManagerVtable_ ||
            !unitManagerCallback_)
            return false;

        __try {
            if (!CallbackSlotMatches(
                    downmenuVtable_,
                    downmenuCallback_) ||
                !CallbackSlotMatches(
                    mainframeVtable_,
                    mainframeCallback_) ||
                !CallbackSlotMatches(
                    unitManagerVtable_,
                    unitManagerCallback_))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        lobbyRequestInvoked_ = false;
        unitRadioInvoked_ = false;
        unitManagementInvoked_ = false;
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(kWaitingForLobby));
        InterlockedExchange(&requestTransportSequence_, 0);
        InterlockedExchange(&unitRadioInvocations_, 0);
        InterlockedExchange(&requestOpcode_, 0);
        InterlockedExchange(&requestSize_, 0);
        InterlockedExchange(&requestResult_, 0);
        InterlockedExchange(&downmenuPresent_, 0);
        InterlockedExchange(&mainframePresent_, 0);
        InterlockedExchange(&unitManagerPresent_, 0);
        InterlockedExchange(&mainframeRootReady_, 0);
        InterlockedExchange(&unitManagerRootReady_, 0);
        InterlockedExchange(&lobbyBusy_, 0);
        bound_ = true;
        return true;
    }

    Result EnsureHangar()
    {
        if (!bound_ || !native_)
            return Publish(kUnbound);

        __try {
            const uint32_t appState = native_->AppState();
            if (appState == kHangarAppState)
                return EnsureUnitManager();

            if (lobbyRequestInvoked_)
                return Publish(kWaitingForHangar);
            if (appState != kGameRoomAppState)
                return Publish(kWaitingForLobby);

            return OpenHangar();
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    // A successful channel-switch reply can precede reconstruction of the
    // native Game Room page. Enter its real radio business and wait for its
    // actual packet consumer/control, rather than send a room request early.
    void RequireGameRoomReentry(uint32_t generation=0){awaitRoomListRemoval_=true;requiredRoomGeneration_=generation;gameRoomInvoked_=false;}

    Result EnsureGameRoom(const void* roomListVtable,const void* modalLoadingVtable=nullptr,uint32_t nativeGeneration=0)
    {
        if (!bound_ || !native_ || !roomListVtable)return Publish(kUnbound);
        __try {
            const auto selected=native_->OnboardingState();
            if (selected.channel_id!=gameRoomChannel_) {
                gameRoomChannel_=selected.channel_id;gameRoomInvoked_=false;
            }
            if (native_->AppState()!=kGameRoomAppState)return Publish(kWaitingForLobby);
            auto* manager=serviceManagedPointer_ ? static_cast<uint8_t*>(*serviceManagedPointer_):nullptr;
            if (!manager)return Publish(kWaitingForMainframe);
            auto* head=*reinterpret_cast<uint8_t**>(manager+0x44);
            auto* node=head ? *reinterpret_cast<uint8_t**>(head):nullptr;
            bool roomPresent=false,roomReady=false,modalPresent=false;
            for (unsigned count=0;node && node!=head && count<512;++count) {
                auto* service=*reinterpret_cast<uint8_t**>(node+8);
                if(service && modalLoadingVtable && *reinterpret_cast<void**>(service)==modalLoadingVtable)modalPresent=true;
                if (service && *reinterpret_cast<void**>(service)==roomListVtable) {
                    roomPresent=true;
                    auto* button=*reinterpret_cast<uint8_t**>(service+0x84);
                    void* root=button ? *reinterpret_cast<void**>(button+kControlParent):nullptr;
                    if (root && button && FindControl(root,L"MakeRoomBtn",4)==button &&
                        ControlHasName(button,L"MakeRoomBtn") && button[0x162] && button[0x167]) {
                        roomReady=true;
                    }
                }
                node=*reinterpret_cast<uint8_t**>(node);
            }
            if (awaitRoomListRemoval_) {
                if(roomPresent && nativeGeneration<=requiredRoomGeneration_)return Publish(kWaitingForGameRoom);
                awaitRoomListRemoval_=false;
            }
            if(modalPresent)return Publish(kWaitingForGameRoom);
            if(roomReady){gameRoomInvoked_=false;return Publish(kReady);}
            if (gameRoomInvoked_)return Publish(kWaitingForGameRoom);
            void* downmenu=nullptr;
            const auto found=ResolveService(L"downmenu",downmenuVtable_,&downmenu);
            if (found==kServiceMismatched)return Publish(kDownmenuServiceMismatch);
            if (found==kServiceAbsent)return Publish(kWaitingForLobbyUi);
            auto* service=static_cast<uint8_t*>(downmenu);
            void* root=*reinterpret_cast<void**>(service+kDownmenuRoot);
            if (!root)return Publish(kWaitingForLobbyUi);
            if (*reinterpret_cast<const uint32_t*>(service+kDownmenuMode)==8 && selected.starter_draw_busy)
                return Publish(kLobbyBusy);
            auto* control=static_cast<uint8_t*>(FindControl(root,L"GameRoomRadio"));
            if (!control)return Publish(kWaitingForLobbyUi);
            if (*reinterpret_cast<void**>(service+0x48)!=control ||
                *reinterpret_cast<void**>(control+kControlParent)!=root || !ControlHasName(control,L"GameRoomRadio"))
                return Publish(kDownmenuControlMismatch);
            if (!control[0x162] || !control[0x167])return Publish(kWaitingForLobbyUi);
            const auto before=native_->TransportState();
            downmenuCallback_(downmenu,kRadioMessage,control+kControlName,control);
            const auto after=native_->TransportState();
            if (after.send_sequence==before.send_sequence || after.opcode!=kHangarRequestOpcode ||
                after.packet_size!=kHangarRequestSize || !after.result)return Publish(kHangarRequestRejected);
            gameRoomInvoked_=true;
            return Publish(kOpeningGameRoom);
        }__except(EXCEPTION_EXECUTE_HANDLER){return Publish(kNativeFault);}
    }

    // UnitRadio selects the 3D unit viewer. The actual unit-management grid
    // is a separate native page, opened by operatormenu's UnitBtn.
    Result EnsureUnitManagementPage()
    {
        if (!bound_ || !native_) return Publish(kUnbound);
        __try {
            void* expected=Symbol<void*>("flow.unit_management_vtable");
            if (!expected) return Publish(kUnbound);
            void* grid=nullptr;
            const auto result=ResolveService(L"UnitIndexingCubbyhole",expected,&grid);
            if (result==kServiceMismatched) return Publish(kUnitManagementMismatch);
            if (grid) {
                auto* root=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(grid)+0x44);
                if (root && root[0x162]) return Publish(kReady);
            }
            if (unitManagementInvoked_) return Publish(kWaitingForUnitManagement);
            const auto hangar=EnsureHangar();
            if (hangar!=kReady) return hangar;
            const auto clicked=ClickOperatorMenuButton(L"UnitBtn",0x80);
            if (clicked!=kReady) return clicked;
            unitManagementInvoked_=true;
            return Publish(kOpeningUnitManagement);
        } __except(EXCEPTION_EXECUTE_HANDLER) { return Publish(kNativeFault); }
    }

    // Validate the original service, native button lookup, cached identity,
    // parent, name and input state before invoking its original callback.
    Result ClickOperatorMenuButton(const wchar_t* name,size_t cachedOffset)
    {
        if (!bound_ || !native_) return Publish(kUnbound);
        __try {
            const auto app=native_->AppState();
            if (app!=kHangarAppState && app!=kGameRoomAppState) return Publish(kWaitingForLobby);
            void* expected=Symbol<void*>("flow.operator_menu_vtable");
            auto callback=Symbol<UiCallbackFn>("flow.operator_menu_callback");
            if (!expected || !callback) return Publish(kUnbound);
            if (!CallbackSlotMatches(expected,callback)) return Publish(kOperatorMenuMismatch);
            void* menu=nullptr;
            const auto result=ResolveService(L"operatormenu",expected,&menu);
            if (result==kServiceMismatched) return Publish(kOperatorMenuMismatch);
            if (!menu) return Publish(kWaitingForLobbyUi);
            auto* control=*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(menu)+cachedOffset);
            if (!control) return Publish(kWaitingForLobbyUi);
            auto* root=*reinterpret_cast<uint8_t**>(control+kControlParent);
            if (!root) return Publish(kWaitingForLobbyUi);
            if (FindControl(root,name,4)!=control || !ControlHasName(control,name))
                return Publish(kOperatorMenuControlMismatch);
            if (!root[0x162] || !control[0x162] || !control[0x167]) return Publish(kWaitingForLobbyUi);
            callback(menu,0x101,control+kControlName,control);
            return Publish(kReady);
        } __except(EXCEPTION_EXECUTE_HANDLER) { return Publish(kNativeFault); }
    }

    Result ClickBottomRadio(const wchar_t* name,size_t cachedOffset)
    {
        if(!bound_ || !native_)return Publish(kUnbound);
        __try {
            void* menu=nullptr;
            const auto result=ResolveService(L"downmenu",downmenuVtable_,&menu);
            if(result==kServiceMismatched)return Publish(kDownmenuServiceMismatch);
            if(!menu)return Publish(kWaitingForLobbyUi);
            auto* bytes=static_cast<uint8_t*>(menu);
            auto* root=*reinterpret_cast<uint8_t**>(bytes+kDownmenuRoot);
            auto* control=*reinterpret_cast<uint8_t**>(bytes+cachedOffset);
            if(!root || !control)return Publish(kWaitingForLobbyUi);
            if(FindControl(root,name)!=control || *reinterpret_cast<void**>(control+kControlParent)!=root || !ControlHasName(control,name))
                return Publish(kDownmenuControlMismatch);
            if(!root[0x162] || !control[0x162] || !control[0x167])return Publish(kWaitingForLobbyUi);
            if(*reinterpret_cast<uint32_t*>(bytes+kDownmenuMode)==8 && native_->OnboardingState().starter_draw_busy)return Publish(kLobbyBusy);
            downmenuCallback_(menu,kRadioMessage,control+kControlName,control);
            return Publish(kReady); // original callback invoked; target page is independently observed
        }__except(EXCEPTION_EXECUTE_HANDLER){return Publish(kNativeFault);}
    }

    Observation State() const
    {
        Observation observed = {};
        observed.result = static_cast<uint32_t>(
            InterlockedCompareExchange(
                &lastResult_,
                0,
                0));
        observed.native_app_state =
            native_ ? native_->AppState() : UINT32_MAX;
        observed.request_transport_sequence =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &requestTransportSequence_,
                    0,
                    0));
        observed.unit_radio_invocations =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &unitRadioInvocations_,
                    0,
                    0));
        observed.request_opcode =
            static_cast<uint16_t>(
                InterlockedCompareExchange(
                    &requestOpcode_,
                    0,
                    0));
        observed.request_size =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &requestSize_,
                    0,
                    0));
        observed.request_result =
            static_cast<uint32_t>(
                InterlockedCompareExchange(
                    &requestResult_,
                    0,
                    0));
        observed.downmenu_present =
            InterlockedCompareExchange(
                &downmenuPresent_,
                0,
                0) != 0;
        observed.mainframe_present =
            InterlockedCompareExchange(
                &mainframePresent_,
                0,
                0) != 0;
        observed.unit_manager_present =
            InterlockedCompareExchange(
                &unitManagerPresent_,
                0,
                0) != 0;
        observed.mainframe_root_ready =
            InterlockedCompareExchange(
                &mainframeRootReady_,
                0,
                0) != 0;
        observed.unit_manager_root_ready =
            InterlockedCompareExchange(
                &unitManagerRootReady_,
                0,
                0) != 0;
        observed.lobby_busy =
            InterlockedCompareExchange(
                &lobbyBusy_,
                0,
                0) != 0;
        return observed;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound:
            return "unbound";
        case kWaitingForLobby:
            return "waiting-for-lobby";
        case kWaitingForLobbyUi:
            return "waiting-for-lobby-ui";
        case kLobbyBusy:
            return "lobby-busy";
        case kOpeningHangar:
            return "opening-hangar";
        case kWaitingForHangar:
            return "waiting-for-hangar";
        case kWaitingForMainframe:
            return "waiting-for-mainframe";
        case kOpeningUnitManager:
            return "opening-unit-manager";
        case kWaitingForUnitManager:
            return "waiting-for-unit-manager";
        case kReady:
            return "ready";
        case kOpeningGameRoom:
            return "opening-game-room";
        case kWaitingForGameRoom:
            return "waiting-for-game-room";
        case kOpeningUnitManagement:
            return "opening-unit-management";
        case kWaitingForUnitManagement:
            return "waiting-for-unit-management";
        case kDownmenuServiceMismatch:
            return "downmenu-service-mismatch";
        case kDownmenuControlMismatch:
            return "downmenu-control-mismatch";
        case kHangarRequestRejected:
            return "hangar-request-rejected";
        case kMainframeServiceMismatch:
            return "mainframe-service-mismatch";
        case kUnitRadioControlMismatch:
            return "unit-radio-control-mismatch";
        case kUnitManagerServiceMismatch:
            return "unit-manager-service-mismatch";
        case kNativeFault:
            return "native-fault";
        case kOperatorMenuMismatch:
            return "operator-menu-mismatch";
        case kOperatorMenuControlMismatch:
            return "operator-menu-control-mismatch";
        case kUnitManagementMismatch:
            return "unit-management-mismatch";
        default:
            return "unknown";
        }
    }

private:
    enum ServiceResult
    {
        kServiceAbsent,
        kServiceMatched,
        kServiceMismatched,
    };

    static constexpr uint32_t kHangarAppState = 5;
    static constexpr uint32_t kGameRoomAppState = 6;
    static constexpr uint32_t kRadioMessage = 0x301;
    static constexpr uint16_t kHangarRequestOpcode = 0x0621;
    static constexpr uint32_t kHangarRequestSize = 10;
    static constexpr uint32_t kRadioControlKind = 0x0C;
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kDownmenuRoot = 0x44;
    static constexpr size_t kDownmenuMyRoomRadio = 0x54;
    static constexpr size_t kDownmenuMode = 0x60;
    static constexpr size_t kMainframeRoot = 0x64;
    static constexpr size_t kUnitManagerRoot = 0x64;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kControlParent = 0x15C;
    static constexpr size_t kNativeStringSize = 0x10;
    static constexpr size_t kNativeStringCapacity = 0x14;
    static constexpr size_t kUiCallbackVtableSlot = 0x38 / sizeof(void*);

    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn =
        void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(void*, const void*, uint32_t);
    using UiCallbackFn =
        uint32_t(__stdcall*)(
            void*,
            uint32_t,
            const void*,
            void*);

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !api_->Symbol || !name)
            return nullptr;
        return reinterpret_cast<T>(
            api_->Symbol(mod_, name));
    }

    static bool CallbackSlotMatches(
        void* vtable,
        UiCallbackFn callback)
    {
        return vtable && callback &&
               static_cast<void**>(vtable)
                       [kUiCallbackVtableSlot] ==
                   reinterpret_cast<void*>(callback);
    }

    Result Publish(Result result)
    {
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(result));
        return result;
    }

    Result OpenHangar()
    {
        void* downmenu = nullptr;
        const ServiceResult service = ResolveService(
            L"downmenu",
            downmenuVtable_,
            &downmenu);
        InterlockedExchange(
            &downmenuPresent_,
            downmenu ? 1 : 0);
        if (service == kServiceMismatched)
            return Publish(kDownmenuServiceMismatch);
        if (service == kServiceAbsent)
            return Publish(kWaitingForLobbyUi);

        uint8_t* bytes =
            static_cast<uint8_t*>(downmenu);
        void* root = *reinterpret_cast<void**>(
            bytes + kDownmenuRoot);
        void* cached = *reinterpret_cast<void**>(
            bytes + kDownmenuMyRoomRadio);
        if (!root || !cached)
            return Publish(kWaitingForLobbyUi);

        const bool busy =
            *reinterpret_cast<const uint32_t*>(
                bytes + kDownmenuMode) == 8 &&
            native_->OnboardingState()
                    .starter_draw_busy == 1;
        InterlockedExchange(
            &lobbyBusy_,
            busy ? 1 : 0);
        if (busy)
            return Publish(kLobbyBusy);

        void* found =
            FindControl(root, L"MyRoomRadio");
        if (!found)
            return Publish(kWaitingForLobbyUi);
        if (found != cached ||
            *reinterpret_cast<void**>(
                static_cast<uint8_t*>(found) +
                kControlParent) != root ||
            !ControlHasName(found, L"MyRoomRadio"))
            return Publish(kDownmenuControlMismatch);

        const NativeActions::TransportObservation before =
            native_->TransportState();
        downmenuCallback_(
            downmenu,
            kRadioMessage,
            static_cast<uint8_t*>(found) +
                kControlName,
            found);
        const NativeActions::TransportObservation after =
            native_->TransportState();
        lobbyRequestInvoked_ = true;
        InterlockedExchange(
            &requestTransportSequence_,
            static_cast<LONG>(after.send_sequence));
        InterlockedExchange(
            &requestOpcode_,
            static_cast<LONG>(after.opcode));
        InterlockedExchange(
            &requestSize_,
            static_cast<LONG>(after.packet_size));
        InterlockedExchange(
            &requestResult_,
            static_cast<LONG>(after.result));

        if (after.send_sequence == before.send_sequence ||
            after.opcode != kHangarRequestOpcode ||
            after.packet_size != kHangarRequestSize ||
            after.result == 0)
            return Publish(kHangarRequestRejected);
        return Publish(kOpeningHangar);
    }

    Result EnsureUnitManager()
    {
        void* unitManager = nullptr;
        ServiceResult unitService = ResolveService(
            L"unitmgr",
            unitManagerVtable_,
            &unitManager);
        InterlockedExchange(
            &unitManagerPresent_,
            unitManager ? 1 : 0);
        if (unitService == kServiceMismatched)
            return Publish(kUnitManagerServiceMismatch);
        if (unitService == kServiceMatched) {
            void* root = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(unitManager) +
                kUnitManagerRoot);
            InterlockedExchange(
                &unitManagerRootReady_,
                root ? 1 : 0);
            return Publish(
                root ? kReady : kWaitingForUnitManager);
        }
        InterlockedExchange(&unitManagerRootReady_, 0);

        void* mainframe = nullptr;
        const ServiceResult mainframeService =
            ResolveService(
                L"mainframe",
                mainframeVtable_,
                &mainframe);
        InterlockedExchange(
            &mainframePresent_,
            mainframe ? 1 : 0);
        if (mainframeService == kServiceMismatched)
            return Publish(kMainframeServiceMismatch);
        if (mainframeService == kServiceAbsent)
            return Publish(kWaitingForMainframe);

        void* root = *reinterpret_cast<void**>(
            static_cast<uint8_t*>(mainframe) +
            kMainframeRoot);
        InterlockedExchange(
            &mainframeRootReady_,
            root ? 1 : 0);
        if (!root)
            return Publish(kWaitingForMainframe);
        if (unitRadioInvoked_)
            return Publish(kWaitingForUnitManager);

        void* control =
            FindControl(root, L"UnitRadio");
        if (!control)
            return Publish(kWaitingForMainframe);
        if (*reinterpret_cast<void**>(
                static_cast<uint8_t*>(control) +
                kControlParent) != root ||
            !ControlHasName(control, L"UnitRadio"))
            return Publish(kUnitRadioControlMismatch);

        unitRadioInvoked_ = true;
        InterlockedIncrement(&unitRadioInvocations_);
        mainframeCallback_(
            mainframe,
            kRadioMessage,
            static_cast<uint8_t*>(control) +
                kControlName,
            control);

        unitManager = nullptr;
        unitService = ResolveService(
            L"unitmgr",
            unitManagerVtable_,
            &unitManager);
        InterlockedExchange(
            &unitManagerPresent_,
            unitManager ? 1 : 0);
        if (unitService == kServiceMismatched)
            return Publish(kUnitManagerServiceMismatch);
        if (unitService == kServiceMatched) {
            void* unitRoot = *reinterpret_cast<void**>(
                static_cast<uint8_t*>(unitManager) +
                kUnitManagerRoot);
            InterlockedExchange(
                &unitManagerRootReady_,
                unitRoot ? 1 : 0);
            if (unitRoot)
                return Publish(kReady);
        }
        return Publish(kOpeningUnitManager);
    }

    ServiceResult ResolveService(
        const wchar_t* name,
        void* expectedVtable,
        void** output) const
    {
        if (output)
            *output = nullptr;
        if (!name || !expectedVtable || !output ||
            !serviceManagedPointer_ ||
            !serviceFindByName_ ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return kServiceAbsent;

        void* manager = *serviceManagedPointer_;
        if (!manager)
            return kServiceAbsent;

        alignas(4) uint8_t descriptor[
            kServiceDescriptorSize] = {};
        *reinterpret_cast<uint32_t*>(descriptor) = 1;
        bool constructed = false;
        void* service = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    descriptor + sizeof(uint32_t),
                    name) != nullptr;
            if (constructed) {
                service =
                    serviceFindByName_(
                        manager,
                        descriptor);
            }
        } __finally {
            if (constructed) {
                nativeWStringDtor_(
                    descriptor +
                    sizeof(uint32_t));
            }
        }
        if (!service)
            return kServiceAbsent;
        *output = service;
        return *reinterpret_cast<void**>(service) ==
                       expectedVtable
                   ? kServiceMatched
                   : kServiceMismatched;
    }

    void* FindControl(
        void* root,
        const wchar_t* name,
        uint32_t kind=kRadioControlKind) const
    {
        if (!root || !name ||
            !nativeUiFindControl_ ||
            !nativeWStringCtor_ ||
            !nativeWStringDtor_)
            return nullptr;

        alignas(4) uint8_t nativeName[
            kNativeWStringSize] = {};
        bool constructed = false;
        void* control = nullptr;
        __try {
            constructed =
                nativeWStringCtor_(
                    nativeName,
                    name) != nullptr;
            if (constructed) {
                control = nativeUiFindControl_(
                    root,
                    nativeName,
                    kind);
            }
        } __finally {
            if (constructed)
                nativeWStringDtor_(nativeName);
        }
        return control;
    }

    static bool ControlHasName(
        const void* control,
        const wchar_t* expected)
    {
        if (!control || !expected)
            return false;
        const uint8_t* nativeName =
            static_cast<const uint8_t*>(control) +
            kControlName;
        const uint32_t size =
            *reinterpret_cast<const uint32_t*>(
                nativeName + kNativeStringSize);
        const uint32_t capacity =
            *reinterpret_cast<const uint32_t*>(
                nativeName + kNativeStringCapacity);
        const size_t expectedSize =
            wcslen(expected);
        if (size != expectedSize || size > 63)
            return false;
        const wchar_t* value =
            capacity < 8
                ? reinterpret_cast<const wchar_t*>(
                      nativeName)
                : *reinterpret_cast<
                      wchar_t* const*>(nativeName);
        return value &&
               wmemcmp(
                   value,
                   expected,
                   size) == 0;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    UiCallbackFn downmenuCallback_ = nullptr;
    UiCallbackFn mainframeCallback_ = nullptr;
    UiCallbackFn unitManagerCallback_ = nullptr;
    uint16_t gameRoomChannel_=0;
    bool gameRoomInvoked_=false;
    bool awaitRoomListRemoval_=false;
    uint32_t requiredRoomGeneration_=0;
    void** serviceManagedPointer_ = nullptr;
    void* downmenuVtable_ = nullptr;
    void* mainframeVtable_ = nullptr;
    void* unitManagerVtable_ = nullptr;
    bool bound_ = false;
    bool lobbyRequestInvoked_ = false;
    bool unitRadioInvoked_ = false;
    bool unitManagementInvoked_ = false;
    mutable volatile LONG lastResult_ = kUnbound;
    mutable volatile LONG requestTransportSequence_ = 0;
    mutable volatile LONG unitRadioInvocations_ = 0;
    mutable volatile LONG requestOpcode_ = 0;
    mutable volatile LONG requestSize_ = 0;
    mutable volatile LONG requestResult_ = 0;
    mutable volatile LONG downmenuPresent_ = 0;
    mutable volatile LONG mainframePresent_ = 0;
    mutable volatile LONG unitManagerPresent_ = 0;
    mutable volatile LONG mainframeRootReady_ = 0;
    mutable volatile LONG unitManagerRootReady_ = 0;
    mutable volatile LONG lobbyBusy_ = 0;
};
