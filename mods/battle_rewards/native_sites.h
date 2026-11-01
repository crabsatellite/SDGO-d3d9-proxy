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
    ptrdiff_t g_display_base;
    std::array<ptrdiff_t, 3> g_display_bonuses;
};
// Sources of result packet +18 (total), +1E (base), +24/+2A/+30 (legacy
// bonuses), and +36 (event) in the supported BattleServer. The native client's
// normal animation uses base + bonuses, rather than the total field used by
// wallet delivery and the skip-animation path.
inline constexpr std::array<SettlementSite, 10> kSettlementSites = {{
    {"battle.rewards.augment.personal", -0x28, -0x87C, -0x758, {{-0x10, -0x76C, -0x744}}},
    {"battle.rewards.augment.team", -0x20, -0x834, -0x744, {{-0x08, -0x754, -0x734}}},
    {"battle.rewards.augment.leader", -0x20, -0x834, -0x744, {{-0x08, -0x754, -0x734}}},
    {"battle.rewards.augment.ai", -0x24, -0x83C, -0x74C, {{-0x08, -0x75C, -0x73C}}},
    {"battle.rewards.augment.ai_ship", -0x24, -0x844, -0x74C, {{-0x08, -0x760, -0x73C}}},
    {"battle.rewards.augment.mode13", -0x20, -0x83C, -0x74C, {{-0x08, -0x75C, -0x73C}}},
    {"battle.rewards.augment.mode14", -0x20, -0x83C, -0x74C, {{-0x08, -0x75C, -0x73C}}},
    {"battle.rewards.augment.tag", -0x20, -0x834, -0x744, {{-0x08, -0x754, -0x734}}},
    {"battle.rewards.augment.death_grid", -0x20, -0x834, -0x744, {{-0x08, -0x754, -0x734}}},
    {"battle.rewards.augment.mission", -0x20, -0x840, -0x74C, {{-0x08, -0x75C, -0x73C}}},
}};
}
