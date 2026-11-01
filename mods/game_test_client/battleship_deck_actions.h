#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cwchar>

#include "native_actions.h"

// Builds a real four-unit battleship deck through the shipped client business
// objects. Required unit UIDs are supplied by the GameTest runner from genuine
// account acquisition receipts; this adapter never grants a unit, invents an
// inventory record, writes a profile, fabricates a UI message/reply, or changes
// the application state directly.
//
// The normal SelectBattleShipBtn callback opens app state 20. Unit records are
// copied from CUnitInfo by its original getters, checked against type-2 grid
// data and passed to the original editor selection/refresh/command methods.
// The editor publishes its normal 0x3AB4 message to the room manager. Save and
// GoOut use the original request workers and are accepted only after the
// original 0x0A27/0x04A6 consumers and manager/profile readback agree.
class BattleShipDeckActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kWaitingForLobby = 1,
        kOpeningBattleShip = 2,
        kWaitingForBattleShip = 3,
        kWaitingForServices = 4,
        kWaitingForShipData = 5,
        kWaitingForInventory = 6,
        kCleaningEditor = 7,
        kSelectingUnit = 8,
        kWaitingForEditorPublish = 9,
        kSelectingLeader = 10,
        kSaving = 11,
        kGoingOut = 12,
        kAccepted = 13,

        kInvalidTarget = 0x100,
        kLobbyServiceMismatch = 0x101,
        kLobbyControlMissing = 0x102,
        kLobbyControlMismatch = 0x103,
        kBattleShipManagerMismatch = 0x104,
        kEditorServiceMismatch = 0x105,
        kInventoryUnavailable = 0x106,
        kRequiredUnitMissing = 0x107,
        kRequiredUnitChanged = 0x108,
        kRequiredUnitDuplicate = 0x109,
        kRequiredUnitTypeMismatch = 0x10A,
        kRequiredUnitCostMismatch = 0x10B,
        kEditorCostLimitMismatch = 0x10C,
        kEditorMutationMismatch = 0x10D,
        kLeaderSelectionFailed = 0x10E,
        kSaveRejected = 0x10F,
        kSaveReplyMismatch = 0x110,
        kGoOutRejected = 0x111,
        kGoOutReplyMismatch = 0x112,
        kCarouselMismatch = 0x113,
        kNativeFault = 0x114,
        kGoOutAlternativeMissing = 0x115,
    };

    struct Observation
    {
        uint32_t result;
        uint32_t native_app_state;
        uint32_t stage;
        uint32_t selected_count;
        uint32_t total_cost;
        uint32_t save_reply_sequence;
        uint32_t goout_reply_sequence;
        uint32_t transport_sequence;
        uint64_t ship_uid;
        uint64_t leader_uid;
        uint64_t unit_uids[4];
        uint16_t save_reply_opcode;
        uint16_t goout_reply_opcode;
        uint8_t editor_matches;
        uint8_t manager_matches;
        uint8_t profile_matches;
        uint8_t reserved;
    };

    bool Bind(const SdgoApi* api, SdgoMod* mod, NativeActions* native)
    {
        api_ = api;
        mod_ = mod;
        native_ = native;

        gameApp_ = Symbol<GameAppFn>("game.app");
        profilePointer_ = Symbol<uint8_t**>("flow.profile_pointer");
        serviceManagedPointer_ =
            Symbol<void**>("flow.service_managed_pointer");
        serviceFindByName_ =
            Symbol<ServiceFindByNameFn>("flow.service_find_by_name");
        nativeWStringCtor_ =
            Symbol<NativeWStringCtorFn>("flow.native_wstring_ctor");
        nativeWStringDtor_ =
            Symbol<NativeWStringDtorFn>("flow.native_wstring_dtor");
        nativeUiFindControl_ =
            Symbol<NativeUiFindControlFn>("flow.native_ui_find_control");

        lobbyManagerVtable_ =
            Symbol<void*>("flow.battleship_lobby_manager_vtable");
        lobbySelectBattleShip_ =
            Symbol<LobbySelectBattleShipFn>(
                "flow.battleship_lobby_select_business");
        battleShipManagerVtable_ =
            Symbol<void*>("flow.battleship_room_manager_vtable");
        editorVtable_ =
            Symbol<void*>("flow.battleship_editor_vtable");

        unitInfoPointer_ =
            Symbol<void**>("flow.unit_info_pointer");
        unitBriefBySlot_ =
            Symbol<UnitBriefBySlotFn>("flow.unit_brief_by_slot");
        unitFullBySlot_ =
            Symbol<UnitFullBySlotFn>("flow.unit_full_by_slot");
        gridData_ = Symbol<GridDataFn>("flow.grid_data");
        gridFind_ = Symbol<GridFindFn>("flow.grid_find");

        editorRemoveUnit_ =
            Symbol<EditorRemoveUnitFn>(
                "flow.battleship_editor_remove_unit");
        editorSelectUnit_ =
            Symbol<EditorSelectUnitFn>(
                "flow.battleship_editor_select_unit");
        editorRefresh_ =
            Symbol<EditorRefreshFn>(
                "flow.battleship_editor_refresh");
        editorQueueState_ =
            Symbol<EditorQueueStateFn>(
                "flow.battleship_editor_queue_state");
        editorSetLeader_ =
            Symbol<EditorSetLeaderFn>(
                "flow.battleship_editor_set_leader");
        saveDeck_ =
            Symbol<SaveDeckFn>("flow.battleship_save_deck");
        goOut_ =
            Symbol<GoOutFn>("flow.battleship_goout");
        saveReply_ =
            Symbol<ReplyConsumerFn>("flow.battleship_save_reply");
        goOutReply_ =
            Symbol<ReplyConsumerFn>("flow.battleship_goout_reply");

        if (!api_ || !mod_ || !native_ || !gameApp_ ||
            !profilePointer_ || !serviceManagedPointer_ ||
            !serviceFindByName_ || !nativeWStringCtor_ ||
            !nativeWStringDtor_ || !nativeUiFindControl_ ||
            !lobbyManagerVtable_ || !lobbySelectBattleShip_ ||
            !battleShipManagerVtable_ || !editorVtable_ ||
            !unitInfoPointer_ || !unitBriefBySlot_ ||
            !unitFullBySlot_ || !gridData_ || !gridFind_ ||
            !editorRemoveUnit_ || !editorSelectUnit_ ||
            !editorRefresh_ || !editorQueueState_ ||
            !editorSetLeader_ || !saveDeck_ || !goOut_ ||
            !saveReply_ || !goOutReply_)
            return false;

        __try {
            void** lobbyVtable =
                static_cast<void**>(lobbyManagerVtable_);
            if (lobbyVtable[kLobbyClickVtableSlot] !=
                reinterpret_cast<void*>(lobbySelectBattleShip_))
                return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }

        if (active_ && active_ != this)
            return false;
        active_ = this;
        if (!api_->HookCode(
                mod_,
                "flow.battleship_save_reply",
                reinterpret_cast<void*>(&ObserveSaveReply),
                reinterpret_cast<void**>(&saveReply_))) {
            active_ = nullptr;
            return false;
        }
        if (!api_->HookCode(
                mod_,
                "flow.battleship_goout_reply",
                reinterpret_cast<void*>(&ObserveGoOutReply),
                reinterpret_cast<void**>(&goOutReply_))) {
            active_ = nullptr;
            return false;
        }

        bound_ = true;
        Reset();
        return true;
    }

    // Poll on the normal client update thread. Exactly one native action is
    // performed per call: the lobby button, one editor removal/selection,
    // leader choice, save enqueue, or GoOut business method.
    Result EnsureDeck(const uint64_t requiredUnitUids[4])
    {
        if (!bound_ || !native_)
            return Publish(kUnbound);
        if (!ValidateTarget(requiredUnitUids))
            return Publish(kInvalidTarget);
        if (!TargetMatches(requiredUnitUids)) {
            if (targetInitialized_)
                return Publish(kInvalidTarget);
            CopyTarget(requiredUnitUids);
        }

        __try {
            const uint32_t appState = native_->AppState();
            if (appState == kLobbyAppState)
                return EnterBattleShipRoom();
            if (appState != kBattleShipAppState)
                return Publish(
                    entryInvoked_
                        ? kWaitingForBattleShip
                        : kWaitingForLobby);

            uint8_t* manager = CurrentManager();
            if (!HasVtable(manager, battleShipManagerVtable_))
                return Publish(kBattleShipManagerMismatch);

            const uint64_t selectedShipUid =
                ReadUid(manager + kManagerSelectedShip);
            if (!selectedShipUid)
                return Publish(kWaitingForShipData);
            if (!shipUid_)
                shipUid_ = selectedShipUid;
            const uint64_t shipUid = shipUid_;

            uint8_t* editor = static_cast<uint8_t*>(
                ResolveService(L"SelectBattleShipUnitList"));
            if (!HasVtable(editor, editorVtable_))
                return Publish(
                    editor ? kEditorServiceMismatch
                           : kWaitingForServices);
            if (editor[kEditorReady] == 0 ||
                *reinterpret_cast<void**>(
                    editor + kEditorModel) == nullptr)
                return Publish(kWaitingForServices);

            if (!inventoryResolved_) {
                const Result inventory = ResolveRequiredInventory(editor);
                if (inventory != kAccepted)
                    return Publish(inventory);
                inventoryResolved_ = true;
            } else if (!RequiredInventoryStillMatches()) {
                return Publish(kRequiredUnitChanged);
            }

            if (EditorBusy(editor))
                return Publish(CurrentBusyResult());

            if (!editorCleaned_) {
                for (uint32_t slot = 0; slot < kDeckSize; ++slot) {
                    uint8_t* record =
                        editor + kEditorUnits +
                        slot * kFullUnitSize;
                    if (ReadUid(record + kFullUnitUid) == 0)
                        continue;
                    editorRemoveUnit_(editor, record);
                    return Publish(kCleaningEditor);
                }
                editorCleaned_ = true;
            }

            if (selectedCount_ < kDeckSize) {
                const uint32_t slot = selectedCount_;
                if (!CanSelect(editor, slot, ownedUnits_[slot]))
                    return Publish(kEditorCostLimitMismatch);
                editorSelectUnit_(
                    editor,
                    slot,
                    ownedUnits_[slot].bytes);
                editorRefresh_(editor);
                editorQueueState_(editor, kEditorPublishState);
                if (ReadUid(
                        editor + kEditorUnits +
                        slot * kFullUnitSize +
                        kFullUnitUid) !=
                    ownedUnits_[slot].uid)
                    return Publish(kEditorMutationMismatch);
                ++selectedCount_;
                return Publish(kSelectingUnit);
            }

            if (!DeckMatches(
                    editor + kEditorUnits,
                    kFullUnitSize))
                return Publish(kWaitingForEditorPublish);

            const uint64_t targetLeader = ownedUnits_[0].uid;
            const uint64_t editorLeader =
                ReadUid(editor + kEditorLeader);
            const uint64_t managerLeader =
                ReadUid(manager + kManagerEditableLeader);
            if (editorLeader != targetLeader ||
                managerLeader != targetLeader) {
                if (!leaderRequested_) {
                    editorSetLeader_(editor, 0);
                    leaderRequested_ = true;
                    return Publish(kSelectingLeader);
                }
                return Publish(kWaitingForEditorPublish);
            }

            if (!DeckMatches(
                    manager + kManagerEditableUnits,
                    kFullUnitSize))
                return Publish(kWaitingForEditorPublish);

            if (!saveRequested_) {
                if (ManagerBusy(manager))
                    return Publish(kWaitingForEditorPublish);
                saveReplyBaseline_ = ReadSaveReply().sequence;
                saveRequested_ = true;
                saveDeck_(manager);
                return Publish(kSaving);
            }

            if (!saveCompleted_) {
                if (ManagerPhase(manager) == kPhaseError)
                    return Publish(kSaveRejected);

                const ReplySnapshot save = ReadSaveReply();
                if (save.sequence > saveReplyBaseline_ &&
                    !SaveReplyMatches(save))
                    return Publish(kSaveReplyMismatch);
                if (!SaveCompleted(manager, save))
                    return Publish(kSaving);
                saveCompleted_ = true;
            }

            const int carousel = FindShipCarousel(manager, shipUid);
            if (carousel < 0)
                return Publish(kCarouselMismatch);

            if (!goOutInvoked_) {
                int requestedCarousel = carousel;
                const uint64_t currentGoOut =
                    ReadUid(manager + kManagerCurrentGoOut);
                if (currentGoOut == shipUid) {
                    requestedCarousel =
                        FindAlternativeShipCarousel(
                            manager,
                            shipUid);
                    if (requestedCarousel < 0)
                        return Publish(kGoOutAlternativeMissing);
                    goOutTargetPending_ = true;
                }
                goOutRequestedShipUid_ =
                    CarouselShipUid(
                        manager,
                        requestedCarousel);
                if (!goOutRequestedShipUid_)
                    return Publish(kCarouselMismatch);
                goOutReplyBaseline_ = ReadGoOutReply().sequence;
                goOutInvoked_ = true;
                goOut_(manager, requestedCarousel);
                return Publish(kGoingOut);
            }

            if (ManagerPhase(manager) == kPhaseError)
                return Publish(kGoOutRejected);

            const ReplySnapshot goout = ReadGoOutReply();
            if (goout.sequence <= goOutReplyBaseline_)
                return Publish(kGoingOut);
            if (!GoOutReplyMatches(
                    manager,
                    goout,
                    goOutRequestedShipUid_,
                    !goOutTargetPending_))
                return Publish(kGoOutReplyMismatch);

            if (goOutTargetPending_) {
                goOutTargetPending_ = false;
                goOutRequestedShipUid_ = shipUid;
                goOutReplyBaseline_ = goout.sequence;
                goOut_(manager, carousel);
                return Publish(kGoingOut);
            }
            return Publish(kAccepted);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return Publish(kNativeFault);
        }
    }

    void Reset()
    {
        targetInitialized_ = false;
        entryInvoked_ = false;
        inventoryResolved_ = false;
        editorCleaned_ = false;
        leaderRequested_ = false;
        saveRequested_ = false;
        saveCompleted_ = false;
        goOutInvoked_ = false;
        goOutTargetPending_ = false;
        selectedCount_ = 0;
        totalCost_ = 0;
        shipUid_ = 0;
        goOutRequestedShipUid_ = 0;
        saveReplyBaseline_ = 0;
        goOutReplyBaseline_ = 0;
        std::memset(targetUids_, 0, sizeof(targetUids_));
        std::memset(ownedUnits_, 0, sizeof(ownedUnits_));
        InterlockedExchange(&lastResult_, kUnbound);
        InterlockedExchange(&stage_, 0);
    }

    Observation State() const
    {
        Observation observed = {};
        observed.result = static_cast<uint32_t>(
            InterlockedCompareExchange(&lastResult_, 0, 0));
        observed.native_app_state =
            native_ ? native_->AppState() : UINT32_MAX;
        observed.stage = static_cast<uint32_t>(
            InterlockedCompareExchange(&stage_, 0, 0));
        observed.selected_count = selectedCount_;
        observed.total_cost = totalCost_;
        observed.ship_uid = shipUid_;
        observed.unit_uids[0] = targetUids_[0];
        observed.unit_uids[1] = targetUids_[1];
        observed.unit_uids[2] = targetUids_[2];
        observed.unit_uids[3] = targetUids_[3];

        const ReplySnapshot save = ReadSaveReply();
        const ReplySnapshot goout = ReadGoOutReply();
        observed.save_reply_sequence = save.sequence;
        observed.goout_reply_sequence = goout.sequence;
        observed.save_reply_opcode = save.opcode;
        observed.goout_reply_opcode = goout.opcode;
        if (native_)
            observed.transport_sequence =
                native_->TransportState().send_sequence;

        __try {
            uint8_t* manager =
                observed.native_app_state == kBattleShipAppState
                    ? CurrentManager()
                    : nullptr;
            uint8_t* editor = static_cast<uint8_t*>(
                ResolveService(L"SelectBattleShipUnitList"));
            if (HasVtable(editor, editorVtable_)) {
                observed.editor_matches =
                    DeckMatches(
                        editor + kEditorUnits,
                        kFullUnitSize) ? 1 : 0;
                observed.leader_uid =
                    ReadUid(editor + kEditorLeader);
            }
            if (HasVtable(manager, battleShipManagerVtable_)) {
                observed.manager_matches =
                    DeckMatches(
                        manager + kManagerEditableUnits,
                        kFullUnitSize) ? 1 : 0;
                if (!observed.leader_uid)
                    observed.leader_uid =
                        ReadUid(
                            manager +
                            kManagerEditableLeader);
                observed.profile_matches =
                    ProfileMatches(
                        ReadUid(
                            manager +
                            kManagerSelectedShip)) ? 1 : 0;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            observed.editor_matches = 0;
            observed.manager_matches = 0;
            observed.profile_matches = 0;
        }
        return observed;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound: return "unbound";
        case kWaitingForLobby: return "waiting-for-lobby";
        case kOpeningBattleShip: return "opening-battleship";
        case kWaitingForBattleShip: return "waiting-for-battleship";
        case kWaitingForServices: return "waiting-for-services";
        case kWaitingForShipData: return "waiting-for-ship-data";
        case kWaitingForInventory: return "waiting-for-inventory";
        case kCleaningEditor: return "cleaning-editor";
        case kSelectingUnit: return "selecting-unit";
        case kWaitingForEditorPublish:
            return "waiting-for-editor-publish";
        case kSelectingLeader: return "selecting-leader";
        case kSaving: return "saving";
        case kGoingOut: return "going-out";
        case kAccepted: return "accepted";
        case kInvalidTarget: return "invalid-target";
        case kLobbyServiceMismatch:
            return "lobby-service-mismatch";
        case kLobbyControlMissing:
            return "lobby-control-missing";
        case kLobbyControlMismatch:
            return "lobby-control-mismatch";
        case kBattleShipManagerMismatch:
            return "battleship-manager-mismatch";
        case kEditorServiceMismatch:
            return "editor-service-mismatch";
        case kInventoryUnavailable:
            return "inventory-unavailable";
        case kRequiredUnitMissing:
            return "required-unit-missing";
        case kRequiredUnitChanged:
            return "required-unit-changed";
        case kRequiredUnitDuplicate:
            return "required-unit-duplicate";
        case kRequiredUnitTypeMismatch:
            return "required-unit-type-mismatch";
        case kRequiredUnitCostMismatch:
            return "required-unit-cost-mismatch";
        case kEditorCostLimitMismatch:
            return "editor-cost-limit-mismatch";
        case kEditorMutationMismatch:
            return "editor-mutation-mismatch";
        case kLeaderSelectionFailed:
            return "leader-selection-failed";
        case kSaveRejected: return "save-rejected";
        case kSaveReplyMismatch:
            return "save-reply-mismatch";
        case kGoOutRejected: return "goout-rejected";
        case kGoOutReplyMismatch:
            return "goout-reply-mismatch";
        case kCarouselMismatch:
            return "carousel-mismatch";
        case kGoOutAlternativeMissing:
            return "goout-alternative-missing";
        case kNativeFault: return "native-fault";
        default: return "unknown";
        }
    }

private:
    static constexpr uint32_t kLobbyAppState = 6;
    static constexpr uint32_t kBattleShipAppState = 20;
    static constexpr uint32_t kDeckSize = 4;
    static constexpr uint32_t kRequiredTotalCost = 8;
    static constexpr uint32_t kMaxInventorySlots = 4096;
    static constexpr uint32_t kMaxCarouselShips = 15;
    static constexpr uint32_t kClickMessage = 0x101;
    static constexpr uint32_t kEditorPublishState = 4;
    static constexpr uint16_t kSaveReplyOpcode = 0x0A27;
    static constexpr uint16_t kGoOutReplyOpcode = 0x04A6;
    static constexpr float kPhaseAccepted = 70.0f;
    static constexpr float kPhaseError = 80.0f;

    static constexpr size_t kNativeWStringSize = 0x1C;
    static constexpr size_t kServiceDescriptorSize =
        sizeof(uint32_t) + kNativeWStringSize;
    static constexpr size_t kGameAppState = 0x4A0;
    static constexpr size_t kGameAppManager = 0x4A4;
    static constexpr size_t kLobbyRoot = 0x64;
    static constexpr size_t kControlName = 0x118;
    static constexpr size_t kControlParent = 0x15C;
    static constexpr size_t kLobbyClickVtableSlot =
        0x38 / sizeof(void*);

    static constexpr size_t kUnitInfoCapacity = 0x0C;
    static constexpr size_t kUnitInfoReady = 0x20;
    static constexpr size_t kBriefUnitSize = 0x19;
    static constexpr size_t kFullUnitSize = 0x99;
    static constexpr size_t kFullUnitId = 0x00;
    static constexpr size_t kFullUnitUid = 0x08;
    static constexpr size_t kGridCost = 0x139;

    static constexpr size_t kEditorCommandCount = 0x118;
    static constexpr size_t kEditorReady = 0x120;
    static constexpr size_t kEditorMaxCost = 0x129;
    static constexpr size_t kEditorCurrentCost = 0x12A;
    static constexpr size_t kEditorLeader = 0x1E0;
    static constexpr size_t kEditorUnits = 0x1E8;
    static constexpr size_t kEditorModel = 0x454;

    static constexpr size_t kManagerCommandCount = 0x18;
    static constexpr size_t kManagerPhase = 0x2080;
    static constexpr size_t kManagerCurrentGoOut = 0x20D8;
    static constexpr size_t kManagerCarouselCount = 0x20CE;
    static constexpr size_t kManagerCarousel = 0x20E0;
    static constexpr size_t kManagerCarouselStride = 0x58;
    static constexpr size_t kManagerSelectedShip = 0x261C;
    static constexpr size_t kManagerEditableLeader = 0x2BC8;
    static constexpr size_t kManagerEditableUnits = 0x2BD0;

    static constexpr size_t kProfileBattleShip = 0x2A6;
    static constexpr size_t kShipPayloadSize = 0x2BC;
    static constexpr size_t kShipLeader = 0x1C;
    static constexpr size_t kShipUnitUids = 0x34;
    static constexpr size_t kShipUnitUidStride = 0x08;

    struct OwnedUnit
    {
        alignas(4) uint8_t bytes[kFullUnitSize];
        uint64_t uid;
        uint32_t id;
        uint32_t slot;
        uint8_t cost;
        uint8_t reserved[3];
    };

    struct ReplySnapshot
    {
        uint32_t sequence;
        uint64_t ship_uid;
        uint64_t unit_uids[kDeckSize];
        uint16_t opcode;
        uint8_t consumed;
        uint8_t reserved;
    };

    using GameAppFn = uint8_t*(__cdecl*)();
    using ServiceFindByNameFn =
        void*(__thiscall*)(void*, const void*);
    using NativeWStringCtorFn =
        void*(__thiscall*)(void*, const wchar_t*);
    using NativeWStringDtorFn = void(__thiscall*)(void*);
    using NativeUiFindControlFn =
        void*(__thiscall*)(void*, const void*, uint32_t);
    using LobbySelectBattleShipFn =
        void(__stdcall*)(void*, uint32_t, const void*, void*);
    using UnitBriefBySlotFn =
        void*(__thiscall*)(void*, void*, uint16_t);
    using UnitFullBySlotFn =
        void*(__thiscall*)(void*, void*, uint16_t);
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn =
        bool(__thiscall*)(
            void*,
            void**,
            uint32_t,
            uint32_t,
            bool);
    using EditorRemoveUnitFn =
        void(__thiscall*)(void*, const void*);
    using EditorSelectUnitFn =
        void(__thiscall*)(void*, uint32_t, const void*);
    using EditorRefreshFn = void(__thiscall*)(void*);
    using EditorQueueStateFn =
        void(__thiscall*)(void*, uint32_t);
    using EditorSetLeaderFn =
        void(__thiscall*)(void*, int);
    using SaveDeckFn = void(__thiscall*)(void*);
    using GoOutFn = void(__thiscall*)(void*, int);
    using ReplyConsumerFn =
        void(__thiscall*)(void*, const void*);
    using ReplyDetourFn =
        void(__fastcall*)(void*, void*, const void*);

    template <typename T>
    T Symbol(const char* name) const
    {
        if (!api_ || !api_->Symbol || !name)
            return nullptr;
        return reinterpret_cast<T>(api_->Symbol(mod_, name));
    }

    Result Publish(Result result) const
    {
        InterlockedExchange(
            &lastResult_,
            static_cast<LONG>(result));
        InterlockedExchange(
            &stage_,
            static_cast<LONG>(result));
        return result;
    }

    static bool ValidateTarget(const uint64_t target[4])
    {
        if (!target)
            return false;
        for (uint32_t i = 0; i < kDeckSize; ++i) {
            if (!target[i])
                return false;
            for (uint32_t j = 0; j < i; ++j) {
                if (target[i] == target[j])
                    return false;
            }
        }
        return true;
    }

    bool TargetMatches(const uint64_t target[4]) const
    {
        if (!targetInitialized_)
            return false;
        return std::memcmp(
                   targetUids_,
                   target,
                   sizeof(targetUids_)) == 0;
    }

    void CopyTarget(const uint64_t target[4])
    {
        std::memcpy(targetUids_, target, sizeof(targetUids_));
        targetInitialized_ = true;
    }

    Result EnterBattleShipRoom()
    {
        uint8_t* manager = static_cast<uint8_t*>(
            ResolveService(L"unitmgr"));
        if (!HasVtable(manager, lobbyManagerVtable_)) {
            api_->Log(mod_,SDGO_LOG_ERROR,"native deck unitmgr=%p vtable=%p expected=%p",
                manager,manager ? *reinterpret_cast<void**>(manager) : nullptr,lobbyManagerVtable_);
            return Publish(kLobbyServiceMismatch);
        }
        if (entryInvoked_)
            return Publish(kWaitingForBattleShip);

        void* root = *reinterpret_cast<void**>(manager + kLobbyRoot);
        if (!root)
            return Publish(kLobbyControlMissing);
        void* control = FindControl(root, L"SelectBattleShipBtn");
        if (!control)
            return Publish(kLobbyControlMissing);
        if (*reinterpret_cast<void**>(
                static_cast<uint8_t*>(control) +
                kControlParent) != root ||
            !ControlHasName(control, L"SelectBattleShipBtn"))
            return Publish(kLobbyControlMismatch);

        entryInvoked_ = true;
        lobbySelectBattleShip_(
            manager,
            kClickMessage,
            static_cast<uint8_t*>(control) + kControlName,
            control);
        return Publish(kOpeningBattleShip);
    }

    Result ResolveRequiredInventory(uint8_t* editor)
    {
        uint8_t* unitInfo =
            unitInfoPointer_
                ? static_cast<uint8_t*>(*unitInfoPointer_)
                : nullptr;
        if (!unitInfo || unitInfo[kUnitInfoReady] == 0)
            return kWaitingForInventory;

        const int32_t capacity =
            *reinterpret_cast<const int32_t*>(
                unitInfo + kUnitInfoCapacity);
        if (capacity <= 0 ||
            capacity > static_cast<int32_t>(kMaxInventorySlots))
            return kInventoryUnavailable;

        bool found[kDeckSize] = {};
        for (int32_t zero = 0; zero < capacity; ++zero) {
            alignas(4) uint8_t brief[kBriefUnitSize] = {};
            unitBriefBySlot_(
                unitInfo,
                brief,
                static_cast<uint16_t>(zero + 1));
            if (*reinterpret_cast<uint32_t*>(
                    brief + kFullUnitId) == 0)
                continue;

            alignas(4) uint8_t full[kFullUnitSize] = {};
            unitFullBySlot_(
                unitInfo,
                full,
                static_cast<uint16_t>(zero + 1));
            const uint64_t uid =
                ReadUid(full + kFullUnitUid);
            for (uint32_t target = 0;
                 target < kDeckSize;
                 ++target) {
                if (uid != targetUids_[target])
                    continue;
                if (found[target])
                    return kRequiredUnitDuplicate;
                std::memcpy(
                    ownedUnits_[target].bytes,
                    full,
                    kFullUnitSize);
                ownedUnits_[target].uid = uid;
                ownedUnits_[target].id =
                    *reinterpret_cast<uint32_t*>(
                        full + kFullUnitId);
                ownedUnits_[target].slot =
                    static_cast<uint32_t>(zero + 1);
                found[target] = true;
            }
        }

        totalCost_ = 0;
        for (uint32_t i = 0; i < kDeckSize; ++i) {
            if (!found[i])
                return kRequiredUnitMissing;
            for (uint32_t j = 0; j < i; ++j) {
                if (ownedUnits_[i].id ==
                    ownedUnits_[j].id)
                    return kRequiredUnitDuplicate;
            }
            void* grid = nullptr;
            if (!FindUnitGrid(
                    ownedUnits_[i].id,
                    &grid) ||
                !grid)
                return kRequiredUnitTypeMismatch;
            ownedUnits_[i].cost =
                *(static_cast<uint8_t*>(grid) +
                  kGridCost);
            totalCost_ += ownedUnits_[i].cost;
        }

        if (totalCost_ != kRequiredTotalCost)
            return kRequiredUnitCostMismatch;
        if (editor[kEditorMaxCost] <
            kRequiredTotalCost)
            return kEditorCostLimitMismatch;
        return kAccepted;
    }

    bool RequiredInventoryStillMatches() const
    {
        uint8_t* unitInfo =
            unitInfoPointer_
                ? static_cast<uint8_t*>(*unitInfoPointer_)
                : nullptr;
        if (!unitInfo || unitInfo[kUnitInfoReady] == 0)
            return false;
        for (uint32_t i = 0; i < kDeckSize; ++i) {
            alignas(4) uint8_t brief[kBriefUnitSize] = {};
            alignas(4) uint8_t full[kFullUnitSize] = {};
            unitBriefBySlot_(
                unitInfo,
                brief,
                static_cast<uint16_t>(ownedUnits_[i].slot));
            if (*reinterpret_cast<uint32_t*>(brief) == 0)
                return false;
            unitFullBySlot_(
                unitInfo,
                full,
                static_cast<uint16_t>(ownedUnits_[i].slot));
            if (ReadUid(full + kFullUnitUid) !=
                    ownedUnits_[i].uid ||
                *reinterpret_cast<uint32_t*>(full) !=
                    ownedUnits_[i].id)
                return false;
        }
        return true;
    }

    bool CanSelect(
        const uint8_t* editor,
        uint32_t slot,
        const OwnedUnit& candidate) const
    {
        if (!editor || slot >= kDeckSize ||
            !candidate.uid || !candidate.id)
            return false;
        uint32_t sum = 0;
        for (uint32_t i = 0; i < kDeckSize; ++i) {
            const uint8_t* record =
                editor + kEditorUnits +
                i * kFullUnitSize;
            const uint64_t uid =
                ReadUid(record + kFullUnitUid);
            if (uid && uid == candidate.uid)
                return false;
            if (i == slot ||
                *reinterpret_cast<const uint32_t*>(
                    record + kFullUnitId) == 0)
                continue;
            void* grid = nullptr;
            if (!FindUnitGrid(
                    *reinterpret_cast<
                        const uint32_t*>(
                        record + kFullUnitId),
                    &grid) ||
                !grid)
                return false;
            sum += *(
                static_cast<const uint8_t*>(grid) +
                kGridCost);
        }
        sum += candidate.cost;
        return sum <= editor[kEditorMaxCost] &&
               sum <= kRequiredTotalCost;
    }

    bool FindUnitGrid(uint32_t id, void** out) const
    {
        if (out)
            *out = nullptr;
        void* data = gridData_ ? gridData_() : nullptr;
        return data && out &&
               gridFind_(
                   data,
                   out,
                   id,
                   2,
                   false);
    }

    bool SaveCompleted(
        const uint8_t* manager,
        const ReplySnapshot& save) const
    {
        if (save.sequence <= saveReplyBaseline_)
            return false;
        if (!SaveReplyMatches(save))
            return false;
        return DeckMatches(
                   manager + kManagerSelectedShip +
                       kShipUnitUids,
                   kShipUnitUidStride,
                   true) &&
               ProfileMatches(shipUid_);
    }

    bool SaveReplyMatches(
        const ReplySnapshot& save) const
    {
        return save.opcode == kSaveReplyOpcode &&
               save.consumed &&
               save.ship_uid == shipUid_ &&
               ReplyDeckMatches(save);
    }

    bool GoOutReplyMatches(
        const uint8_t* manager,
        const ReplySnapshot& goout,
        uint64_t expectedShipUid,
        bool requireTargetDeck) const
    {
        if (!manager || !expectedShipUid ||
            goout.opcode != kGoOutReplyOpcode ||
            !goout.consumed ||
            goout.ship_uid != expectedShipUid ||
            ReadUid(manager + kManagerCurrentGoOut) !=
                expectedShipUid ||
            !ProfileShipUidMatches(expectedShipUid))
            return false;
        if (!requireTargetDeck)
            return true;
        return expectedShipUid == shipUid_ &&
               ReplyDeckMatches(goout) &&
               DeckMatches(
                   manager + kManagerSelectedShip +
                       kShipUnitUids,
                   kShipUnitUidStride,
                   true) &&
               DeckMatches(
                   manager + kManagerEditableUnits,
                   kFullUnitSize) &&
               ReadUid(
                   manager + kManagerEditableLeader) ==
                   targetUids_[0] &&
               ProfileMatches(expectedShipUid);
    }

    bool ReplyDeckMatches(
        const ReplySnapshot& reply) const
    {
        for (uint32_t i = 0; i < kDeckSize; ++i) {
            if (reply.unit_uids[i] !=
                targetUids_[i])
                return false;
        }
        return true;
    }

    bool ProfileMatches(uint64_t shipUid) const
    {
        uint8_t* profile =
            profilePointer_ ? *profilePointer_ : nullptr;
        if (!profile || !shipUid)
            return false;
        const uint8_t* ship =
            profile + kProfileBattleShip;
        return ReadUid(ship) == shipUid &&
               ReadUid(ship + kShipLeader) ==
                   targetUids_[0] &&
               DeckMatches(
                   ship + kShipUnitUids,
                   kShipUnitUidStride,
                   true);
    }

    bool ProfileShipUidMatches(uint64_t shipUid) const
    {
        uint8_t* profile =
            profilePointer_ ? *profilePointer_ : nullptr;
        return profile && shipUid &&
               ReadUid(
                   profile + kProfileBattleShip) ==
                   shipUid;
    }

    bool DeckMatches(
        const uint8_t* records,
        size_t stride,
        bool uidOnly = false) const
    {
        if (!records || !targetInitialized_)
            return false;
        for (uint32_t i = 0; i < kDeckSize; ++i) {
            const uint8_t* record =
                records + i * stride;
            const uint64_t uid =
                ReadUid(
                    record +
                    (uidOnly ? 0 : kFullUnitUid));
            if (uid != targetUids_[i])
                return false;
        }
        return true;
    }

    int FindShipCarousel(
        const uint8_t* manager,
        uint64_t shipUid) const
    {
        if (!manager || !shipUid)
            return -1;
        const uint16_t count =
            *reinterpret_cast<const uint16_t*>(
                manager + kManagerCarouselCount);
        if (!count || count > kMaxCarouselShips)
            return -1;
        for (uint16_t i = 0; i < count; ++i) {
            if (ReadUid(
                    manager + kManagerCarousel +
                    static_cast<size_t>(i) *
                        kManagerCarouselStride) ==
                shipUid)
                return static_cast<int>(i);
        }
        return -1;
    }

    int FindAlternativeShipCarousel(
        const uint8_t* manager,
        uint64_t excludedShipUid) const
    {
        if (!manager || !excludedShipUid)
            return -1;
        const uint16_t count =
            *reinterpret_cast<const uint16_t*>(
                manager + kManagerCarouselCount);
        if (!count || count > kMaxCarouselShips)
            return -1;
        for (uint16_t i = 0; i < count; ++i) {
            const uint64_t uid =
                CarouselShipUid(manager, i);
            if (uid && uid != excludedShipUid)
                return static_cast<int>(i);
        }
        return -1;
    }

    static uint64_t CarouselShipUid(
        const uint8_t* manager,
        int index)
    {
        if (!manager || index < 0 ||
            index >= static_cast<int>(kMaxCarouselShips))
            return 0;
        return ReadUid(
            manager + kManagerCarousel +
            static_cast<size_t>(index) *
                kManagerCarouselStride);
    }

    Result CurrentBusyResult() const
    {
        if (!editorCleaned_)
            return kCleaningEditor;
        if (selectedCount_ < kDeckSize)
            return kSelectingUnit;
        if (!saveRequested_)
            return kWaitingForEditorPublish;
        return kSaving;
    }

    bool EditorBusy(const uint8_t* editor) const
    {
        return *reinterpret_cast<const uint32_t*>(
                   editor + kEditorCommandCount) != 0;
    }

    bool ManagerBusy(const uint8_t* manager) const
    {
        return *reinterpret_cast<const uint32_t*>(
                   manager + kManagerCommandCount) != 0;
    }

    static float ManagerPhase(const uint8_t* manager)
    {
        return *reinterpret_cast<const float*>(
            manager + kManagerPhase);
    }

    uint8_t* CurrentManager() const
    {
        uint8_t* app = gameApp_ ? gameApp_() : nullptr;
        if (!app)
            return nullptr;
        return static_cast<uint8_t*>(
            *reinterpret_cast<void**>(
                app + kGameAppManager));
    }

    static bool HasVtable(
        const void* object,
        const void* expected)
    {
        return object && expected &&
               *static_cast<void* const*>(object) ==
                   expected;
    }

    void* ResolveService(const wchar_t* name) const
    {
        if (!name || !serviceManagedPointer_ ||
            !*serviceManagedPointer_ || !serviceFindByName_ ||
            !nativeWStringCtor_ || !nativeWStringDtor_)
            return nullptr;
        alignas(4) uint8_t descriptor[
            kServiceDescriptorSize] = {};
        *reinterpret_cast<uint32_t*>(descriptor) = 1;
        bool constructed = false;
        void* found = nullptr;
        __try {
            nativeWStringCtor_(
                descriptor + sizeof(uint32_t),
                name);
            constructed = true;
            found = serviceFindByName_(
                *serviceManagedPointer_,
                descriptor);
        } __finally {
            if (constructed)
                nativeWStringDtor_(
                    descriptor + sizeof(uint32_t));
        }
        return found;
    }

    void* FindControl(
        void* root,
        const wchar_t* name) const
    {
        if (!root || !name)
            return nullptr;
        alignas(4) uint8_t nativeName[kNativeWStringSize] = {};
        bool constructed = false;
        void* found = nullptr;
        __try {
            nativeWStringCtor_(nativeName, name);
            constructed = true;
            found = nativeUiFindControl_(
                root,
                nativeName,
                4);
        } __finally {
            if (constructed)
                nativeWStringDtor_(nativeName);
        }
        return found;
    }

    static const wchar_t* NativeStringData(
        const void* storage)
    {
        if (!storage)
            return nullptr;
        const uint8_t* bytes =
            static_cast<const uint8_t*>(storage);
        const uint32_t capacity =
            *reinterpret_cast<const uint32_t*>(
                bytes + 0x14);
        return capacity < 8
            ? reinterpret_cast<const wchar_t*>(bytes)
            : *reinterpret_cast<
                  wchar_t* const*>(bytes);
    }

    static bool ControlHasName(
        const void* control,
        const wchar_t* expected)
    {
        const wchar_t* actual = NativeStringData(
            static_cast<const uint8_t*>(control) +
            kControlName);
        return actual && expected &&
               std::wcscmp(actual, expected) == 0;
    }

    static uint64_t ReadUid(const void* bytes)
    {
        uint64_t value = 0;
        if (bytes)
            std::memcpy(&value, bytes, sizeof(value));
        return value;
    }

    static bool TryCopyExpectedReply(
        bool save,
        void* self,
        const void* packet,
        void* copy)
    {
        BattleShipDeckActions* active = active_;
        if (!active || !self || !packet || !copy)
            return false;
        __try {
            if (self != active->CurrentManager() ||
                !HasVtable(
                    self,
                    active->battleShipManagerVtable_))
                return false;
            uint16_t opcode = 0;
            std::memcpy(&opcode, packet, sizeof(opcode));
            if (opcode !=
                (save ? kSaveReplyOpcode
                      : kGoOutReplyOpcode))
                return false;
            std::memcpy(
                copy,
                packet,
                6 + kShipPayloadSize);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static bool OriginalReplyConsumed(
        bool save,
        void* self,
        const void* copy)
    {
        BattleShipDeckActions* active = active_;
        if (!active || !self || !copy)
            return false;
        __try {
            if (self != active->CurrentManager() ||
                !HasVtable(
                    self,
                    active->battleShipManagerVtable_))
                return false;
            const auto* manager =
                static_cast<const uint8_t*>(self);
            if (save)
                return ManagerPhase(manager) ==
                       kPhaseAccepted;
            const uint64_t shipUid =
                ReadUid(
                    static_cast<const uint8_t*>(
                        copy) +
                    6);
            return shipUid &&
                   ReadUid(
                       manager +
                       kManagerCurrentGoOut) ==
                       shipUid &&
                   active->ProfileShipUidMatches(
                       shipUid);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    static void WriteObservedReply(
        bool save,
        const void* packet,
        bool consumed)
    {
        BattleShipDeckActions* self = active_;
        if (!self || !packet)
            return;
        const uint8_t* bytes =
            static_cast<const uint8_t*>(packet);
        const uint8_t* ship = bytes + 6;
        volatile LONG* version =
            save ? &saveReplyWriteVersion_
                 : &goOutReplyWriteVersion_;
        volatile LONG* sequence =
            save ? &saveReplySequence_
                 : &goOutReplySequence_;
        volatile LONG* opcode =
            save ? &saveReplyOpcode_
                 : &goOutReplyOpcode_;
        volatile LONG* consumedField =
            save ? &saveReplyConsumed_
                 : &goOutReplyConsumed_;
        volatile LONG* shipLow =
            save ? &saveReplyShipLow_
                 : &goOutReplyShipLow_;
        volatile LONG* shipHigh =
            save ? &saveReplyShipHigh_
                 : &goOutReplyShipHigh_;
        volatile LONG* units =
            save ? saveReplyUnits_
                 : goOutReplyUnits_;

        InterlockedIncrement(version);
        const uint64_t shipUid = ReadUid(ship);
        InterlockedExchange(
            opcode,
            *reinterpret_cast<const uint16_t*>(
                bytes));
        InterlockedExchange(
            shipLow,
            static_cast<LONG>(
                shipUid & 0xFFFFFFFFu));
        InterlockedExchange(
            shipHigh,
            static_cast<LONG>(shipUid >> 32));
        for (uint32_t i = 0; i < kDeckSize; ++i) {
            const uint64_t uid = ReadUid(
                ship + kShipUnitUids +
                i * kShipUnitUidStride);
            InterlockedExchange(
                &units[i * 2],
                static_cast<LONG>(
                    uid & 0xFFFFFFFFu));
            InterlockedExchange(
                &units[i * 2 + 1],
                static_cast<LONG>(uid >> 32));
        }
        InterlockedExchange(
            consumedField,
            consumed ? 1 : 0);
        InterlockedIncrement(sequence);
        InterlockedIncrement(version);
    }

    static void __fastcall ObserveSaveReply(
        void* self,
        void*,
        const void* packet)
    {
        alignas(4) uint8_t copy[
            6 + kShipPayloadSize] = {};
        const bool expected =
            TryCopyExpectedReply(
                true,
                self,
                packet,
                copy);
        saveReply_(self, packet);
        if (expected &&
            OriginalReplyConsumed(
                true,
                self,
                copy))
            WriteObservedReply(true, copy, true);
    }

    static void __fastcall ObserveGoOutReply(
        void* self,
        void*,
        const void* packet)
    {
        alignas(4) uint8_t copy[
            6 + kShipPayloadSize] = {};
        const bool expected =
            TryCopyExpectedReply(
                false,
                self,
                packet,
                copy);
        goOutReply_(self, packet);
        if (expected &&
            OriginalReplyConsumed(
                false,
                self,
                copy))
            WriteObservedReply(false, copy, true);
    }

    static ReplySnapshot ReadReply(
        bool save)
    {
        ReplySnapshot reply = {};
        volatile LONG* version =
            save ? &saveReplyWriteVersion_
                 : &goOutReplyWriteVersion_;
        volatile LONG* sequence =
            save ? &saveReplySequence_
                 : &goOutReplySequence_;
        volatile LONG* opcode =
            save ? &saveReplyOpcode_
                 : &goOutReplyOpcode_;
        volatile LONG* consumed =
            save ? &saveReplyConsumed_
                 : &goOutReplyConsumed_;
        volatile LONG* shipLow =
            save ? &saveReplyShipLow_
                 : &goOutReplyShipLow_;
        volatile LONG* shipHigh =
            save ? &saveReplyShipHigh_
                 : &goOutReplyShipHigh_;
        volatile LONG* units =
            save ? saveReplyUnits_
                 : goOutReplyUnits_;
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before =
                InterlockedCompareExchange(version, 0, 0);
            if (before & 1)
                continue;
            reply.sequence = static_cast<uint32_t>(
                InterlockedCompareExchange(
                    sequence,
                    0,
                    0));
            reply.opcode = static_cast<uint16_t>(
                InterlockedCompareExchange(
                    opcode,
                    0,
                    0));
            reply.consumed =
                InterlockedCompareExchange(
                    consumed,
                    0,
                    0) != 0 ? 1 : 0;
            const uint32_t low =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        shipLow,
                        0,
                        0));
            const uint32_t high =
                static_cast<uint32_t>(
                    InterlockedCompareExchange(
                        shipHigh,
                        0,
                        0));
            reply.ship_uid =
                static_cast<uint64_t>(low) |
                (static_cast<uint64_t>(high) << 32);
            for (uint32_t i = 0; i < kDeckSize; ++i) {
                const uint32_t unitLow =
                    static_cast<uint32_t>(
                        InterlockedCompareExchange(
                            &units[i * 2],
                            0,
                            0));
                const uint32_t unitHigh =
                    static_cast<uint32_t>(
                        InterlockedCompareExchange(
                            &units[i * 2 + 1],
                            0,
                            0));
                reply.unit_uids[i] =
                    static_cast<uint64_t>(unitLow) |
                    (static_cast<uint64_t>(
                         unitHigh) << 32);
            }
            const LONG after =
                InterlockedCompareExchange(version, 0, 0);
            if (before == after && !(after & 1))
                return reply;
        }
        std::memset(&reply, 0, sizeof(reply));
        return reply;
    }

    static ReplySnapshot ReadSaveReply()
    {
        return ReadReply(true);
    }

    static ReplySnapshot ReadGoOutReply()
    {
        return ReadReply(false);
    }

    const SdgoApi* api_ = nullptr;
    SdgoMod* mod_ = nullptr;
    NativeActions* native_ = nullptr;
    bool bound_ = false;
    bool targetInitialized_ = false;
    bool entryInvoked_ = false;
    bool inventoryResolved_ = false;
    bool editorCleaned_ = false;
    bool leaderRequested_ = false;
    bool saveRequested_ = false;
    bool saveCompleted_ = false;
    bool goOutInvoked_ = false;
    bool goOutTargetPending_ = false;
    uint32_t selectedCount_ = 0;
    uint32_t totalCost_ = 0;
    uint32_t saveReplyBaseline_ = 0;
    uint32_t goOutReplyBaseline_ = 0;
    uint64_t shipUid_ = 0;
    uint64_t goOutRequestedShipUid_ = 0;
    uint64_t targetUids_[kDeckSize] = {};
    OwnedUnit ownedUnits_[kDeckSize] = {};
    mutable volatile LONG lastResult_ = kUnbound;
    mutable volatile LONG stage_ = 0;

    GameAppFn gameApp_ = nullptr;
    uint8_t** profilePointer_ = nullptr;
    void** serviceManagedPointer_ = nullptr;
    ServiceFindByNameFn serviceFindByName_ = nullptr;
    NativeWStringCtorFn nativeWStringCtor_ = nullptr;
    NativeWStringDtorFn nativeWStringDtor_ = nullptr;
    NativeUiFindControlFn nativeUiFindControl_ = nullptr;
    void* lobbyManagerVtable_ = nullptr;
    LobbySelectBattleShipFn lobbySelectBattleShip_ = nullptr;
    void* battleShipManagerVtable_ = nullptr;
    void* editorVtable_ = nullptr;
    void** unitInfoPointer_ = nullptr;
    UnitBriefBySlotFn unitBriefBySlot_ = nullptr;
    UnitFullBySlotFn unitFullBySlot_ = nullptr;
    GridDataFn gridData_ = nullptr;
    GridFindFn gridFind_ = nullptr;
    EditorRemoveUnitFn editorRemoveUnit_ = nullptr;
    EditorSelectUnitFn editorSelectUnit_ = nullptr;
    EditorRefreshFn editorRefresh_ = nullptr;
    EditorQueueStateFn editorQueueState_ = nullptr;
    EditorSetLeaderFn editorSetLeader_ = nullptr;
    SaveDeckFn saveDeck_ = nullptr;
    GoOutFn goOut_ = nullptr;

    static inline BattleShipDeckActions* active_ = nullptr;
    static inline ReplyConsumerFn saveReply_ = nullptr;
    static inline ReplyConsumerFn goOutReply_ = nullptr;
    static inline volatile LONG saveReplyWriteVersion_ = 0;
    static inline volatile LONG saveReplySequence_ = 0;
    static inline volatile LONG saveReplyOpcode_ = 0;
    static inline volatile LONG saveReplyConsumed_ = 0;
    static inline volatile LONG saveReplyShipLow_ = 0;
    static inline volatile LONG saveReplyShipHigh_ = 0;
    static inline volatile LONG saveReplyUnits_[kDeckSize * 2] = {};
    static inline volatile LONG goOutReplyWriteVersion_ = 0;
    static inline volatile LONG goOutReplySequence_ = 0;
    static inline volatile LONG goOutReplyOpcode_ = 0;
    static inline volatile LONG goOutReplyConsumed_ = 0;
    static inline volatile LONG goOutReplyShipLow_ = 0;
    static inline volatile LONG goOutReplyShipHigh_ = 0;
    static inline volatile LONG goOutReplyUnits_[kDeckSize * 2] = {};
};
