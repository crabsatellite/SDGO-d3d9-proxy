// Verified post-bonus boundaries in BattleServer f74d1603...7b63a4.
// These sites are six bytes after daily_rewards' boundaries, so the two mods
// never own the same instruction. This mod replaces G from its own 1x policy;
// daily_rewards may independently retain pilot/unit experience multipliers.
#pragma once
#include <array>
#include <cstddef>

namespace sdgo::battle_rewards {
struct SettlementSite {
    const char* symbol;
    ptrdiff_t g_amount;
    ptrdiff_t g_event;
};
inline constexpr std::array<SettlementSite, 10> kSettlementSites = {{
    {"battle.rewards.augment.personal", -0x28, -0x87C},
    {"battle.rewards.augment.team", -0x20, -0x834},
    {"battle.rewards.augment.leader", -0x20, -0x834},
    {"battle.rewards.augment.ai", -0x24, -0x83C},
    {"battle.rewards.augment.ai_ship", -0x24, -0x844},
    {"battle.rewards.augment.mode13", -0x20, -0x83C},
    {"battle.rewards.augment.mode14", -0x20, -0x83C},
    {"battle.rewards.augment.tag", -0x20, -0x834},
    {"battle.rewards.augment.death_grid", -0x20, -0x834},
    {"battle.rewards.augment.mission", -0x20, -0x840},
}};
}
