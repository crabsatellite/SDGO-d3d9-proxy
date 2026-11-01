#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "difficulty.h"

namespace battle_ai::loadout {

// Unit levels are one-based in the original client/server contract.  Zero is
// reserved here to mean "leave the native level unchanged".
constexpr std::uint8_t kPreserveNativeLevel = 0;
constexpr std::uint8_t kMediumLevel = 12;
constexpr std::uint8_t kHardExLevel = 13;

// These are the three player catalogue parts used by the Hard preset.  The
// server's source unit has exactly one optional SkillPartID at +0x52, which is
// resolved into the fourth native skill slot.  A loadout therefore contains
// at most one of these IDs.
constexpr std::uint32_t kNewtypeAwakening = 75002;
constexpr std::uint32_t kFighterTechnique = 75003;
constexpr std::uint32_t kSniperManual = 75014;

enum class UnitRole : std::uint8_t {
    Ranged,
    Melee,
    Sniper,
};

// Both client and server must build this profile from the unit definition and
// its native weapon records.  The profile deliberately contains no unit ID, so
// a caller cannot turn the preset into a per-machine allow-list.  Sniper
// eligibility is explicit because RangeType 3 alone is only a long-range
// family and also contains ordinary cannon/projectile weapons.
struct NativeProfile {
    UnitRole role = UnitRole::Ranged;
    bool lowFuel = false;
    bool sniperEligible = false;
};
struct WeaponProfile {
    std::uint8_t style=0,rangeType=0,maxCombo=0;
    std::int32_t damage=0,range=0;
    std::int16_t magazine=0;
};
inline NativeProfile ProfileFromWeapons(const WeaponProfile* weapons,std::size_t count) noexcept {
    NativeProfile profile;bool melee=false,ranged=false;double closeBurst=0,rangedBurst=0;
    if(!weapons||count>3) return profile;
    for(std::size_t i=0;i<count;++i) {
        const auto& w=weapons[i];if(w.style==2) continue;
        if(w.style<1||w.style>3||w.rangeType<1||w.rangeType>8) continue;
        double damage=w.damage>0?w.damage:0;
        if(w.rangeType==1) {
            melee=true;unsigned combo=w.maxCombo?w.maxCombo:1;if(combo>32) combo=32;
            double burst=damage*combo;if(burst>closeBurst) closeBurst=burst;
        } else {
            ranged=true;int rounds=w.magazine>0?w.magazine:1;if(rounds>3) rounds=3;
            double reach=w.range>2000?w.range/2000.0:1;if(reach>2) reach=2;
            double burst=damage*rounds*reach;if(burst>rangedBurst) rangedBurst=burst;
        }
    }
    // This is a conservative fixed-loadout preference, not a DPS model: the
    // server's compact weapon record has no CoolTime. Strong hybrid brawlers
    // can select Fighting; balanced/ranged units select the mobility part.
    if(melee&&(!ranged||(closeBurst>0&&closeBurst>rangedBurst*2))) profile.role=UnitRole::Melee;
    return profile;
}

using SourceNativeFunction =
    bool (*)(void* context, const void* nativeUnitDefinition,
             NativeProfile* profile);

struct NativeProfileSource {
    void* context = nullptr;
    SourceNativeFunction read = nullptr;
};

struct NativeLoadout {
    std::uint8_t level = kPreserveNativeLevel;
    std::uint32_t skillPartId = 0;

    constexpr bool operator==(const NativeLoadout& other) const noexcept {
        return level == other.level && skillPartId == other.skillPartId;
    }
    constexpr bool operator!=(const NativeLoadout& other) const noexcept {
        return !(*this == other);
    }
};

constexpr bool IsPresetPart(std::uint32_t skillPartId) noexcept {
    return skillPartId == 0 ||
           skillPartId == kNewtypeAwakening ||
           skillPartId == kFighterTechnique ||
           skillPartId == kSniperManual;
}

constexpr bool IsValid(const NativeLoadout& value) noexcept {
    if (value.level == kPreserveNativeLevel)
        return value.skillPartId == 0;
    if (value.level == kMediumLevel)
        return value.skillPartId == 0;
    return value.level == kHardExLevel &&
           value.skillPartId != 0 &&
           IsPresetPart(value.skillPartId);
}

constexpr UnitRole ClassifyRole(const NativeProfile& profile) noexcept {
    return profile.role == UnitRole::Sniper && !profile.sniperEligible
               ? UnitRole::Ranged
               : profile.role;
}

constexpr std::uint32_t SelectHardPart(const NativeProfile& profile) noexcept {
    // The mobility part wins for a definition identified as fuel-limited.
    // Otherwise the choice is a strict one-of-three mapping.
    if (profile.lowFuel)
        return kNewtypeAwakening;
    switch (ClassifyRole(profile)) {
    case UnitRole::Melee:
        return kFighterTechnique;
    case UnitRole::Sniper:
        return kSniperManual;
    case UnitRole::Ranged:
    default:
        return kNewtypeAwakening;
    }
}

constexpr NativeLoadout ForDifficulty(
    Difficulty difficulty, const NativeProfile& profile = {}) noexcept {
    switch (difficulty) {
    case Difficulty::Easy:
        return {};
    case Difficulty::Hard:
        return {kHardExLevel, SelectHardPart(profile)};
    case Difficulty::Medium:
    default:
        return {kMediumLevel, 0};
    }
}

inline bool FromNativeSource(Difficulty difficulty,
                             const NativeProfileSource& source,
                             const void* nativeUnitDefinition,
                             NativeLoadout* output) noexcept {
    if (!output)
        return false;
    if (difficulty != Difficulty::Hard) {
        *output = ForDifficulty(difficulty);
        return true;
    }
    if (!source.read || !nativeUnitDefinition)
        return false;
    NativeProfile profile{};
    if (!source.read(source.context, nativeUnitDefinition, &profile))
        return false;
    *output = ForDifficulty(difficulty, profile);
    return true;
}

#if defined(_MSC_VER)
#define BATTLE_AI_LOADOUT_THISCALL __thiscall
#elif defined(__i386__)
#define BATTLE_AI_LOADOUT_THISCALL __attribute__((thiscall))
#else
#define BATTLE_AI_LOADOUT_THISCALL
#endif

// GOnline.exe SHA256
// e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92
constexpr std::uintptr_t kClientSetUnitLevelAddress = 0x006705B0;
constexpr std::uintptr_t kClientApplySkillPartAddress = 0x006704F0;
constexpr std::uintptr_t kClientSkillServiceGetterAddress = 0x004ADA90;
constexpr std::uintptr_t kClientApplySourceAddress = 0x00AC9CD0;

using SetUnitLevelFn =
    void (BATTLE_AI_LOADOUT_THISCALL*)(std::uint8_t* unit, int level);
using ApplySkillPartFn =
    void (BATTLE_AI_LOADOUT_THISCALL*)(std::uint8_t* unit,
                                      std::uint32_t skillPartId);
using SkillServiceGetterFn =
    void* (BATTLE_AI_LOADOUT_THISCALL*)(void* serviceFacade);
using ApplySourceNativeFn =
    void (BATTLE_AI_LOADOUT_THISCALL*)(void* skillService,
                                      std::uint8_t* unit,
                                      const void* fullSourceRecord,
                                      const void* compactSourceRecord);

struct ClientNativeFunctions {
    SetUnitLevelFn setUnitLevel = nullptr;
    ApplySkillPartFn applySkillPart = nullptr;
};

// BattleServer.exe SHA256
// f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4
constexpr std::uintptr_t kServerSkillRegistryAddress = 0x0082B3AC;
constexpr std::uintptr_t kServerFindSkillAddress = 0x00462480;
constexpr std::size_t kServerBattleUnitLevelOffset = 0x0C;
constexpr std::size_t kServerBattleUnitFourthSkillOffset = 0x54;
constexpr std::size_t kServerBattleUnitFourthActiveOffset = 0x47;

using FindServerSkillFn =
    void (BATTLE_AI_LOADOUT_THISCALL*)(void* skillRegistry,
                                      std::uint32_t skillId,
                                      void** resolvedDefinition);

struct ServerNativeFunctions {
    void* skillRegistry = nullptr;
    FindServerSkillFn findSkill = nullptr;
};

#undef BATTLE_AI_LOADOUT_THISCALL

enum class ApplyResult : std::uint8_t {
    Applied,
    AlreadyApplied,
    InvalidArgument,
    InvalidLoadout,
    NativeUnavailable,
    SkillNotFound,
    ConflictingLoadout,
};

// Keep one state per native unit/deck generation.  0x006704F0 owns the single
// client skill-part object at unit+0xAC8: it destroys/frees the previous
// object, allocates the replacement, resolves the full 32-bit 750xx ID, binds
// it to the unit and activates it.  The fixed-generation gate keeps room
// difficulty immutable and prevents needless destroy/recreate churn.
struct ClientApplyState {
    const std::uint8_t* unit = nullptr;
    std::uint32_t generation = 0;
    NativeLoadout loadout{};
    bool applied = false;
};

struct ServerApplyState {
    const std::uint8_t* battleUnit = nullptr;
    std::uint32_t generation = 0;
    NativeLoadout loadout{};
    bool applied = false;
};

inline bool SameGeneration(const ClientApplyState& state,
                           const std::uint8_t* unit,
                           std::uint32_t generation) noexcept {
    return state.applied && state.unit == unit &&
           state.generation == generation;
}

inline bool SameGeneration(const ServerApplyState& state,
                           const std::uint8_t* battleUnit,
                           std::uint32_t generation) noexcept {
    return state.applied && state.battleUnit == battleUnit &&
           state.generation == generation;
}

inline ApplyResult ApplyClient(std::uint8_t* unit, std::uint32_t generation,
                               const NativeLoadout& requested,
                               const ClientNativeFunctions& native,
                               ClientApplyState* state) noexcept {
    if (!unit || generation == 0 || !state)
        return ApplyResult::InvalidArgument;
    if (!IsValid(requested))
        return ApplyResult::InvalidLoadout;
    if (SameGeneration(*state, unit, generation))
        return state->loadout == requested
                   ? ApplyResult::AlreadyApplied
                   : ApplyResult::ConflictingLoadout;

    if (requested.level != kPreserveNativeLevel && !native.applySkillPart)
        return ApplyResult::NativeUnavailable;
    if (requested.level != kPreserveNativeLevel && !native.setUnitLevel)
        return ApplyResult::NativeUnavailable;

    // Original SwitchDeck applies the full-width SkillPartID first and calls
    // SetUnitLevel afterwards.  Preserve that order so the optional part and
    // three unit-native skill objects observe the same loadout.
    if (requested.level != kPreserveNativeLevel) {
        native.applySkillPart(unit, requested.skillPartId);
        native.setUnitLevel(unit, requested.level);
    }

    state->unit = unit;
    state->generation = generation;
    state->loadout = requested;
    state->applied = true;
    return ApplyResult::Applied;
}

namespace detail {

template <typename T>
inline void Store(std::uint8_t* base, std::size_t offset,
                  const T& value) noexcept {
    std::memcpy(base + offset, &value, sizeof(value));
}

}  // namespace detail

inline ApplyResult ApplyServer(std::uint8_t* battleUnit,
                               std::uint32_t generation,
                               const NativeLoadout& requested,
                               const ServerNativeFunctions& native,
                               ServerApplyState* state) noexcept {
    if (!battleUnit || generation == 0 || !state)
        return ApplyResult::InvalidArgument;
    if (!IsValid(requested))
        return ApplyResult::InvalidLoadout;
    if (SameGeneration(*state, battleUnit, generation))
        return state->loadout == requested
                   ? ApplyResult::AlreadyApplied
                   : ApplyResult::ConflictingLoadout;

    std::uint32_t resolvedPart = 0;
    if (requested.skillPartId != 0) {
        if (!native.skillRegistry || !native.findSkill)
            return ApplyResult::NativeUnavailable;
        void* resolved = nullptr;
        native.findSkill(native.skillRegistry, requested.skillPartId, &resolved);
        const auto raw = reinterpret_cast<std::uintptr_t>(resolved);
        if (!resolved || raw > (std::numeric_limits<std::uint32_t>::max)())
            return ApplyResult::SkillNotFound;
        resolvedPart = static_cast<std::uint32_t>(raw);
    }

    // Native AI setup skips the fourth slot's reset. Initialize its active
    // byte explicitly; the normal native activation path owns later changes.
    // This is a borrowed registry pointer and must never be freed by the mod.
    if (requested.level != kPreserveNativeLevel) {
        detail::Store(battleUnit, kServerBattleUnitFourthActiveOffset,
                      std::uint8_t{0});
        detail::Store(battleUnit, kServerBattleUnitFourthSkillOffset,
                      resolvedPart);
        detail::Store(battleUnit, kServerBattleUnitLevelOffset,
                      requested.level);
    }

    state->battleUnit = battleUnit;
    state->generation = generation;
    state->loadout = requested;
    state->applied = true;
    return ApplyResult::Applied;
}

}  // namespace battle_ai::loadout
