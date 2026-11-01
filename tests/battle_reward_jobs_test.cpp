#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#include "mods/battle_rewards/wallet_jobs.h"

using namespace sdgo::battle_rewards;

namespace {

constexpr uint32_t kAccount = 123456789;
constexpr uint32_t kMb = 37;
constexpr uint32_t kHistoricalMb = 1037;
constexpr uint32_t kCurrentMb = 2048;
const char* const kMatch = "battle:interop:20261101:42";
const char* const kPlan =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const char* const kPolicy =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

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

std::string Utf8(const wchar_t* value) {
    if (!value) return {};
    const int required =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr,
                            0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1,
                             result.data(), required, nullptr, nullptr)) {
        return {};
    }
    result.pop_back();
    return result;
}

WalletJob Job(const std::string& scope) {
    WalletJob job;
    job.match_id = kMatch;
    job.account = kAccount;
    job.plan_hash = kPlan;
    job.mb = kMb;
    job.policy_revision = kPolicy;
    job.scope = scope;
    return job;
}

std::string Bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void Emit(const std::filesystem::path& runtime, const std::string& scope) {
    const WalletJob job = Job(scope);
    const WalletHealth absent = ReadWalletHealth(runtime);
    CHECK(!absent.healthy);

    std::string name;
    CHECK(WalletJobName(job, &name));
    std::string expected_name;
    CHECK(BossCatalog::Sha256(job.match_id + ":" + std::to_string(job.account),
                              &expected_name));
    CHECK(name == expected_name + ".ini");

    std::string error;
    CHECK(PublishWalletJob(runtime, job, &name, &error));
    CHECK(error.empty());
    const auto path = runtime / L"outbox" / std::filesystem::path(name);
    CHECK(std::filesystem::is_regular_file(path));
    CHECK(Bytes(path) == job_detail::Serialize(job));
    CHECK(PublishWalletJob(runtime, job, &name, &error));
    CHECK(error.empty());

    WalletJob changed = job;
    changed.mb += 1;
    CHECK(!PublishWalletJob(runtime, changed, &name, &error));
    CHECK(error == "native wallet job identity changed");
    changed = job;
    changed.policy_revision.assign(64, 'c');
    CHECK(!PublishWalletJob(runtime, changed, &name, &error));
    CHECK(error == "native wallet job identity changed");
    changed = job;
    changed.plan_hash.assign(64, 'd');
    CHECK(!PublishWalletJob(runtime, changed, &name, &error));
    CHECK(error == "native wallet job identity changed");
    CHECK(Bytes(path) == job_detail::Serialize(job));
}

void Receipt(const std::filesystem::path& runtime, const std::string& scope) {
    WalletReceipt boundary;
    constexpr uint64_t boundary_now = 1000;
    constexpr uint64_t maximum_age = 15;
    CHECK(!WalletReceiptFresh(boundary, boundary_now, maximum_age));
    boundary.published_unix = boundary_now;
    CHECK(WalletReceiptFresh(boundary, boundary_now, maximum_age));
    boundary.published_unix = boundary_now - maximum_age;
    CHECK(WalletReceiptFresh(boundary, boundary_now, maximum_age));
    boundary.published_unix = boundary_now - maximum_age - 1;
    CHECK(!WalletReceiptFresh(boundary, boundary_now, maximum_age));
    boundary.published_unix = boundary_now + 5;
    CHECK(WalletReceiptFresh(boundary, boundary_now, maximum_age));
    boundary.published_unix = boundary_now + 6;
    CHECK(!WalletReceiptFresh(boundary, boundary_now, maximum_age));

    const WalletJob job = Job(scope);
    const WalletHealth health = ReadWalletHealth(runtime);
    CHECK(health.healthy);
    CHECK(health.scope == scope);
    CHECK(health.revision == "sdgo-native-gc-ledger-v1");
    CHECK(health.error.empty());

    WalletReceipt receipt;
    std::string error;
    CHECK(ReadWalletReceipt(runtime, job, &receipt, &error));
    CHECK(error.empty());
    CHECK(receipt.historical_post_mb == kHistoricalMb);
    CHECK(receipt.current_mb == kCurrentMb);
    CHECK(!receipt.already_applied);
    const uint64_t now = job_detail::UnixNow();
    CHECK(receipt.published_unix <= now + 5);
    CHECK(now <= receipt.published_unix + 15);

    std::string name;
    CHECK(WalletJobName(job, &name));
    CHECK(!std::filesystem::exists(runtime / L"outbox" /
                                   std::filesystem::path(name)));
    CHECK(std::filesystem::is_regular_file(
        runtime / L"jobs-completed" / std::filesystem::path(name)));

    WalletJob changed = job;
    changed.policy_revision.assign(64, 'c');
    CHECK(!ReadWalletReceipt(runtime, changed, &receipt, &error));
    CHECK(error == "wallet receipt does not match the captured native job");
    changed = job;
    changed.scope = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    CHECK(!ReadWalletReceipt(runtime, changed, &receipt, &error));
    CHECK(error == "wallet receipt does not match the captured native job");
    changed = job;
    changed.mb += 1;
    CHECK(!ReadWalletReceipt(runtime, changed, &receipt, &error));
    CHECK(error == "wallet receipt does not match the captured native job");
    changed = job;
    changed.plan_hash.assign(64, 'd');
    CHECK(!ReadWalletReceipt(runtime, changed, &receipt, &error));
    CHECK(error == "wallet receipt does not match the captured native job");
}

void Stale(const std::filesystem::path& runtime, const std::string& scope) {
    const WalletHealth health = ReadWalletHealth(runtime);
    CHECK(!health.healthy);
    CHECK(health.scope == scope);
    CHECK(health.revision == "sdgo-native-gc-ledger-v1");
    CHECK(health.error == "wallet worker heartbeat is stale or invalid");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 4) {
        std::cerr << "usage: battle_reward_jobs_test "
                     "emit|receipt|stale RUNTIME SCOPE_UUID\n";
        return 2;
    }
    const std::wstring operation = argv[1];
    const std::filesystem::path runtime = argv[2];
    const std::string scope = Utf8(argv[3]);
    if (scope.empty()) return 2;

    if (operation == L"emit") {
        Emit(runtime, scope);
    } else if (operation == L"receipt") {
        Receipt(runtime, scope);
    } else if (operation == L"stale") {
        Stale(runtime, scope);
    } else {
        return 2;
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
