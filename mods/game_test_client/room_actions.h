#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>

#include "native_actions.h"

// Drives the original room-list and wait-menu business paths. Requests go to
// the real server through NativeActions, while observations are published only
// after the original client handlers have consumed real replies/events.
class RoomActions
{
public:
    enum CreateResult : uint32_t
    {
        kCreateNone = 0,
        kCreateRejected = 1,
        kCreateAccepted = 2,
    };

    enum LeaveResult : uint32_t
    {
        kLeaveNone = 0,
        kLeaveRejected = 1,
        kLeaveAccepted = 2,
    };

    enum CreateStage : uint32_t
    {
        kCreateStageNone = 0,
        kCreateStagePlan = 1,
        kCreateStageMapLookup = 2,
        kCreateStagePacketInit = 3,
        kCreateStageSend = 4,
        kCreateStageComplete = 5,
        kCreateStageException = 6,
    };

    enum CreateError : uint32_t
    {
        kCreateErrorNone = 0,
        kCreateErrorUnavailable = 1,
        kCreateErrorInvalidPlan = 2,
        kCreateErrorMapGridUnavailable = 3,
        kCreateErrorMapRecordMissing = 4,
        kCreateErrorMapIndexOutOfRange = 5,
        kCreateErrorPacketInit = 6,
        kCreateErrorSend = 7,
        kCreateErrorException = 8,
    };

    struct CreateDiagnostics
    {
        uint32_t attempt;
        uint32_t stage;
        uint32_t error;
        uint32_t configured_map_id;
        uint16_t room_mode;
        uint8_t map_selection;
        uint8_t map_index;
    };

    struct Observation
    {
        uint32_t reply_sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint32_t room_id;
        uint16_t reply_opcode;
        uint8_t room_variant;
        uint8_t reserved;
    };

    struct LeaveObservation
    {
        uint32_t reply_sequence;
        uint32_t result;
        uint32_t native_app_state;
        uint16_t reply_opcode;
        uint16_t error;
        uint8_t room_object_type;
        uint8_t reserved[3];
    };

    struct NativeRoomObservation
    {
        uint32_t native_app_state;
        uint32_t room_id;
        uint32_t player_count;
        uint8_t room_variant;
        uint8_t room_mode;
        uint8_t model_present;
        uint8_t local_is_host;
        uint8_t wait_menu_observed;
        uint8_t reserved[3];
    };

    struct FreshRoomObservation
    {
        uint32_t sequence;
        uint32_t native_app_state;
        uint32_t room_id;
        uint8_t room_variant;
        uint8_t room_mode;
        uint8_t model_present;
        uint8_t original_factory_succeeded;
    };

    struct BattleEndObservation
    {
        uint32_t sequence;
        uint32_t native_app_state;
        uint16_t reply_opcode;
        uint8_t result_type;
        uint8_t original_handler_completed;
    };

    bool Bind(
        const SdgoApi* api,
        SdgoMod* mod,
        NativeActions* native,
        const NativeActions::Config& config)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;
        config_ = config;
        gameApp_ = Symbol<GameAppFn>("game.app");
        createRoomInit_ =
            Symbol<CreateRoomInitFn>("flow.create_room_packet_init");
        createRoomDialogDispatch_ =
            Symbol<CreateRoomDialogDispatchFn>(
                "flow.create_room_dialog_dispatch");
        waitRoomObjectFactory_ =
            Symbol<WaitRoomObjectFactoryFn>(
                "flow.wait_room_object_factory");
        waitRoomAlwaysDispatch_ =
            Symbol<WaitRoomAlwaysDispatchFn>(
                "flow.waitroom_always_dispatch");
        waitMenuUpdate_ =
            Symbol<WaitMenuUpdateFn>("flow.wait_menu_update");
        startOrReady_ =
            Symbol<StartOrReadyFn>("flow.wait_menu_start_or_ready");
        isRoomMaster_ =
            Symbol<IsRoomMasterFn>("flow.is_room_master");
        waitRoomPlayerCount_ =
            Symbol<WaitRoomPlayerCountFn>(
                "flow.wait_room_player_count");
        profilePointer_ = Symbol<uint8_t**>("flow.profile_pointer");
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>("flow.service_find_by_name");
        waitMenuVtable_ =
            Symbol<void*>("flow.wait_menu_vtable");
        threeDWaitRoomVtable_ =
            Symbol<void*>("flow.three_d_wait_room_vtable");
        threeDStartCommandVtable_ =
            Symbol<void*>("flow.three_d_start_command_vtable");
        threeDRoomHasState_ =
            Symbol<ThreeDRoomHasStateFn>("flow.three_d_room_has_state");
        threeDRoomAddState_ =
            Symbol<ThreeDRoomAddStateFn>("flow.three_d_room_add_state");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>("flow.native_wstring_dtor");
        gridData_ = Symbol<GridDataFn>("grid.data");
        gridFind_ = Symbol<GridFindFn>("grid.find");
        battleEndHandler_ =
            Symbol<BattleEndHandlerFn>("flow.battle_end_handler");
        normalThreeDLeave_ = Symbol<StartOrReadyFn>("flow.normal_3d_leave_business");
        normalThreeDLeaveReply_ = Symbol<CreateRoomDialogDispatchFn>("flow.normal_3d_leave_reply_dispatch");
        normalThreeDMainVtable_ = Symbol<void*>("flow.normal_3d_main_state_vtable");
        normalThreeDManagerVtables_[0] = Symbol<void*>("flow.normal_3d_manager_vtable");
        normalThreeDManagerVtables_[1] = Symbol<void*>("flow.ai_3d_manager_vtable");
        normalThreeDManagerVtables_[2] = Symbol<void*>("flow.battleship_3d_manager_vtable");

        if (!api_ || !mod_ || !native_ || !gameApp_ || !createRoomInit_ ||
            !createRoomDialogDispatch_ || !waitRoomObjectFactory_ ||
            !waitRoomAlwaysDispatch_ || !waitMenuUpdate_ || !startOrReady_ ||
            !isRoomMaster_ || !waitRoomPlayerCount_ || !profilePointer_ ||
            !serviceManagedPointer_ || !serviceFindByName_ ||
            !waitMenuVtable_ || !threeDWaitRoomVtable_ ||
            !threeDStartCommandVtable_ || !threeDRoomHasState_ ||
            !threeDRoomAddState_ || !nativeWStringCtor_ ||
            !nativeWStringDtor_ || !gridData_ || !gridFind_ ||
            !battleEndHandler_ || !normalThreeDLeave_ || !normalThreeDLeaveReply_ || !normalThreeDMainVtable_ ||
            !normalThreeDManagerVtables_[0] || !normalThreeDManagerVtables_[1] || !normalThreeDManagerVtables_[2] || !ValidPlan())
            return false;
        if (active_ && active_ != this)
            return false;

        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.create_room_dialog_dispatch",
                reinterpret_cast<void*>(&ObserveCreateRoomDialogDispatch),
                reinterpret_cast<void**>(&createRoomDialogDispatch_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.wait_menu_update",
                reinterpret_cast<void*>(&ObserveWaitMenuUpdate),
                reinterpret_cast<void**>(&waitMenuUpdate_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.wait_room_object_factory",
                reinterpret_cast<void*>(&ObserveWaitRoomObjectFactory),
                reinterpret_cast<void**>(&waitRoomObjectFactory_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.waitroom_always_dispatch",
                reinterpret_cast<void*>(&ObserveWaitRoomAlwaysDispatch),
                reinterpret_cast<void**>(&waitRoomAlwaysDispatch_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.battle_end_handler",
                reinterpret_cast<void*>(&ObserveBattleEndHandler),
                reinterpret_cast<void**>(&battleEndHandler_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(mod_, "flow.normal_3d_leave_reply_dispatch",
                reinterpret_cast<void*>(&ObserveNormalThreeDLeaveReply),
                reinterpret_cast<void**>(&normalThreeDLeaveReply_))) {
            active_ = nullptr;
            return false;
        }
        return true;
    }

    // Changes only the next GameTest create-room plan. The live client room
    // model is never touched; callers use this after the previous real room
    // has completed and left.
    bool SetRoomMode(uint16_t mode)
    {
        if (!ValidRoomMode(mode))
            return false;
        config_.room_mode = mode;
        return true;
    }

    // Clears per-request payloads between sequential real-room scenarios.
    // Reply sequences intentionally remain monotonic, so each scenario can
    // capture CreateState/LeaveState.reply_sequence before sending and wait
    // for a strictly greater value from the original reply handler.
    void ResetObservations()
    {
        InterlockedIncrement(&writeVersion_);
        InterlockedExchange(&reply_, 0);
        InterlockedExchange(&createResult_, kCreateNone);
        InterlockedExchange(&nativeAppState_, 0);
        InterlockedExchange(&roomId_, 0);
        InterlockedIncrement(&writeVersion_);

        InterlockedIncrement(&createDiagnosticsVersion_);
        InterlockedExchange(&createStage_, kCreateStageNone);
        InterlockedExchange(&createError_, kCreateErrorNone);
        InterlockedExchange(&createDiagnosticMap_, 0);
        InterlockedIncrement(&createDiagnosticsVersion_);

        InterlockedExchange(&roomObjectType_, -1);
        InterlockedIncrement(&leaveWriteVersion_);
        InterlockedExchange(&leaveReplyOpcode_, 0);
        InterlockedExchange(&leaveError_, 0);
        InterlockedExchange(&leaveResult_, kLeaveNone);
        InterlockedExchange(&leaveNativeAppState_, 0);
        InterlockedIncrement(&leaveWriteVersion_);

        InterlockedIncrement(&battleEndWriteVersion_);
        InterlockedExchange(&battleEndReplyOpcode_, 0);
        InterlockedExchange(&battleEndResultType_, 0);
        InterlockedExchange(&battleEndNativeAppState_, 0);
        InterlockedExchange(&battleEndHandlerCompleted_, 0);
        InterlockedIncrement(&battleEndWriteVersion_);

        InterlockedIncrement(&freshRoomWriteVersion_);
        InterlockedExchange(&freshRoomOwner_, 0);
        InterlockedExchange(&freshRoomDetails_, 0);
        InterlockedIncrement(&freshRoomWriteVersion_);
    }

    // Builds PT_REQ_CREATE_WAITROOM (0x0525) with the original packet
    // initializer and sends it through the original network object. The zeroed
    // 0x50-byte backing store supplies deterministic values for the bytes that
    // the game's stack-local packet leaves unused.
    bool Create() const
    {
        InterlockedIncrement(&createAttempt_);
        RecordCreateDiagnostics(
            kCreateStagePlan,
            kCreateErrorNone,
            0,
            0);
        __try {
            if (!native_ || !createRoomInit_) {
                RecordCreateDiagnostics(
                    kCreateStagePlan,
                    kCreateErrorUnavailable,
                    0,
                    0);
                return false;
            }
            if (!ValidPlan()) {
                RecordCreateDiagnostics(
                    kCreateStagePlan,
                    kCreateErrorInvalidPlan,
                    0,
                    0);
                return false;
            }

            uint8_t packet[kCreatePacketSize] = {};
            *reinterpret_cast<uint16_t*>(packet) = kCreateRoomOpcode;

            uint8_t mapSelection = 0;
            uint8_t map = 0;
            RecordCreateDiagnostics(
                kCreateStageMapLookup,
                kCreateErrorNone,
                mapSelection,
                map);
            const CreateError mapError =
                ResolveMap(&mapSelection, &map);
            if (mapError != kCreateErrorNone) {
                RecordCreateDiagnostics(
                    kCreateStageMapLookup,
                    mapError,
                    mapSelection,
                    map);
                return false;
            }

            // This is the normal quick-room baseline from 0x008CB1A0:
            // 2, mode, 5, max, map selection/map, 0, 1, 4, 0, 0.
            // Only the title/map are supplied by the GameTest plan.
            RecordCreateDiagnostics(
                kCreateStagePacketInit,
                kCreateErrorNone,
                mapSelection,
                map);
            if (!createRoomInit_(
                    packet,
                    0,
                    2,
                    static_cast<uint8_t>(config_.room_mode),
                    5,
                    MaxPlayers(config_.room_mode),
                    mapSelection,
                    map,
                    0,
                    1,
                    4,
                    0,
                    0,
                    config_.room_title,
                    "")) {
                RecordCreateDiagnostics(
                    kCreateStagePacketInit,
                    kCreateErrorPacketInit,
                    mapSelection,
                    map);
                return false;
            }

            RecordCreateDiagnostics(
                kCreateStageSend,
                kCreateErrorNone,
                mapSelection,
                map);
            if (!native_->SendRequest(packet, sizeof(packet))) {
                RecordCreateDiagnostics(
                    kCreateStageSend,
                    kCreateErrorSend,
                    mapSelection,
                    map);
                return false;
            }
            RecordCreateDiagnostics(
                kCreateStageComplete,
                kCreateErrorNone,
                mapSelection,
                map);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            RecordCreateDiagnostics(
                kCreateStageException,
                kCreateErrorException,
                0,
                0);
            return false;
        }
    }

    // Uses the original 3D room-state manager entry selected by the shipped
    // Start business path. State 0x0C is queued through AddState; the native
    // room tick later invokes the already-created BattleStartSend command and
    // sends PT_REQ_GAME_START (0x08A7). No UI event, packet replica, direct
    // command call or room/model write is used. Older non-3D rooms retain
    // their original wait-menu business routine below.
    bool StartBattleNativeHost() const
    {
        if (!gameApp_ || !startOrReady_ || !isRoomMaster_ ||
            !profilePointer_ || !threeDWaitRoomVtable_ ||
            !threeDStartCommandVtable_ || !threeDRoomHasState_ ||
            !threeDRoomAddState_)
            return false;
        __try {
            uint8_t* profile = *profilePointer_;
            if (!profile)
                return false;
            void* room = *reinterpret_cast<void**>(profile + kProfileRoom);
            if (!room || !isRoomMaster_(profile, room))
                return false;

            uint8_t* app = gameApp_();
            if (!app)
                return false;
            if (*reinterpret_cast<const uint32_t*>(
                    app + kGameAppState) == kThreeDWaitRoomState) {
                uint8_t* state = *reinterpret_cast<uint8_t**>(
                    app + kGameAppStateObject);
                if (!state ||
                    *reinterpret_cast<void**>(state) !=
                        threeDWaitRoomVtable_)
                    return false;
                uint8_t* roomObject =
                    *reinterpret_cast<uint8_t**>(state + kWaitRoomObject);
                if (!roomObject)
                    return false;
                uint8_t* stateArray =
                    *reinterpret_cast<uint8_t**>(
                        roomObject + kThreeDRoomStateArray);
                if (!stateArray)
                    return false;
                void* command = *reinterpret_cast<void**>(
                    stateArray + kThreeDStartCommand);
                if (!command ||
                    *reinterpret_cast<void**>(command) !=
                        threeDStartCommandVtable_)
                    return false;
                if (threeDRoomHasState_(
                        roomObject,
                        kThreeDStartState,
                        0))
                    return false;
                threeDRoomAddState_(
                    roomObject,
                    kThreeDStartState,
                    nullptr,
                    kThreeDQueueBack,
                    0);
                return true;
            }

            void* waitMenu = ResolveWaitMenu();
            if (!waitMenu)
                return false;
            startOrReady_(waitMenu);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    // The 3D UI queues the local pilot's MyLeaveRoom command through its
    // original Main-state business routine. Merely sending its request leaves
    // the registered reply consumer outside stage 2 and cannot leave the room.
    // Legacy rooms send the native leave-wait-room request selected by the room
    // object type received by CService_3D_WaitRoom::CreateRoomObject. Normal,
    // AI and battleship rooms use PT_REQ_LEAVE_WAITROOM 0x066D; team,
    // matchmaking, clan and mission rooms use the team variant 0x0487.
    // The original always-packet handler consumes the real 0x057B reply and
    // performs the normal client transition.
    bool Leave() const
    {
        if (!native_ || !profilePointer_)
            return false;

        __try {
            uint8_t* profile = *profilePointer_;
            if (!profile ||
                !*reinterpret_cast<void**>(profile + kProfileRoom))
                return false;

            uint8_t* app = gameApp_ ? gameApp_() : nullptr;
            if (app && *reinterpret_cast<const uint32_t*>(app + kGameAppState) == kThreeDWaitRoomState) {
                uint8_t* owner = *reinterpret_cast<uint8_t**>(app + kGameAppStateObject);
                if (!owner || *reinterpret_cast<void**>(owner) != threeDWaitRoomVtable_) return false;
                uint8_t* manager = *reinterpret_cast<uint8_t**>(owner + kWaitRoomObject);
                if (!manager) return false;
                void* vtable = *reinterpret_cast<void**>(manager);
                if (vtable != normalThreeDManagerVtables_[0] && vtable != normalThreeDManagerVtables_[1] &&
                    vtable != normalThreeDManagerVtables_[2]) return false;
                uint8_t** states = *reinterpret_cast<uint8_t***>(manager + kThreeDRoomStateArray);
                uint8_t* main = states ? states[0] : nullptr;
                if (!main || *reinterpret_cast<void**>(main) != normalThreeDMainVtable_ ||
                    *reinterpret_cast<uint8_t**>(main + 0x0C) != manager + 0x24 ||
                    *reinterpret_cast<uint8_t**>(main + 0x10) != manager ||
                    *reinterpret_cast<const uint32_t*>(main + 8) != 2) return false;
                const uint8_t* context = manager + 0x24;
                if (context[0x25] == 1 || context[0x24] == 1 || !context[2]) return false;
                normalThreeDLeave_(main);
                return true;
            }

            const LONG objectType =
                InterlockedCompareExchange(&roomObjectType_, 0, 0);
            uint16_t opcode = 0;
            switch (objectType) {
            case 0: // Normal
            case 4: // AI mode
            case 5: // Battleship mode
                opcode = kLeaveRoomNormalOpcode;
                break;
            case 1: // Matchmaking/team
            case 2: // Clan
            case 3: // Mission
                opcode = kLeaveRoomTeamOpcode;
                break;
            default:
                return false;
            }

            uint8_t packet[kLeavePacketSize] = {};
            *reinterpret_cast<uint16_t*>(packet) = opcode;
            return native_->SendRequest(packet, sizeof(packet));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    Observation CreateState() const
    {
        Observation observed = {};
        __try {
            for (int attempt = 0; attempt < 3; ++attempt) {
                const LONG before =
                    InterlockedCompareExchange(&writeVersion_, 0, 0);
                if (before & 1)
                    continue;
                observed.reply_sequence = static_cast<uint32_t>(
                    InterlockedCompareExchange(&replySequence_, 0, 0));
                observed.result = static_cast<uint32_t>(
                    InterlockedCompareExchange(&createResult_, 0, 0));
                observed.native_app_state = static_cast<uint32_t>(
                    InterlockedCompareExchange(&nativeAppState_, 0, 0));
                observed.room_id = static_cast<uint32_t>(
                    InterlockedCompareExchange(&roomId_, 0, 0));
                const uint32_t reply = static_cast<uint32_t>(
                    InterlockedCompareExchange(&reply_, 0, 0));
                observed.reply_opcode =
                    static_cast<uint16_t>(reply & 0xFFFFu);
                observed.room_variant =
                    static_cast<uint8_t>((reply >> 16) & 0xFFu);
                const LONG after =
                    InterlockedCompareExchange(&writeVersion_, 0, 0);
                if (before == after && !(after & 1))
                    return observed;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
            observed.native_app_state = UINT32_MAX;
        }
        return observed;
    }

    LeaveObservation LeaveState() const
    {
        LeaveObservation observed = {};
        __try {
            for (int attempt = 0; attempt < 3; ++attempt) {
                const LONG before =
                    InterlockedCompareExchange(&leaveWriteVersion_, 0, 0);
                if (before & 1)
                    continue;
                observed.reply_sequence = static_cast<uint32_t>(
                    InterlockedCompareExchange(&leaveReplySequence_, 0, 0));
                observed.result = static_cast<uint32_t>(
                    InterlockedCompareExchange(&leaveResult_, 0, 0));
                observed.native_app_state = static_cast<uint32_t>(
                    InterlockedCompareExchange(&leaveNativeAppState_, 0, 0));
                observed.reply_opcode = static_cast<uint16_t>(
                    InterlockedCompareExchange(&leaveReplyOpcode_, 0, 0));
                observed.error = static_cast<uint16_t>(
                    InterlockedCompareExchange(&leaveError_, 0, 0));
                observed.room_object_type = static_cast<uint8_t>(
                    InterlockedCompareExchange(&roomObjectType_, 0, 0));
                const LONG after =
                    InterlockedCompareExchange(&leaveWriteVersion_, 0, 0);
                if (before == after && !(after & 1))
                    return observed;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
            observed.native_app_state = UINT32_MAX;
            observed.room_object_type = UINT8_MAX;
        }
        return observed;
    }

    BattleEndObservation BattleEndState() const
    {
        BattleEndObservation observed = {};
        __try {
            for (int attempt = 0; attempt < 3; ++attempt) {
                const LONG before = InterlockedCompareExchange(
                    &battleEndWriteVersion_,
                    0,
                    0);
                if (before & 1)
                    continue;
                observed.sequence = static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &battleEndSequence_,
                        0,
                        0));
                observed.native_app_state = static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &battleEndNativeAppState_,
                        0,
                        0));
                observed.reply_opcode = static_cast<uint16_t>(
                    InterlockedCompareExchange(
                        &battleEndReplyOpcode_,
                        0,
                        0));
                observed.result_type = static_cast<uint8_t>(
                    InterlockedCompareExchange(
                        &battleEndResultType_,
                        0,
                        0));
                observed.original_handler_completed = static_cast<uint8_t>(
                    InterlockedCompareExchange(
                        &battleEndHandlerCompleted_,
                        0,
                        0));
                const LONG after = InterlockedCompareExchange(
                    &battleEndWriteVersion_,
                    0,
                    0);
                if (before == after && !(after & 1))
                    return observed;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
            observed.native_app_state = UINT32_MAX;
        }
        return observed;
    }

    bool HasObservedWaitMenu() const { return ResolveWaitMenu() != nullptr; }

    // Reads the room model installed by the original 0x0918 path at
    // profile+0xEC. This does not construct a room or modify client state.
    NativeRoomObservation NativeRoomState() const
    {
        NativeRoomObservation observed = {};
        observed.native_app_state =
            native_ ? native_->AppState() : UINT32_MAX;
        observed.wait_menu_observed =
            HasObservedWaitMenu() ? 1 : 0;
        if (!profilePointer_ || !isRoomMaster_ || !waitRoomPlayerCount_)
            return observed;

        __try {
            uint8_t* profile = *profilePointer_;
            if (!profile)
                return observed;
            void* room = *reinterpret_cast<void**>(
                profile + kProfileRoom);
            if (!room)
                return observed;
            const uint8_t* bytes = static_cast<const uint8_t*>(room);
            observed.room_id =
                *reinterpret_cast<const uint32_t*>(bytes);
            observed.player_count =
                static_cast<uint32_t>(waitRoomPlayerCount_(room));
            observed.room_variant = *(bytes + 4);
            observed.room_mode = *(bytes + 5);
            observed.model_present = 1;
            observed.local_is_host =
                isRoomMaster_(profile, room) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            observed.native_app_state = UINT32_MAX;
            observed.room_id = 0;
            observed.player_count = 0;
            observed.room_variant = 0;
            observed.room_mode = 0;
            observed.model_present = 0;
            observed.local_is_host = 0;
        }
        return observed;
    }

    // Published after the original wait-room factory completes.
    // The sequence remains monotonic across ResetObservations(), allowing a
    // scenario to capture it before Create() and require a strictly newer
    // factory completion backed by the native profile+0xEC room model.
    FreshRoomObservation FreshRoomState() const
    {
        FreshRoomObservation observed = {};
        __try {
            uint8_t* factoryOwner = nullptr;
            for (int attempt = 0; attempt < 3; ++attempt) {
                const LONG before = InterlockedCompareExchange(
                    &freshRoomWriteVersion_,
                    0,
                    0);
                if (before & 1)
                    continue;
                observed.sequence = static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &freshRoomSequence_,
                        0,
                        0));
                factoryOwner = reinterpret_cast<uint8_t*>(
                    static_cast<uintptr_t>(
                        static_cast<uint32_t>(
                            InterlockedCompareExchange(
                                &freshRoomOwner_,
                                0,
                                0))));
                const uint32_t details = static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &freshRoomDetails_,
                        0,
                        0));
                observed.original_factory_succeeded =
                    static_cast<uint8_t>((details >> 24) & 0xFFu);
                const LONG after = InterlockedCompareExchange(
                    &freshRoomWriteVersion_,
                    0,
                    0);
                if (before == after && !(after & 1))
                    break;
                factoryOwner = nullptr;
            }

            // The factory installs its room controller at self+4 before the
            // profile's room model becomes observable. Bind the later model
            // read to that exact still-active app-state owner instead of
            // freezing the profile+0xEC value at the factory return instant.
            observed.native_app_state =
                native_ ? native_->AppState() : UINT32_MAX;
            if (!factoryOwner || !gameApp_ || !profilePointer_ || !threeDWaitRoomVtable_)
                return observed;
            uint8_t* app = gameApp_();
            if (!app)
                return observed;
            uint8_t* activeOwner = *reinterpret_cast<uint8_t**>(
                app + kGameAppStateObject);
            if (activeOwner != factoryOwner ||
                *reinterpret_cast<void**>(factoryOwner) !=
                    threeDWaitRoomVtable_ ||
                !*reinterpret_cast<void**>(
                    factoryOwner + kWaitRoomObject))
                return observed;
            // The active original owner and its installed controller are the
            // authoritative success receipt. The function's AL value is not
            // retained as the only gate because profile publication is
            // deferred to the following native lifecycle work.
            observed.original_factory_succeeded = 1;
            uint8_t* profile = *profilePointer_;
            if (!profile)
                return observed;
            const uint8_t* room =
                *reinterpret_cast<uint8_t**>(profile + kProfileRoom);
            if (!room)
                return observed;
            observed.room_id =
                *reinterpret_cast<const uint32_t*>(room);
            observed.room_variant = *(room + 4);
            observed.room_mode = *(room + 5);
            observed.model_present = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
            observed.native_app_state = UINT32_MAX;
        }
        return observed;
    }

    CreateError LastError() const
    {
        return static_cast<CreateError>(
            InterlockedCompareExchange(&createError_, 0, 0));
    }

    CreateDiagnostics Diagnostics() const
    {
        CreateDiagnostics observed = {};
        __try {
            for (int attempt = 0; attempt < 3; ++attempt) {
                const LONG before = InterlockedCompareExchange(
                    &createDiagnosticsVersion_,
                    0,
                    0);
                if (before & 1)
                    continue;
                observed.attempt = static_cast<uint32_t>(
                    InterlockedCompareExchange(&createAttempt_, 0, 0));
                observed.stage = static_cast<uint32_t>(
                    InterlockedCompareExchange(&createStage_, 0, 0));
                observed.error = static_cast<uint32_t>(
                    InterlockedCompareExchange(&createError_, 0, 0));
                observed.configured_map_id = config_.map_id;
                observed.room_mode = config_.room_mode;
                const uint32_t map = static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        &createDiagnosticMap_,
                        0,
                        0));
                observed.map_selection =
                    static_cast<uint8_t>(map & 0xFFu);
                observed.map_index =
                    static_cast<uint8_t>((map >> 8) & 0xFFu);
                const LONG after = InterlockedCompareExchange(
                    &createDiagnosticsVersion_,
                    0,
                    0);
                if (before == after && !(after & 1))
                    return observed;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&observed, 0, sizeof(observed));
            observed.stage = kCreateStageException;
            observed.error = kCreateErrorException;
        }
        return observed;
    }

private:
    static constexpr uint16_t kCreateRoomOpcode = 0x0525;
    static constexpr uint16_t kCreateRoomReplyOpcode = 0x0918;
    static constexpr uint16_t kLeaveRoomNormalOpcode = 0x066D;
    static constexpr uint16_t kLeaveRoomTeamOpcode = 0x0487;
    static constexpr uint16_t kLeaveRoomReplyOpcode = 0x057B;
    static constexpr uint16_t kBattleEndReplyOpcode = 0x0871;
    static constexpr size_t kCreatePacketSize = 0x50;
    static constexpr size_t kLeavePacketSize = 0x06;
    static constexpr size_t kReplyRoomId = 0x06;
    static constexpr size_t kReplyError = 0x06;
    static constexpr size_t kBattleEndResultType = 0x12;
    static constexpr size_t kWaitRoomObjectType = 0x10;
    static constexpr size_t kProfileRoom = 0xEC;
    static constexpr size_t kGameAppState = 0x4A0;
    static constexpr size_t kGameAppStateObject = 0x4A4;
    static constexpr uint32_t kThreeDWaitRoomState = 18;
    static constexpr size_t kWaitRoomObject = 0x04;
    static constexpr size_t kThreeDRoomStateArray = 0x0C;
    static constexpr size_t kThreeDStartCommand = 0x30;
    static constexpr int kThreeDStartState = 0x0C;
    static constexpr int kThreeDQueueBack = 1;
    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr int kMapGridType = 0x0F;
    static constexpr uint32_t kRandomMap = 0;
    static constexpr uint32_t kRandomMapSetOne = UINT32_MAX;
    static constexpr uint32_t kRandomMapSetTwo = UINT32_MAX - 1;

    using GameAppFn = uint8_t*(__cdecl*)();
    using CreateRoomInitFn = bool(__thiscall*)(
        void*,
        uint32_t,
        uint8_t,
        uint8_t,
        uint8_t,
        uint8_t,
        uint8_t,
        uint8_t,
        uint8_t,
        uint8_t,
        uint8_t,
        uint8_t,
        uint8_t,
        const char*,
        const char*);
    using CreateRoomDialogDispatchFn =
        int(__thiscall*)(void*, int, int, int, int, void*);
    using WaitRoomObjectFactoryFn = bool(__thiscall*)(void*, void*);
    using WaitRoomAlwaysDispatchFn =
        void(__thiscall*)(void*, int, int, int, int, void*);
    using WaitMenuUpdateFn = void(__thiscall*)(void*);
    using StartOrReadyFn = void(__thiscall*)(void*);
    using ThreeDRoomHasStateFn =
        bool(__thiscall*)(void*, int, int);
    using ThreeDRoomAddStateFn =
        void(__thiscall*)(void*, int, void*, int, int);
    using IsRoomMasterFn = bool(__thiscall*)(void*, void*);
    using WaitRoomPlayerCountFn = int(__stdcall*)(void*);
    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn = void(__thiscall*)(void*);
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn =
        int(__thiscall*)(void*, void**, int, int, char);
    using BattleEndHandlerFn = int(__thiscall*)(void*, void*);

    static int __fastcall ObserveCreateRoomDialogDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        RoomActions* actions = active_;
        if (!actions || !actions->createRoomDialogDispatch_)
            return 0;

        uint32_t roomId = 0;
        const bool isCreateReply =
            (opcode & 0xFFFF) == kCreateRoomReplyOpcode && packet;
        if (isCreateReply) {
            // Snapshot the server-owned reply before the original handler.
            // Publication still happens only after the handler has completed.
            __try {
                const uint8_t* bytes = static_cast<const uint8_t*>(packet);
                roomId = *reinterpret_cast<const uint32_t*>(
                    bytes + kReplyRoomId);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                roomId = 0;
            }
        }

        const int result = actions->createRoomDialogDispatch_(
            self,
            a,
            b,
            c,
            opcode,
            packet);
        if (!isCreateReply)
            return result;

        InterlockedIncrement(&actions->writeVersion_);
        InterlockedExchange(
            &actions->reply_,
            static_cast<LONG>(
                kCreateRoomReplyOpcode));
        InterlockedExchange(
            &actions->createResult_,
            roomId ? static_cast<LONG>(kCreateAccepted)
                   : static_cast<LONG>(kCreateRejected));
        InterlockedExchange(&actions->roomId_, static_cast<LONG>(roomId));
        InterlockedExchange(
            &actions->nativeAppState_,
            static_cast<LONG>(
                actions->native_ ? actions->native_->AppState()
                                 : UINT32_MAX));
        InterlockedIncrement(&actions->replySequence_);
        InterlockedIncrement(&actions->writeVersion_);
        return result;
    }

    static void __fastcall ObserveWaitMenuUpdate(void* self, void*)
    {
        RoomActions* actions = active_;
        if (!actions || !actions->waitMenuUpdate_)
            return;
        actions->waitMenuUpdate_(self);
        // Update is called by the native service manager every frame. Its
        // `this` is the already-registered CServiceWaitMenuBar instance, so
        // retaining it requires no input and does not construct a service or
        // replace room/world state.
        actions->waitMenuSelf_ = self;
    }

    // Uses the original CServiceManaged name lookup for the already-registered
    // waitmenubar service. The native descriptor is temporary; the returned UI
    // object is only accepted after its shipped vtable is verified.
    void* ResolveWaitMenu() const
    {
        __try {
            if (!serviceManagedPointer_ || !serviceFindByName_ ||
                !waitMenuVtable_ || !nativeWStringCtor_ ||
                !nativeWStringDtor_)
                return nullptr;
            void* manager = *serviceManagedPointer_;
            if (manager) {
                alignas(4) uint8_t descriptor[kServiceDescriptorSize] = {};
                *reinterpret_cast<uint32_t*>(descriptor) = 1;
                bool nameConstructed = false;
                void* service = nullptr;
                __try {
                    nativeWStringCtor_(
                        descriptor + sizeof(uint32_t),
                        L"waitmenubar");
                    nameConstructed = true;
                    service = serviceFindByName_(manager, descriptor);
                } __finally {
                    if (nameConstructed) {
                        nativeWStringDtor_(
                            descriptor + sizeof(uint32_t));
                    }
                }
                if (service &&
                    *reinterpret_cast<void**>(service) ==
                        waitMenuVtable_)
                    return service;
            }

            // The update hook is retained as a fallback observation path.
            if (waitMenuSelf_ &&
                *reinterpret_cast<void**>(waitMenuSelf_) ==
                    waitMenuVtable_)
                return waitMenuSelf_;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
        return nullptr;
    }

    static bool __fastcall ObserveWaitRoomObjectFactory(
        void* self,
        void*,
        void* packet)
    {
        RoomActions* actions = active_;
        if (!actions || !actions->waitRoomObjectFactory_)
            return false;

        LONG objectType = -1;
        __try {
            if (packet) {
                objectType = static_cast<LONG>(
                    *(static_cast<const uint8_t*>(packet) +
                      kWaitRoomObjectType));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            objectType = -1;
        }

        // The original consumes and frees the server-owned creation packet.
        const bool result =
            actions->waitRoomObjectFactory_(self, packet);
        if (objectType >= 0 && objectType <= 5)
            InterlockedExchange(&actions->roomObjectType_, objectType);
        // Every completed invocation publishes a generation. FreshRoomState
        // accepts it only while this exact factory owner is the live app-state
        // object and owns the original room controller, so an early/failed
        // invocation cannot satisfy the room assertion.
        const uint32_t details =
            result ? (1u << 24) : 0u;
        InterlockedIncrement(&actions->freshRoomWriteVersion_);
        InterlockedExchange(
            &actions->freshRoomOwner_,
            static_cast<LONG>(
                reinterpret_cast<uintptr_t>(self)));
        InterlockedExchange(
            &actions->freshRoomDetails_,
            static_cast<LONG>(details));
        InterlockedIncrement(&actions->freshRoomSequence_);
        InterlockedIncrement(&actions->freshRoomWriteVersion_);
        return result;
    }

    static void __fastcall ObserveWaitRoomAlwaysDispatch(
        void* self,
        void*,
        int a,
        int b,
        int c,
        int opcode,
        void* packet)
    {
        RoomActions* actions = active_;
        if (!actions || !actions->waitRoomAlwaysDispatch_)
            return;

        const bool isLeaveReply =
            (opcode & 0xFFFF) == kLeaveRoomReplyOpcode && packet;
        uint16_t error = UINT16_MAX;
        if (isLeaveReply) {
            // The original handler may consume the server-owned packet.
            __try {
                error = *reinterpret_cast<const uint16_t*>(
                    static_cast<const uint8_t*>(packet) + kReplyError);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                error = UINT16_MAX;
            }
        }

        actions->waitRoomAlwaysDispatch_(self, a, b, c, opcode, packet);
        if (!isLeaveReply)
            return;

        InterlockedIncrement(&actions->leaveWriteVersion_);
        InterlockedExchange(
            &actions->leaveReplyOpcode_,
            static_cast<LONG>(kLeaveRoomReplyOpcode));
        InterlockedExchange(
            &actions->leaveError_,
            static_cast<LONG>(error));
        InterlockedExchange(
            &actions->leaveResult_,
            error == 0 ? static_cast<LONG>(kLeaveAccepted)
                       : static_cast<LONG>(kLeaveRejected));
        InterlockedExchange(
            &actions->leaveNativeAppState_,
            static_cast<LONG>(
                actions->native_ ? actions->native_->AppState()
                                 : UINT32_MAX));
        InterlockedIncrement(&actions->leaveReplySequence_);
        InterlockedIncrement(&actions->leaveWriteVersion_);
    }

    static int __fastcall ObserveBattleEndHandler(
        void* self,
        void*,
        void* packet)
    {
        RoomActions* actions = active_;
        if (!actions || !actions->battleEndHandler_)
            return 0;

        uint8_t resultType = 0;
        __try {
            if (packet) {
                resultType = *(
                    static_cast<const uint8_t*>(packet) +
                    kBattleEndResultType);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            resultType = 0;
        }

        // The original owns all battle-result processing and model changes.
        // Publish only after it has consumed the real server packet.
        const int handled = actions->battleEndHandler_(self, packet);

        InterlockedIncrement(&actions->battleEndWriteVersion_);
        InterlockedExchange(
            &actions->battleEndReplyOpcode_,
            static_cast<LONG>(kBattleEndReplyOpcode));
        InterlockedExchange(
            &actions->battleEndResultType_,
            static_cast<LONG>(resultType));
        InterlockedExchange(
            &actions->battleEndNativeAppState_,
            static_cast<LONG>(
                actions->native_ ? actions->native_->AppState()
                                 : UINT32_MAX));
        InterlockedExchange(&actions->battleEndHandlerCompleted_, 1);
        InterlockedIncrement(&actions->battleEndSequence_);
        InterlockedIncrement(&actions->battleEndWriteVersion_);
        return handled;
    }

    static int __fastcall ObserveNormalThreeDLeaveReply(
        void* self, void*, int a, int b, int c, int opcode, void* packet)
    {
        RoomActions* actions = active_;
        if (!actions || !actions->normalThreeDLeaveReply_) return 0;
        bool consumed = false;
        uint16_t error = UINT16_MAX;
        __try {
            consumed = self && packet && (opcode & 0xFFFF) == kLeaveRoomReplyOpcode &&
                       *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(self) + 8) == 2;
            if (consumed) error = *reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(packet) + kReplyError);
        } __except (EXCEPTION_EXECUTE_HANDLER) { consumed = false; }
        const int handled = actions->normalThreeDLeaveReply_(self, a, b, c, opcode, packet);
        if (consumed) {
            InterlockedIncrement(&actions->leaveWriteVersion_);
            InterlockedExchange(&actions->leaveReplyOpcode_, kLeaveRoomReplyOpcode);
            InterlockedExchange(&actions->leaveError_, error);
            InterlockedExchange(&actions->leaveResult_, error == 0 ? kLeaveAccepted : kLeaveRejected);
            InterlockedExchange(&actions->leaveNativeAppState_, static_cast<LONG>(actions->native_->AppState()));
            InterlockedIncrement(&actions->leaveReplySequence_);
            InterlockedIncrement(&actions->leaveWriteVersion_);
        }
        return handled;
    }

    CreateError ResolveMap(uint8_t* selection, uint8_t* map) const
    {
        if (!selection || !map)
            return kCreateErrorMapGridUnavailable;
        if (config_.map_id == kRandomMap) {
            *selection = 1;
            *map = 0;
            return kCreateErrorNone;
        }
        if (config_.map_id == kRandomMapSetOne) {
            *selection = 2;
            *map = 0;
            return kCreateErrorNone;
        }
        if (config_.map_id == kRandomMapSetTwo) {
            *selection = 4;
            *map = 0;
            return kCreateErrorNone;
        }
        if (config_.map_id <= UINT8_MAX) {
            *selection = 0;
            *map = static_cast<uint8_t>(config_.map_id);
            return *map ? kCreateErrorNone
                        : kCreateErrorMapIndexOutOfRange;
        }

        void* grid = gridData_();
        void* record = nullptr;
        if (!grid)
            return kCreateErrorMapGridUnavailable;
        if (!gridFind_(
                grid,
                &record,
                static_cast<int>(config_.map_id),
                kMapGridType,
                0) ||
            !record)
            return kCreateErrorMapRecordMissing;
        const uint32_t actual = *reinterpret_cast<const uint32_t*>(
            static_cast<const uint8_t*>(record) + 5);
        if (actual > UINT8_MAX)
            return kCreateErrorMapIndexOutOfRange;
        if (!actual) {
            // Type 0x0F record 100001 is the native "random map" row. Its
            // map-index field is zero, while PT_REQ_CREATE_WAITROOM encodes
            // that choice as selection=1/map=0 (the original quick-room
            // packet at 0x008CB1A0).
            *selection = 1;
            *map = 0;
            return kCreateErrorNone;
        }
        *selection = 0;
        *map = static_cast<uint8_t>(actual);
        return kCreateErrorNone;
    }

    void RecordCreateDiagnostics(
        CreateStage stage,
        CreateError error,
        uint8_t mapSelection,
        uint8_t map) const
    {
        InterlockedIncrement(&createDiagnosticsVersion_);
        InterlockedExchange(&createStage_, static_cast<LONG>(stage));
        InterlockedExchange(&createError_, static_cast<LONG>(error));
        InterlockedExchange(
            &createDiagnosticMap_,
            static_cast<LONG>(
                static_cast<uint32_t>(mapSelection) |
                (static_cast<uint32_t>(map) << 8)));
        InterlockedIncrement(&createDiagnosticsVersion_);
    }

    bool ValidPlan() const
    {
        if (!config_.room_title)
            return false;
        const size_t titleLength = strlen(config_.room_title);
        if (titleLength == 0 || titleLength > 20)
            return false;
        return ValidRoomMode(config_.room_mode);
    }

    static bool ValidRoomMode(uint16_t mode)
    {
        switch (mode) {
        case 1:
        case 3:
        case 6:
        case 7:
        case 9:
        case 11:
        case 12:
            return true;
        default:
            return false;
        }
    }

    static uint8_t MaxPlayers(uint16_t mode)
    {
        switch (mode) {
        case 1:
        case 9:
            return 12;
        case 3:
        case 6:
        case 7:
            return 8;
        case 11:
            return 6;
        case 12:
            return 4;
        default:
            return 0;
        }
    }

    template <typename Fn>
    Fn Symbol(const char* name) const
    {
        return api_ ? reinterpret_cast<Fn>(api_->Symbol(mod_, name)) : nullptr;
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    NativeActions::Config config_ = {};
    GameAppFn gameApp_ = nullptr;
    CreateRoomInitFn createRoomInit_ = nullptr;
    CreateRoomDialogDispatchFn createRoomDialogDispatch_ = nullptr;
    WaitRoomObjectFactoryFn waitRoomObjectFactory_ = nullptr;
    WaitRoomAlwaysDispatchFn waitRoomAlwaysDispatch_ = nullptr;
    WaitMenuUpdateFn waitMenuUpdate_ = nullptr;
    StartOrReadyFn startOrReady_ = nullptr;
    IsRoomMasterFn isRoomMaster_ = nullptr;
    WaitRoomPlayerCountFn waitRoomPlayerCount_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    void* waitMenuVtable_ = nullptr;
    void* threeDWaitRoomVtable_ = nullptr;
    void* threeDStartCommandVtable_ = nullptr;
    ThreeDRoomHasStateFn threeDRoomHasState_ = nullptr;
    ThreeDRoomAddStateFn threeDRoomAddState_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    GridDataFn gridData_ = nullptr;
    GridFindFn gridFind_ = nullptr;
    BattleEndHandlerFn battleEndHandler_ = nullptr;
    StartOrReadyFn normalThreeDLeave_ = nullptr;
    CreateRoomDialogDispatchFn normalThreeDLeaveReply_ = nullptr;
    void* normalThreeDMainVtable_ = nullptr;
    void* normalThreeDManagerVtables_[3] = {};
    void* waitMenuSelf_ = nullptr;
    mutable volatile LONG writeVersion_ = 0;
    mutable volatile LONG replySequence_ = 0;
    mutable volatile LONG reply_ = 0;
    mutable volatile LONG createResult_ = kCreateNone;
    mutable volatile LONG nativeAppState_ = 0;
    mutable volatile LONG roomId_ = 0;
    mutable volatile LONG createDiagnosticsVersion_ = 0;
    mutable volatile LONG createAttempt_ = 0;
    mutable volatile LONG createStage_ = kCreateStageNone;
    mutable volatile LONG createError_ = kCreateErrorNone;
    mutable volatile LONG createDiagnosticMap_ = 0;
    mutable volatile LONG roomObjectType_ = -1;
    mutable volatile LONG leaveWriteVersion_ = 0;
    mutable volatile LONG leaveReplySequence_ = 0;
    mutable volatile LONG leaveReplyOpcode_ = 0;
    mutable volatile LONG leaveError_ = 0;
    mutable volatile LONG leaveResult_ = kLeaveNone;
    mutable volatile LONG leaveNativeAppState_ = 0;
    mutable volatile LONG battleEndWriteVersion_ = 0;
    mutable volatile LONG battleEndSequence_ = 0;
    mutable volatile LONG battleEndReplyOpcode_ = 0;
    mutable volatile LONG battleEndResultType_ = 0;
    mutable volatile LONG battleEndNativeAppState_ = 0;
    mutable volatile LONG battleEndHandlerCompleted_ = 0;
    mutable volatile LONG freshRoomWriteVersion_ = 0;
    mutable volatile LONG freshRoomSequence_ = 0;
    mutable volatile LONG freshRoomOwner_ = 0;
    mutable volatile LONG freshRoomDetails_ = 0;
    inline static RoomActions* active_ = nullptr;
};
