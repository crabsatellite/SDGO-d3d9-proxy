#pragma once

#ifndef _WIN32
#error battle reward runtime state requires Windows
#endif

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>

#include "policy_config.h"

namespace sdgo {
namespace battle_rewards {

enum class RuntimeRootKind : uint8_t {
    OwnerWorkspace,
    ExplicitRuntimeDirectory
};

struct RuntimeLimits {
    int32_t max_gb = kNativeGpFieldMaximum;
    int32_t max_mb = kWalletMaximum;
    uint32_t max_base_multiplier_micro = 0;
};

struct ActivityWindow {
    bool present = false;
    std::string start;
    std::string end;
    bool active = false;
};

struct PayoutCounters {
    int64_t matches_seen = 0;
    int64_t settlements_committed = 0;
    int64_t settlements_failed = 0;
    int64_t payouts_committed = 0;
    int64_t payouts_rejected = 0;
    int64_t idempotent_replays = 0;
};

namespace runtime_detail {

inline bool IsSafeInstanceId(const std::string& value) {
    if (value.empty() || value.size() > 64) return false;
    const auto alpha_numeric = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    if (!alpha_numeric(static_cast<unsigned char>(value[0]))) return false;
    for (unsigned char c : value) {
        if (!alpha_numeric(c) && c != '_' && c != '.' && c != '-') return false;
    }
    return true;
}

inline bool IsTime(const std::string& value) {
    if (value.size() != 5 || value[2] != ':') return false;
    if (value[0] < '0' || value[0] > '2' ||
        value[1] < '0' || value[1] > '9' ||
        value[3] < '0' || value[3] > '5' ||
        value[4] < '0' || value[4] > '9') {
        return false;
    }
    return value[0] != '2' || value[1] <= '3';
}

inline std::string WindowsError(const char* operation, DWORD code) {
    std::ostringstream output;
    output << operation << " failed (" << code << ")";
    return output.str();
}

inline std::string DecimalMicro(uint32_t value) {
    const uint32_t whole = value / static_cast<uint32_t>(kMicro);
    const uint32_t fraction = value % static_cast<uint32_t>(kMicro);
    std::ostringstream output;
    output << whole;
    if (fraction != 0) {
        std::string digits = std::to_string(fraction + static_cast<uint32_t>(kMicro));
        digits.erase(0, 1);
        while (!digits.empty() && digits.back() == '0') digits.pop_back();
        output << '.' << digits;
    }
    return output.str();
}

inline std::string JsonString(const std::string& value, size_t maximum = SIZE_MAX) {
    std::string output;
    output.push_back('"');
    const size_t count = (std::min)(value.size(), maximum);
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        switch (c) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (c < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[c >> 4]);
                    output.push_back(hex[c & 0x0F]);
                } else {
                    output.push_back(static_cast<char>(c));
                }
        }
    }
    output.push_back('"');
    return output;
}

inline std::string UtcNow() {
    SYSTEMTIME time = {};
    GetSystemTime(&time);
    char value[32] = {};
    std::snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                  static_cast<unsigned>(time.wYear),
                  static_cast<unsigned>(time.wMonth),
                  static_cast<unsigned>(time.wDay),
                  static_cast<unsigned>(time.wHour),
                  static_cast<unsigned>(time.wMinute),
                  static_cast<unsigned>(time.wSecond),
                  static_cast<unsigned>(time.wMilliseconds));
    return value;
}

inline bool WorstCaseSafe(const Policy& source, uint32_t base_multiplier_micro,
                          Error* last_error = nullptr) {
    Policy policy = source;
    policy.base_multiplier_micro = base_multiplier_micro;
    ParticipantStats stats;
    stats.kind = ParticipantKind::Human;
    stats.active_seconds = policy.full_reward_seconds;
    stats.kills = 255;
    stats.assists = 255;
    stats.minion_kills = 10000;
    stats.damage = 100000000;
    stats.objectives = 1000;
    stats.boss_credit_micro = 100 * static_cast<uint32_t>(kMicro);
    stats.is_mvp = true;

    MatchContext context;
    context.human_count = 12;
    context.event_multiplier = 2;
    context.duration_seconds = policy.full_reward_seconds;
    context.bosses_defeated = 100;

    Reward reward;
    Error error;
    context.mode = Mode::Pvp;
    const Outcome pvp_outcomes[] = {Outcome::Win, Outcome::Lose, Outcome::Draw};
    for (Outcome outcome : pvp_outcomes) {
        context.outcome = outcome;
        if (!Calculate(policy, context, stats, &reward, &error,
                       NativeSettlementLimits())) {
            if (last_error) *last_error = error;
            return false;
        }
    }

    context.mode = Mode::Pve;
    const Outcome pve_outcomes[] = {Outcome::Clear, Outcome::Fail};
    const Difficulty difficulties[] = {
        Difficulty::Normal, Difficulty::Hard, Difficulty::Expert
    };
    for (Outcome outcome : pve_outcomes) {
        context.outcome = outcome;
        for (Difficulty difficulty : difficulties) {
            context.difficulty = difficulty;
            if (!Calculate(policy, context, stats, &reward, &error,
                           NativeSettlementLimits())) {
                if (last_error) *last_error = error;
                return false;
            }
        }
    }
    if (last_error) *last_error = {};
    return true;
}

inline bool ComputeRuntimeLimits(const Policy& policy, RuntimeLimits* limits,
                                 std::string* error) {
    if (!limits) {
        if (error) *error = "runtime limits output is required";
        return false;
    }
    Error validation;
    if (!ValidatePolicy(policy, &validation)) {
        if (error) *error = validation.message;
        return false;
    }

    uint32_t low = 0;
    uint32_t high = 100 * static_cast<uint32_t>(kMicro);
    while (low < high) {
        const uint32_t middle =
            low + static_cast<uint32_t>((static_cast<uint64_t>(high) - low + 1) / 2);
        if (WorstCaseSafe(policy, middle)) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    if (!WorstCaseSafe(policy, low, &validation)) {
        if (error) *error = validation.message;
        return false;
    }
    limits->max_gb = kNativeGpFieldMaximum;
    limits->max_mb = kWalletMaximum;
    limits->max_base_multiplier_micro = low;
    if (policy.base_multiplier_micro > low) {
        if (error) {
            *error = "configured Base exceeds native safe maximum " +
                     DecimalMicro(low);
        }
        return false;
    }
    if (error) error->clear();
    return true;
}

inline bool LoadSharedPolicy(const std::filesystem::path& path, Policy* policy, PolicyConfigError* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return config_detail::ConfigFail(error, 0, "unable to open policy file");
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 131072) {
        CloseHandle(file); return config_detail::ConfigFail(error, 0, "policy file is invalid or too large");
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    bool complete = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    CloseHandle(file);
    if (!complete) return config_detail::ConfigFail(error, 0, "unable to read complete policy file");
    return ParsePolicyIni(bytes, policy, error);
}

inline bool AtomicWrite(const std::filesystem::path& path,
                        const std::string& bytes, std::string* error) {
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path(), directory_error);
    if (directory_error) {
        if (error) {
            *error = "create status directory failed (" +
                     std::to_string(directory_error.value()) + ")";
        }
        return false;
    }

    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        std::wostringstream name;
        name << L"." << path.filename().wstring() << L"." << GetCurrentProcessId()
             << L"." << GetCurrentThreadId() << L"." << GetTickCount64()
             << L"." << attempt << L".tmp";
        temporary = path.parent_path() / name.str();
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            if (error) *error = WindowsError("CreateFileW", GetLastError());
            return false;
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "unable to allocate unique status temporary file";
        return false;
    }

    bool success = true;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>(
            (std::min)(bytes.size() - offset,
                       static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, request, &written, nullptr) ||
            written == 0) {
            if (error) *error = WindowsError("WriteFile", GetLastError());
            success = false;
            break;
        }
        offset += written;
    }
    if (success && !FlushFileBuffers(file)) {
        if (error) *error = WindowsError("FlushFileBuffers", GetLastError());
        success = false;
    }
    if (!CloseHandle(file) && success) {
        if (error) *error = WindowsError("CloseHandle", GetLastError());
        success = false;
    }
    if (success &&
        !MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        if (error) *error = WindowsError("MoveFileExW", GetLastError());
        success = false;
    }
    if (!success) DeleteFileW(temporary.c_str());
    return success;
}

}  // namespace runtime_detail

inline bool ComputeRuntimeLimits(const Policy& policy, RuntimeLimits* limits,
                                 std::string* error = nullptr) {
    if (policy.pvp.objective.gb_micro || policy.pvp.objective.mb_micro ||
        policy.pve.objective.gb_micro || policy.pve.objective.mb_micro) {
        if (error) *error = "native adapter does not collect independent objective events; ObjectiveGb/Mb must be zero";
        return false;
    }
    return runtime_detail::ComputeRuntimeLimits(policy, limits, error);
}

class RuntimeState {
public:
    RuntimeState(const std::wstring& root, const std::string& instance_id,
                 RuntimeRootKind root_kind = RuntimeRootKind::OwnerWorkspace)
        : runtime_root_(root_kind == RuntimeRootKind::OwnerWorkspace
                            ? std::filesystem::path(root) / L"runtime"
                            : std::filesystem::path(root)),
          policy_path_(runtime_root_ / L"reward-policy.ini"),
          status_path_(runtime_root_ / L"status" /
                       std::filesystem::path(
                           std::wstring(instance_id.begin(), instance_id.end()) +
                           L".json")),
          instance_id_(instance_id),
          instance_valid_(runtime_detail::IsSafeInstanceId(instance_id)) {
        if (!instance_valid_) {
            policy_error_ =
                "instance_id must match [A-Za-z0-9][A-Za-z0-9_.-]{0,63}";
        }
    }

    RuntimeState(const RuntimeState&) = delete;
    RuntimeState& operator=(const RuntimeState&) = delete;

    const std::filesystem::path& RuntimeRoot() const { return runtime_root_; }
    const std::filesystem::path& PolicyPath() const { return policy_path_; }
    const std::filesystem::path& StatusPath() const { return status_path_; }

    bool ReloadPolicy(std::string* error = nullptr) {
        if (!instance_valid_) {
            if (error) {
                std::lock_guard<std::mutex> lock(mutex_);
                *error = policy_error_;
            }
            return false;
        }

        Policy parsed;
        PolicyConfigError parse_error;
        if (!runtime_detail::LoadSharedPolicy(policy_path_, &parsed, &parse_error)) {
            std::string message = parse_error.message.empty()
                                      ? "unable to load reward policy"
                                      : parse_error.message;
            if (parse_error.line != 0) {
                message += " at line " + std::to_string(parse_error.line);
            }
            std::lock_guard<std::mutex> lock(mutex_);
            policy_loaded_ = false;
            observed_revision_.clear();
            policy_error_ = message;
            if (error) *error = message;
            return false;
        }

        const std::string revision(parsed.revision.data(), 64);
        RuntimeLimits limits;
        std::string limit_error;
        if (!ComputeRuntimeLimits(parsed, &limits, &limit_error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            policy_loaded_ = false;
            observed_revision_ = revision;
            policy_error_ = limit_error;
            limits_ = limits;
            if (error) *error = limit_error;
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        policy_ = parsed;
        limits_ = limits;
        observed_revision_ = revision;
        policy_error_.clear();
        policy_loaded_ = true;
        if (error) error->clear();
        return true;
    }

    bool SnapshotPolicy(Policy* policy) const {
        if (!policy) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!policy_loaded_) return false;
        *policy = policy_;
        return true;
    }

    RuntimeLimits Limits() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return limits_;
    }

    std::string PolicyRevision() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return observed_revision_;
    }

    std::string LastError() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return EffectiveErrorLocked();
    }

    void SetSettlementReady(bool ready, const std::string& error = std::string()) {
        std::lock_guard<std::mutex> lock(mutex_);
        settlement_ready_ = ready;
        settlement_error_ = error;
    }

    void SetWalletWorkerHealth(bool healthy, const std::string& revision,
                               const std::string& error = std::string()) {
        std::lock_guard<std::mutex> lock(mutex_);
        wallet_worker_healthy_ = healthy;
        wallet_worker_revision_ = revision;
        wallet_error_ = error;
    }

    bool SetActivityWindow(const std::string& start, const std::string& end,
                           bool active, std::string* error = nullptr) {
        if (!runtime_detail::IsTime(start) || !runtime_detail::IsTime(end)) {
            if (error) *error = "activity window must use HH:MM";
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        activity_window_ = {true, start, end, active};
        if (error) error->clear();
        return true;
    }

    void ClearActivityWindow() {
        std::lock_guard<std::mutex> lock(mutex_);
        activity_window_ = {};
    }

    bool SetPayoutCounters(const PayoutCounters& counters,
                           std::string* error = nullptr) {
        if (counters.matches_seen < 0 || counters.settlements_committed < 0 ||
            counters.settlements_failed < 0 || counters.payouts_committed < 0 ||
            counters.payouts_rejected < 0 || counters.idempotent_replays < 0) {
            if (error) *error = "payout counters must be nonnegative";
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        payout_counters_ = counters;
        if (error) error->clear();
        return true;
    }

    bool WriteHeartbeat(std::string* error = nullptr) const {
        if (!instance_valid_) {
            if (error) {
                std::lock_guard<std::mutex> lock(mutex_);
                *error = policy_error_;
            }
            return false;
        }

        std::string revision;
        std::string worker_revision;
        std::string last_error;
        RuntimeLimits limits;
        ActivityWindow window;
        PayoutCounters counters;
        bool policy_loaded = false;
        bool settlement_ready = false;
        bool wallet_healthy = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            revision = observed_revision_;
            worker_revision = wallet_worker_revision_;
            last_error = EffectiveErrorLocked();
            limits = limits_;
            window = activity_window_;
            counters = payout_counters_;
            policy_loaded = policy_loaded_;
            settlement_ready = settlement_ready_;
            wallet_healthy = wallet_worker_healthy_;
        }

        std::ostringstream json;
        json << "{\n"
             << "  \"schema_version\": 1,\n"
             << "  \"instance_id\": " << runtime_detail::JsonString(instance_id_)
             << ",\n"
             << "  \"component\": \"battle_reward\",\n"
             << "  \"heartbeat_utc\": "
             << runtime_detail::JsonString(runtime_detail::UtcNow()) << ",\n"
             << "  \"policy_revision\": "
             << runtime_detail::JsonString(revision) << ",\n"
             << "  \"policy_loaded\": " << (policy_loaded ? "true" : "false")
             << ",\n"
             << "  \"settlement_ready\": "
             << (settlement_ready ? "true" : "false") << ",\n"
             << "  \"wallet_worker_healthy\": "
             << (wallet_healthy ? "true" : "false");
        if (!worker_revision.empty()) {
            json << ",\n  \"wallet_worker_revision\": "
                 << runtime_detail::JsonString(worker_revision);
        }
        if (!last_error.empty()) {
            json << ",\n  \"last_error\": "
                 << runtime_detail::JsonString(last_error, 1024);
        } else {
            json << ",\n  \"last_error\": null";
        }
        json << ",\n  \"limits\": {\"max_gb\": " << limits.max_gb
             << ", \"max_mb\": " << limits.max_mb
             << ", \"max_base_multiplier\": "
             << runtime_detail::JsonString(runtime_detail::DecimalMicro(
                    limits.max_base_multiplier_micro))
             << "}";
        if (window.present) {
            json << ",\n  \"activity_window\": {\"start\": "
                 << runtime_detail::JsonString(window.start)
                 << ", \"end\": " << runtime_detail::JsonString(window.end)
                 << ", \"active\": " << (window.active ? "true" : "false")
                 << "}";
        }
        json << ",\n  \"payout_counters\": {"
             << "\"matches_seen\": " << counters.matches_seen
             << ", \"settlements_committed\": " << counters.settlements_committed
             << ", \"settlements_failed\": " << counters.settlements_failed
             << ", \"payouts_committed\": " << counters.payouts_committed
             << ", \"payouts_rejected\": " << counters.payouts_rejected
             << ", \"idempotent_replays\": " << counters.idempotent_replays
             << "}\n}\n";

        std::string write_error;
        bool written = false;
        {
            std::lock_guard<std::mutex> write_lock(write_mutex_);
            written =
                runtime_detail::AtomicWrite(status_path_, json.str(), &write_error);
        }
        if (error) *error = write_error;
        return written;
    }

private:
    std::string EffectiveErrorLocked() const {
        if (!policy_error_.empty()) return policy_error_;
        if (!settlement_error_.empty()) return settlement_error_;
        return wallet_error_;
    }

    const std::filesystem::path runtime_root_;
    const std::filesystem::path policy_path_;
    const std::filesystem::path status_path_;
    const std::string instance_id_;
    const bool instance_valid_;

    mutable std::mutex mutex_;
    mutable std::mutex write_mutex_;
    Policy policy_;
    RuntimeLimits limits_;
    ActivityWindow activity_window_;
    PayoutCounters payout_counters_;
    std::string observed_revision_;
    std::string policy_error_;
    std::string settlement_error_;
    std::string wallet_error_;
    std::string wallet_worker_revision_;
    bool policy_loaded_ = false;
    bool settlement_ready_ = false;
    bool wallet_worker_healthy_ = false;
};

}  // namespace battle_rewards
}  // namespace sdgo
