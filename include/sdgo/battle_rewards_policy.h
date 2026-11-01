#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sdgo {
namespace battle_rewards {

constexpr uint64_t kMicro = 1000000ULL;
constexpr int32_t kWalletMaximum = 2147483647;
constexpr int32_t kNativeGpFieldMaximum = 65535;

struct CurrencyRate {
    uint64_t gb_micro = 0;
    uint64_t mb_micro = 0;
};

struct PvpPolicy {
    CurrencyRate base_win;
    CurrencyRate base_lose;
    CurrencyRate base_draw;
    CurrencyRate kill;
    CurrencyRate assist;
    CurrencyRate damage_per_1000;
    CurrencyRate objective;
    CurrencyRate contribution_cap;
    CurrencyRate mvp;
};

struct PvePolicy {
    CurrencyRate base_clear;
    CurrencyRate base_fail;
    CurrencyRate minion;
    CurrencyRate boss_credit;
    CurrencyRate damage_per_1000;
    CurrencyRate objective;
    CurrencyRate contribution_cap;
    CurrencyRate total_cap_1x;
    uint32_t difficulty_normal_micro = 0;
    uint32_t difficulty_hard_micro = 0;
    uint32_t difficulty_expert_micro = 0;
};

struct Policy {
    uint32_t schema_version = 0;
    uint32_t base_multiplier_micro = 0;
    uint32_t extra_human_multiplier_micro = 0;
    uint32_t full_reward_seconds = 0;
    uint32_t minimum_active_seconds = 0;
    std::array<char, 65> revision{};
    PvpPolicy pvp;
    PvePolicy pve;
};

enum class Mode : uint8_t { Pvp, Pve };
enum class Outcome : uint8_t { Win, Lose, Draw, Clear, Fail };
enum class Difficulty : uint8_t { Normal, Hard, Expert };
enum class ParticipantKind : uint8_t { Human, Ai, Observer };
enum class EligibilityReason : uint8_t { Eligible, AiOrObserver, TooShort };

struct MatchContext {
    Mode mode = Mode::Pvp;
    Outcome outcome = Outcome::Win;
    Difficulty difficulty = Difficulty::Normal;
    uint32_t human_count = 1;
    uint32_t event_multiplier = 1;
    uint32_t duration_seconds = 0;
    uint32_t bosses_defeated = 0;
};

struct ParticipantStats {
    ParticipantKind kind = ParticipantKind::Human;
    uint32_t active_seconds = 0;
    uint32_t kills = 0;
    uint32_t assists = 0;
    uint32_t minion_kills = 0;
    uint32_t damage = 0;
    uint32_t objectives = 0;
    uint32_t boss_credit_micro = 0;
    bool is_mvp = false;
};

struct DeliveryLimits {
    int32_t max_gb = kWalletMaximum;
    int32_t max_mb = kWalletMaximum;
};

inline DeliveryLimits NativeSettlementLimits() {
    return DeliveryLimits{kNativeGpFieldMaximum, kWalletMaximum};
}

struct Reward {
    bool eligible = false;
    EligibilityReason reason = EligibilityReason::TooShort;
    int32_t gb = 0;
    int32_t mb = 0;
};

enum class ErrorCode : uint8_t {
    None,
    InvalidPolicy,
    InvalidMode,
    InvalidOutcome,
    InvalidDifficulty,
    InvalidParticipantKind,
    InvalidHumanCount,
    InvalidEventMultiplier,
    InvalidDuration,
    InvalidCount,
    InvalidBossCredit,
    ArithmeticOverflow,
    WalletRange
};

struct Error {
    ErrorCode code = ErrorCode::None;
    const char* message = "";
};

namespace detail {

class UInt256 {
public:
    UInt256() = default;
    explicit UInt256(uint64_t value) {
        words_[0] = static_cast<uint32_t>(value);
        words_[1] = static_cast<uint32_t>(value >> 32);
    }

    bool Add(const UInt256& other) {
        uint64_t carry = 0;
        for (size_t i = 0; i < words_.size(); ++i) {
            const uint64_t sum = static_cast<uint64_t>(words_[i]) + other.words_[i] + carry;
            words_[i] = static_cast<uint32_t>(sum);
            carry = sum >> 32;
        }
        return carry == 0;
    }

    bool Multiply(uint32_t factor) {
        uint64_t carry = 0;
        for (size_t i = 0; i < words_.size(); ++i) {
            const uint64_t product = static_cast<uint64_t>(words_[i]) * factor + carry;
            words_[i] = static_cast<uint32_t>(product);
            carry = product >> 32;
        }
        return carry == 0;
    }

    uint32_t Divide(uint32_t divisor) {
        uint64_t remainder = 0;
        for (size_t i = words_.size(); i-- > 0;) {
            const uint64_t current = (remainder << 32) | words_[i];
            words_[i] = static_cast<uint32_t>(current / divisor);
            remainder = current % divisor;
        }
        return static_cast<uint32_t>(remainder);
    }

    int Compare(const UInt256& other) const {
        for (size_t i = words_.size(); i-- > 0;) {
            if (words_[i] < other.words_[i]) return -1;
            if (words_[i] > other.words_[i]) return 1;
        }
        return 0;
    }

    bool ToUint64(uint64_t* value) const {
        if (!value) return false;
        for (size_t i = 2; i < words_.size(); ++i) {
            if (words_[i] != 0) return false;
        }
        *value = static_cast<uint64_t>(words_[0]) |
                 (static_cast<uint64_t>(words_[1]) << 32);
        return true;
    }

private:
    std::array<uint32_t, 8> words_{};
};

inline bool Fail(Error* error, ErrorCode code, const char* message) {
    if (error) {
        error->code = code;
        error->message = message;
    }
    return false;
}

inline uint64_t Rate(const CurrencyRate& rate, bool mb) {
    return mb ? rate.mb_micro : rate.gb_micro;
}

inline bool ValidRate(const CurrencyRate& rate) {
    constexpr uint64_t kMaximumRateMicro = 1000000ULL * kMicro;
    return rate.gb_micro <= kMaximumRateMicro && rate.mb_micro <= kMaximumRateMicro;
}

inline bool Product(uint64_t initial, const uint32_t* factors, size_t count,
                    UInt256* result, Error* error) {
    UInt256 value(initial);
    for (size_t i = 0; i < count; ++i) {
        if (!value.Multiply(factors[i])) {
            return Fail(error, ErrorCode::ArithmeticOverflow,
                        "reward arithmetic exceeded 256 bits");
        }
    }
    *result = value;
    return true;
}

inline bool Add(UInt256* destination, const UInt256& value, Error* error) {
    if (!destination->Add(value)) {
        return Fail(error, ErrorCode::ArithmeticOverflow,
                    "reward arithmetic exceeded 256 bits");
    }
    return true;
}

inline bool PicoRate(uint64_t rate_micro, UInt256* result, Error* error) {
    const uint32_t factors[] = {static_cast<uint32_t>(kMicro)};
    return Product(rate_micro, factors, 1, result, error);
}

inline bool IntegerContribution(uint64_t rate_micro, uint32_t units,
                                UInt256* result, Error* error) {
    const uint32_t factors[] = {units, static_cast<uint32_t>(kMicro)};
    return Product(rate_micro, factors, 2, result, error);
}

inline bool DamageContribution(uint64_t rate_micro, uint32_t damage,
                               UInt256* result, Error* error) {
    const uint32_t factors[] = {damage, 1000};
    return Product(rate_micro, factors, 2, result, error);
}

inline bool BossContribution(uint64_t rate_micro, uint32_t credit_micro,
                             UInt256* result, Error* error) {
    const uint32_t factors[] = {credit_micro};
    return Product(rate_micro, factors, 1, result, error);
}

inline bool CalculateCurrency(const Policy& policy, const MatchContext& context,
                              const ParticipantStats& stats, bool mb,
                              uint64_t* amount, Error* error) {
    const bool pvp = context.mode == Mode::Pvp;
    const CurrencyRate* base = nullptr;
    if (pvp) {
        if (context.outcome == Outcome::Win) base = &policy.pvp.base_win;
        else if (context.outcome == Outcome::Lose) base = &policy.pvp.base_lose;
        else if (context.outcome == Outcome::Draw) base = &policy.pvp.base_draw;
    } else {
        if (context.outcome == Outcome::Clear) base = &policy.pve.base_clear;
        else if (context.outcome == Outcome::Fail) base = &policy.pve.base_fail;
    }
    if (!base) {
        return Fail(error, ErrorCode::InvalidOutcome,
                    "outcome does not belong to the selected mode");
    }

    UInt256 base_value;
    if (!PicoRate(Rate(*base, mb), &base_value, error)) return false;

    UInt256 contribution;
    UInt256 component;
    if (pvp) {
        if (!IntegerContribution(Rate(policy.pvp.kill, mb), stats.kills,
                                 &component, error) ||
            !Add(&contribution, component, error) ||
            !IntegerContribution(Rate(policy.pvp.assist, mb), stats.assists,
                                 &component, error) ||
            !Add(&contribution, component, error) ||
            !DamageContribution(Rate(policy.pvp.damage_per_1000, mb), stats.damage,
                                &component, error) ||
            !Add(&contribution, component, error) ||
            !IntegerContribution(Rate(policy.pvp.objective, mb), stats.objectives,
                                 &component, error) ||
            !Add(&contribution, component, error)) {
            return false;
        }
    } else {
        if (!IntegerContribution(Rate(policy.pve.minion, mb), stats.minion_kills,
                                 &component, error) ||
            !Add(&contribution, component, error) ||
            !BossContribution(Rate(policy.pve.boss_credit, mb), stats.boss_credit_micro,
                              &component, error) ||
            !Add(&contribution, component, error) ||
            !DamageContribution(Rate(policy.pve.damage_per_1000, mb), stats.damage,
                                &component, error) ||
            !Add(&contribution, component, error) ||
            !IntegerContribution(Rate(policy.pve.objective, mb), stats.objectives,
                                 &component, error) ||
            !Add(&contribution, component, error)) {
            return false;
        }
    }

    UInt256 contribution_cap;
    const CurrencyRate& cap = pvp ? policy.pvp.contribution_cap
                                  : policy.pve.contribution_cap;
    if (!PicoRate(Rate(cap, mb), &contribution_cap, error)) return false;
    if (contribution.Compare(contribution_cap) > 0) contribution = contribution_cap;

    UInt256 raw = base_value;
    if (!Add(&raw, contribution, error)) return false;
    if (pvp) {
        const bool meaningful = stats.kills != 0 || stats.assists != 0 ||
                                stats.damage != 0 || stats.objectives != 0;
        if (meaningful && stats.is_mvp) {
            UInt256 mvp;
            if (!PicoRate(Rate(policy.pvp.mvp, mb), &mvp, error) ||
                !Add(&raw, mvp, error)) {
                return false;
            }
        }
    }

    uint32_t raw_scale_divisions = 2;
    if (!pvp) {
        uint32_t difficulty = policy.pve.difficulty_normal_micro;
        if (context.difficulty == Difficulty::Hard) {
            difficulty = policy.pve.difficulty_hard_micro;
        } else if (context.difficulty == Difficulty::Expert) {
            difficulty = policy.pve.difficulty_expert_micro;
        }
        if (!raw.Multiply(difficulty)) {
            return Fail(error, ErrorCode::ArithmeticOverflow,
                        "reward arithmetic exceeded 256 bits");
        }
        UInt256 total_cap;
        const uint32_t cap_factors[] = {
            static_cast<uint32_t>(kMicro), static_cast<uint32_t>(kMicro)
        };
        if (!Product(Rate(policy.pve.total_cap_1x, mb), cap_factors, 2,
                     &total_cap, error)) {
            return false;
        }
        if (raw.Compare(total_cap) > 0) raw = total_cap;
        raw_scale_divisions = 3;
    }

    if (!raw.Multiply(policy.base_multiplier_micro) ||
        !raw.Multiply(context.event_multiplier)) {
        return Fail(error, ErrorCode::ArithmeticOverflow,
                    "reward arithmetic exceeded 256 bits");
    }
    for (uint32_t i = 1; i < context.human_count; ++i) {
        if (!raw.Multiply(11)) {
            return Fail(error, ErrorCode::ArithmeticOverflow,
                        "reward arithmetic exceeded 256 bits");
        }
    }
    const uint32_t active_for_reward =
        stats.active_seconds < policy.full_reward_seconds
            ? stats.active_seconds
            : policy.full_reward_seconds;
    if (!raw.Multiply(active_for_reward)) {
        return Fail(error, ErrorCode::ArithmeticOverflow,
                    "reward arithmetic exceeded 256 bits");
    }

    for (uint32_t i = 0; i < raw_scale_divisions; ++i) {
        raw.Divide(static_cast<uint32_t>(kMicro));
    }
    raw.Divide(static_cast<uint32_t>(kMicro));  // base multiplier scale
    for (uint32_t i = 1; i < context.human_count; ++i) raw.Divide(10);
    raw.Divide(policy.full_reward_seconds);

    if (!raw.ToUint64(amount)) {
        return Fail(error, ErrorCode::ArithmeticOverflow,
                    "reward amount does not fit in 64 bits");
    }
    return true;
}

}  // namespace detail

inline Policy DefaultPolicy() {
    Policy policy;
    policy.schema_version = 1;
    policy.base_multiplier_micro = 10 * static_cast<uint32_t>(kMicro);
    policy.extra_human_multiplier_micro = 1100000;
    policy.full_reward_seconds = 180;
    policy.minimum_active_seconds = 30;
    policy.pvp.base_win = {500 * kMicro, 2 * kMicro};
    policy.pvp.base_lose = {300 * kMicro, 1 * kMicro};
    policy.pvp.base_draw = {400 * kMicro, 1500000};
    policy.pvp.kill = {60 * kMicro, 200000};
    policy.pvp.assist = {20 * kMicro, 100000};
    policy.pvp.damage_per_1000 = {15 * kMicro, 100000};
    policy.pvp.objective = {0, 0};
    policy.pvp.contribution_cap = {400 * kMicro, 2 * kMicro};
    policy.pvp.mvp = {200 * kMicro, 6 * kMicro};
    policy.pve.base_clear = {100 * kMicro, 0};
    policy.pve.base_fail = {35 * kMicro, 0};
    policy.pve.minion = {2 * kMicro, 0};
    policy.pve.boss_credit = {45 * kMicro, 500000};
    policy.pve.damage_per_1000 = {1 * kMicro, 0};
    policy.pve.objective = {0, 0};
    policy.pve.contribution_cap = {150 * kMicro, 1500000};
    policy.pve.total_cap_1x = {300 * kMicro, 2 * kMicro};
    policy.pve.difficulty_normal_micro = 1000000;
    policy.pve.difficulty_hard_micro = 1250000;
    policy.pve.difficulty_expert_micro = 1500000;
    return policy;
}

inline bool ValidatePolicy(const Policy& policy, Error* error = nullptr) {
    if (error) *error = {};
    if (policy.schema_version != 1 ||
        policy.base_multiplier_micro > 100 * kMicro ||
        policy.extra_human_multiplier_micro != 1100000 ||
        policy.full_reward_seconds == 0 ||
        policy.full_reward_seconds > 3600 ||
        policy.minimum_active_seconds == 0 ||
        policy.minimum_active_seconds > policy.full_reward_seconds) {
        return detail::Fail(error, ErrorCode::InvalidPolicy,
                            "policy metadata or timing is invalid");
    }
    const CurrencyRate rates[] = {
        policy.pvp.base_win, policy.pvp.base_lose, policy.pvp.base_draw,
        policy.pvp.kill, policy.pvp.assist, policy.pvp.damage_per_1000,
        policy.pvp.objective, policy.pvp.contribution_cap, policy.pvp.mvp,
        policy.pve.base_clear, policy.pve.base_fail, policy.pve.minion,
        policy.pve.boss_credit, policy.pve.damage_per_1000,
        policy.pve.objective, policy.pve.contribution_cap,
        policy.pve.total_cap_1x
    };
    for (const CurrencyRate& rate : rates) {
        if (!detail::ValidRate(rate)) {
            return detail::Fail(error, ErrorCode::InvalidPolicy,
                                "policy currency rate exceeds 1000000");
        }
    }
    const uint32_t difficulties[] = {
        policy.pve.difficulty_normal_micro,
        policy.pve.difficulty_hard_micro,
        policy.pve.difficulty_expert_micro
    };
    for (uint32_t difficulty : difficulties) {
        if (difficulty == 0 || difficulty > 3 * kMicro) {
            return detail::Fail(error, ErrorCode::InvalidPolicy,
                                "PVE difficulty multiplier is outside 0..3");
        }
    }
    return true;
}

inline bool Calculate(const Policy& policy, const MatchContext& context,
                      const ParticipantStats& stats, Reward* reward,
                      Error* error = nullptr,
                      DeliveryLimits limits = DeliveryLimits{}) {
    if (error) *error = {};
    if (!reward) {
        return detail::Fail(error, ErrorCode::InvalidPolicy,
                            "reward output is required");
    }
    *reward = {};
    if (!ValidatePolicy(policy, error)) return false;
    if (context.mode != Mode::Pvp && context.mode != Mode::Pve) {
        return detail::Fail(error, ErrorCode::InvalidMode,
                            "mode must be PVP or PVE");
    }
    if (stats.kind != ParticipantKind::Human &&
        stats.kind != ParticipantKind::Ai &&
        stats.kind != ParticipantKind::Observer) {
        return detail::Fail(error, ErrorCode::InvalidParticipantKind,
                            "participant kind is invalid");
    }
    if (context.human_count == 0 || context.human_count > 12) {
        return detail::Fail(error, ErrorCode::InvalidHumanCount,
                            "qualifying human count must be 1..12");
    }
    if (context.event_multiplier != 1 && context.event_multiplier != 2) {
        return detail::Fail(error, ErrorCode::InvalidEventMultiplier,
                            "event multiplier must be 1 or 2");
    }
    if (stats.active_seconds > 86400 || context.duration_seconds > 86400 ||
        stats.active_seconds > context.duration_seconds) {
        return detail::Fail(error, ErrorCode::InvalidDuration,
                            "active duration exceeds match duration");
    }
    if (stats.kills > 255 || stats.assists > 255 ||
        stats.minion_kills > 10000 || stats.damage > 100000000 ||
        stats.objectives > 1000 || context.bosses_defeated > 100) {
        return detail::Fail(error, ErrorCode::InvalidCount,
                            "authoritative battle statistic is outside its bound");
    }
    if (stats.boss_credit_micro > 100 * kMicro ||
        static_cast<uint64_t>(stats.boss_credit_micro) >
            static_cast<uint64_t>(context.bosses_defeated) * kMicro) {
        return detail::Fail(error, ErrorCode::InvalidBossCredit,
                            "boss credit exceeds defeated bosses");
    }
    if (context.mode == Mode::Pvp) {
        if (context.outcome != Outcome::Win &&
            context.outcome != Outcome::Lose &&
            context.outcome != Outcome::Draw) {
            return detail::Fail(error, ErrorCode::InvalidOutcome,
                                "PVP outcome must be win, lose or draw");
        }
    } else if (context.outcome != Outcome::Clear &&
               context.outcome != Outcome::Fail) {
        return detail::Fail(error, ErrorCode::InvalidOutcome,
                            "PVE outcome must be clear or fail");
    }
    if (context.mode == Mode::Pve &&
        context.difficulty != Difficulty::Normal &&
        context.difficulty != Difficulty::Hard &&
        context.difficulty != Difficulty::Expert) {
        return detail::Fail(error, ErrorCode::InvalidDifficulty,
                            "PVE difficulty is invalid");
    }
    if (limits.max_gb < 0 || limits.max_mb < 0) {
        return detail::Fail(error, ErrorCode::WalletRange,
                            "delivery limits must be nonnegative");
    }

    reward->reason =
        stats.kind != ParticipantKind::Human
            ? EligibilityReason::AiOrObserver
            : stats.active_seconds < policy.minimum_active_seconds
                  ? EligibilityReason::TooShort
                  : EligibilityReason::Eligible;
    reward->eligible = reward->reason == EligibilityReason::Eligible;

    uint64_t gb = 0;
    uint64_t mb = 0;
    if (!detail::CalculateCurrency(policy, context, stats, false, &gb, error) ||
        !detail::CalculateCurrency(policy, context, stats, true, &mb, error)) {
        return false;
    }
    if (!reward->eligible) {
        reward->gb = 0;
        reward->mb = 0;
        return true;
    }
    if (gb > static_cast<uint64_t>(kWalletMaximum) ||
        mb > static_cast<uint64_t>(kWalletMaximum) ||
        gb > static_cast<uint64_t>(limits.max_gb) ||
        mb > static_cast<uint64_t>(limits.max_mb)) {
        return detail::Fail(error, ErrorCode::WalletRange,
                            "reward exceeds the configured delivery field");
    }
    reward->gb = static_cast<int32_t>(gb);
    reward->mb = static_cast<int32_t>(mb);
    return true;
}

}  // namespace battle_rewards
}  // namespace sdgo
