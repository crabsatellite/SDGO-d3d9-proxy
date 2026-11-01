#pragma once

#include <cctype>
#include <cstdio>
#include <limits>
#include <map>
#include <sstream>
#include <string>

#include "sdgo/battle_rewards_policy.h"

namespace sdgo {
namespace battle_rewards {

struct PolicyConfigError {
    size_t line = 0;
    std::string message;
};

namespace config_detail {

inline std::string Trim(const std::string& value) {
    size_t first = 0;
    while (first < value.size() &&
           std::isspace(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    size_t last = value.size();
    while (last > first &&
           std::isspace(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return value.substr(first, last - first);
}

inline bool ConfigFail(PolicyConfigError* error, size_t line,
                       const std::string& message) {
    if (error) {
        error->line = line;
        error->message = message;
    }
    return false;
}

inline bool ParseUnsigned(const std::string& text, uint64_t maximum,
                          uint64_t* value) {
    if (text.empty() || !value) return false;
    uint64_t result = 0;
    for (char c : text) {
        if (c < '0' || c > '9') return false;
        const uint32_t digit = static_cast<uint32_t>(c - '0');
        if (result > maximum / 10 ||
            (result == maximum / 10 && digit > maximum % 10)) {
            return false;
        }
        result = result * 10 + digit;
    }
    *value = result;
    return true;
}

inline bool ParseDecimalMicro(const std::string& text, uint64_t maximum_units,
                              uint64_t* value) {
    if (text.empty() || !value) return false;
    const size_t dot = text.find('.');
    if (dot != std::string::npos && text.find('.', dot + 1) != std::string::npos) {
        return false;
    }
    const std::string whole_text =
        dot == std::string::npos ? text : text.substr(0, dot);
    const std::string fraction_text =
        dot == std::string::npos ? std::string() : text.substr(dot + 1);
    if (whole_text.empty() || fraction_text.size() > 6 ||
        (dot != std::string::npos && fraction_text.empty())) {
        return false;
    }
    uint64_t whole = 0;
    if (!ParseUnsigned(whole_text, maximum_units, &whole)) return false;
    uint64_t fraction = 0;
    if (!fraction_text.empty() &&
        !ParseUnsigned(fraction_text, 999999, &fraction)) {
        return false;
    }
    for (size_t i = fraction_text.size(); i < 6; ++i) fraction *= 10;
    if (whole == maximum_units && fraction != 0) return false;
    if (whole > (std::numeric_limits<uint64_t>::max() - fraction) / kMicro) {
        return false;
    }
    *value = whole * kMicro + fraction;
    return true;
}

inline bool ValidRevision(const std::string& revision) {
    if (revision.size() != 64) return false;
    for (char c : revision) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

class Values {
public:
    bool Parse(const std::string& text, PolicyConfigError* error) {
        std::istringstream input(text);
        std::string section;
        std::string line;
        size_t number = 0;
        while (std::getline(input, line)) {
            ++number;
            line = Trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#') continue;
            if (line.front() == '[' && line.back() == ']') {
                section = Trim(line.substr(1, line.size() - 2));
                if (section.empty()) {
                    return ConfigFail(error, number, "empty INI section");
                }
                continue;
            }
            const size_t equals = line.find('=');
            if (section.empty() || equals == std::string::npos) {
                return ConfigFail(error, number, "expected Section and Key=Value");
            }
            const std::string key = Trim(line.substr(0, equals));
            const std::string value = Trim(line.substr(equals + 1));
            if (key.empty() || value.empty()) {
                return ConfigFail(error, number, "empty INI key or value");
            }
            const std::string full_key = section + "." + key;
            if (!entries_.emplace(full_key, Entry{value, number}).second) {
                return ConfigFail(error, number, "duplicate key: " + full_key);
            }
        }
        return true;
    }

    bool Take(const char* section, const char* key, std::string* value,
              PolicyConfigError* error) {
        const std::string full_key = std::string(section) + "." + key;
        auto found = entries_.find(full_key);
        if (found == entries_.end()) {
            return ConfigFail(error, 0, "missing key: " + full_key);
        }
        *value = found->second.value;
        entries_.erase(found);
        return true;
    }

    bool TakeUnsigned(const char* section, const char* key, uint64_t maximum,
                      uint64_t* value, PolicyConfigError* error) {
        std::string text;
        if (!Take(section, key, &text, error)) return false;
        if (!ParseUnsigned(text, maximum, value)) {
            return ConfigFail(error, 0,
                              std::string("invalid integer: ") + section + "." + key);
        }
        return true;
    }

    bool TakeDecimal(const char* section, const char* key, uint64_t maximum,
                     uint64_t* value, PolicyConfigError* error) {
        std::string text;
        if (!Take(section, key, &text, error)) return false;
        if (!ParseDecimalMicro(text, maximum, value)) {
            return ConfigFail(error, 0,
                              std::string("invalid decimal: ") + section + "." + key);
        }
        return true;
    }

    bool Empty() const { return entries_.empty(); }
    std::string FirstKey() const {
        return entries_.empty() ? std::string() : entries_.begin()->first;
    }

private:
    struct Entry {
        std::string value;
        size_t line;
    };
    std::map<std::string, Entry> entries_;
};

inline bool TakeRate(Values* values, const char* section, const char* prefix,
                     CurrencyRate* rate, PolicyConfigError* error) {
    const std::string gb = std::string(prefix) + "Gb";
    const std::string mb = std::string(prefix) + "Mb";
    return values->TakeDecimal(section, gb.c_str(), 1000000,
                               &rate->gb_micro, error) &&
           values->TakeDecimal(section, mb.c_str(), 1000000,
                               &rate->mb_micro, error);
}

inline bool ReadFile(FILE* file, std::string* text, PolicyConfigError* error) {
    if (!file || !text) return ConfigFail(error, 0, "unable to open policy file");
    if (std::fseek(file, 0, SEEK_END) != 0) {
        return ConfigFail(error, 0, "unable to seek policy file");
    }
    const long size = std::ftell(file);
    if (size < 0 || size > 131072 || std::fseek(file, 0, SEEK_SET) != 0) {
        return ConfigFail(error, 0, "policy file is invalid or too large");
    }
    text->assign(static_cast<size_t>(size), '\0');
    if (size != 0 &&
        std::fread(&(*text)[0], 1, static_cast<size_t>(size), file) !=
            static_cast<size_t>(size)) {
        return ConfigFail(error, 0, "unable to read complete policy file");
    }
    return true;
}

}  // namespace config_detail

inline bool ParsePolicyIni(const std::string& text, Policy* policy,
                           PolicyConfigError* error = nullptr) {
    if (error) *error = {};
    if (!policy) {
        return config_detail::ConfigFail(error, 0, "policy output is required");
    }
    config_detail::Values values;
    if (!values.Parse(text, error)) return false;

    Policy parsed;
    uint64_t integer = 0;
    uint64_t decimal = 0;
    std::string revision;
    if (!values.TakeUnsigned("meta", "SchemaVersion", 1, &integer, error) ||
        integer != 1) {
        return config_detail::ConfigFail(error, 0,
                                         "SchemaVersion must be 1");
    }
    parsed.schema_version = static_cast<uint32_t>(integer);
    if (!values.Take("meta", "PolicyRevision", &revision, error) ||
        !config_detail::ValidRevision(revision)) {
        return config_detail::ConfigFail(error, 0,
                                         "PolicyRevision must be 64 hex characters");
    }
    for (size_t i = 0; i < revision.size(); ++i) parsed.revision[i] = revision[i];

    if (!values.TakeUnsigned("timing", "FullRewardSeconds", 3600,
                             &integer, error)) {
        return false;
    }
    parsed.full_reward_seconds = static_cast<uint32_t>(integer);
    if (!values.TakeUnsigned("timing", "MinimumActiveSeconds", 3600,
                             &integer, error)) {
        return false;
    }
    parsed.minimum_active_seconds = static_cast<uint32_t>(integer);
    if (!values.TakeDecimal("multipliers", "Base", 100, &decimal, error)) {
        return false;
    }
    parsed.base_multiplier_micro = static_cast<uint32_t>(decimal);
    if (!values.TakeDecimal("multipliers", "ExtraHuman", 2,
                            &decimal, error)) {
        return false;
    }
    parsed.extra_human_multiplier_micro = static_cast<uint32_t>(decimal);

    if (!config_detail::TakeRate(&values, "pvp", "BaseWin",
                                 &parsed.pvp.base_win, error) ||
        !config_detail::TakeRate(&values, "pvp", "BaseLose",
                                 &parsed.pvp.base_lose, error) ||
        !config_detail::TakeRate(&values, "pvp", "BaseDraw",
                                 &parsed.pvp.base_draw, error) ||
        !config_detail::TakeRate(&values, "pvp", "Kill",
                                 &parsed.pvp.kill, error) ||
        !config_detail::TakeRate(&values, "pvp", "Assist",
                                 &parsed.pvp.assist, error) ||
        !config_detail::TakeRate(&values, "pvp", "DamagePer1000",
                                 &parsed.pvp.damage_per_1000, error) ||
        !config_detail::TakeRate(&values, "pvp", "Objective",
                                 &parsed.pvp.objective, error) ||
        !config_detail::TakeRate(&values, "pvp", "ContributionCap",
                                 &parsed.pvp.contribution_cap, error) ||
        !config_detail::TakeRate(&values, "pvp", "Mvp",
                                 &parsed.pvp.mvp, error) ||
        !config_detail::TakeRate(&values, "pve", "BaseClear",
                                 &parsed.pve.base_clear, error) ||
        !config_detail::TakeRate(&values, "pve", "BaseFail",
                                 &parsed.pve.base_fail, error) ||
        !config_detail::TakeRate(&values, "pve", "Minion",
                                 &parsed.pve.minion, error) ||
        !config_detail::TakeRate(&values, "pve", "BossCredit",
                                 &parsed.pve.boss_credit, error) ||
        !config_detail::TakeRate(&values, "pve", "DamagePer1000",
                                 &parsed.pve.damage_per_1000, error) ||
        !config_detail::TakeRate(&values, "pve", "Objective",
                                 &parsed.pve.objective, error) ||
        !config_detail::TakeRate(&values, "pve", "ContributionCap",
                                 &parsed.pve.contribution_cap, error) ||
        !config_detail::TakeRate(&values, "pve", "TotalCap1x",
                                 &parsed.pve.total_cap_1x, error)) {
        return false;
    }
    if (!values.TakeDecimal("pve", "DifficultyNormal", 3,
                            &decimal, error)) {
        return false;
    }
    parsed.pve.difficulty_normal_micro = static_cast<uint32_t>(decimal);
    if (!values.TakeDecimal("pve", "DifficultyHard", 3,
                            &decimal, error)) {
        return false;
    }
    parsed.pve.difficulty_hard_micro = static_cast<uint32_t>(decimal);
    if (!values.TakeDecimal("pve", "DifficultyExpert", 3,
                            &decimal, error)) {
        return false;
    }
    parsed.pve.difficulty_expert_micro = static_cast<uint32_t>(decimal);

    if (!values.Empty()) {
        return config_detail::ConfigFail(error, 0,
                                         "unknown key: " + values.FirstKey());
    }
    Error validation;
    if (!ValidatePolicy(parsed, &validation)) {
        return config_detail::ConfigFail(error, 0, validation.message);
    }
    *policy = parsed;
    return true;
}

inline bool LoadPolicyIniFile(const char* path, Policy* policy,
                              PolicyConfigError* error = nullptr) {
    if (error) *error = {};
    FILE* file = nullptr;
#ifdef _MSC_VER
    if (path) fopen_s(&file, path, "rb");
#else
    if (path) file = std::fopen(path, "rb");
#endif
    std::string text;
    const bool read = config_detail::ReadFile(file, &text, error);
    if (file) std::fclose(file);
    return read && ParsePolicyIni(text, policy, error);
}

#ifdef _WIN32
inline bool LoadPolicyIniFile(const wchar_t* path, Policy* policy,
                              PolicyConfigError* error = nullptr) {
    if (error) *error = {};
    FILE* file = nullptr;
#ifdef _MSC_VER
    if (path) _wfopen_s(&file, path, L"rb");
#else
    if (path) file = _wfopen(path, L"rb");
#endif
    std::string text;
    const bool read = config_detail::ReadFile(file, &text, error);
    if (file) std::fclose(file);
    return read && ParsePolicyIni(text, policy, error);
}
#endif

}  // namespace battle_rewards
}  // namespace sdgo
