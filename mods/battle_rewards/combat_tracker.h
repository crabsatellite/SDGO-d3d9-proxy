// Statistics are fed only by the native effective-HP-loss helper. This class
// never accepts HTTP/client telemetry as payout authority.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include "sdgo/battle_rewards_policy.h"

namespace sdgo::battle_rewards {
struct CombatEvent {
    uint32_t attacker = 0;
    uint32_t victim = 0;
    uint8_t attacker_team = 0;
    uint8_t victim_team = 0;
    uint32_t effective_damage = 0;
    bool death = false;
    bool victim_npc = false;
    bool victim_boss = false;
};

struct TrackedContribution {
    uint64_t damage = 0;
    uint32_t kills = 0;
    uint32_t assists = 0;
    uint32_t minions = 0;
    uint32_t boss_credit_micro = 0;
};

class CombatTracker {
    struct VictimLife {
        uint64_t total = 0;
        bool npc = false, boss = false;
        std::map<uint32_t, uint64_t> damage;
    };
    std::map<uint32_t, VictimLife> victims_;
    std::map<uint32_t, TrackedContribution> participants_;
    uint32_t bosses_ = 0;
    bool valid_ = true;
    std::string error_;

    bool Fail(const char* error) { valid_ = false; error_ = error; return false; }
public:
    bool Valid() const { return valid_; }
    const std::string& Error() const { return error_; }
    uint32_t BossesDefeated() const { return bosses_; }

    bool Record(const CombatEvent& event, Mode mode) {
        if (!valid_) return false;
        if (!event.attacker || !event.victim || event.attacker == event.victim || !event.effective_damage)
            return true;
        // Team zero is used by native free-for-all/mission entities and does
        // not prove an allied relationship. Equal nonzero teams are allies.
        if (event.attacker_team && event.attacker_team == event.victim_team) return true;
        if (event.victim_boss && !event.victim_npc) return Fail("boss classification requires a native NPC");
        if (participants_.size() >= 64 && participants_.find(event.attacker) == participants_.end())
            return Fail("native attacker count exceeded the supported match bound");
        auto& attacker = participants_[event.attacker];
        attacker.damage += event.effective_damage;
        if (attacker.damage > 100000000ULL) return Fail("effective damage exceeded policy statistic range");
        if (victims_.size() >= 4096 && victims_.find(event.victim) == victims_.end())
            return Fail("simultaneous native victim count exceeded the supported bound");
        auto& life = victims_[event.victim];
        if (life.total && (life.npc != event.victim_npc || life.boss != event.victim_boss))
            return Fail("native victim classification changed during one life");
        life.npc = event.victim_npc; life.boss = event.victim_boss;
        life.total += event.effective_damage;
        life.damage[event.attacker] += event.effective_damage;
        if (!event.death) return true;

        if (mode == Mode::Pvp) {
            if (++attacker.kills > 255) return Fail("kill count exceeded policy statistic range");
            // A kill assist needs at least 10% of that life's effective HP
            // loss. Last-hit gets the kill; it does not get a second assist.
            for (const auto& share : life.damage) {
                if (share.first != event.attacker && share.second * 10 >= life.total)
                    if (++participants_[share.first].assists > 255)
                        return Fail("assist count exceeded policy statistic range");
            }
        } else if (life.boss) {
            if (++bosses_ > 100) return Fail("boss count exceeded policy statistic range");
            // Shares are floored, so all recipients together can never claim
            // more than one credit for one Boss. No floating point is used.
            for (const auto& share : life.damage) {
                uint64_t credit = share.second * kMicro / life.total;
                if (credit > 100 * kMicro - participants_[share.first].boss_credit_micro)
                    return Fail("boss contribution exceeded policy statistic range");
                participants_[share.first].boss_credit_micro += static_cast<uint32_t>(credit);
            }
        } else if (life.npc) {
            if (++attacker.minions > 10000) return Fail("minion count exceeded policy statistic range");
        }
        victims_.erase(event.victim); // a respawned entity starts a new life
        return true;
    }

    TrackedContribution Find(uint32_t account) const {
        auto found = participants_.find(account);
        return found == participants_.end() ? TrackedContribution{} : found->second;
    }

    bool Apply(uint32_t account, ParticipantStats* stats) const {
        if (!stats || !valid_) return false;
        auto value = Find(account);
        stats->damage = static_cast<uint32_t>(value.damage);
        stats->assists = value.assists;
        stats->minion_kills = value.minions;
        stats->boss_credit_micro = value.boss_credit_micro;
        return true; // native final kill count remains authoritative in PvP
    }
};
}
