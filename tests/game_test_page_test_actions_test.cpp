// OFFLINE-ONLY page adapter state-machine tests.
//
// The injected functions model only the public observer/action ABI. This test
// never starts GOnline, loads a Mod, sends a packet, writes UI/game data or
// claims gameplay evidence.

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mods/game_test_client/page_test_actions.h"

namespace
{
int g_failures = 0;
constexpr const char* kRunId =
    "12345678-1234-1234-1234-123456789abc";
uint64_t g_runHash = 0;
SdgoHangarGameTestSnapshotV1 g_snapshot = {};
std::vector<SdgoHangarGameTestUnitV1> g_units;
std::vector<std::string> g_messages;
std::array<uint32_t, 7> g_actionCalls = {};
uint32_t g_gbRemoved = 0;
uint32_t g_itemRemoved = 0;
bool g_exchange = false;
bool g_itemPending = false;
bool g_snapshotCallFails = false;
bool g_unitPageMismatch = false;
bool g_rejectOpen = false;
bool g_acceptEmptyConfirm = false;
uint32_t g_storageSnapshotCalls = 0;
uint32_t g_storageActionCalls = 0;

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void __cdecl Observe(const char* message)
{
    g_messages.emplace_back(
        message ? message : "");
}

void __cdecl AssertUnused(int, const char*)
{
}

SdgoGameTestContextV1 Context(
    const char* role = "client")
{
    SdgoGameTestContextV1 context = {};
    context.size = sizeof(context);
    context.version = SDGO_GAME_TEST_VERSION;
    context.run_id = kRunId;
    context.role = role;
    context.case_id = "module.hangar.fixture";
    context.Assert = &AssertUnused;
    context.Observe = &Observe;
    return context;
}

SdgoHangarGameTestUnitV1 Unit(
    uint64_t uid,
    uint32_t unit,
    uint16_t slot,
    uint32_t protection = 0)
{
    SdgoHangarGameTestUnitV1 value = {};
    value.uid = uid;
    value.unit_id = unit;
    value.slot = slot;
    value.level = 1;
    value.flags =
        SDGO_HANGAR_GAME_TEST_UNIT_FULL_READY;
    value.protection_flags = protection;
    return value;
}

void ResetFixture(
    uint32_t gbRemoved = 0,
    uint32_t itemRemoved = 0)
{
    g_runHash =
        sdgo::game_test_page_observer::
            ClientRunHash(
                kRunId,
                std::strlen(kRunId),
                "client",
                6);
    g_units = {
        Unit(101, 10, 1),
        Unit(102, 20, 3),
        Unit(
            103,
            30,
            5,
            SDGO_HANGAR_GAME_TEST_PROTECTED_ACTIVE),
    };
    g_snapshot = {};
    g_snapshot.size = sizeof(g_snapshot);
    g_snapshot.version =
        SDGO_GAME_TEST_PAGE_OBSERVER_VERSION;
    g_snapshot.run_hash = g_runHash;
    g_snapshot.session = 0x1122334455667788ULL;
    g_snapshot.process_id =
        GetCurrentProcessId();
    g_snapshot.ui_thread_id =
        GetCurrentThreadId();
    g_snapshot.loaded = 1;
    g_snapshot.visible = 1;
    g_snapshot.generation = 7;
    g_snapshot.capacity = 120;
    g_snapshot.active_slot = 5;
    g_snapshot.unit_count =
        static_cast<uint32_t>(g_units.size());
    g_snapshot.runner_state =
        SDGO_HANGAR_GAME_TEST_RUNNER_IDLE;
    g_messages.clear();
    g_actionCalls.fill(0);
    g_gbRemoved = gbRemoved;
    g_itemRemoved = itemRemoved;
    g_exchange = false;
    g_itemPending = false;
    g_snapshotCallFails = false;
    g_unitPageMismatch = false;
    g_rejectOpen = false;
    g_acceptEmptyConfirm = false;
    g_storageSnapshotCalls = 0;
    g_storageActionCalls = 0;
}

int __cdecl FakeHangarSnapshot(
    SdgoHangarGameTestSnapshotV1* output)
{
    if (g_snapshotCallFails ||
        !output ||
        output->size < sizeof(*output) ||
        output->version !=
            SDGO_GAME_TEST_PAGE_OBSERVER_VERSION)
        return 0;
    *output = g_snapshot;
    return 1;
}

int __cdecl FakeHangarUnits(
    uint64_t runHash,
    uint32_t offset,
    SdgoHangarGameTestUnitV1* output,
    uint32_t elementSize,
    uint32_t capacity)
{
    if (runHash != g_runHash ||
        !output ||
        elementSize != sizeof(*output) ||
        !capacity ||
        offset >= g_units.size())
        return 0;
    const uint32_t count =
        static_cast<uint32_t>(
            std::min<size_t>(
                capacity,
                g_units.size() - offset));
    for (uint32_t i = 0; i < count; ++i)
        output[i] = g_units[offset + i];
    return g_unitPageMismatch
        ? static_cast<int>(count - 1)
        : static_cast<int>(count);
}

int __cdecl FakeHangarAction(
    uint64_t runHash,
    uint32_t action)
{
    if (runHash != g_runHash ||
        action >= g_actionCalls.size())
        return 0;
    ++g_actionCalls[action];
    switch (action) {
    case SDGO_HANGAR_GAME_TEST_ACTION_OPEN:
        if (g_exchange || g_rejectOpen)
            return 0;
        g_exchange = true;
        break;
    case SDGO_HANGAR_GAME_TEST_ACTION_GB:
        if (!g_exchange)
            return 0;
        g_snapshot.preview_removed =
            g_gbRemoved;
        g_snapshot.busy = 0;
        break;
    case SDGO_HANGAR_GAME_TEST_ACTION_ITEM:
        if (!g_exchange)
            return 0;
        g_itemPending = true;
        g_snapshot.busy = 1;
        break;
    case SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM:
        if (!g_exchange ||
            (!g_snapshot.preview_removed &&
             !g_acceptEmptyConfirm))
            return 0;
        break;
    case SDGO_HANGAR_GAME_TEST_ACTION_CANCEL:
        if (!g_exchange)
            return 0;
        g_exchange = false;
        g_snapshot.busy = 0;
        g_snapshot.armed = 0;
        break;
    default:
        return 0;
    }
    ++g_snapshot.action_sequence;
    g_snapshot.last_action = action;
    return 1;
}

int __cdecl FakeStorageSnapshot(
    SdgoStorageGameTestSnapshotV1*)
{
    ++g_storageSnapshotCalls;
    return 0;
}

int __cdecl FakeStorageAction(
    uint64_t,
    uint32_t)
{
    ++g_storageActionCalls;
    return 0;
}

void CompleteItemPreview()
{
    Check(
        g_itemPending,
        "item preview completion follows an accepted item action");
    g_itemPending = false;
    g_snapshot.busy = 0;
    g_snapshot.preview_removed =
        g_itemRemoved;
    ++g_snapshot.reply_sequence;
}

PageTestActions::Exports AllExports()
{
    PageTestActions::Exports exports = {};
    exports.hangar_snapshot =
        &FakeHangarSnapshot;
    exports.hangar_units =
        &FakeHangarUnits;
    exports.hangar_action =
        &FakeHangarAction;
    exports.storage_snapshot =
        &FakeStorageSnapshot;
    exports.storage_action =
        &FakeStorageAction;
    return exports;
}

void RunToItemAccepted(
    PageTestActions& pages,
    const SdgoGameTestContextV1& context,
    bool expectGbConfirmRejection)
{
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kActionAccepted,
        "preview opens through the exported native action");
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kPreviewStepDone,
        "open acceptance receives a separate done receipt");
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kActionAccepted,
        "GB preview action is accepted separately");
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kPreviewStepDone,
        "GB preview completion is observed separately");
    if (expectGbConfirmRejection) {
        Check(
            pages.PollHangarPreview(&context) ==
                PageTestActions::kConfirmRejected,
            "zero-removal GB preview proves native Confirm is disabled");
    }
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kActionAccepted,
        "item preview action is accepted without invoking exchange");
}

void TestMissingAndIdentityFailures()
{
    ResetFixture();
    auto context = Context();

    PageTestActions missing;
    missing.Bind(PageTestActions::Exports{});
    Check(
        missing.PollHangarRead(&context) ==
            PageTestActions::kHangarUnavailable,
        "missing hangar ABI is unavailable rather than pass");
    Check(
        missing.PollStorageBinding(&context) ==
            PageTestActions::kStorageUnavailable,
        "missing storage ABI is unavailable rather than pass");

    PageTestActions pages;
    pages.Bind(AllExports());
    auto server = Context("server");
    Check(
        pages.PollHangarRead(&server) ==
            PageTestActions::kInvalidContext,
        "non-client context cannot bind page actions");

    g_snapshot.loaded = 0;
    Check(
        pages.PollHangarRead(&context) ==
            PageTestActions::kWaitingSnapshot,
        "not-yet-loaded native page remains waiting");
    g_snapshot.loaded = 1;

    g_snapshot.process_id ^= 1;
    Check(
        pages.PollHangarRead(&context) ==
            PageTestActions::kProcessMismatch,
        "snapshot PID must match the actual GameTest process");
    g_snapshot.process_id =
        GetCurrentProcessId();
    g_snapshot.ui_thread_id ^= 1;
    Check(
        pages.PollHangarRead(&context) ==
            PageTestActions::kThreadMismatch,
        "snapshot UI thread must match the polling main thread");
    g_snapshot.ui_thread_id =
        GetCurrentThreadId();
    g_snapshot.run_hash ^= 1;
    Check(
        pages.PollHangarRead(&context) ==
            PageTestActions::kRunMismatch,
        "snapshot run hash must match the current case");
    g_snapshot.run_hash = g_runHash;
    g_unitPageMismatch = true;
    Check(
        pages.PollHangarRead(&context) ==
            PageTestActions::kUnitPageMismatch,
        "unit page count must exactly match snapshot unit count");
}

void TestNegativePreviewAndCancel()
{
    ResetFixture(0, 0);
    auto context = Context();
    PageTestActions pages;
    pages.Bind(AllExports());

    Check(
        pages.PollStorageBinding(&context) ==
            PageTestActions::kStorageReady,
        "storage readiness checks bindings only");
    Check(
        !g_storageSnapshotCalls &&
            !g_storageActionCalls,
        "storage readiness performs no navigation or action");
    Check(
        pages.PollHangarRead(&context) ==
            PageTestActions::kReadReady,
        "hangar read captures a real ABI snapshot");
    Check(
        pages.BeforeUnits().size() == 3 &&
            pages.BeforeUnits()[0].uid == 101 &&
            pages.BeforeUnits()[0].unit_id == 10 &&
            pages.BeforeUnits()[0].slot == 1,
        "read preserves exact unit UID, ID and slot records");

    RunToItemAccepted(pages, context, true);
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kWaitingAction,
        "item preview waits while the real page reports busy");
    CompleteItemPreview();
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kPreviewStepDone,
        "item preview completes only after busy clears");
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kConfirmRejected,
        "zero-removal item preview proves native Confirm is disabled");
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kPreviewReady,
        "negative preview reaches reviewable ready state");

    const auto preview = pages.State();
    Check(
        preview.scope ==
                PageTestActions::
                    kScopeProtectedOrNoDuplicateNegative &&
            preview.gb_preview_removed == 0 &&
            preview.item_preview_removed == 0 &&
            preview.confirm_rejections == 2,
        "negative scope is limited to protected or no-duplicate state");
    Check(
        g_actionCalls[
            SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM] == 2 &&
            g_snapshot.action_sequence == 3,
        "rejected Confirm probes never increment action sequence");

    const auto& receipts = pages.Receipts();
    const auto openAccepted =
        std::find_if(
            receipts.begin(),
            receipts.end(),
            [](const PageTestActions::SourceReceipt& receipt) {
                return receipt.step ==
                           PageTestActions::kStepOpen &&
                       receipt.accepted &&
                       !receipt.done;
            });
    const auto openDone =
        std::find_if(
            receipts.begin(),
            receipts.end(),
            [](const PageTestActions::SourceReceipt& receipt) {
                return receipt.step ==
                           PageTestActions::kStepOpen &&
                       receipt.accepted &&
                       receipt.done;
            });
    Check(
        openAccepted != receipts.end() &&
            openDone != receipts.end() &&
            openAccepted->sequence <
                openDone->sequence,
        "accepted and done are separate ordered source receipts");

    Check(
        pages.PollHangarCancel(&context) ==
            PageTestActions::kCancelAccepted,
        "cancel enters the real action export");
    Check(
        pages.PollHangarCancel(&context) ==
            PageTestActions::kWaitingAction,
        "cancel completion is observed separately");
    Check(
        pages.PollHangarCancel(&context) ==
            PageTestActions::kCancelled,
        "hidden Cancel probe confirms the preview page closed");
    const auto cancelled = pages.State();
    Check(
        cancelled.layout_unchanged &&
            cancelled.active_slot_unchanged &&
            pages.CurrentUnits()[0].uid == 101 &&
            pages.CurrentUnits()[0].slot == 1,
        "cancel preserves exact units and active slot");
    Check(
        !g_messages.empty(),
        "each source transition is published through context Observe");
}

void TestCandidatePreviewNeverConfirms()
{
    ResetFixture(2, 1);
    auto context = Context();
    PageTestActions pages;
    pages.Bind(AllExports());
    Check(
        pages.PollHangarRead(&context) ==
            PageTestActions::kReadReady,
        "candidate case captures baseline");
    RunToItemAccepted(pages, context, false);
    CompleteItemPreview();
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kPreviewStepDone,
        "candidate item preview completes without exchange");
    Check(
        pages.PollHangarPreview(&context) ==
            PageTestActions::kPreviewReady,
        "candidate preview stops for review");
    const auto state = pages.State();
    Check(
        state.scope ==
                PageTestActions::
                    kScopePreviewCandidatesPresent &&
            state.gb_preview_removed == 2 &&
            state.item_preview_removed == 1,
        "candidate scope records actual preview counts");
    Check(
        !g_actionCalls[
            SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM] &&
            !g_actionCalls[
                SDGO_HANGAR_GAME_TEST_ACTION_SORT],
        "eligible previews never call Confirm or Sort");
    Check(
        pages.PollHangarCancel(&context) ==
                PageTestActions::kCancelAccepted &&
            pages.PollHangarCancel(&context) ==
                PageTestActions::kWaitingAction &&
            pages.PollHangarCancel(&context) ==
                PageTestActions::kCancelled,
        "candidate preview is cancelled without exchange");
    Check(
        pages.State().layout_unchanged &&
            pages.State().active_slot_unchanged,
        "candidate cancellation preserves unit layout and active slot");
}

void TestUnsafeAndDriftFailures()
{
    ResetFixture();
    auto context = Context();
    PageTestActions rejected;
    rejected.Bind(AllExports());
    Check(
        rejected.PollHangarRead(&context) ==
            PageTestActions::kReadReady,
        "rejected-action case captures baseline");
    g_rejectOpen = true;
    Check(
        rejected.PollHangarPreview(&context) ==
            PageTestActions::kActionRejected,
        "expected page action rejection is a failure, not wait or pass");

    ResetFixture();
    PageTestActions unsafe;
    unsafe.Bind(AllExports());
    Check(
        unsafe.PollHangarRead(&context) ==
            PageTestActions::kReadReady,
        "unsafe-confirm case captures baseline");
    RunToItemAccepted(unsafe, context, true);
    CompleteItemPreview();
    Check(
        unsafe.PollHangarPreview(&context) ==
            PageTestActions::kPreviewStepDone,
        "unsafe-confirm fixture reaches item preview");
    g_acceptEmptyConfirm = true;
    Check(
        unsafe.PollHangarPreview(&context) ==
            PageTestActions::kUnsafeConfirmAccepted,
        "accepted zero-removal Confirm fails closed");

    ResetFixture();
    PageTestActions drift;
    drift.Bind(AllExports());
    Check(
        drift.PollHangarRead(&context) ==
            PageTestActions::kReadReady,
        "drift case captures baseline");
    Check(
        drift.PollHangarPreview(&context) ==
            PageTestActions::kActionAccepted,
        "drift case accepts Open");
    g_units[0].slot = 2;
    Check(
        drift.PollHangarPreview(&context) ==
            PageTestActions::kInventoryChanged,
        "unit UID/ID/slot drift aborts the page scenario");
}
}

int main()
{
    static_assert(
        sizeof(void*) == 4,
        "page GameTest fixture requires Win32 ABI");
    TestMissingAndIdentityFailures();
    TestNegativePreviewAndCancel();
    TestCandidatePreviewNeverConfirms();
    TestUnsafeAndDriftFailures();
    if (g_failures) {
        std::fprintf(
            stderr,
            "%d page action checks failed\n",
            g_failures);
        return 1;
    }
    std::printf(
        "PAGE TEST ACTIONS OFFLINE FIXTURE PASSED: ABI binding, identity gates, read/preview/disabled-confirm/cancel receipts, candidate safety and storage binding-only scope\n");
    return 0;
}
