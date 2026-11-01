#include <cstdint>
#include <cstdio>

#include "../mods/pvp_ai/battle_items.h"

using namespace pvp_ai::battle_items;

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
        ++failures;
}

struct FakeNative {
    BotKey mapped{};
    bool hasMap = false;
    bool generationCurrent = true;
    bool pilotPresent = true;
    bool hard = true;
    bool alive = true;
    bool modeAllowed = true;
    bool playerLegal = true;
    std::uint32_t equipped = 0;
    std::int32_t capacity = 0;
    std::int32_t used = 0;
    int inserts = 0;
    int erases = 0;
    int equips = 0;
    int useCalls = 0;
    int nextUseResult = 0;
    bool rejectNextMappedCheck = false;
    UseEquipItemPacket lastPacket{};

    static FakeNative& Self(void* context) {
        return *static_cast<FakeNative*>(context);
    }

    static bool MapMatches(void* context, const BotKey& key) {
        auto& self = Self(context);
        if (self.rejectNextMappedCheck && self.hasMap) {
            self.rejectNextMappedCheck = false;
            return false;
        }
        return self.hasMap && SameBot(self.mapped, key);
    }

    static bool MapInsert(void* context, const BotKey& key) {
        auto& self = Self(context);
        if (self.hasMap && !SameBot(self.mapped, key))
            return false;
        self.hasMap = true;
        self.mapped = key;
        ++self.inserts;
        return true;
    }

    static bool MapEraseIfExact(void* context, const BotKey& key) {
        auto& self = Self(context);
        if (!self.hasMap || !SameBot(self.mapped, key))
            return false;
        self.hasMap = false;
        self.mapped = {};
        ++self.erases;
        return true;
    }

    static bool Snapshot(void* context, const BotKey& key,
                         std::uint32_t, NativeSnapshot* out) {
        auto& self = Self(context);
        if (!out)
            return false;
        out->generationCurrent = self.generationCurrent;
        out->pilotPresent = self.pilotPresent;
        out->mapMatches = MapMatches(context, key);
        out->hardDifficulty = self.hard;
        out->alive = self.alive;
        out->modeAllowed = self.modeAllowed;
        out->selectedItemPlayerLegal = self.playerLegal;
        out->equippedItemId = self.equipped;
        out->capacity = self.capacity;
        out->used = self.used;
        return true;
    }

    static bool Equip(void* context, const BotKey& key,
                      const SetItemSource& source,
                      std::int32_t* nativeCapacity) {
        auto& self = Self(context);
        if (!MapMatches(context, key) ||
            source.requestedUses != kRequestedNativeUses ||
            (source.itemId != kTeleportItemId &&
             source.itemId != kHidingItemId)) {
            return false;
        }
        self.equipped = source.itemId;
        self.capacity = 2;  // stand-in for the native ItemData+0x6C clamp
        self.used = 0;
        ++self.equips;
        if (nativeCapacity)
            *nativeCapacity = self.capacity;
        return true;
    }

    static int Use(void* context, const BotKey& key,
                   const UseEquipItemPacket& packet) {
        auto& self = Self(context);
        ++self.useCalls;
        self.lastPacket = packet;
        if (!MapMatches(context, key))
            return 0x1393;
        const int result = self.nextUseResult;
        self.nextUseResult = 0;
        if (result == 0)
            ++self.used;
        return result;
    }

    NativeOps Ops() {
        return {this, &MapMatches, &MapInsert, &MapEraseIfExact,
                &Snapshot, &Equip, &Use};
    }
};

BotKey Key(std::uint32_t generation = 1) {
    return {0x12345000u, 7, generation, 0x80001001u, 1};
}

DamageReceipt Hit(const BotKey& key, std::uint64_t receipt,
                  std::uint32_t now, std::uint32_t attacker,
                  std::uint8_t attackerTeam, std::uint8_t rangeType,
                  std::uint32_t before = 1000,
                  std::uint32_t after = 900) {
    DamageReceipt result{};
    result.target = key;
    result.receiptId = receipt;
    result.nowSeconds = now;
    result.attackerPilotId = attacker;
    result.attackerTeam = attackerTeam;
    result.weaponRangeType = rangeType;
    result.hpBefore = before;
    result.hpAfter = after;
    return result;
}

}  // namespace

int main() {
    static_assert(CooldownSeconds(ItemKind::Teleport) == 50);
    static_assert(CooldownSeconds(ItemKind::Hiding) == 45);
    Check(sizeof(UseEquipItemPacket) == 0xA3 &&
              offsetof(UseEquipItemPacket, targetPilotId) == 0x0E,
          "native item packet preserves the 0xA3 ABI and self-target offset");

    {
        Controller<4> controller;
        FakeNative native;
        auto ops = native.Ops();
        const BotKey key = Key();
        Check(controller.BeginMatch(key, ops) && native.inserts == 1,
              "begin registers the AI pilot in the native pilot-to-room map");
        Check(controller.BeginMatch(key, ops) && native.inserts == 1,
              "same-generation begin is idempotent and does not re-register");
        Check(!controller.BeginMatch(Key(2), ops),
              "a reused pilot id cannot replace a live generation");

        Check(!controller.OnAcceptedDamage(Hit(key, 1, 10, 101, 1, 2)) &&
                  !controller.OnAcceptedDamage(
                      Hit(key, 2, 10, 101, 2, 2, 1000, 1000)),
              "friendly and zero-HP-delta reports are not authoritative hits");
        Check(!controller.OnAcceptedDamage(Hit(key, 3, 10, 101, 2, 2)) &&
                  !controller.OnAcceptedDamage(Hit(key, 4, 11, 101, 2, 2)),
              "repeated fire from one attacker is not focus fire");
        Check(controller.OnAcceptedDamage(Hit(key, 5, 12, 102, 2, 2)),
              "two distinct enemy damage receipts queue Hiding");
        ItemStateView view{};
        Check(controller.Inspect(key, &view) &&
                  view.pending == ItemKind::Hiding &&
                  view.fixed == ItemKind::None,
              "focus trigger is deferred and does not call native code in damage");

        Check(controller.OnAcceptedDamage(Hit(key, 6, 12, 103, 2, 1)),
              "an accepted melee hit upgrades an uncommitted Hiding request");
        Check(controller.Inspect(key, &view) &&
                  view.pending == ItemKind::Teleport,
              "melee escape has priority before the per-match item is selected");

        auto report = controller.Drain(12, ops);
        Check(report.attempted == 1 && report.succeeded == 1 &&
                  native.equips == 1 && native.used == 1,
              "unlocked drain equips once, consumes the native clamp and uses once");
        Check(native.lastPacket.opcode == kUseEquipItemOpcode &&
                  native.lastPacket.itemId == kTeleportItemId &&
                  native.lastPacket.pilotId == key.pilotId &&
                  native.lastPacket.targetPilotId == key.pilotId &&
                  native.lastPacket.actorId == 0 &&
                  native.lastPacket.contactCount == 0,
              "Teleport uses the native ordinary self-target request");
        Check(controller.Inspect(key, &view) &&
                  view.fixed == ItemKind::Teleport &&
                  view.pending == ItemKind::None && view.hasLastUse,
              "first native equip fixes one consumable type for the match");

        controller.OnAcceptedDamage(Hit(key, 7, 20, 104, 2, 2));
        controller.OnAcceptedDamage(Hit(key, 8, 20, 105, 2, 2));
        Check(controller.Inspect(key, &view) &&
                  view.pending == ItemKind::None,
              "a Teleport-selected bot cannot switch to Hiding later");

        Check(controller.OnAcceptedDamage(Hit(key, 9, 30, 106, 2, 1)),
              "new melee damage may request the already selected item");
        report = controller.Drain(30, ops);
        Check(report.attempted == 0 && native.used == 1,
              "3000 source updates enforce a fifty-second Teleport cooldown");
        report = controller.Drain(34, ops);
        Check(report.expired == 1 && native.used == 1,
              "an old emergency is discarded instead of firing after cooldown");

        Check(controller.OnAcceptedDamage(Hit(key, 10, 62, 107, 2, 1)),
              "fresh melee damage after cooldown queues a new escape");
        native.nextUseResult = 0x139D;
        report = controller.Drain(62, ops);
        Check(report.attempted == 1 && report.succeeded == 0 &&
                  native.used == 1,
              "transient native room failure does not consume an item");
        report = controller.Drain(62, ops);
        Check(report.attempted == 0,
              "a failed request is not retried twice in the same second");
        report = controller.Drain(63, ops);
        Check(report.succeeded == 1 && native.used == 2 &&
                  native.equips == 1,
              "transient request retries once without re-equipping or double use");
        Check(!controller.OnAcceptedDamage(Hit(key, 10, 63, 107, 2, 1)),
              "a repeated authoritative receipt id is idempotent");

        Check(controller.OnAcceptedDamage(Hit(key, 11, 120, 108, 2, 1)),
              "a third eligible event can be observed");
        report = controller.Drain(120, ops);
        Check(report.rejected == 1 && native.used == 2,
              "native per-match capacity remains authoritative");

        Check(!controller.EndMatch(Key(2), ops) && native.erases == 0,
              "stale generation cleanup cannot erase the current mapping");
        Check(controller.EndMatch(key, ops) && native.erases == 1 &&
                  !native.hasMap,
              "exact match cleanup removes only its own pilot mapping");
    }

    {
        Controller<1> controller;
        FakeNative native;
        auto ops = native.Ops();
        native.rejectNextMappedCheck = true;
        Check(!controller.BeginMatch(Key(6), ops) && !native.hasMap &&
                  native.inserts == 1 && native.erases == 1,
              "failed post-insert verification rolls the native map entry back");
    }

    {
        Controller<2> controller;
        FakeNative native;
        auto ops = native.Ops();
        const BotKey key = Key(9);
        Check(controller.BeginMatch(key, ops), "second fixture begins");
        controller.OnAcceptedDamage(Hit(key, 20, 100, 201, 2, 2));
        controller.OnAcceptedDamage(Hit(key, 21, 101, 202, 2, 2));
        auto report = controller.DrainRoom(8, 101, ops);
        Check(report.attempted == 0 && native.equips == 0,
              "room-scoped drain leaves another native room untouched");
        report = controller.DrainRoom(key.roomIndex, 101, ops);
        Check(report.succeeded == 1 && native.equipped == kHidingItemId,
              "locked room drain selects native Hiding when no melee receipt wins");
        controller.OnAcceptedDamage(Hit(key, 22, 140, 203, 2, 2));
        controller.OnAcceptedDamage(Hit(key, 23, 140, 204, 2, 2));
        report = controller.Drain(140, ops);
        Check(report.attempted == 0,
              "2700 source updates enforce a forty-five-second Hiding cooldown");
        controller.Drain(144, ops);
        controller.OnAcceptedDamage(Hit(key, 24, 146, 205, 2, 2));
        controller.OnAcceptedDamage(Hit(key, 25, 146, 206, 2, 2));
        report = controller.Drain(146, ops);
        Check(report.succeeded == 1 && native.used == 2,
              "fresh focus after forty-five seconds can use Hiding again");
    }

    {
        Controller<2> controller;
        FakeNative native;
        auto ops = native.Ops();
        const BotKey key = Key(12);
        Check(controller.BeginMatch(key, ops), "gate fixture begins");
        controller.OnAcceptedDamage(Hit(key, 30, 200, 301, 2, 1));
        native.hard = false;
        auto report = controller.Drain(200, ops);
        Check(report.rejected == 1 && native.equips == 0,
              "non-Hard runtime state cannot equip or use consumables");
        native.hard = true;
        controller.OnAcceptedDamage(Hit(key, 31, 201, 302, 2, 1));
        native.alive = false;
        report = controller.Drain(201, ops);
        Check(report.rejected == 1 && native.equips == 0,
              "dead bot drops the event instead of replaying it on respawn");
        native.alive = true;
        controller.OnAcceptedDamage(Hit(key, 32, 202, 303, 2, 1));
        native.modeAllowed = false;
        report = controller.Drain(202, ops);
        Check(report.rejected == 1 && native.equips == 0,
              "native item mode restrictions are rechecked before equip");
        native.modeAllowed = true;
        controller.OnAcceptedDamage(Hit(key, 33, 203, 304, 2, 1));
        native.playerLegal = false;
        report = controller.Drain(203, ops);
        Check(report.rejected == 1 && native.equips == 0,
              "an item unavailable to players is never granted to Hard AI");
    }

    std::printf("BATTLE ITEM CHECKS %s\n",
                failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
