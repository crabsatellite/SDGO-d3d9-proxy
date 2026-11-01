// Durable bridge to the owner's native-GC ledger worker. No HTTP payout API.
#pragma once
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "boss_catalog.h"
#include "runtime_state.h"

namespace sdgo::battle_rewards {
struct WalletHealth {
    bool healthy = false;
    std::string scope, revision, error;
};
struct WalletJob {
    std::string match_id, plan_hash, policy_revision, scope;
    uint32_t account = 0, mb = 0;
};
struct WalletReceipt {
    uint32_t historical_post_mb = 0, current_mb = 0;
    uint64_t published_unix = 0;
    bool already_applied = false;
};

namespace job_detail {
inline uint64_t UnixNow() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}
inline bool UInt(const std::string& text, uint64_t maximum, uint64_t* value) {
    if (text.empty()) return false;
    uint64_t n = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        uint64_t digit = static_cast<uint64_t>(c - '0');
        if (n > maximum / 10 || (n == maximum / 10 && digit > maximum % 10)) return false;
        n = n * 10 + digit;
    }
    *value = n; return true;
}
inline bool Hex(const std::string& value, size_t count) {
    if (value.size() != count) return false;
    for (char c : value) if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
    return true;
}
inline bool Scope(const std::string& value) {
    if (value.size() != 36) return false;
    for (size_t i = 0; i < value.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (value[i] != '-') return false; }
        else if (!(value[i] >= '0' && value[i] <= '9') && !(value[i] >= 'a' && value[i] <= 'f')) return false;
    }
    return true;
}
inline bool Read(const std::filesystem::path& path, const char* section,
                 std::map<std::string, std::string>* result) {
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || size.QuadPart > 16384) { CloseHandle(handle); return false; }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool complete = ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    CloseHandle(handle);
    if (!complete) return false;
    std::istringstream file(bytes);
    std::string line;
    std::map<std::string, std::string> values;
    bool opened = false;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '[') {
            if (opened || line != std::string("[") + section + "]") return false;
            opened = true; continue;
        }
        size_t equals = line.find('=');
        if (!opened || equals == std::string::npos || !equals || line.find('\0') != std::string::npos) return false;
        if (!values.emplace(line.substr(0, equals), line.substr(equals + 1)).second) return false;
    }
    if (!opened || values["SchemaVersion"] != "1") return false;
    *result = std::move(values); return true;
}
inline bool Valid(const WalletJob& job) {
    if (job.match_id.empty() || job.match_id.size() > 128 || !job.account || job.account >= 0x80000000u ||
        !job.mb || job.mb >= 0x80000000u || !Hex(job.plan_hash, 64) || !Hex(job.policy_revision, 64) || !Scope(job.scope)) return false;
    for (char c : job.match_id)
        if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') &&
            c != '-' && c != '_' && c != '.' && c != ':') return false;
    return true;
}
inline std::string Serialize(const WalletJob& job) {
    return "[reward]\r\nSchemaVersion=1\r\nMatchId=" + job.match_id + "\r\nAccountNumber=" + std::to_string(job.account) +
        "\r\nPlanHash=" + job.plan_hash + "\r\nMb=" + std::to_string(job.mb) + "\r\nPolicyRevision=" + job.policy_revision +
        "\r\nExpectedScope=" + job.scope + "\r\n";
}
}

inline WalletHealth ReadWalletHealth(const std::filesystem::path& runtime) {
    WalletHealth result;
    std::map<std::string, std::string> values;
    if (!job_detail::Read(runtime / L"wallet-status.ini", "wallet", &values)) {
        result.error = "wallet worker heartbeat is unavailable"; return result;
    }
    result.scope = values["ScopeId"]; result.revision = values["Revision"];
    result.error = values["LastError"];
    uint64_t heartbeat = 0;
    const auto now = job_detail::UnixNow();
    if (!job_detail::UInt(values["HeartbeatUnix"], UINT64_MAX, &heartbeat) || heartbeat > now + 5 ||
        now > heartbeat + 15 || values["Healthy"] != "1" || !job_detail::Scope(result.scope) ||
        result.revision != "sdgo-native-gc-ledger-v1") {
        if (result.error.empty()) result.error = "wallet worker heartbeat is stale or invalid";
        return result;
    }
    result.healthy = true; return result;
}

inline bool WalletJobName(const WalletJob& job, std::string* name) {
    if (!job_detail::Valid(job)) return false;
    std::string digest;
    if (!BossCatalog::Sha256(job.match_id + ":" + std::to_string(job.account), &digest)) return false;
    *name = digest + ".ini"; return true;
}

inline bool PublishWalletJob(const std::filesystem::path& runtime, const WalletJob& job,
                             std::string* name, std::string* error = nullptr) {
    if (!name || !WalletJobName(job, name)) { if (error) *error = "invalid native wallet job"; return false; }
    const auto path = runtime / L"outbox" / *name;
    const std::string body = job_detail::Serialize(job);
    // An already queued request is immutable, even if an owner changes policy.
    std::ifstream existing(path, std::ios::binary);
    if (existing) {
        std::string previous(std::istreambuf_iterator<char>(existing), {});
        if (previous != body) { if (error) *error = "native wallet job identity changed"; return false; }
        return true;
    }
    return runtime_detail::AtomicWrite(path, body, error);
}

inline bool ReadWalletReceipt(const std::filesystem::path& runtime, const WalletJob& job,
                              WalletReceipt* result, std::string* error = nullptr) {
    auto fail = [error](const char* message) { if (error) *error = message; return false; };
    std::string name, sourceHash;
    if (!result || !WalletJobName(job, &name) || !BossCatalog::Sha256(job_detail::Serialize(job), &sourceHash))
        return fail("invalid native wallet receipt identity");
    std::map<std::string, std::string> value;
    if (!job_detail::Read(runtime / L"receipts" / name, "receipt", &value)) return fail("wallet receipt pending");
    if (value["MatchId"] != job.match_id || value["AccountNumber"] != std::to_string(job.account) ||
        value["PlanHash"] != job.plan_hash || value["PolicyRevision"] != job.policy_revision || value["ScopeId"] != job.scope ||
        value["Mb"] != std::to_string(job.mb) || value["NativeRetVal"] != "0" || value["SourceHash"] != sourceHash)
        return fail("wallet receipt does not match the captured native job");
    uint64_t before = 0, current = 0, published = 0;
    if (!job_detail::UInt(value["MbAfter"], 2147483647, &before) || !job_detail::UInt(value["CurrentMb"], 2147483647, &current) ||
        !job_detail::UInt(value["PublishedUnix"], UINT64_MAX, &published) || (value["AlreadyApplied"] != "0" && value["AlreadyApplied"] != "1"))
        return fail("wallet receipt amount/time fields are invalid");
    *result = {static_cast<uint32_t>(before), static_cast<uint32_t>(current), published, value["AlreadyApplied"] == "1"};
    if (error) error->clear();
    return true;
}

inline bool WalletReceiptFresh(const WalletReceipt& receipt, uint64_t now,
                               uint64_t max_age_seconds) {
    if (receipt.published_unix == 0) return false;
    if (receipt.published_unix > now) {
        // All components run on the same host, but tolerate the same small
        // wall-clock step already accepted by the wallet heartbeat.
        return receipt.published_unix - now <= 5;
    }
    return now - receipt.published_unix <= max_age_seconds;
}
}
