#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "sdgo/sdgo_api.h"
#include "../mods/game_test_shared/runner.h"

namespace
{
    const char* Scenario()
    {
        static char value[64] = {};
        if (!value[0])
            GetEnvironmentVariableA("SDGO_GAME_TEST_FIXTURE_SCENARIO", value, sizeof(value));
        return value;
    }

    void MarkOffline(const SdgoGameTestContextV1* context)
    {
        if (context->tick == 0)
            context->Observe("OFFLINE controller fixture; no game executable or game callbacks");
    }

    int __cdecl PollFixture(const SdgoGameTestContextV1* context)
    {
        MarkOffline(context);
        const char* scenario = Scenario();
        if (!strcmp(scenario, "valid")) {
            if (context->tick < 2)
                return SDGO_GAME_TEST_WAIT;
            context->Assert(1, "WAIT ticks preserved state before PASS");
            return SDGO_GAME_TEST_PASS;
        }
        if (!strcmp(scenario, "pass_no_assert"))
            return SDGO_GAME_TEST_PASS;
        if (!strcmp(scenario, "failed_assert_pass")) {
            context->Assert(0, "fixture failed assertion survives PASS");
            return SDGO_GAME_TEST_PASS;
        }
        if (!strcmp(scenario, "failed_assert_wait_pass")) {
            if (context->tick == 0) {
                context->Assert(0, "fixture failed assertion during WAIT");
                return SDGO_GAME_TEST_WAIT;
            }
            if (context->tick < 2)
                return SDGO_GAME_TEST_WAIT;
            context->Assert(1, "later PASS cannot erase the WAIT assertion failure");
            return SDGO_GAME_TEST_PASS;
        }
        if (!strcmp(scenario, "dependency_short_circuit")) {
            if (!strcmp(context->case_id, "framework.bootstrap")) {
                context->Assert(0, "fixture dependency failed");
                return SDGO_GAME_TEST_FAIL;
            }
            context->Observe("DEPENDENT CASE EXECUTED");
            context->Assert(1, "dependent fixture should never run");
            return SDGO_GAME_TEST_PASS;
        }
        if (!strcmp(scenario, "timeout_wait"))
            return SDGO_GAME_TEST_WAIT;
        if (!strcmp(scenario, "skip"))
            return SDGO_GAME_TEST_SKIP;
        if (!strcmp(scenario, "seh")) {
            RaiseException(0xE0421001u, 0, 0, nullptr);
            return SDGO_GAME_TEST_PASS;
        }
        context->Assert(0, "unknown fixture scenario");
        return SDGO_GAME_TEST_FAIL;
    }

    void __cdecl Log(SdgoMod*, int, const char*, ...)
    {
    }
}

SDGO_GAME_TEST_PROVIDER_ENTRY
{
    static SdgoGameTestCaseV1 tests[2]{};
    static SdgoGameTestProviderV1 provider{};
    const char* scenario = Scenario();
    tests[0].id = "framework.bootstrap";
    tests[0].timeout_ms =
        !strcmp(scenario, "timeout_wait") ? 1u : 2000u;
    tests[0].poll = &PollFixture;
    tests[1].id =
        !strcmp(scenario, "duplicate_case")
            ? "framework.bootstrap"
            : "pvp.room";
    tests[1].timeout_ms = 2000u;
    tests[1].poll = &PollFixture;
    provider.size = sizeof(provider);
    provider.version = SDGO_GAME_TEST_VERSION;
    provider.suite = "pvp-ai";
    provider.role = "server";
    provider.count =
        !strcmp(scenario, "duplicate_case") ||
        !strcmp(scenario, "dependency_short_circuit")
            ? 2u
            : 1u;
    provider.cases = tests;
    return &provider;
}

int main()
{
    SdgoApi api{};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.host_key = "BattleServer-548A891D-0046C000";
    api.host_base = GetModuleHandleW(nullptr);
    api.Log = &Log;
    int modStorage = 0;
    auto* mod = reinterpret_cast<SdgoMod*>(&modStorage);
    if (!sdgo_test::Open(&api, mod, "server"))
        return 4;
    for (;;) {
        sdgo_test::Poll();
        Sleep(2);
    }
}
