// Current BattleServer SHA256 f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4.
// EBP-relative fields after native bonus aggregation, before unit EXP and DB/client records.
// Order: pilot EXP, G points, unit EXP, and their native event bonus buckets.
#pragma once
#include <array>
#include <cstddef>
namespace sdgo::rewards {
    struct Site { const char* symbol; std::array<ptrdiff_t, 6> fields; };
    inline constexpr std::array<Site, 10> kSites = {{
        {"battle.reward.final.personal", {-0x770, -0x28, -0x89C, -0x18, -0x87C, -0x844}},
        {"battle.reward.final.team", {-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
        {"battle.reward.final.leader", {-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
        {"battle.reward.final.ai", {-0x760, -0x24, -0x908, -0x14, -0x83C, -0x834}},
        {"battle.reward.final.ai_ship", {-0x764, -0x24, -0x918, -0x14, -0x844, -0x83C}},
        {"battle.reward.final.mode13", {-0x760, -0x20, -0x90C, -0x10, -0x83C, -0x834}},
        {"battle.reward.final.mode14", {-0x760, -0x20, -0x90C, -0x10, -0x83C, -0x834}},
        {"battle.reward.final.tag", {-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
        {"battle.reward.final.death_grid", {-0x758, -0x20, -0x90C, -0x10, -0x834, -0x82C}},
        {"battle.reward.final.mission", {-0x760, -0x20, -0x860, -0x10, -0x840, -0x834}},
    }};
}
