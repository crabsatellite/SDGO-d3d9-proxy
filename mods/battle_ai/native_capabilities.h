#pragma once

#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace battle_ai::native {

enum class WeaponKind { Melee, Hitscan, Projectile, Missile, Other };

struct Weapon {
    int slot;
    bool valid;
    bool ammoReady;
    bool ready;
    bool movingShot;
    WeaponKind kind;
    float range;
    float damage;
    float cycleMs;
    float projectileSpeed;
    std::uint32_t id;
};

struct Capabilities {
    std::uint32_t unitId;
    int selectedSlot;
    bool alternate;
    float boostFraction;
    bool meleeBusy;
    bool specialBusy;
    Weapon weapons[4];
};

namespace detail {

// These offsets are pinned to GOnline.exe
// SHA256 e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92.
constexpr std::size_t kAttackFsm = 0x9CC;
constexpr std::size_t kActiveUnitDefinition = 0x9C4;
constexpr std::size_t kAlternateBankFlag = 0xE34;

constexpr std::size_t kSelectedSlot = 0x240;
constexpr std::size_t kSelectedWeapon = 0x248;
constexpr std::size_t kCurrentState = 0x004;
constexpr std::size_t kPendingState = 0x008;
constexpr std::size_t kStandardBank = 0x24C;
constexpr std::size_t kAlternateBank = 0x56C;
constexpr std::size_t kSlotStride = 0xC8;
constexpr std::size_t kSlotAmmo = 0x00;
constexpr std::size_t kSlotCooldown = 0x08;
constexpr std::size_t kSlotWeapon = 0x0C;

constexpr std::size_t kUnitId = 0x000;
constexpr std::size_t kOrdinaryWeaponCount = 0x247;
constexpr std::size_t kWeaponIds = 0x249;

constexpr std::size_t kWeaponId = 0x00;
constexpr std::size_t kAttackStyle = 0x69;
constexpr std::size_t kRangeType = 0x6A;
constexpr std::size_t kHitType = 0x6B;
constexpr std::size_t kDamage = 0x6C;
constexpr std::size_t kSpeedRate = 0x70;
constexpr std::size_t kRangeSize = 0x72;
constexpr std::size_t kMaxAmmo = 0x7C;
constexpr std::size_t kConfiguredCooldown = 0x82;
constexpr std::size_t kMovingShot = 0x8F;
// 0238.gdt is the active weapon table in this client.  It has the four-byte
// layout insertion already consumed by the native record binding, so these are
// the runtime offsets (the normalized 0012.gdt offsets are four bytes lower).
constexpr std::size_t kRangedScriptFunction = 0x621;
constexpr std::size_t kProjectileInitializerType = 0x6E9;
constexpr std::size_t kProjectileInitializerFunction = 0x6EB;

constexpr float kSourceUpdatesPerSecond = 60.0f;

struct ReadableRegion {
    std::uintptr_t begin = 0;
    std::uintptr_t end = 0;
};

struct ReadableRegionCache {
    ReadableRegion regions[32]{};
    std::size_t count = 0;
    std::size_t virtualQueryCount = 0;
};

inline thread_local ReadableRegionCache* g_readableRegionCache = nullptr;

class ScopedReadableRegionCache {
public:
    explicit ScopedReadableRegionCache(ReadableRegionCache& cache) noexcept
        : previous_(g_readableRegionCache) {
        g_readableRegionCache = &cache;
    }
    ~ScopedReadableRegionCache() noexcept {
        g_readableRegionCache = previous_;
    }
    ScopedReadableRegionCache(const ScopedReadableRegionCache&) = delete;
    ScopedReadableRegionCache& operator=(const ScopedReadableRegionCache&) = delete;

private:
    ReadableRegionCache* previous_;
};

inline bool Readable(const void* address, std::size_t size) noexcept {
    if (!address || size == 0) return false;
    const auto first = reinterpret_cast<std::uintptr_t>(address);
    if (first < 0x10000 || first > (std::numeric_limits<std::uintptr_t>::max)() - size)
        return false;
    const auto last = first + size;
    auto cursor = first;
    while (cursor < last) {
        bool cached = false;
        if (g_readableRegionCache) {
            for (std::size_t i = 0; i < g_readableRegionCache->count; ++i) {
                const auto& region = g_readableRegionCache->regions[i];
                if (cursor >= region.begin && cursor < region.end) {
                    cursor = region.end < last ? region.end : last;
                    cached = true;
                    break;
                }
            }
        }
        if (cached) continue;
        MEMORY_BASIC_INFORMATION info{};
        if (g_readableRegionCache)
            ++g_readableRegionCache->virtualQueryCount;
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) != sizeof(info))
            return false;
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        const DWORD access = info.Protect & 0xFF;
        if (access != PAGE_READONLY && access != PAGE_READWRITE &&
            access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READ &&
            access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY)
            return false;
        const auto region = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
        if (region > (std::numeric_limits<std::uintptr_t>::max)() - info.RegionSize)
            return false;
        const auto regionEnd = region + info.RegionSize;
        if (regionEnd <= cursor) return false;
        if (g_readableRegionCache &&
            g_readableRegionCache->count <
                sizeof(g_readableRegionCache->regions) /
                    sizeof(g_readableRegionCache->regions[0])) {
            g_readableRegionCache->regions[g_readableRegionCache->count++] =
                ReadableRegion{region, regionEnd};
        }
        cursor = regionEnd < last ? regionEnd : last;
    }
    return true;
}

inline bool Copy(const void* source, void* destination, std::size_t size) noexcept {
    if (!Readable(source, size)) return false;
#if defined(_MSC_VER)
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(destination, source, size);
    return true;
#endif
}

template <typename T>
inline bool Read(const std::uint8_t* base, std::size_t offset, T& value) noexcept {
    if (!base || reinterpret_cast<std::uintptr_t>(base) >
                         (std::numeric_limits<std::uintptr_t>::max)() - offset)
        return false;
    return Copy(base + offset, &value, sizeof(value));
}

inline bool ReadPointer(const std::uint8_t* base, std::size_t offset,
                        std::uint8_t*& value) noexcept {
    std::uint32_t raw = 0;
    if (!Read(base, offset, raw) || raw < 0x10000) return false;
    value = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(raw));
    return Readable(value, 1);
}

inline bool DecodeFloat(const std::uint8_t* unit, std::size_t permutationOffset,
                        std::size_t bytesOffset, std::size_t keyOffset,
                        float& value) noexcept {
    std::uint8_t permutation[4]{};
    std::uint8_t bytes[16]{};
    std::uint8_t key = 0;
    if (!Copy(unit + permutationOffset, permutation, sizeof(permutation)) ||
        !Copy(unit + bytesOffset, bytes, sizeof(bytes)) ||
        !Read(unit, keyOffset, key))
        return false;
    unsigned seen = 0;
    std::uint8_t decoded[4]{};
    for (unsigned i = 0; i < 4; ++i) {
        // Protected-float constructor 0x00671900 chooses four distinct indices
        // from the full 16-byte storage area.  The getter at 0x00656CB0 uses
        // those indices with signed byte loads.
        if (permutation[i] > 15 || (seen & (1u << permutation[i])) != 0) return false;
        seen |= 1u << permutation[i];
        decoded[i] = bytes[permutation[i]] ^ key;
    }
    std::memcpy(&value, decoded, sizeof(value));
    return std::isfinite(value);
}

inline bool StateIs(const std::uint8_t* fsm, std::uint32_t current,
                    std::size_t offset) noexcept {
    const auto expected = reinterpret_cast<std::uintptr_t>(fsm) + offset;
    return expected <= (std::numeric_limits<std::uint32_t>::max)() &&
           current == static_cast<std::uint32_t>(expected);
}

// BE38A0 promotes +8 to +4, clears +8, then runs Entry/Update. A queued
// neutral state must not hide a still-active swing during its Release.
inline bool InState(const std::uint8_t* fsm, std::size_t offset) noexcept {
    std::uint32_t current=0,pending=0;
    return Read(fsm,kCurrentState,current) && Read(fsm,kPendingState,pending) &&
        (StateIs(fsm,current,offset) || StateIs(fsm,pending,offset));
}
inline bool Neutral(const std::uint8_t* fsm) noexcept {
    std::uint32_t current=0,pending=0;
    return Read(fsm,kCurrentState,current) && Read(fsm,kPendingState,pending) &&
        StateIs(fsm,current,0x20) && (pending==0 || StateIs(fsm,pending,0x20));
}

inline bool ReadBoostFraction(const std::uint8_t* unit, float& result) noexcept {
    // CNPC::GetBoostGage implementation 0x00656CB0 returns the quotient of
    // these two obfuscated floats: current at +BE5 and maximum at +BD0.
    float current = 0.0f;
    float maximum = 0.0f;
    if (!DecodeFloat(unit, 0xBE1, 0xBE5, 0xBF5, current) ||
        !DecodeFloat(unit, 0xBCC, 0xBD0, 0xBE0, maximum) ||
        maximum <= 0.0f || current < -0.001f || current > maximum * 1.05f)
        return false;
    result = current / maximum;
    if (result < 0.0f) result = 0.0f;
    if (result > 1.0f) result = 1.0f;
    return true;
}

inline std::int32_t SlotDamageAddition(const std::uint8_t* unit, bool alternate,
                                       int slot, bool& ok) noexcept {
    if (slot < 0 || slot > 2) return 0;
    std::int32_t value = 0;
    const std::size_t base = alternate ? 0xA65 : 0xA59;
    ok = Read(unit, base + static_cast<std::size_t>(slot) * 4, value);
    return value;
}

inline std::int16_t SlotRangeAddition(const std::uint8_t* unit, bool alternate,
                                      int slot, bool& ok) noexcept {
    if (slot < 0 || slot > 2) return 0;
    std::int16_t value = 0;
    const std::size_t base = alternate ? 0xA77 : 0xA71;
    ok = Read(unit, base + static_cast<std::size_t>(slot) * 2, value);
    return value;
}

inline WeaponKind Classify(std::uint8_t attackStyle, std::uint8_t rangeType,
                           std::int16_t speedRate, bool hasRangedScript,
                           bool hasProjectileInitializer) noexcept {
    if (attackStyle == 2) return WeaponKind::Other;
    if (rangeType == 8) return WeaponKind::Melee;
    if (rangeType == 1)
        return hasProjectileInitializer ? WeaponKind::Other : WeaponKind::Melee;
    if (rangeType == 2 || rangeType == 3) {
        if (speedRate > 0) return WeaponKind::Projectile;
        return hasRangedScript ? WeaponKind::Other : WeaponKind::Hitscan;
    }
    // RangeType 4/6/7 are funnel families and 5 is scripted remote melee.
    // They remain attackable through the native FSM but have no generic,
    // source-independent flight model. Missile remains reserved for an exact
    // resolver rather than guessing from a script/function name.
    return WeaponKind::Other;
}

inline bool ReadOnce(std::uint8_t* unit, Capabilities& out) noexcept {
#if defined(_WIN64)
    (void)unit;
    (void)out;
    return false;
#else
    // Capability fields repeatedly touch the same handful of committed heap
    // regions. Cache only this attempt's VirtualQuery results; every Copy still
    // retains its range check and SEH guard, and a retry starts with a new cache.
    ReadableRegionCache readableRegions{};
    ScopedReadableRegionCache readableScope(readableRegions);
    if (!unit || !Readable(unit, 0xE35)) return false;

    Capabilities value{};
    value.selectedSlot = -1;
    for (int slot = 0; slot < 4; ++slot) {
        value.weapons[slot] = Weapon{
            slot, false, false, false, false, WeaponKind::Other,
            0.0f, 0.0f, 0.0f, 0.0f, 0
        };
    }

    std::uint8_t* fsm = nullptr;
    std::uint8_t* definition = nullptr;
    if (!ReadPointer(unit, kAttackFsm, fsm) ||
        !ReadPointer(unit, kActiveUnitDefinition, definition) ||
        !Readable(fsm, 0x9B2) || !Readable(definition, kWeaponIds + 16))
        return false;

    if (!Read(definition, kUnitId, value.unitId) ||
        value.unitId == 0 || value.unitId > 9'999'999)
        return false;

    std::uint8_t alternate = 0;
    std::uint8_t ordinaryCount = 0;
    std::int32_t selected = -1;
    std::uint32_t currentState = 0, pendingState = 0;
    if (!Read(unit, kAlternateBankFlag, alternate) ||
        !Read(definition, kOrdinaryWeaponCount, ordinaryCount) ||
        !Read(fsm, kSelectedSlot, selected) ||
        !Read(fsm, kCurrentState, currentState) ||
        !Read(fsm, kPendingState, pendingState) ||
        alternate > 1 || ordinaryCount == 0 || ordinaryCount > 3)
        return false;
    value.alternate = alternate != 0;
    if (selected >= 0 && selected < 4) value.selectedSlot = selected;

    // 0x0068D2D0 defines the native non-interruptible special-state set.
    auto busyState=[&](std::size_t offset) {return StateIs(fsm,currentState,offset)||StateIs(fsm,pendingState,offset);};
    value.specialBusy = busyState(0x098)||busyState(0x0AC)||busyState(0x0C0)||busyState(0x0D4)||busyState(0x1D8)||busyState(0x1EC);
    // GetCurrentStateId 0x0068CFC0 identifies the melee chain/tracking states.
    value.meleeBusy =
        busyState(0x048)||busyState(0x05C)||busyState(0x070)||busyState(0x084)||busyState(0x098)||
        busyState(0x0E8)||busyState(0x0FC)||busyState(0x1D8)||busyState(0x1EC);

    if (!ReadBoostFraction(unit, value.boostFraction)) return false;

    const std::size_t bank = value.alternate ? kAlternateBank : kStandardBank;
    for (int slot = 0; slot < 4; ++slot) {
        Weapon& weapon = value.weapons[slot];
        std::uint32_t expectedId = 0;
        if (!Read(definition, kWeaponIds + static_cast<std::size_t>(slot) * 4, expectedId))
            return false;
        weapon.id = expectedId;
        if (expectedId == 0) continue;
        if (expectedId > 9'999'999) return false;

        const std::size_t state = bank + static_cast<std::size_t>(slot) * kSlotStride;
        std::int16_t ammo = 0;
        std::int16_t remainingCooldown = 0;
        std::uint8_t* record = nullptr;
        if (!Read(fsm, state + kSlotAmmo, ammo) ||
            !Read(fsm, state + kSlotCooldown, remainingCooldown))
            return false;
        if (!ReadPointer(fsm, state + kSlotWeapon, record)) continue;
        if (!Readable(record, kProjectileInitializerFunction + sizeof(std::uint16_t)))
            return false;

        std::uint32_t recordId = 0;
        std::uint8_t attackStyle = 0;
        std::uint8_t rangeType = 0;
        std::uint8_t hitType = 0;
        std::uint8_t movingShot = 0;
        std::int32_t baseDamage = 0;
        std::int16_t speedRate = 0;
        std::uint32_t packedRange = 0;
        std::int16_t maxAmmo = 0;
        std::int16_t configuredCooldown = 0;
        std::uint16_t rangedScriptFirstCharacter = 0;
        std::uint8_t projectileInitializerType = 0;
        std::uint16_t projectileInitializerFirstCharacter = 0;
        if (!Read(record, kWeaponId, recordId) ||
            !Read(record, kAttackStyle, attackStyle) ||
            !Read(record, kRangeType, rangeType) ||
            !Read(record, kHitType, hitType) ||
            !Read(record, kMovingShot, movingShot) ||
            !Read(record, kDamage, baseDamage) ||
            !Read(record, kSpeedRate, speedRate) ||
            !Read(record, kRangeSize, packedRange) ||
            !Read(record, kMaxAmmo, maxAmmo) ||
            !Read(record, kConfiguredCooldown, configuredCooldown) ||
            !Read(record, kRangedScriptFunction, rangedScriptFirstCharacter) ||
            !Read(record, kProjectileInitializerType, projectileInitializerType) ||
            !Read(record, kProjectileInitializerFunction,
                  projectileInitializerFirstCharacter))
            return false;
        if (recordId != expectedId || attackStyle < 1 || attackStyle > 3 ||
            rangeType < 1 || rangeType > 8 || hitType > 3 || movingShot > 1 ||
            (packedRange & 0xFFFF0000u) != 0 || speedRate < 0 ||
            maxAmmo < 0 || configuredCooldown < 0 ||
            projectileInitializerType > 2)
            return false;

        const bool hasRangedScript = rangedScriptFirstCharacter != 0;
        const bool hasProjectileInitializer =
            projectileInitializerType != 0 ||
            projectileInitializerFirstCharacter != 0;
        weapon.kind = Classify(attackStyle, rangeType, speedRate,
                               hasRangedScript, hasProjectileInitializer);
        weapon.movingShot = movingShot != 0;

        bool modifierOk = true;
        const std::int32_t damageAddition =
            SlotDamageAddition(unit, value.alternate, slot, modifierOk);
        if (!modifierOk) return false;
        const std::int16_t rangeAddition =
            SlotRangeAddition(unit, value.alternate, slot, modifierOk);
        if (!modifierOk) return false;

        std::int64_t damage = static_cast<std::int64_t>(baseDamage) + damageAddition;
        std::int64_t range = static_cast<std::uint16_t>(packedRange) + rangeAddition;
        if (attackStyle != 2 && rangeType == 2) {
            std::int32_t addition = 0;
            if (!Read(unit, 0xCA0, addition)) return false;
            range += addition;
        } else if (attackStyle != 2 && rangeType == 3) {
            std::int32_t addition = 0;
            if (!Read(unit, 0xCA4, addition)) return false;
            range += addition;
        }
        if (damage < 0 || damage > 10'000'000 || range < 0 || range > 1'000'000)
            continue;
        weapon.damage = static_cast<float>(damage);
        weapon.range = static_cast<float>(range);

        // CoolTime is copied to the bank's int16 counter and decremented by the
        // native 60 Hz attack update (0x006B6570).  This is configured cooldown
        // only, not the complete animation/combo attack cycle.
        weapon.cycleMs = configuredCooldown == 0
            ? 0.0f
            : static_cast<float>(configuredCooldown) * (1000.0f / kSourceUpdatesPerSecond);

        // Every non-fatal record in an ordinary slot can still be executed by
        // the native FSM.  Keep scripted/funnel records valid as Other so the
        // caller can retain native behavior instead of making the unit stop.
        const bool ordinary = slot < ordinaryCount && attackStyle != 2;
        weapon.valid = ordinary;
        const bool unlimitedMelee = rangeType == 1 && maxAmmo == 0;
        weapon.ammoReady = unlimitedMelee || (maxAmmo > 0 && ammo > 0);
        weapon.ready = weapon.valid && weapon.ammoReady && remainingCooldown <= 0;

        const bool exactDefaultLinearProjectile =
            ordinary && (rangeType == 2 || rangeType == 3) && speedRate > 0 &&
            !hasRangedScript && !hasProjectileInitializer;
        if (exactDefaultLinearProjectile) {
            std::int64_t speedPerUpdate = speedRate;
            // CMissile::BattleUpdate 0x006504A0 applies these signed additions
            // for ordinary RangeType 2/3 projectiles before each position step.
            if (attackStyle != 2 && rangeType == 2) {
                std::int32_t addition = 0;
                if (!Read(unit, 0xCEC, addition)) return false;
                speedPerUpdate += addition;
            } else if (attackStyle != 2 && rangeType == 3) {
                std::int32_t addition = 0;
                if (!Read(unit, 0xCF0, addition)) return false;
                speedPerUpdate += addition;
            }
            if (speedPerUpdate > 0 && speedPerUpdate <= 1'000'000)
                weapon.projectileSpeed =
                    static_cast<float>(speedPerUpdate) * kSourceUpdatesPerSecond;
        }
    }

    // The active form and bank can swap during transform. Reject a torn sample;
    // the caller can try again on its next observation.
    std::uint8_t* endingFsm = nullptr;
    std::uint8_t* endingDefinition = nullptr;
    std::uint8_t endingAlternate = 0;
    if (!ReadPointer(unit, kAttackFsm, endingFsm) ||
        !ReadPointer(unit, kActiveUnitDefinition, endingDefinition) ||
        !Read(unit, kAlternateBankFlag, endingAlternate) ||
        endingFsm != fsm || endingDefinition != definition || endingAlternate != alternate)
        return false;

    // When the selected slot is ordinary, +248 is the actual native bound
    // weapon record. Check it against the sampled bank rather than trusting a
    // ChangeWeapon request, which only queues message 0x48.
    if (value.selectedSlot >= 0 && value.selectedSlot < ordinaryCount) {
        std::uint8_t* selectedRecord = nullptr;
        std::uint8_t* bankRecord = nullptr;
        const std::size_t selectedState =
            bank + static_cast<std::size_t>(value.selectedSlot) * kSlotStride;
        if (!ReadPointer(fsm, kSelectedWeapon, selectedRecord) ||
            !ReadPointer(fsm, selectedState + kSlotWeapon, bankRecord) ||
            selectedRecord != bankRecord)
            return false;
    }

    out = value;
    return true;
#endif
}

}  // namespace detail

inline bool ReadCapabilities(std::uint8_t* unit, Capabilities& out) noexcept {
    Capabilities first{};
    if (detail::ReadOnce(unit, first)) {
        out = first;
        return true;
    }
    // A transform or native selection commit can race a read. One immediate
    // retry still fails closed for a persistent layout/pointer mismatch.
    Capabilities second{};
    if (!detail::ReadOnce(unit, second)) return false;
    out = second;
    return true;
}

}  // namespace battle_ai::native
