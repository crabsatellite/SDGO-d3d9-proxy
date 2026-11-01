// Build one immutable batch before any participant's native amount is changed.
// Collection and wallet delivery live at their verified native boundaries.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include "sdgo/battle_rewards_policy.h"

namespace sdgo::battle_rewards {
struct CollectedParticipant {
    uint32_t account = 0;
    uint32_t native_slot = 0;
    bool native_eligible = false;
    Outcome outcome = Outcome::Lose;
    ParticipantStats stats;
};

struct PlannedParticipant {
    uint32_t account = 0;
    uint32_t native_slot = 0;
    bool native_eligible = false;
    Reward amount;
};

struct SettlementPlan {
    Policy policy;
    MatchContext context;
    std::array<PlannedParticipant, 12> participants{};
    unsigned count = 0;

    const PlannedParticipant* Find(uint32_t account, uint32_t slot) const {
        for (unsigned i = 0; i < count; ++i)
            if (participants[i].account == account && participants[i].native_slot == slot)
                return &participants[i];
        return nullptr;
    }
};

inline bool PrepareSettlement(const Policy& policy, MatchContext context,
                              const CollectedParticipant* collected, unsigned count,
                              SettlementPlan* plan, Error* error = nullptr) {
    if (!plan || !collected || !count || count > 12)
        return detail::Fail(error, ErrorCode::InvalidCount, "settlement needs 1..12 collected participants");
    SettlementPlan candidate;
    candidate.policy = policy;
    candidate.context = context;
    candidate.count = count;
    unsigned humans = 0;
    uint64_t bossCredit = 0;
    for (unsigned i = 0; i < count; ++i) {
        const auto& p = collected[i];
        if (p.native_slot >= 12)
            return detail::Fail(error, ErrorCode::InvalidCount, "native slot is outside 0..11");
        if (p.stats.kind == ParticipantKind::Human && (!p.account || p.account >= 0x80000000u))
            return detail::Fail(error, ErrorCode::InvalidCount, "human account is invalid");
        for (unsigned previous = 0; previous < i; ++previous) {
            if (collected[previous].native_slot == p.native_slot ||
                (p.stats.kind == ParticipantKind::Human &&
                 collected[previous].stats.kind == ParticipantKind::Human && collected[previous].account == p.account))
                return detail::Fail(error, ErrorCode::InvalidCount, "duplicate native slot or wallet account");
        }
        if (p.stats.kind == ParticipantKind::Human && p.native_eligible &&
            p.stats.active_seconds >= policy.minimum_active_seconds) ++humans;
        bossCredit += p.stats.boss_credit_micro;
    }
    if (context.mode == Mode::Pve && bossCredit > static_cast<uint64_t>(context.bosses_defeated) * kMicro)
        return detail::Fail(error, ErrorCode::InvalidBossCredit, "aggregate boss credit exceeds defeated bosses");
    candidate.context.human_count = humans ? humans : 1;
    for (unsigned i = 0; i < count; ++i) {
        const auto& p = collected[i];
        auto oneContext = candidate.context;
        oneContext.outcome = p.outcome;
        Reward amount;
        if (!Calculate(candidate.policy, oneContext, p.stats, &amount, error, NativeSettlementLimits())) return false;
        if (!p.native_eligible) {
            amount.eligible = false;
            amount.gb = amount.mb = 0;
        }
        candidate.participants[i] = {p.account, p.native_slot, p.native_eligible, amount};
    }
    *plan = candidate; // never publish a partial batch if any participant failed validation
    return true;
}
}
