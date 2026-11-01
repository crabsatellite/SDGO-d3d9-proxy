#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "mods/battle_rewards/runtime_state.h"

using namespace sdgo::battle_rewards;

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL line " << line << ": " << expression << "\n";
    }
}

#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

void LimitTests() {
    Policy policy = DefaultPolicy();
    RuntimeLimits limits;
    std::string error;
    CHECK(ComputeRuntimeLimits(policy, &limits, &error));
    CHECK(error.empty());
    Policy unsupported = policy;
    unsupported.pvp.objective.gb_micro = kMicro;
    CHECK(!ComputeRuntimeLimits(unsupported, &limits, &error));
    CHECK(error.find("ObjectiveGb/Mb must be zero") != std::string::npos);
    CHECK(ComputeRuntimeLimits(policy, &limits, &error));
    CHECK(limits.max_gb == 65535);
    CHECK(limits.max_mb == 2147483647);
    CHECK(limits.max_base_multiplier_micro >= policy.base_multiplier_micro);
    CHECK(runtime_detail::WorstCaseSafe(
        policy, limits.max_base_multiplier_micro));
    if (limits.max_base_multiplier_micro < 100 * kMicro) {
        CHECK(!runtime_detail::WorstCaseSafe(
            policy, limits.max_base_multiplier_micro + 1));
    }

    policy.base_multiplier_micro = limits.max_base_multiplier_micro + 1;
    CHECK(!ComputeRuntimeLimits(policy, &limits, &error));
    CHECK(error.find("configured Base exceeds native safe maximum") !=
          std::string::npos);
}

void ValidationTests(const std::wstring& owner_root,
                     const std::string& expected_revision) {
    RuntimeState invalid(owner_root, "../escape");
    std::string error;
    CHECK(!invalid.ReloadPolicy(&error));
    CHECK(error.find("instance_id") != std::string::npos);
    CHECK(!invalid.WriteHeartbeat(&error));

    RuntimeState state(owner_root, "battle-1");
    CHECK(state.PolicyPath() ==
          std::filesystem::path(owner_root) / L"runtime" /
              L"reward-policy.ini");
    CHECK(state.ReloadPolicy(&error));
    CHECK(error.empty());
    CHECK(state.PolicyRevision() == expected_revision);

    Policy snapshot;
    CHECK(state.SnapshotPolicy(&snapshot));
    CHECK(std::string(snapshot.revision.data(), 64) == expected_revision);
    CHECK(snapshot.base_multiplier_micro == 10 * kMicro);

    const RuntimeLimits limits = state.Limits();
    CHECK(limits.max_base_multiplier_micro >= snapshot.base_multiplier_micro);
    CHECK(state.SetActivityWindow("18:00", "22:00", false, &error));
    CHECK(!state.SetActivityWindow("24:00", "22:00", false, &error));

    PayoutCounters counters;
    counters.matches_seen = 8;
    counters.settlements_committed = 7;
    counters.settlements_failed = 1;
    counters.payouts_committed = 20;
    counters.payouts_rejected = 1;
    counters.idempotent_replays = 2;
    CHECK(state.SetPayoutCounters(counters, &error));
    counters.matches_seen = -1;
    CHECK(!state.SetPayoutCounters(counters, &error));

    state.SetSettlementReady(true);
    state.SetWalletWorkerHealth(true, "wallet-schema-1");
    CHECK(state.WriteHeartbeat(&error));
    CHECK(error.empty());
    std::atomic<int> write_failures{0};
    std::vector<std::thread> writers;
    for (int thread = 0; thread < 4; ++thread) {
        writers.emplace_back([&state, &write_failures]() {
            for (int write = 0; write < 10; ++write) {
                if (!state.WriteHeartbeat()) ++write_failures;
            }
        });
    }
    for (std::thread& writer : writers) writer.join();
    CHECK(write_failures.load() == 0);

    RuntimeState explicit_root(
        std::filesystem::path(owner_root).append(L"runtime").wstring(),
        "battle-explicit", RuntimeRootKind::ExplicitRuntimeDirectory);
    CHECK(explicit_root.PolicyPath() == state.PolicyPath());
    CHECK(explicit_root.ReloadPolicy(&error));
}

void UnsafePolicyTests(const std::wstring& owner_root,
                       const std::string& expected_revision) {
    RuntimeState state(owner_root, "battle-unsafe");
    std::string error;
    CHECK(!state.ReloadPolicy(&error));
    CHECK(error.find("configured Base exceeds native safe maximum") !=
          std::string::npos);
    CHECK(state.PolicyRevision() == expected_revision);
    Policy snapshot;
    CHECK(!state.SnapshotPolicy(&snapshot));
    state.SetSettlementReady(true);
    state.SetWalletWorkerHealth(true, "wallet-schema-1");
    CHECK(state.WriteHeartbeat(&error));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    LimitTests();
    if (argc != 3 && argc != 4) {
        std::cerr << "usage: battle_reward_runtime_state_test OWNER_ROOT "
                     "EXPECTED_REVISION [unsafe]\n";
        return 2;
    }
    const int required =
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, nullptr, 0, nullptr, nullptr);
    std::string revision(static_cast<size_t>(required > 0 ? required : 0), '\0');
    if (required > 0) {
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, &revision[0], required,
                            nullptr, nullptr);
        if (!revision.empty() && revision.back() == '\0') revision.pop_back();
    }
    if (argc == 4 && std::wstring(argv[3]) == L"unsafe") {
        UnsafePolicyTests(argv[1], revision);
    } else {
        ValidationTests(argv[1], revision);
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
