// Captured independently from the shipped BattleServer f74d1603...7b63a4.
// Do not derive these expected frame locations from the production Mod.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace native_reward_contract {
struct Site { const char* symbol; std::array<ptrdiff_t,6> fields; std::array<uint8_t,6> instruction; bool writesEcx; };
inline constexpr std::array<Site,10> sites = {{
{"battle.reward.final.personal", {-1904, -40, -2204, -24, -2172, -2116}, {0x8b, 0x85, 0xbc, 0xfd, 0xff, 0xff}, false},
{"battle.reward.final.team", {-1880, -32, -2308, -16, -2100, -2092}, {0x8b, 0x85, 0xcc, 0xfd, 0xff, 0xff}, false},
{"battle.reward.final.leader", {-1880, -32, -2308, -16, -2100, -2092}, {0x8b, 0x85, 0xcc, 0xfd, 0xff, 0xff}, false},
{"battle.reward.final.ai", {-1888, -36, -2312, -20, -2108, -2100}, {0x8b, 0x85, 0xc4, 0xfd, 0xff, 0xff}, false},
{"battle.reward.final.ai_ship", {-1892, -36, -2328, -20, -2116, -2108}, {0x8b, 0x85, 0xc4, 0xfd, 0xff, 0xff}, false},
{"battle.reward.final.mode13", {-1888, -32, -2316, -16, -2108, -2100}, {0x8b, 0x85, 0xcc, 0xfd, 0xff, 0xff}, false},
{"battle.reward.final.mode14", {-1888, -32, -2316, -16, -2108, -2100}, {0x8b, 0x85, 0xcc, 0xfd, 0xff, 0xff}, false},
{"battle.reward.final.tag", {-1880, -32, -2308, -16, -2100, -2092}, {0x8b, 0x85, 0xcc, 0xfd, 0xff, 0xff}, false},
{"battle.reward.final.death_grid", {-1880, -32, -2316, -16, -2100, -2092}, {0x8b, 0x8d, 0xcc, 0xfd, 0xff, 0xff}, true},
{"battle.reward.final.mission", {-1888, -32, -2144, -16, -2112, -2100}, {0x8b, 0x85, 0xcc, 0xfd, 0xff, 0xff}, false},
}};
}
