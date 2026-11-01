#include <cstdio>
#include <cstring>
#include "mods/battle_rewards/settlement_plan.h"
using namespace sdgo::battle_rewards;
namespace {
int failures = 0;
void Check(bool ok, const char* message) { printf("%s %s\n", ok ? "ok  " : "FAIL", message); if (!ok) ++failures; }
}
int main() {
    Policy policy = DefaultPolicy();
    MatchContext context; context.duration_seconds = 180; context.event_multiplier = 2;
    std::array<CollectedParticipant, 3> players{};
    for (unsigned i = 0; i < players.size(); ++i) {
        players[i].account = 100 + i;
        players[i].native_slot = i;
        players[i].native_eligible = true;
        players[i].outcome = Outcome::Win;
        players[i].stats.kind = ParticipantKind::Human;
        players[i].stats.active_seconds = 180;
        players[i].stats.kills = 3;
        players[i].stats.damage = 8000;
    }
    players[2].account = 0x80000001;
    players[2].stats.kind = ParticipantKind::Ai;
    SettlementPlan plan; Error error;
    Check(PrepareSettlement(policy, context, players.data(), 3, &plan, &error), "prepare the whole mixed human/AI match");
    Check(plan.context.human_count == 2, "AI accounts never multiply human attendance");
    Check(plan.Find(100, 0) && plan.Find(100, 0)->amount.gb == 17600, "both humans share one compounded event/policy snapshot");
    Check(plan.participants[2].amount.gb == 0 && plan.participants[2].amount.mb == 0, "AI receives no wallet amount");
    policy.base_multiplier_micro = 20000000;
    Check(plan.policy.base_multiplier_micro == 10000000, "editing the source policy cannot change an already prepared match");
    players[1].native_eligible = false;
    Check(PrepareSettlement(DefaultPolicy(), context, players.data(), 3, &plan, &error) && plan.context.human_count == 1 &&
          plan.participants[1].amount.gb == 0, "native eligibility is preserved and excluded from attendance");
    SettlementPlan previous = plan;
    players[1].account = 100;
    Check(!PrepareSettlement(DefaultPolicy(), context, players.data(), 3, &plan, &error) &&
          plan.context.human_count == previous.context.human_count, "duplicate accounts reject atomically");
    players[1].account = 101; players[1].native_slot = 0;
    Check(!PrepareSettlement(DefaultPolicy(), context, players.data(), 3, &plan, &error), "duplicate native slots reject");
    context.mode = Mode::Pve; context.bosses_defeated = 1;
    players[1].native_slot = 1;
    for (auto& p : players) p.outcome = Outcome::Clear;
    players[0].stats.boss_credit_micro = 600000;
    players[1].stats.boss_credit_micro = 600000;
    Check(!PrepareSettlement(DefaultPolicy(), context, players.data(), 3, &plan, &error), "aggregate boss contributions cannot claim the same kill twice");
    puts(failures ? "SETTLEMENT PLAN FAILED" : "ALL SETTLEMENT PLAN CHECKS PASSED");
    return failures ? 1 : 0;
}
