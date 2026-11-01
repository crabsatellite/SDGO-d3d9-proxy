#include <cstdio>
#include "mods/battle_rewards/combat_tracker.h"
using namespace sdgo::battle_rewards;
namespace {
int failures = 0;
void Check(bool value, const char* message) { printf("%s %s\n", value ? "ok  " : "FAIL", message); if (!value) ++failures; }
CombatEvent Hit(uint32_t actor, uint32_t victim, uint32_t loss, bool death = false, bool npc = false, bool boss = false) {
    return CombatEvent{actor, victim, 1, 2, loss, death, npc, boss};
}
}
int main() {
    CombatTracker pvp;
    Check(pvp.Record(Hit(11, 20, 200), Mode::Pvp) && pvp.Record(Hit(12, 20, 800, true), Mode::Pvp), "record only effective native HP losses");
    Check(pvp.Find(11).damage == 200 && pvp.Find(11).assists == 1 && pvp.Find(12).kills == 1 && pvp.Find(12).assists == 0,
          "kill and qualifying assist are distinct while both attackers receive damage credit");
    Check(pvp.Record(Hit(12, 20, 0, true), Mode::Pvp) && pvp.Find(12).kills == 1, "zero remaining HP cannot produce a second kill");
    Check(pvp.Record(Hit(11, 20, 1000, true), Mode::Pvp) && pvp.Find(11).kills == 1, "respawned victim can contribute a new native life");
    CombatEvent allied = Hit(11, 12, 500); allied.victim_team = 1;
    pvp.Record(allied, Mode::Pvp); pvp.Record(Hit(11, 11, 500), Mode::Pvp);
    Check(pvp.Find(11).damage == 1200, "self and allied damage are excluded");
    CombatTracker mission;
    mission.Record(Hit(11, 0x80000001, 600, false, true, true), Mode::Pve);
    mission.Record(Hit(12, 0x80000001, 400, true, true, true), Mode::Pve);
    Check(mission.BossesDefeated() == 1 && mission.Find(11).boss_credit_micro == 600000 && mission.Find(12).boss_credit_micro == 400000,
          "Boss payout follows effective damage contribution rather than the last hit");
    mission.Record(Hit(12, 0x80000002, 100, true, true), Mode::Pve);
    Check(mission.Find(12).minions == 1 && mission.Find(11).minions == 0, "ordinary mission NPC kill is counted separately");
    ParticipantStats stats; stats.kills = 7;
    Check(mission.Apply(11, &stats) && stats.kills == 7 && stats.damage == 600 && stats.boss_credit_micro == 600000,
          "tracker augments statistics without replacing native final PvP kills");
    CombatTracker invalid;
    Check(!invalid.Record(Hit(11, 20, 1, true, false, true), Mode::Pve) && !invalid.Valid(), "unverified NPC/Boss classification fails closed");
    puts(failures ? "COMBAT TRACKER CHECKS FAILED" : "ALL COMBAT TRACKER CHECKS PASSED");
    return failures ? 1 : 0;
}
