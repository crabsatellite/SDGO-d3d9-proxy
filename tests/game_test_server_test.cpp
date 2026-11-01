// OFFLINE unit checks for the passive GameTest server observer.
//
// This includes the production implementation in the same translation unit so
// its lifecycle, correlation, filtering and bounded queue logic can be checked
// without a BattleServer process. Passing these checks is not evidence of real
// authentication, room creation, PvP AI setup, battle ticks or server runtime.

#include "../mods/game_test_server/game_test_server.cpp"

#include <cstdio>
#include <cstring>

namespace
{
    unsigned g_checks = 0;
    unsigned g_failures = 0;

    void Check(bool passed, const char* expression, int line)
    {
        ++g_checks;
        if (passed)
            return;
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }

#define CHECK(expression) Check((expression), #expression, __LINE__)

    SdgoPvpAiObserverEventV1 Event(
        uint32_t type,
        uint32_t roomCookie = 0x10203040u,
        uint32_t hostPilotId = 9229,
        uint8_t mode = 3,
        const char* title = "!ai e2 T7x")
    {
        SdgoPvpAiObserverEventV1 event = {};
        event.size = sizeof(event);
        event.version = SDGO_PVP_AI_OBSERVER_VERSION;
        event.type = type;
        event.room_cookie = roomCookie;
        event.host_pilot_id = hostPilotId;
        event.mode = mode;
        event.ai_team_1 = 1;
        event.ai_team_2 = 1;
        event.active_slots = 3;
        if (title)
            strncpy_s(event.title, title, _TRUNCATE);
        return event;
    }

    void ResetConfig(
        const char* nonce = "T7x",
        uint32_t expectedHost = 9229,
        uint8_t mode = 3,
        uint8_t minimumAi = 1,
        bool requireEnd = false)
    {
        g_config = RunConfig{};
        strcpy_s(g_config.nonce, nonce);
        g_config.mode = mode;
        g_config.minimumAi = minimumAi;
        g_config.requireEnd = requireEnd;
        InterlockedExchange(&g_expectedHost, static_cast<LONG>(expectedHost));
    }

    void ResetQueue()
    {
        EnterCriticalSection(&g_queueLock);
        memset(g_queue, 0, sizeof(g_queue));
        g_queueRead = 0;
        g_queueWrite = 0;
        g_queueCount = 0;
        LeaveCriticalSection(&g_queueLock);
        InterlockedExchange(&g_dropped, 0);
    }

    void TestAbiAndValidation()
    {
        CHECK(sizeof(SdgoPvpAiObserverSlotV1) ==
              SDGO_PVP_AI_OBSERVER_SLOT_V1_SIZE);
        CHECK(sizeof(SdgoPvpAiObserverEventV1) ==
              SDGO_PVP_AI_OBSERVER_EVENT_V1_SIZE);
        CHECK(offsetof(SdgoPvpAiObserverEventV1, sequence) == 16);
        CHECK(offsetof(SdgoPvpAiObserverEventV1, title) == 60);
        CHECK(offsetof(SdgoPvpAiObserverEventV1, slots) == 84);

        CHECK(IsUuid("01234567-89ab-cdef-0123-456789abcdef"));
        CHECK(!IsUuid("01234567-89AB-cdef-0123-456789abcdef"));
        CHECK(!IsUuid("0123456789ab-cdef-0123-456789abcdef"));
        CHECK(!IsUuid("01234567-89ab-cdef-0123-456789abcdeg"));

        ResetConfig();
        CHECK(TitleContainsNonce("prefix-T7x"));
        CHECK(!TitleContainsNonce("prefix-t7x"));
        CHECK(!TitleContainsNonce("no marker"));
        CHECK(std::string(EventName(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE)) ==
              "battle_active");
        CHECK(std::string(EventName(0xffffffffu)) == "unknown");
        CHECK(EscapeJson("\"\\\n\t") == "\\\"\\\\\\n\\t");
    }

    void TestObserverFiltering()
    {
        ResetConfig();
        ResetQueue();

        SdgoPvpAiObserverEventV1 event =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED);

        SdgoPvpAiObserverEventV1 invalid = event;
        invalid.size = sizeof(invalid) - 1;
        ObservePvpAi(&invalid, nullptr);
        invalid = event;
        invalid.version = SDGO_PVP_AI_OBSERVER_VERSION + 1;
        ObservePvpAi(&invalid, nullptr);
        invalid = event;
        invalid.host_pilot_id = event.host_pilot_id + 1;
        ObservePvpAi(&invalid, nullptr);
        invalid = event;
        invalid.mode = static_cast<uint8_t>(event.mode + 1);
        ObservePvpAi(&invalid, nullptr);
        invalid = event;
        strcpy_s(invalid.title, "!ai e2 wrong");
        ObservePvpAi(&invalid, nullptr);
        CHECK(g_queueCount == 0);

        ObservePvpAi(&event, nullptr);
        CHECK(g_queueCount == 1);
        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        CHECK(Drain(output) == 1);
        CHECK(output[0].host_pilot_id == 9229);
        CHECK(output[0].room_cookie == 0x10203040u);
        CHECK(g_queueCount == 0);
    }

    void TestObserverWildcardAndDisabledHost()
    {
        ResetConfig("T7x", 0, 0);
        ResetQueue();

        SdgoPvpAiObserverEventV1 event =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 7, 9229, 12);
        ObservePvpAi(nullptr, nullptr);
        ObservePvpAi(&event, nullptr);
        CHECK(g_queueCount == 0);

        ResetConfig("T7x", 9229, 0);
        ObservePvpAi(&event, nullptr);
        CHECK(g_queueCount == 1);

        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        CHECK(Drain(output) == 1);
        CHECK(output[0].mode == 12);
        CHECK(g_queueCount == 0);
    }

    void TestTitleBoundary()
    {
        ResetConfig("ABCDEFGH");
        ResetQueue();

        SdgoPvpAiObserverEventV1 event =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 7, 9229, 3, nullptr);
        memset(event.title, 'x', sizeof(event.title));
        memcpy(event.title + 12, "ABCDEFGH", 8);
        event.title[20] = 'Z'; // Deliberately no NUL in the producer's 21 bytes.
        ObservePvpAi(&event, nullptr);
        CHECK(g_queueCount == 1);

        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        CHECK(Drain(output) == 1);
        CHECK(output[0].title[19] == 'H');
        CHECK(output[0].title[20] == '\0');
        CHECK(strlen(output[0].title) == 20);
        CHECK(TitleContainsNonce(output[0].title));
    }

    void TestNormalLifecycle()
    {
        ResetConfig("T7x", 9229, 3, 2, false);
        Lifecycle lifecycle;
        const SdgoPvpAiObserverEventV1 request =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED);
        SdgoPvpAiObserverEventV1 setup =
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE);
        setup.ai_team_1 = 1;
        setup.ai_team_2 = 1;
        const SdgoPvpAiObserverEventV1 active =
            Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE);
        const SdgoPvpAiObserverEventV1 ended =
            Event(SDGO_PVP_AI_EVENT_BATTLE_END);

        ProcessEvent(&lifecycle, request);
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 1);
        CHECK(SameBattle(lifecycle, setup));

        ProcessEvent(&lifecycle, setup);
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 2);

        ProcessEvent(&lifecycle, active);
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 3);

        SdgoPvpAiObserverEventV1 unrelated =
            Event(SDGO_PVP_AI_EVENT_SETUP_FAILED, 0x55667788u);
        ProcessEvent(&lifecycle, unrelated);
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 3);

        ProcessEvent(&lifecycle, ended);
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 4);
        CHECK(lifecycle.eventCount == 5);
    }

    void TestRequireEndLifecycle()
    {
        ResetConfig("T7x", 9229, 3, 1, true);
        Lifecycle lifecycle;
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED));
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        CHECK(lifecycle.outcome == Lifecycle::Running);
        CHECK(lifecycle.phase == 3);

        ProcessEvent(
            &lifecycle, Event(SDGO_PVP_AI_EVENT_BATTLE_END));
        CHECK(lifecycle.outcome == Lifecycle::Pass);
        CHECK(lifecycle.phase == 4);
    }

    void TestLifecycleFailures()
    {
        ResetConfig("T7x", 9229, 3, 1, false);
        Lifecycle missingRequest;
        ProcessEvent(
            &missingRequest, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        CHECK(missingRequest.outcome == Lifecycle::Fail);
        CHECK(missingRequest.phase == 0);

        Lifecycle setupFailed;
        SdgoPvpAiObserverEventV1 failure =
            Event(SDGO_PVP_AI_EVENT_SETUP_FAILED);
        failure.status = 0xc0000005u;
        ProcessEvent(&setupFailed, failure);
        CHECK(setupFailed.outcome == Lifecycle::Fail);
        CHECK(setupFailed.reason ==
              "PvP AI setup raised a native exception");

        ResetConfig("T7x", 9229, 3, 3, false);
        Lifecycle tooFewAi;
        ProcessEvent(
            &tooFewAi, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED));
        SdgoPvpAiObserverEventV1 setup =
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE);
        setup.ai_team_1 = 1;
        setup.ai_team_2 = 1;
        ProcessEvent(&tooFewAi, setup);
        CHECK(tooFewAi.outcome == Lifecycle::Fail);
        CHECK(tooFewAi.phase == 1);

        ResetConfig();
        Lifecycle wrongRoom;
        ProcessEvent(
            &wrongRoom, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 1));
        ProcessEvent(
            &wrongRoom, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, 2));
        CHECK(wrongRoom.outcome == Lifecycle::Fail);

        Lifecycle wrongHost;
        ProcessEvent(
            &wrongHost,
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 1, 9229));
        ProcessEvent(
            &wrongHost,
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, 1, 9230));
        CHECK(wrongHost.outcome == Lifecycle::Fail);

        Lifecycle wrongMode;
        ProcessEvent(
            &wrongMode,
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, 1, 9229, 3));
        ProcessEvent(
            &wrongMode,
            Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, 1, 9229, 4));
        CHECK(wrongMode.outcome == Lifecycle::Fail);

        Lifecycle prematureEnd;
        ProcessEvent(
            &prematureEnd, Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED));
        ProcessEvent(
            &prematureEnd, Event(SDGO_PVP_AI_EVENT_BATTLE_END));
        CHECK(prematureEnd.outcome == Lifecycle::Fail);
    }

    void TestLifecycleDuplicatesAndTerminalOutcomes()
    {
        ResetConfig("T7x", 9229, 3, 1, false);

        Lifecycle repeatedRequest;
        const SdgoPvpAiObserverEventV1 request =
            Event(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED);
        ProcessEvent(&repeatedRequest, request);
        ProcessEvent(&repeatedRequest, request);
        CHECK(repeatedRequest.outcome == Lifecycle::Running);
        CHECK(repeatedRequest.phase == 1);
        CHECK(repeatedRequest.eventCount == 2);

        ProcessEvent(
            &repeatedRequest, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        CHECK(repeatedRequest.outcome == Lifecycle::Fail);
        CHECK(repeatedRequest.phase == 1);
        const std::string failureReason = repeatedRequest.reason;
        ProcessEvent(
            &repeatedRequest, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        CHECK(repeatedRequest.outcome == Lifecycle::Fail);
        CHECK(repeatedRequest.phase == 1);
        CHECK(repeatedRequest.reason == failureReason);
        CHECK(repeatedRequest.eventCount == 4);

        Lifecycle duplicateSetup;
        ProcessEvent(&duplicateSetup, request);
        ProcessEvent(
            &duplicateSetup, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &duplicateSetup, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        CHECK(duplicateSetup.outcome == Lifecycle::Fail);
        CHECK(duplicateSetup.phase == 2);

        Lifecycle terminalPass;
        ProcessEvent(&terminalPass, request);
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        CHECK(terminalPass.outcome == Lifecycle::Pass);
        CHECK(terminalPass.phase == 3);
        const std::string passReason = terminalPass.reason;

        ProcessEvent(&terminalPass, request);
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_SETUP_COMPLETE));
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE));
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_SETUP_FAILED));
        ProcessEvent(
            &terminalPass,
            Event(SDGO_PVP_AI_EVENT_BATTLE_END, 0x55667788u));
        CHECK(terminalPass.outcome == Lifecycle::Pass);
        CHECK(terminalPass.phase == 3);
        CHECK(terminalPass.reason == passReason);

        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_BATTLE_END));
        CHECK(terminalPass.outcome == Lifecycle::Pass);
        CHECK(terminalPass.phase == 4);
        const std::string completedReason = terminalPass.reason;

        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_SETUP_FAILED));
        ProcessEvent(
            &terminalPass, Event(SDGO_PVP_AI_EVENT_BATTLE_END));
        CHECK(terminalPass.outcome == Lifecycle::Pass);
        CHECK(terminalPass.phase == 4);
        CHECK(terminalPass.reason == completedReason);
        CHECK(terminalPass.eventCount == 11);
    }

    void TestQueueCapacityAndOrder()
    {
        ResetConfig();
        ResetQueue();
        for (unsigned i = 0; i < kQueueCapacity; ++i) {
            SdgoPvpAiObserverEventV1 event =
                Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE);
            event.sequence = i + 1;
            ObservePvpAi(&event, nullptr);
        }
        CHECK(g_queueCount == kQueueCapacity);
        CHECK(InterlockedCompareExchange(&g_dropped, 0, 0) == 0);

        SdgoPvpAiObserverEventV1 overflow =
            Event(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE);
        overflow.sequence = kQueueCapacity + 1;
        ObservePvpAi(&overflow, nullptr);
        CHECK(g_queueCount == kQueueCapacity);
        CHECK(InterlockedCompareExchange(&g_dropped, 0, 0) == 1);

        uint64_t expectedSequence = 1;
        unsigned drained = 0;
        SdgoPvpAiObserverEventV1 output[kDrainBatch] = {};
        for (;;) {
            const unsigned count = Drain(output);
            if (!count)
                break;
            CHECK(count <= kDrainBatch);
            for (unsigned i = 0; i < count; ++i) {
                CHECK(output[i].sequence == expectedSequence);
                ++expectedSequence;
            }
            drained += count;
        }
        CHECK(drained == kQueueCapacity);
        CHECK(expectedSequence == kQueueCapacity + 1);
        CHECK(g_queueCount == 0);
    }
}

int main()
{
    std::puts(
        "OFFLINE ONLY: no BattleServer/client/authentication/room/gameplay is exercised.");
    InitializeCriticalSection(&g_queueLock);

    TestAbiAndValidation();
    TestObserverFiltering();
    TestObserverWildcardAndDisabledHost();
    TestTitleBoundary();
    TestNormalLifecycle();
    TestRequireEndLifecycle();
    TestLifecycleFailures();
    TestLifecycleDuplicatesAndTerminalOutcomes();
    TestQueueCapacityAndOrder();

    DeleteCriticalSection(&g_queueLock);
    std::printf("%u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
