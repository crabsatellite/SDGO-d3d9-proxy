#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pvp_ai::battle_items {

enum class ItemKind : std::uint8_t {
    None = 0,
    Teleport,
    Hiding,
};

constexpr std::uint32_t kTeleportItemId = 51002;
constexpr std::uint32_t kHidingItemId = 51005;
constexpr std::uint16_t kUseEquipItemOpcode = 0x094D;
constexpr std::int16_t kRequestedNativeUses =
    (std::numeric_limits<std::int16_t>::max)();
constexpr std::uint32_t kFocusWindowSeconds = 3;
constexpr std::uint32_t kPendingLifetimeSeconds = 3;

constexpr std::uint32_t ItemId(ItemKind kind) {
    return kind == ItemKind::Teleport ? kTeleportItemId
         : kind == ItemKind::Hiding ? kHidingItemId
                                    : 0;
}

constexpr std::uint32_t SourceCooldownTicks(ItemKind kind) {
    return kind == ItemKind::Teleport ? 3000
         : kind == ItemKind::Hiding ? 2700
                                    : 0;
}

constexpr std::uint32_t CooldownSeconds(ItemKind kind) {
    return (SourceCooldownTicks(kind) + 59) / 60;
}

constexpr bool IsMeleeRangeType(std::uint8_t rangeType) {
    return rangeType == 1 || rangeType == 5;
}

#pragma pack(push, 1)
// Native SetBattleCombatItem (0x004D7D70) reads exactly these fields.
struct SetItemSource {
    std::uint8_t reserved[8]{};
    std::int16_t sourceSlot = 0;
    std::uint32_t itemId = 0;
    std::int16_t requestedUses = kRequestedNativeUses;
};

// Native REQ_USE_EQUIPITEM (0x004BFC30) receives a 0xA3-byte request. The
// ordinary self-use items below require no contact rows. targetPilotId must be
// the bot itself: zero skips the native Teleport target branch.
struct UseEquipItemPacket {
    std::uint16_t opcode = kUseEquipItemOpcode;
    std::uint32_t pilotId = 0;
    std::uint32_t actorId = 0;
    std::uint32_t itemId = 0;
    std::uint32_t targetPilotId = 0;
    std::uint8_t contactCount = 0;
    std::uint8_t reserved[0xA3 - 0x13]{};
};
#pragma pack(pop)

static_assert(sizeof(SetItemSource) == 0x10);
static_assert(offsetof(SetItemSource, sourceSlot) == 0x08);
static_assert(offsetof(SetItemSource, itemId) == 0x0A);
static_assert(offsetof(SetItemSource, requestedUses) == 0x0E);
static_assert(sizeof(UseEquipItemPacket) == 0xA3);
static_assert(offsetof(UseEquipItemPacket, pilotId) == 0x02);
static_assert(offsetof(UseEquipItemPacket, actorId) == 0x06);
static_assert(offsetof(UseEquipItemPacket, itemId) == 0x0A);
static_assert(offsetof(UseEquipItemPacket, targetPilotId) == 0x0E);
static_assert(offsetof(UseEquipItemPacket, contactCount) == 0x12);

struct BotKey {
    std::uintptr_t room = 0;
    std::uint32_t roomIndex = 0;
    std::uint32_t generation = 0;
    std::uint32_t pilotId = 0;
    std::uint8_t team = 0;
};

constexpr bool SameBot(const BotKey& a, const BotKey& b) {
    return a.room == b.room && a.roomIndex == b.roomIndex &&
           a.generation == b.generation && a.pilotId == b.pilotId &&
           a.team == b.team;
}

struct DamageReceipt {
    BotKey target{};
    std::uint64_t receiptId = 0;
    std::uint32_t nowSeconds = 0;
    std::uint32_t attackerPilotId = 0;
    std::uint8_t attackerTeam = 0;
    std::uint8_t weaponRangeType = 0;
    std::uint32_t hpBefore = 0;
    std::uint32_t hpAfter = 0;
};

struct NativeSnapshot {
    bool generationCurrent = false;
    bool pilotPresent = false;
    bool mapMatches = false;
    bool hardDifficulty = false;
    bool alive = false;
    bool modeAllowed = false;
    bool selectedItemPlayerLegal = false;
    std::uint32_t equippedItemId = 0;
    std::int32_t capacity = 0;
    std::int32_t used = 0;
};

// mapEraseIfExact must compare pilotId, roomIndex and the caller-owned
// generation while holding the map lock, then erase only that exact entry.
// The native 0x004ADAF0 helper erases before returning its room index and is
// therefore not sufficient on its own for this callback.
struct NativeOps {
    void* context = nullptr;
    bool (*mapMatches)(void*, const BotKey&) = nullptr;
    bool (*mapInsert)(void*, const BotKey&) = nullptr;
    bool (*mapEraseIfExact)(void*, const BotKey&) = nullptr;
    bool (*snapshot)(void*, const BotKey&, std::uint32_t selectedItemId,
                     NativeSnapshot*) = nullptr;
    bool (*equip)(void*, const BotKey&, const SetItemSource&,
                  std::int32_t* nativeCapacity) = nullptr;
    int (*use)(void*, const BotKey&, const UseEquipItemPacket&) = nullptr;
};

struct DrainReport {
    std::uint32_t attempted = 0;
    std::uint32_t succeeded = 0;
    std::uint32_t expired = 0;
    std::uint32_t rejected = 0;
};

struct ItemStateView {
    bool active = false;
    bool mapRegistered = false;
    bool disabled = false;
    ItemKind fixed = ItemKind::None;
    ItemKind pending = ItemKind::None;
    bool hasLastUse = false;
    std::uint32_t lastUseSeconds = 0;
};

// The controller has no internal lock. The integration must serialize these
// short state operations. Damage hooks call OnAcceptedDamage only; native
// equip/use callbacks are invoked exclusively from Drain after the original
// CWaitRoomProc tick returned and released every per-room lock.
template <std::size_t MaxBots = 480>
class Controller {
public:
    bool BeginMatch(const BotKey& key, const NativeOps& ops) {
        if (!ValidKey(key) || !ops.mapMatches || !ops.mapInsert)
            return false;
        if (State* current = FindPilot(key.pilotId)) {
            return SameBot(current->key, key);
        }
        State* state = FindFree();
        if (!state)
            return false;
        bool inserted = false;
        bool mapped = ops.mapMatches(ops.context, key);
        if (!mapped) {
            mapped = ops.mapInsert(ops.context, key);
            inserted = mapped;
        }
        if (!mapped || !ops.mapMatches(ops.context, key)) {
            if (inserted && ops.mapEraseIfExact)
                ops.mapEraseIfExact(ops.context, key);
            return false;
        }
        *state = {};
        state->active = true;
        state->mapRegistered = true;
        state->key = key;
        return true;
    }

    bool EndMatch(const BotKey& key, const NativeOps& ops) {
        State* state = FindExact(key);
        if (!state)
            return false;
        bool erased = true;
        if (state->mapRegistered) {
            if (!ops.mapEraseIfExact)
                erased = false;
            else
                erased = ops.mapEraseIfExact(ops.context, key);
        }
        *state = {};
        return erased;
    }

    bool OnAcceptedDamage(const DamageReceipt& receipt) {
        State* state = FindExact(receipt.target);
        if (!state || state->disabled || receipt.receiptId == 0 ||
            receipt.attackerPilotId == 0 ||
            receipt.attackerPilotId == receipt.target.pilotId ||
            receipt.target.team == 0 || receipt.attackerTeam == 0 ||
            receipt.attackerTeam == receipt.target.team ||
            receipt.hpAfter >= receipt.hpBefore ||
            Seen(*state, receipt.receiptId)) {
            return false;
        }
        Remember(*state, receipt.receiptId);
        RememberAttacker(*state, receipt.attackerPilotId, receipt.nowSeconds);

        // A confirmed melee HP receipt is the emergency trigger. If the bot
        // has not selected its one per-match item yet, it outranks a queued
        // Hiding request.
        if (IsMeleeRangeType(receipt.weaponRangeType) &&
            Queue(*state, ItemKind::Teleport, receipt.nowSeconds,
                  receipt.receiptId)) {
            return true;
        }
        if (FocusedAttackers(*state, receipt.nowSeconds) >= 2)
            return Queue(*state, ItemKind::Hiding, receipt.nowSeconds,
                         receipt.receiptId);
        return false;
    }

    DrainReport Drain(std::uint32_t nowSeconds, const NativeOps& ops) {
        return DrainMatching(nowSeconds, ops, 0, false);
    }

    // The server integration calls this while it owns the corresponding
    // native room lock. Keeping each native snapshot/equip/use transaction
    // inside one room avoids reversing the room -> controller lock order used
    // by accepted-damage and battle-lifecycle callbacks.
    DrainReport DrainRoom(std::uint32_t roomIndex, std::uint32_t nowSeconds,
                          const NativeOps& ops) {
        return DrainMatching(nowSeconds, ops, roomIndex, true);
    }

    bool Inspect(const BotKey& key, ItemStateView* view) const {
        if (!view)
            return false;
        const State* state = FindExact(key);
        if (!state)
            return false;
        view->active = state->active;
        view->mapRegistered = state->mapRegistered;
        view->disabled = state->disabled;
        view->fixed = state->fixed;
        view->pending = state->pending;
        view->hasLastUse = state->hasLastUse;
        view->lastUseSeconds = state->lastUseSeconds;
        return true;
    }

private:
    DrainReport DrainMatching(std::uint32_t nowSeconds, const NativeOps& ops,
                              std::uint32_t roomIndex, bool filterRoom) {
        DrainReport report{};
        if (!ops.snapshot || !ops.equip || !ops.use)
            return report;
        for (State& state : states_) {
            if (!state.active || state.pending == ItemKind::None ||
                (filterRoom && state.key.roomIndex != roomIndex))
                continue;
            if (state.disabled) {
                ClearPending(state);
                ++report.rejected;
                continue;
            }
            if (Age(nowSeconds, state.pendingSince) >
                kPendingLifetimeSeconds) {
                ClearPending(state);
                ++report.expired;
                continue;
            }
            if (!Reached(nowSeconds, state.nextAttempt))
                continue;

            const ItemKind requested =
                state.fixed == ItemKind::None ? state.pending : state.fixed;
            if (requested != state.pending || requested == ItemKind::None) {
                ClearPending(state);
                ++report.rejected;
                continue;
            }
            const std::uint32_t itemId = ItemId(requested);
            NativeSnapshot snapshot{};
            if (!ops.snapshot(ops.context, state.key, itemId, &snapshot)) {
                Retry(state, nowSeconds);
                continue;
            }
            if (!CurrentAndUsable(snapshot)) {
                ClearPending(state);
                ++report.rejected;
                continue;
            }
            if (!snapshot.selectedItemPlayerLegal) {
                state.disabled = true;
                ClearPending(state);
                ++report.rejected;
                continue;
            }

            if (state.fixed == ItemKind::None) {
                SetItemSource source{};
                source.itemId = itemId;
                std::int32_t nativeCapacity = 0;
                if (!ops.equip(ops.context, state.key, source,
                               &nativeCapacity)) {
                    Retry(state, nowSeconds);
                    continue;
                }
                // SetBattleCombatItem performs the authoritative clamp.
                if (nativeCapacity <= 0) {
                    state.disabled = true;
                    ClearPending(state);
                    ++report.rejected;
                    continue;
                }
                state.fixed = requested;
                snapshot = {};
                if (!ops.snapshot(ops.context, state.key, itemId, &snapshot)) {
                    Retry(state, nowSeconds);
                    continue;
                }
            }

            if (!CurrentAndUsable(snapshot) ||
                !snapshot.selectedItemPlayerLegal ||
                snapshot.equippedItemId != itemId) {
                // Never switch or reinitialize an already selected item.
                state.disabled = true;
                ClearPending(state);
                ++report.rejected;
                continue;
            }
            if (snapshot.capacity <= snapshot.used ||
                snapshot.capacity <= 0 || snapshot.used < 0) {
                ClearPending(state);
                ++report.rejected;
                continue;
            }
            if (state.hasLastUse &&
                Age(nowSeconds, state.lastUseSeconds) <
                    CooldownSeconds(state.fixed)) {
                continue;
            }

            UseEquipItemPacket packet{};
            packet.pilotId = state.key.pilotId;
            packet.itemId = itemId;
            packet.targetPilotId = state.key.pilotId;
            ++report.attempted;
            const int result = ops.use(ops.context, state.key, packet);
            if (result == 0) {
                state.hasLastUse = true;
                state.lastUseSeconds = nowSeconds;
                ClearPending(state);
                ++report.succeeded;
            } else if (PermanentNativeFailure(result)) {
                if (result == 0x139A || result == 0x139B ||
                    result == 0x139F)
                    state.disabled = true;
                ClearPending(state);
                ++report.rejected;
            } else {
                Retry(state, nowSeconds);
            }
        }
        return report;
    }
    struct AttackerStamp {
        std::uint32_t pilotId = 0;
        std::uint32_t time = 0;
    };

    struct State {
        bool active = false;
        bool mapRegistered = false;
        bool disabled = false;
        BotKey key{};
        ItemKind fixed = ItemKind::None;
        ItemKind pending = ItemKind::None;
        std::uint32_t pendingSince = 0;
        std::uint32_t nextAttempt = 0;
        std::uint64_t pendingReceiptId = 0;
        bool hasLastUse = false;
        std::uint32_t lastUseSeconds = 0;
        std::array<std::uint64_t, 8> receipts{};
        std::size_t receiptCursor = 0;
        std::array<AttackerStamp, 4> attackers{};
    };

    static constexpr bool ValidKey(const BotKey& key) {
        return key.room != 0 && key.generation != 0 && key.pilotId != 0 &&
               key.team != 0;
    }

    static constexpr std::uint32_t Age(std::uint32_t now,
                                       std::uint32_t then) {
        return now - then;
    }

    static constexpr bool Reached(std::uint32_t now,
                                  std::uint32_t deadline) {
        return static_cast<std::int32_t>(now - deadline) >= 0;
    }

    static bool CurrentAndUsable(const NativeSnapshot& snapshot) {
        return snapshot.generationCurrent && snapshot.pilotPresent &&
               snapshot.mapMatches && snapshot.hardDifficulty &&
               snapshot.alive && snapshot.modeAllowed;
    }

    static bool PermanentNativeFailure(int result) {
        return result == 0x1399 || result == 0x139A ||
               result == 0x139B || result == 0x139C ||
               result == 0x139E || result == 0x139F;
    }

    static bool Seen(const State& state, std::uint64_t receiptId) {
        for (std::uint64_t seen : state.receipts)
            if (seen == receiptId)
                return true;
        return false;
    }

    static void Remember(State& state, std::uint64_t receiptId) {
        state.receipts[state.receiptCursor] = receiptId;
        state.receiptCursor =
            (state.receiptCursor + 1) % state.receipts.size();
    }

    static void RememberAttacker(State& state, std::uint32_t pilotId,
                                 std::uint32_t now) {
        AttackerStamp* free = nullptr;
        AttackerStamp* oldest = &state.attackers[0];
        for (AttackerStamp& attacker : state.attackers) {
            if (attacker.pilotId == pilotId) {
                attacker.time = now;
                return;
            }
            if (attacker.pilotId == 0 ||
                Age(now, attacker.time) > kFocusWindowSeconds) {
                if (!free)
                    free = &attacker;
            }
            if (Age(now, attacker.time) > Age(now, oldest->time))
                oldest = &attacker;
        }
        AttackerStamp* selected = free ? free : oldest;
        selected->pilotId = pilotId;
        selected->time = now;
    }

    static std::size_t FocusedAttackers(const State& state,
                                        std::uint32_t now) {
        std::size_t count = 0;
        for (const AttackerStamp& attacker : state.attackers)
            if (attacker.pilotId != 0 &&
                Age(now, attacker.time) <= kFocusWindowSeconds)
                ++count;
        return count;
    }

    static bool Queue(State& state, ItemKind kind, std::uint32_t now,
                      std::uint64_t receiptId) {
        if (kind == ItemKind::None)
            return false;
        if (state.fixed != ItemKind::None && state.fixed != kind)
            return false;
        if (state.pending == kind)
            return false;
        if (state.fixed == ItemKind::None &&
            state.pending == ItemKind::Teleport &&
            kind == ItemKind::Hiding) {
            return false;
        }
        state.pending = kind;
        state.pendingSince = now;
        state.nextAttempt = now;
        state.pendingReceiptId = receiptId;
        return true;
    }

    static void ClearPending(State& state) {
        state.pending = ItemKind::None;
        state.pendingSince = 0;
        state.nextAttempt = 0;
        state.pendingReceiptId = 0;
    }

    static void Retry(State& state, std::uint32_t now) {
        state.nextAttempt = now + 1;
    }

    State* FindFree() {
        for (State& state : states_)
            if (!state.active)
                return &state;
        return nullptr;
    }

    State* FindPilot(std::uint32_t pilotId) {
        for (State& state : states_)
            if (state.active && state.key.pilotId == pilotId)
                return &state;
        return nullptr;
    }

    State* FindExact(const BotKey& key) {
        for (State& state : states_)
            if (state.active && SameBot(state.key, key))
                return &state;
        return nullptr;
    }

    const State* FindExact(const BotKey& key) const {
        for (const State& state : states_)
            if (state.active && SameBot(state.key, key))
                return &state;
        return nullptr;
    }

    std::array<State, MaxBots> states_{};
};

}  // namespace pvp_ai::battle_items
