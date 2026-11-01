#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "sdgo/game_test.h"
#include "sdgo/game_test_page_observer.h"

// Main-thread adapter for page-only native GameTest cases.
//
// It resolves the observer/action exports from already-loaded Mods. It never
// loads a DLL, finds a control itself, sends a packet, writes native UI/data or
// interprets action acceptance as completion. The page Mods remain the sole
// owners of native identity checks and business callbacks.
class PageTestActions
{
public:
    enum Result : uint32_t
    {
        kUnbound = 0,
        kHangarUnavailable = 1,
        kStorageUnavailable = 2,
        kWaitingSnapshot = 3,
        kReadReady = 4,
        kActionAccepted = 5,
        kWaitingAction = 6,
        kPreviewStepDone = 7,
        kConfirmRejected = 8,
        kPreviewReady = 9,
        kCancelAccepted = 10,
        kCancelled = 11,
        kStorageReady = 12,

        kInvalidContext = 0x100,
        kSnapshotCallFailed = 0x101,
        kProcessMismatch = 0x102,
        kThreadMismatch = 0x103,
        kRunMismatch = 0x104,
        kUnitPageMismatch = 0x105,
        kSnapshotChanged = 0x106,
        kInventoryChanged = 0x107,
        kActionRejected = 0x108,
        kActionSequenceMismatch = 0x109,
        kUnsafeConfirmAccepted = 0x10A,
        kCancelStateMissing = 0x10B,
        kCancelStillVisible = 0x10C,
    };

    enum Step : uint32_t
    {
        kStepNone = 0,
        kStepRead = 1,
        kStepOpen = 2,
        kStepPreviewGb = 3,
        kStepRejectConfirmGb = 4,
        kStepPreviewItem = 5,
        kStepRejectConfirmItem = 6,
        kStepCancel = 7,
        kStepCancelHiddenProbe = 8,
        kStepStorageBinding = 9,
    };

    enum Scope : uint32_t
    {
        kScopeUnknown = 0,
        kScopeProtectedOrNoDuplicateNegative = 1,
        kScopePreviewCandidatesPresent = 2,
    };

    struct Exports
    {
        SdgoHangarGameTestGetSnapshotV1Fn hangar_snapshot = nullptr;
        SdgoHangarGameTestGetUnitsV1Fn hangar_units = nullptr;
        SdgoHangarGameTestActionV1Fn hangar_action = nullptr;
        SdgoStorageGameTestGetSnapshotV1Fn storage_snapshot = nullptr;
        SdgoStorageGameTestActionV1Fn storage_action = nullptr;
    };

    struct SourceReceipt
    {
        uint32_t sequence = 0;
        uint32_t step = kStepNone;
        uint32_t action = 0;
        uint32_t accepted = 0;
        uint32_t done = 0;
        uint32_t action_sequence_before = 0;
        uint32_t action_sequence_after = 0;
        uint32_t reply_sequence_before = 0;
        uint32_t reply_sequence_after = 0;
        uint32_t busy_before = 0;
        uint32_t busy_after = 0;
        uint32_t preview_removed_before = 0;
        uint32_t preview_removed_after = 0;
    };

    struct Observation
    {
        uint32_t result = kUnbound;
        uint32_t stage = kStepNone;
        uint32_t scope = kScopeUnknown;
        uint32_t hangar_available = 0;
        uint32_t storage_available = 0;
        uint32_t before_unit_count = 0;
        uint32_t current_unit_count = 0;
        uint32_t layout_unchanged = 0;
        uint32_t active_slot_unchanged = 0;
        uint32_t gb_preview_removed = 0;
        uint32_t item_preview_removed = 0;
        uint32_t confirm_rejections = 0;
        uint32_t receipt_count = 0;
        SdgoHangarGameTestSnapshotV1 snapshot = {};
        SourceReceipt latest_receipt = {};
    };

    bool Bind()
    {
        Exports exports = {};
        if (HMODULE hangar = GetModuleHandleW(
                L"hangar_manager.dll")) {
            exports.hangar_snapshot =
                reinterpret_cast<
                    SdgoHangarGameTestGetSnapshotV1Fn>(
                    GetProcAddress(
                        hangar,
                        SDGO_HANGAR_GAME_TEST_SNAPSHOT_EXPORT));
            exports.hangar_units =
                reinterpret_cast<
                    SdgoHangarGameTestGetUnitsV1Fn>(
                    GetProcAddress(
                        hangar,
                        SDGO_HANGAR_GAME_TEST_UNITS_EXPORT));
            exports.hangar_action =
                reinterpret_cast<
                    SdgoHangarGameTestActionV1Fn>(
                    GetProcAddress(
                        hangar,
                        SDGO_HANGAR_GAME_TEST_ACTION_EXPORT));
        }
        if (HMODULE storage = GetModuleHandleW(
                L"storage_claim.dll")) {
            exports.storage_snapshot =
                reinterpret_cast<
                    SdgoStorageGameTestGetSnapshotV1Fn>(
                    GetProcAddress(
                        storage,
                        SDGO_STORAGE_GAME_TEST_SNAPSHOT_EXPORT));
            exports.storage_action =
                reinterpret_cast<
                    SdgoStorageGameTestActionV1Fn>(
                    GetProcAddress(
                        storage,
                        SDGO_STORAGE_GAME_TEST_ACTION_EXPORT));
        }
        return Bind(exports);
    }

    // Offline tests inject these exact C ABI functions. A successful Bind means
    // only that resolution completed; availability is reported independently.
    bool Bind(const Exports& exports)
    {
        exports_ = exports;
        hangarAvailable_ =
            exports_.hangar_snapshot &&
            exports_.hangar_units &&
            exports_.hangar_action;
        storageAvailable_ =
            exports_.storage_snapshot &&
            exports_.storage_action;
        bound_ = true;
        ResetRun();
        Publish(
            hangarAvailable_
                ? kWaitingSnapshot
                : kHangarUnavailable);
        return true;
    }

    Result PollHangarRead(
        const SdgoGameTestContextV1* context)
    {
        Result prepared = PrepareRun(context);
        if (IsTerminalFailure(prepared) ||
            prepared == kHangarUnavailable)
            return Publish(prepared);
        if (readReady_)
            return Publish(kReadReady);

        const Result refreshed =
            Refresh(context, false);
        if (refreshed != kReadReady)
            return Publish(refreshed);
        SaveBaseline();
        previewStage_ = kStepOpen;
        PushReceipt(
            context,
            kStepRead,
            SDGO_HANGAR_GAME_TEST_ACTION_NONE,
            true,
            true,
            currentSnapshot_,
            currentSnapshot_);
        readReady_ = true;
        return Publish(kReadReady);
    }

    Result PollHangarPreview(
        const SdgoGameTestContextV1* context)
    {
        Result prepared = PrepareRun(context);
        if (IsTerminalFailure(prepared) ||
            prepared == kHangarUnavailable)
            return Publish(prepared);
        if (!readReady_) {
            const Result read = PollHangarRead(context);
            return Publish(
                read == kReadReady
                    ? kWaitingAction
                    : read);
        }

        switch (previewStage_) {
        case kStepOpen:
            previewStage_ = kWaitOpen;
            return Publish(
                IssueAccepted(
                    context,
                    kStepOpen,
                    SDGO_HANGAR_GAME_TEST_ACTION_OPEN));
        case kWaitOpen:
            return Publish(
                CompleteAccepted(
                    context,
                    kStepOpen,
                    kStepPreviewGb,
                    false));
        case kStepPreviewGb:
            previewStage_ = kWaitPreviewGb;
            return Publish(
                IssueAccepted(
                    context,
                    kStepPreviewGb,
                    SDGO_HANGAR_GAME_TEST_ACTION_GB));
        case kWaitPreviewGb: {
            const Result completed =
                CompleteAccepted(
                    context,
                    kStepPreviewGb,
                    kStepNone,
                    true);
            if (completed != kPreviewStepDone)
                return Publish(completed);
            gbPreviewRemoved_ =
                currentSnapshot_.preview_removed;
            previewStage_ =
                gbPreviewRemoved_ == 0
                    ? kStepRejectConfirmGb
                    : kStepPreviewItem;
            return Publish(kPreviewStepDone);
        }
        case kStepRejectConfirmGb: {
            const Result rejected =
                ExpectRejected(
                    context,
                    kStepRejectConfirmGb,
                    SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM);
            if (rejected == kConfirmRejected) {
                ++confirmRejections_;
                previewStage_ = kStepPreviewItem;
            }
            return Publish(rejected);
        }
        case kStepPreviewItem:
            previewStage_ = kWaitPreviewItem;
            return Publish(
                IssueAccepted(
                    context,
                    kStepPreviewItem,
                    SDGO_HANGAR_GAME_TEST_ACTION_ITEM));
        case kWaitPreviewItem: {
            const Result completed =
                CompleteAccepted(
                    context,
                    kStepPreviewItem,
                    kStepNone,
                    true);
            if (completed != kPreviewStepDone)
                return Publish(completed);
            itemPreviewRemoved_ =
                currentSnapshot_.preview_removed;
            previewStage_ =
                itemPreviewRemoved_ == 0
                    ? kStepRejectConfirmItem
                    : kPreviewComplete;
            return Publish(kPreviewStepDone);
        }
        case kStepRejectConfirmItem: {
            const Result rejected =
                ExpectRejected(
                    context,
                    kStepRejectConfirmItem,
                    SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM);
            if (rejected == kConfirmRejected) {
                ++confirmRejections_;
                previewStage_ = kPreviewComplete;
            }
            return Publish(rejected);
        }
        case kPreviewComplete:
            previewReady_ = true;
            scope_ =
                gbPreviewRemoved_ == 0 &&
                        itemPreviewRemoved_ == 0
                    ? kScopeProtectedOrNoDuplicateNegative
                    : kScopePreviewCandidatesPresent;
            return Publish(kPreviewReady);
        default:
            return Publish(kWaitingAction);
        }
    }

    Result PollHangarCancel(
        const SdgoGameTestContextV1* context)
    {
        Result prepared = PrepareRun(context);
        if (IsTerminalFailure(prepared) ||
            prepared == kHangarUnavailable)
            return Publish(prepared);
        if (!previewReady_)
            return Publish(kCancelStateMissing);

        if (cancelStage_ == kStepNone) {
            cancelStage_ = kWaitCancel;
            const Result issued =
                IssueAccepted(
                    context,
                    kStepCancel,
                    SDGO_HANGAR_GAME_TEST_ACTION_CANCEL);
            return Publish(
                issued == kActionAccepted
                    ? kCancelAccepted
                    : issued);
        }
        if (cancelStage_ == kWaitCancel) {
            const Result completed =
                CompleteAccepted(
                    context,
                    kStepCancel,
                    0,
                    true);
            if (completed != kPreviewStepDone)
                return Publish(completed);
            if (currentSnapshot_.armed)
                return Publish(kCancelStillVisible);
            cancelStage_ = kStepCancelHiddenProbe;
            return Publish(kWaitingAction);
        }
        if (cancelStage_ ==
            kStepCancelHiddenProbe) {
            const Result hidden =
                ExpectRejected(
                    context,
                    kStepCancelHiddenProbe,
                    SDGO_HANGAR_GAME_TEST_ACTION_CANCEL);
            if (hidden != kConfirmRejected)
                return Publish(
                    hidden == kUnsafeConfirmAccepted
                        ? kCancelStillVisible
                        : hidden);
            cancelStage_ = kCancelComplete;
            previewReady_ = false;
            return Publish(kCancelled);
        }
        return Publish(
            cancelStage_ == kCancelComplete
                ? kCancelled
                : kWaitingAction);
    }

    Result PollStorageBinding(
        const SdgoGameTestContextV1* context)
    {
        if (!bound_)
            return Publish(kUnbound);
        const uint64_t hash =
            ContextRunHash(context);
        if (!hash)
            return Publish(kInvalidContext);
        if (!storageAvailable_)
            return Publish(kStorageUnavailable);
        if (!storageReceiptPublished_) {
            SdgoHangarGameTestSnapshotV1 empty = {};
            PushReceipt(
                context,
                kStepStorageBinding,
                SDGO_STORAGE_GAME_TEST_ACTION_NONE,
                true,
                true,
                empty,
                empty);
            storageReceiptPublished_ = true;
        }
        return Publish(kStorageReady);
    }

    Observation State() const
    {
        Observation observed = {};
        observed.result =
            static_cast<uint32_t>(lastResult_);
        observed.stage =
            cancelStage_ != kStepNone
                ? cancelStage_
                : static_cast<uint32_t>(
                      previewReady_
                          ? kPreviewComplete
                          : previewStage_);
        observed.scope =
            static_cast<uint32_t>(scope_);
        observed.hangar_available =
            hangarAvailable_ ? 1u : 0u;
        observed.storage_available =
            storageAvailable_ ? 1u : 0u;
        observed.before_unit_count =
            static_cast<uint32_t>(
                beforeUnits_.size());
        observed.current_unit_count =
            static_cast<uint32_t>(
                currentUnits_.size());
        observed.layout_unchanged =
            hasBaseline_ &&
                    SameUnitLayout(
                        beforeUnits_,
                        currentUnits_)
                ? 1u
                : 0u;
        observed.active_slot_unchanged =
            hasBaseline_ &&
                    beforeSnapshot_.active_slot ==
                        currentSnapshot_.active_slot
                ? 1u
                : 0u;
        observed.gb_preview_removed =
            gbPreviewRemoved_;
        observed.item_preview_removed =
            itemPreviewRemoved_;
        observed.confirm_rejections =
            confirmRejections_;
        observed.receipt_count =
            static_cast<uint32_t>(
                receipts_.size());
        observed.snapshot = currentSnapshot_;
        if (!receipts_.empty())
            observed.latest_receipt =
                receipts_.back();
        return observed;
    }

    bool HangarAvailable() const
    {
        return bound_ && hangarAvailable_;
    }

    bool StorageAvailable() const
    {
        return bound_ && storageAvailable_;
    }

    const std::vector<
        SdgoHangarGameTestUnitV1>&
    BeforeUnits() const
    {
        return beforeUnits_;
    }

    const std::vector<
        SdgoHangarGameTestUnitV1>&
    CurrentUnits() const
    {
        return currentUnits_;
    }

    const std::vector<SourceReceipt>&
    Receipts() const
    {
        return receipts_;
    }

    static const char* ResultName(Result result)
    {
        switch (result) {
        case kUnbound:
            return "unbound";
        case kHangarUnavailable:
            return "hangar-unavailable";
        case kStorageUnavailable:
            return "storage-unavailable";
        case kWaitingSnapshot:
            return "waiting-snapshot";
        case kReadReady:
            return "read-ready";
        case kActionAccepted:
            return "action-accepted";
        case kWaitingAction:
            return "waiting-action";
        case kPreviewStepDone:
            return "preview-step-done";
        case kConfirmRejected:
            return "confirm-rejected";
        case kPreviewReady:
            return "preview-ready";
        case kCancelAccepted:
            return "cancel-accepted";
        case kCancelled:
            return "cancelled";
        case kStorageReady:
            return "storage-ready";
        case kInvalidContext:
            return "invalid-context";
        case kSnapshotCallFailed:
            return "snapshot-call-failed";
        case kProcessMismatch:
            return "process-mismatch";
        case kThreadMismatch:
            return "thread-mismatch";
        case kRunMismatch:
            return "run-mismatch";
        case kUnitPageMismatch:
            return "unit-page-mismatch";
        case kSnapshotChanged:
            return "snapshot-changed";
        case kInventoryChanged:
            return "inventory-changed";
        case kActionRejected:
            return "action-rejected";
        case kActionSequenceMismatch:
            return "action-sequence-mismatch";
        case kUnsafeConfirmAccepted:
            return "unsafe-confirm-accepted";
        case kCancelStateMissing:
            return "cancel-state-missing";
        case kCancelStillVisible:
            return "cancel-still-visible";
        default:
            return "unknown";
        }
    }

    static const char* StepName(Step step)
    {
        switch (step) {
        case kStepRead:
            return "read";
        case kStepOpen:
            return "open";
        case kStepPreviewGb:
            return "preview-gb";
        case kStepRejectConfirmGb:
            return "reject-confirm-gb";
        case kStepPreviewItem:
            return "preview-item";
        case kStepRejectConfirmItem:
            return "reject-confirm-item";
        case kStepCancel:
            return "cancel";
        case kStepCancelHiddenProbe:
            return "cancel-hidden-probe";
        case kStepStorageBinding:
            return "storage-binding";
        default:
            return "none";
        }
    }

    static const char* ScopeName(Scope scope)
    {
        switch (scope) {
        case kScopeProtectedOrNoDuplicateNegative:
            return "protected/no-duplicate-negative";
        case kScopePreviewCandidatesPresent:
            return "preview-candidates-present";
        default:
            return "unknown";
        }
    }

private:
    static constexpr uint32_t kWaitOpen = 0x1000;
    static constexpr uint32_t kWaitPreviewGb = 0x1001;
    static constexpr uint32_t kWaitPreviewItem = 0x1002;
    static constexpr uint32_t kPreviewComplete = 0x1003;
    static constexpr uint32_t kWaitCancel = 0x1004;
    static constexpr uint32_t kCancelComplete = 0x1005;

    static bool IsTerminalFailure(Result result)
    {
        return result >= kInvalidContext;
    }

    Result Publish(Result result)
    {
        lastResult_ = result;
        return result;
    }

    uint64_t ContextRunHash(
        const SdgoGameTestContextV1* context) const
    {
        if (!context ||
            context->size <
                sizeof(SdgoGameTestContextV1) ||
            context->version !=
                SDGO_GAME_TEST_VERSION ||
            !context->run_id ||
            !context->role)
            return 0;
        return sdgo::game_test_page_observer::
            ClientRunHash(
                context->run_id,
                std::strlen(context->run_id),
                context->role,
                std::strlen(context->role));
    }

    Result PrepareRun(
        const SdgoGameTestContextV1* context)
    {
        if (!bound_)
            return kUnbound;
        if (!hangarAvailable_)
            return kHangarUnavailable;
        const uint64_t hash =
            ContextRunHash(context);
        if (!hash)
            return kInvalidContext;
        if (runHash_ && runHash_ != hash)
            ResetRun();
        runHash_ = hash;
        return kWaitingSnapshot;
    }

    void ResetRun()
    {
        runHash_ = 0;
        readReady_ = false;
        hasBaseline_ = false;
        previewReady_ = false;
        storageReceiptPublished_ = false;
        previewStage_ = kStepNone;
        cancelStage_ = kStepNone;
        pendingStep_ = kStepNone;
        pendingActionSequence_ = 0;
        gbPreviewRemoved_ = 0;
        itemPreviewRemoved_ = 0;
        confirmRejections_ = 0;
        receiptSequence_ = 0;
        scope_ = kScopeUnknown;
        beforeSnapshot_ = {};
        currentSnapshot_ = {};
        pendingBefore_ = {};
        beforeUnits_.clear();
        currentUnits_.clear();
        receipts_.clear();
    }

    Result Refresh(
        const SdgoGameTestContextV1* context,
        bool compareBaseline)
    {
        if (!hangarAvailable_)
            return kHangarUnavailable;
        const uint64_t expected =
            ContextRunHash(context);
        if (!expected || expected != runHash_)
            return kInvalidContext;

        SdgoHangarGameTestSnapshotV1 snapshot = {};
        snapshot.size = sizeof(snapshot);
        snapshot.version =
            SDGO_GAME_TEST_PAGE_OBSERVER_VERSION;
        if (!exports_.hangar_snapshot(&snapshot))
            return kSnapshotCallFailed;
        currentSnapshot_ = snapshot;

        if (!snapshot.loaded ||
            !snapshot.visible ||
            !snapshot.session)
            return kWaitingSnapshot;
        if (snapshot.process_id !=
            GetCurrentProcessId())
            return kProcessMismatch;
        if (snapshot.ui_thread_id !=
            GetCurrentThreadId())
            return kThreadMismatch;
        if (snapshot.run_hash != expected)
            return kRunMismatch;
        if (!snapshot.unit_count ||
            snapshot.unit_count >
                snapshot.capacity)
            return kUnitPageMismatch;

        std::vector<
            SdgoHangarGameTestUnitV1>
            units(snapshot.unit_count);
        const int read =
            exports_.hangar_units(
                expected,
                0,
                units.data(),
                sizeof(
                    SdgoHangarGameTestUnitV1),
                snapshot.unit_count);
        if (read < 0 ||
            static_cast<uint32_t>(read) !=
                snapshot.unit_count)
            return kUnitPageMismatch;
        currentUnits_ = std::move(units);

        if (compareBaseline && hasBaseline_) {
            if (snapshot.session !=
                    beforeSnapshot_.session ||
                snapshot.generation !=
                    beforeSnapshot_.generation)
                return kSnapshotChanged;
            if (!SameUnitLayout(
                    beforeUnits_,
                    currentUnits_) ||
                snapshot.active_slot !=
                    beforeSnapshot_.active_slot)
                return kInventoryChanged;
        }
        return kReadReady;
    }

    void SaveBaseline()
    {
        beforeSnapshot_ = currentSnapshot_;
        beforeUnits_ = currentUnits_;
        hasBaseline_ = true;
    }

    static bool SameUnitLayout(
        const std::vector<
            SdgoHangarGameTestUnitV1>& left,
        const std::vector<
            SdgoHangarGameTestUnitV1>& right)
    {
        if (left.size() != right.size())
            return false;
        for (size_t i = 0;
             i < left.size();
             ++i) {
            if (left[i].uid != right[i].uid ||
                left[i].unit_id !=
                    right[i].unit_id ||
                left[i].slot != right[i].slot)
                return false;
        }
        return true;
    }

    Result IssueAccepted(
        const SdgoGameTestContextV1* context,
        Step step,
        uint32_t action)
    {
        const Result before =
            Refresh(context, true);
        if (before != kReadReady)
            return before;
        const auto source = currentSnapshot_;
        const int accepted =
            exports_.hangar_action(
                runHash_,
                action);
        const Result after =
            Refresh(context, true);
        if (after != kReadReady)
            return after;
        PushReceipt(
            context,
            step,
            action,
            accepted != 0,
            false,
            source,
            currentSnapshot_);
        if (!accepted)
            return kActionRejected;
        if (currentSnapshot_.action_sequence !=
                source.action_sequence + 1 ||
            currentSnapshot_.last_action != action)
            return kActionSequenceMismatch;
        pendingStep_ = step;
        pendingBefore_ = source;
        pendingActionSequence_ =
            source.action_sequence + 1;
        return kActionAccepted;
    }

    Result CompleteAccepted(
        const SdgoGameTestContextV1* context,
        Step step,
        uint32_t nextStage,
        bool waitUntilIdle)
    {
        const Result refreshed =
            Refresh(context, true);
        if (refreshed != kReadReady)
            return refreshed;
        if (pendingStep_ != step)
            return kActionSequenceMismatch;
        if (currentSnapshot_.action_sequence <
                pendingActionSequence_ ||
            (waitUntilIdle &&
             currentSnapshot_.busy))
            return kWaitingAction;
        if (currentSnapshot_.action_sequence !=
            pendingActionSequence_)
            return kActionSequenceMismatch;
        PushReceipt(
            context,
            step,
            currentSnapshot_.last_action,
            true,
            true,
            pendingBefore_,
            currentSnapshot_);
        pendingStep_ = kStepNone;
        pendingActionSequence_ = 0;
        if (nextStage)
            previewStage_ = nextStage;
        return kPreviewStepDone;
    }

    Result ExpectRejected(
        const SdgoGameTestContextV1* context,
        Step step,
        uint32_t action)
    {
        const Result before =
            Refresh(context, true);
        if (before != kReadReady)
            return before;
        const auto source = currentSnapshot_;
        if (action ==
                SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM &&
            source.preview_removed != 0)
            return kSnapshotChanged;
        const int accepted =
            exports_.hangar_action(
                runHash_,
                action);
        const Result after =
            Refresh(context, true);
        if (after != kReadReady)
            return after;
        PushReceipt(
            context,
            step,
            action,
            accepted != 0,
            true,
            source,
            currentSnapshot_);
        if (accepted)
            return kUnsafeConfirmAccepted;
        if (currentSnapshot_.action_sequence !=
            source.action_sequence)
            return kActionSequenceMismatch;
        return kConfirmRejected;
    }

    void PushReceipt(
        const SdgoGameTestContextV1* context,
        Step step,
        uint32_t action,
        bool accepted,
        bool done,
        const SdgoHangarGameTestSnapshotV1& before,
        const SdgoHangarGameTestSnapshotV1& after)
    {
        SourceReceipt receipt = {};
        receipt.sequence =
            ++receiptSequence_;
        receipt.step =
            static_cast<uint32_t>(step);
        receipt.action = action;
        receipt.accepted =
            accepted ? 1u : 0u;
        receipt.done = done ? 1u : 0u;
        receipt.action_sequence_before =
            before.action_sequence;
        receipt.action_sequence_after =
            after.action_sequence;
        receipt.reply_sequence_before =
            before.reply_sequence;
        receipt.reply_sequence_after =
            after.reply_sequence;
        receipt.busy_before =
            before.busy;
        receipt.busy_after =
            after.busy;
        receipt.preview_removed_before =
            before.preview_removed;
        receipt.preview_removed_after =
            after.preview_removed;
        receipts_.push_back(receipt);

        if (context && context->Observe) {
            char message[320] = {};
            std::snprintf(
                message,
                sizeof(message),
                "page-source step=%s accepted=%u done=%u action=%u action_seq=%u->%u reply_seq=%u->%u busy=%u->%u preview_removed=%u->%u",
                StepName(step),
                receipt.accepted,
                receipt.done,
                receipt.action,
                receipt.action_sequence_before,
                receipt.action_sequence_after,
                receipt.reply_sequence_before,
                receipt.reply_sequence_after,
                receipt.busy_before,
                receipt.busy_after,
                receipt.preview_removed_before,
                receipt.preview_removed_after);
            context->Observe(message);
        }
    }

    Exports exports_ = {};
    bool bound_ = false;
    bool hangarAvailable_ = false;
    bool storageAvailable_ = false;
    bool readReady_ = false;
    bool hasBaseline_ = false;
    bool previewReady_ = false;
    bool storageReceiptPublished_ = false;
    uint64_t runHash_ = 0;
    uint32_t previewStage_ = kStepNone;
    uint32_t cancelStage_ = kStepNone;
    Step pendingStep_ = kStepNone;
    uint32_t pendingActionSequence_ = 0;
    uint32_t gbPreviewRemoved_ = 0;
    uint32_t itemPreviewRemoved_ = 0;
    uint32_t confirmRejections_ = 0;
    uint32_t receiptSequence_ = 0;
    Scope scope_ = kScopeUnknown;
    Result lastResult_ = kUnbound;
    SdgoHangarGameTestSnapshotV1 beforeSnapshot_ = {};
    SdgoHangarGameTestSnapshotV1 currentSnapshot_ = {};
    SdgoHangarGameTestSnapshotV1 pendingBefore_ = {};
    std::vector<SdgoHangarGameTestUnitV1> beforeUnits_;
    std::vector<SdgoHangarGameTestUnitV1> currentUnits_;
    std::vector<SourceReceipt> receipts_;
};
