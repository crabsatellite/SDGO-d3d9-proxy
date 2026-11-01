#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "../mods/battle_ai/native_capabilities.h"
#include "../mods/battle_ai/difficulty.h"
#include "../mods/battle_ai/native_defence.h"

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++failures;
}

template <typename T>
void Put(std::uint8_t* base, std::size_t offset, T value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

void PutPointer(std::uint8_t* base, std::size_t offset, std::uint8_t* value) {
    Put(base, offset, static_cast<std::uint32_t>(
        reinterpret_cast<std::uintptr_t>(value)));
}

void EncodeProtectedFloat(std::uint8_t* unit, std::size_t permutationOffset,
                          std::size_t storageOffset, std::size_t keyOffset,
                          float value) {
    constexpr std::uint8_t indices[4] = {15, 2, 11, 7};
    constexpr std::uint8_t key = 0xA6;
    std::uint8_t raw[4]{};
    std::memcpy(raw, &value, sizeof(raw));
    for (unsigned i = 0; i < 16; ++i)
        unit[storageOffset + i] = static_cast<std::uint8_t>(0x30 + i);
    for (unsigned i = 0; i < 4; ++i) {
        unit[permutationOffset + i] = indices[i];
        unit[storageOffset + indices[i]] = raw[i] ^ key;
    }
    unit[keyOffset] = key;
}

struct Fixture {
    std::uint8_t unit[0x3300]{};
    std::uint8_t definition[0x300]{};
    std::uint8_t fsm[0xA00]{};
    std::uint8_t weapons[3][0x700]{};

    Fixture() {
        PutPointer(unit, 0x9C4, definition);
        PutPointer(unit, 0x9CC, fsm);
        Put(definition, 0x000, std::uint32_t{10001});
        definition[0x247] = 3;
        definition[0x248] = 1;

        for (int slot = 0; slot < 3; ++slot) {
            const std::uint32_t id = static_cast<std::uint32_t>(41001 + slot);
            Put(definition, 0x249 + slot * 4, id);
            Put(weapons[slot], 0x00, id);
            weapons[slot][0x69] = 1;
            weapons[slot][0x6A] = slot == 0 ? 1 : (slot == 1 ? 2 : 3);
            weapons[slot][0x6B] = slot == 0 ? 1 : (slot == 1 ? 3 : 2);
            Put(weapons[slot], 0x6C, std::int32_t{1000 + slot * 100});
            Put(weapons[slot], 0x70,
                static_cast<std::int16_t>(slot == 2 ? 20 : 0));
            Put(weapons[slot], 0x72,
                static_cast<std::uint32_t>(slot == 0 ? 800 : 2800));
            Put(weapons[slot], 0x7C,
                static_cast<std::int16_t>(slot == 0 ? 0 : 10));
            Put(weapons[slot], 0x82,
                static_cast<std::int16_t>(slot == 2 ? 6 : 30));
            weapons[slot][0x8F] = 1;

            for (std::size_t bank : {std::size_t{0x24C}, std::size_t{0x56C}}) {
                const std::size_t state = bank + slot * 0xC8;
                Put(fsm, state, std::int16_t{10});
                Put(fsm, state + 8, std::int16_t{0});
                PutPointer(fsm, state + 0x0C, weapons[slot]);
            }
        }

        Put(fsm, 0x240, std::int32_t{0});
        PutPointer(fsm, 0x248, weapons[0]);
        Put(fsm, 0x004, static_cast<std::uint32_t>(
            reinterpret_cast<std::uintptr_t>(fsm + 0x20)));
        Put(unit, 0xCF0, std::int32_t{5});
        EncodeProtectedFloat(unit, 0xBCC, 0xBD0, 0xBE0, 120.0f);
        EncodeProtectedFloat(unit, 0xBE1, 0xBE5, 0xBF5, 60.0f);
    }
};

}  // namespace

int main() {
    static_assert(sizeof(void*) == 4, "GOnline capability fixture is Win32");
    Fixture fixture;
    Check(battle_ai::RoomDifficulty(L"!ai")==-1&&battle_ai::RoomDifficulty(L"usual room")==-1,
          "unmarked room defers to the Medium default");
    Check(battle_ai::RoomDifficulty(L"!ai hard")==2&&battle_ai::RoomDifficulty(L"!AI3 Easy")==0,
          "room command accepts full difficulty names and existing bot counts");
    Check(battle_ai::RoomDifficulty(L"!ai e3 a1 medium")==1&&battle_ai::RoomDifficulty(L"!ai lv4 hard")==2,
          "difficulty remains separate from NPC pool level and team counts");
    Check(battle_ai::RoomDifficulty(L"!aimer hard")==-1&&battle_ai::RoomDifficulty(L"!ai harder")==-1,
          "partial command and partial difficulty tokens are rejected");
    battle_ai::native::Capabilities capabilities{};
    Put(fixture.unit,0x2B70,500.0f);Put(fixture.unit,0x2B74,500.0f);
    Put(fixture.definition,0x145,std::int32_t{500});fixture.definition[0x149]=1;fixture.definition[0x14E]=4;
    auto shield=battle_ai::native::ReadShield(fixture.unit);
    Check(battle_ai::native::ShieldCovers(shield,2)&&!battle_ai::native::ShieldCovers(shield,1),
          "live native back-shield mode is rear coverage, not an invented front shield");
    Put(fixture.unit,0x2B70,0.0f);shield=battle_ai::native::ReadShield(fixture.unit);
    Check(shield.known&&!shield.intact&&!battle_ai::native::ShieldCovers(shield,2),
          "zero shield HP removes rear shield protection immediately");
    Put(fixture.unit,0x2B70,500.0f);fixture.definition[0x14E]=6;
    Check(battle_ai::native::ShieldCovers(battle_ai::native::ReadShield(fixture.unit),1),
          "active weapon/form coverage is read afresh instead of cached by machine ID");
    fixture.definition[0x14E]=9;
    Check(!battle_ai::native::ReadShield(fixture.unit).known,"unknown shield coverage is rejected");
    fixture.definition[0x14E]=0;

    Check(battle_ai::native::ReadCapabilities(fixture.unit, capabilities),
          "reads a complete standard-bank capability snapshot");
    Check(!capabilities.alternate && capabilities.selectedSlot == 0,
          "selected weapon slot is native zero-based");
    PutPointer(fixture.fsm,4,fixture.fsm+0x5C);
    PutPointer(fixture.fsm,8,fixture.fsm+0x20);
    Check(battle_ai::native::ReadCapabilities(fixture.unit,capabilities)&&capabilities.meleeBusy&&
          !battle_ai::native::detail::Neutral(fixture.fsm),
          "pending neutral never hides an actual active melee state");
    PutPointer(fixture.fsm,4,fixture.fsm+0x20);
    PutPointer(fixture.fsm,8,fixture.fsm+0x48);
    Check(battle_ai::native::ReadCapabilities(fixture.unit,capabilities)&&capabilities.meleeBusy&&
          !battle_ai::native::detail::Neutral(fixture.fsm),
          "pending knife tracking is busy even before native Entry");
    Put(fixture.fsm,8,std::uint32_t{0});
    Check(battle_ai::native::detail::Neutral(fixture.fsm),"actual neutral with empty pending queue admits a new action");
    Check(std::fabs(capabilities.boostFraction - 0.5f) < 0.0001f,
          "boost decoder uses the full sixteen-byte protected storage");
    Check(capabilities.weapons[0].valid &&
              capabilities.weapons[0].ammoReady &&
              capabilities.weapons[0].kind ==
                  battle_ai::native::WeaponKind::Melee,
          "zero-ammo ordinary melee remains ready");
    Check(capabilities.weapons[1].kind ==
              battle_ai::native::WeaponKind::Hitscan,
          "zero-speed unscripted ranged weapon is immediate");
    Check(std::fabs(capabilities.weapons[2].cycleMs - 100.0f) < 0.001f &&
              std::fabs(capabilities.weapons[2].projectileSpeed - 1500.0f) <
                  0.001f,
          "cooldown and exact default projectile speed use sixty-Hz units");

    fixture.unit[0xE34] = 1;
    Put(fixture.fsm, 0x56C + 0xC8, std::int16_t{0});
    Put(fixture.fsm, 0x240, std::int32_t{1});
    PutPointer(fixture.fsm, 0x248, fixture.weapons[1]);
    Check(battle_ai::native::ReadCapabilities(fixture.unit, capabilities) &&
              capabilities.alternate &&
              !capabilities.weapons[1].ammoReady,
          "alternate bank supplies its own live ammo and readiness");

    fixture.unit[0xE34] = 0;
    Put(fixture.fsm, 0x240, std::int32_t{1});
    PutPointer(fixture.fsm, 0x248, fixture.weapons[0]);
    Check(!battle_ai::native::ReadCapabilities(fixture.unit, capabilities),
          "queued weapon change is rejected until native binding commits");
    PutPointer(fixture.fsm, 0x248, fixture.weapons[1]);
    Check(battle_ai::native::ReadCapabilities(fixture.unit, capabilities),
          "committed selected record is accepted");

    Put(fixture.weapons[2], 0x621, std::uint16_t{'X'});
    Check(battle_ai::native::ReadCapabilities(fixture.unit, capabilities) &&
              capabilities.weapons[2].kind ==
                  battle_ai::native::WeaponKind::Projectile &&
              capabilities.weapons[2].projectileSpeed == 0.0f,
          "scripted projectile stays usable without guessed lead speed");

    Put(fixture.weapons[2], 0x621, std::uint16_t{0});
    fixture.weapons[2][0x6A] = 4;
    Check(battle_ai::native::ReadCapabilities(fixture.unit, capabilities) &&
              capabilities.weapons[2].valid &&
              capabilities.weapons[2].kind ==
                  battle_ai::native::WeaponKind::Other,
          "ordinary funnel remains valid for native fallback");

    std::printf("CAPABILITY CHECKS %s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
