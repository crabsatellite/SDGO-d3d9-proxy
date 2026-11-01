#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../mods/battle_ai/native_loadout.h"

using namespace battle_ai;
using namespace battle_ai::loadout;

namespace {

int failures = 0;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition)
        ++failures;
}

struct FakeClient {
    int partCalls = 0;
    int levelCalls = 0;
    std::uint32_t appliedPart = 0;
    int level = 0;
    std::array<int, 2> order{};
    int orderSize = 0;

    static FakeClient*& Active() {
        static FakeClient* active = nullptr;
        return active;
    }

    static void __fastcall ApplyPart(std::uint8_t*, void*,
                                     std::uint32_t skillPartId) {
        auto& self = *Active();
        ++self.partCalls;
        self.appliedPart = skillPartId;
        self.order[self.orderSize++] = 1;
    }

    static void __fastcall SetLevel(std::uint8_t*, void*, int value) {
        auto& self = *Active();
        ++self.levelCalls;
        self.level = value;
        self.order[self.orderSize++] = 2;
    }

    ClientNativeFunctions Functions() {
        Active() = this;
        return {
            reinterpret_cast<SetUnitLevelFn>(&SetLevel),
            reinterpret_cast<ApplySkillPartFn>(&ApplyPart),
        };
    }
};

struct FakeServer {
    int finds = 0;
    std::uint32_t lastSkill = 0;
    void* result = reinterpret_cast<void*>(0x12345000u);

    static FakeServer*& Active() {
        static FakeServer* active = nullptr;
        return active;
    }

    static void __fastcall Find(void*, void*, std::uint32_t skillId,
                                void** output) {
        auto& self = *Active();
        ++self.finds;
        self.lastSkill = skillId;
        *output = self.result;
    }

    ServerNativeFunctions Functions() {
        Active() = this;
        return {
            this,
            reinterpret_cast<FindServerSkillFn>(&Find),
        };
    }
};

struct FakeSource {
    int calls = 0;
    NativeProfile profile{};

    static bool Read(void* context, const void*, NativeProfile* output) {
        auto& self = *static_cast<FakeSource*>(context);
        ++self.calls;
        if (!output)
            return false;
        *output = self.profile;
        return true;
    }
};

template <typename T>
T ReadAt(const std::array<std::uint8_t, 0x80>& storage,
         std::size_t offset) {
    T value{};
    std::memcpy(&value, storage.data() + offset, sizeof(value));
    return value;
}

}  // namespace

int main() {
    {
        WeaponProfile hybrid[]={{1,1,5,1200,250,0},{1,2,0,300,1800,8}};
        Check(SelectHardPart(ProfileFromWeapons(hybrid,2))==kFighterTechnique,
              "strong mixed-loadout melee burst selects a normal player fighting part without a machine-ID rule");
        hybrid[0].damage=400;hybrid[1].damage=1200;hybrid[1].range=4000;
        Check(SelectHardPart(ProfileFromWeapons(hybrid,2))==kNewtypeAwakening,
              "ranged reach and magazine potential choose legal mobility instead of classifying every mixed unit as melee");
        Check(ClassifyRole(ProfileFromWeapons(nullptr,0))==UnitRole::Ranged,
              "missing weapon facts never fabricate sniper eligibility");
    }
    static_assert(kMediumLevel == 12);
    static_assert(kHardExLevel == 13);
    static_assert(kServerBattleUnitLevelOffset == 0x0C);
    static_assert(kServerBattleUnitFourthSkillOffset == 0x54);
    static_assert(kClientSetUnitLevelAddress == 0x006705B0);
    static_assert(kClientApplySkillPartAddress == 0x006704F0);
    static_assert(kServerFindSkillAddress == 0x00462480);

    {
        const auto easy = ForDifficulty(Difficulty::Easy);
        const auto medium = ForDifficulty(Difficulty::Medium);
        Check(easy.level == kPreserveNativeLevel &&
                  easy.skillPartId == 0,
              "Easy preserves the original native level and part");
        Check(medium.level == 12 && medium.skillPartId == 0,
              "Medium uses canonical one-based level 12 without a bonus part");
        Check(!IsValid({12, 75002}) && !IsValid({13, 0}),
              "only canonical Medium and one-part Hard combinations are valid");
    }

    {
        NativeProfile melee{};
        melee.role = UnitRole::Melee;
        NativeProfile ranged{};
        ranged.role = UnitRole::Ranged;
        NativeProfile sniper = ranged;
        sniper.role = UnitRole::Sniper;
        sniper.sniperEligible = true;
        NativeProfile unprovenSniper = sniper;
        unprovenSniper.sniperEligible = false;
        NativeProfile lowFuel = melee;
        lowFuel.lowFuel = true;

        Check(ForDifficulty(Difficulty::Hard, melee) ==
                  NativeLoadout{13, 75003},
              "Hard melee receives EX13 and one ordinary player fighting part");
        Check(ForDifficulty(Difficulty::Hard, ranged) ==
                  NativeLoadout{13, 75002},
              "Hard ordinary ranged receives EX13 and one mobility part");
        Check(ForDifficulty(Difficulty::Hard, sniper) ==
                  NativeLoadout{13, 75014},
              "Hard proven sniper profile receives Sniper Manual");
        Check(ForDifficulty(Difficulty::Hard, unprovenSniper) ==
                  NativeLoadout{13, 75002},
              "an unproven sniper claim falls back to the ordinary ranged part");
        Check(ForDifficulty(Difficulty::Hard, lowFuel) ==
                  NativeLoadout{13, 75002},
              "the proven low-fuel trait selects mobility before combat role");
    }

    {
        FakeSource source;
        source.profile.role = UnitRole::Sniper;
        source.profile.sniperEligible = true;
        NativeLoadout value{};
        const NativeProfileSource adapter{&source, &FakeSource::Read};
        Check(FromNativeSource(Difficulty::Hard, adapter,
                               reinterpret_cast<void*>(1), &value) &&
                  value == NativeLoadout{13, 75014} &&
                  source.calls == 1,
              "both ends can source the same role profile without unit IDs");
        Check(!FromNativeSource(Difficulty::Hard, {}, nullptr, &value),
              "Hard role selection fails closed without a native source");
    }

    {
        FakeClient client;
        const auto functions = client.Functions();
        ClientApplyState state{};
        std::array<std::uint8_t, 8> unit{};
        const NativeLoadout hard{13, 75003};

        Check(ApplyClient(unit.data(), 7, hard, functions, &state) ==
                  ApplyResult::Applied &&
                  client.partCalls == 1 && client.appliedPart == 75003 &&
                  client.levelCalls == 1 && client.level == 13 &&
                  client.order == std::array<int, 2>{1, 2},
              "client applies the full-width native SkillPartID before SetUnitLevel");
        Check(ApplyClient(unit.data(), 7, hard, functions, &state) ==
                  ApplyResult::AlreadyApplied &&
                  client.partCalls == 1 && client.levelCalls == 1,
              "same unit generation does not destroy and recreate the part");
        Check(ApplyClient(unit.data(), 7, {13, 75002}, functions, &state) ==
                  ApplyResult::ConflictingLoadout &&
                  client.partCalls == 1,
              "one generation cannot equip two optional fourth-slot parts");
    }

    {
        FakeClient client;
        const auto functions = client.Functions();
        ClientApplyState state{};
        std::array<std::uint8_t, 8> unit{};
        Check(ApplyClient(unit.data(), 4, {12, 0}, functions, &state) ==
                  ApplyResult::Applied &&
                  client.partCalls == 1 && client.appliedPart == 0 &&
                  client.levelCalls == 1 && client.level == 12,
              "Medium clears the native optional part and applies level 12");
    }

    {
        FakeServer server;
        const auto functions = server.Functions();
        ServerApplyState state{};
        std::array<std::uint8_t, 0x80> battleUnit{};
        battleUnit.fill(0xCC);

        Check(ApplyServer(battleUnit.data(), 9, {13, 75014},
                          functions, &state) == ApplyResult::Applied &&
                  server.finds == 1 && server.lastSkill == 75014 &&
                  ReadAt<std::uint8_t>(
                      battleUnit, kServerBattleUnitLevelOffset) == 13 &&
                  ReadAt<std::uint32_t>(
                      battleUnit, kServerBattleUnitFourthSkillOffset) ==
                      0x12345000u && battleUnit[kServerBattleUnitFourthActiveOffset] == 0,
              "server resolves a borrowed native definition into slot four and sets EX13");
        Check(ApplyServer(battleUnit.data(), 9, {13, 75014},
                          functions, &state) ==
                  ApplyResult::AlreadyApplied && server.finds == 1,
              "server application is idempotent for one BattleUnit generation");
        Check(ApplyServer(battleUnit.data(), 9, {13, 75003},
                          functions, &state) ==
                  ApplyResult::ConflictingLoadout && server.finds == 1,
              "server refuses a second fourth-slot part in the same generation");
    }

    {
        FakeServer server;
        server.result = nullptr;
        const auto functions = server.Functions();
        ServerApplyState state{};
        std::array<std::uint8_t, 0x80> battleUnit{};
        battleUnit.fill(0xA5);
        const auto before = battleUnit;
        Check(ApplyServer(battleUnit.data(), 3, {13, 75002},
                          functions, &state) ==
                  ApplyResult::SkillNotFound &&
                  battleUnit == before && !state.applied,
              "failed registry lookup leaves level and slot bytes untouched");
    }

    {
        FakeServer server;
        const auto functions = server.Functions();
        ServerApplyState state{};
        std::array<std::uint8_t, 0x80> battleUnit{};
        battleUnit.fill(0xA5);
        Check(ApplyServer(battleUnit.data(), 5, {12, 0},
                          functions, &state) == ApplyResult::Applied &&
                  server.finds == 0 &&
                  ReadAt<std::uint8_t>(
                      battleUnit, kServerBattleUnitLevelOffset) == 12 &&
                  ReadAt<std::uint32_t>(
                      battleUnit, kServerBattleUnitFourthSkillOffset) == 0 &&
                  battleUnit[kServerBattleUnitFourthActiveOffset] == 0,
              "Medium clears only the optional slot and applies level 12");
    }

    {
        FakeServer server;
        const auto functions = server.Functions();
        ServerApplyState state{};
        std::array<std::uint8_t, 0x80> battleUnit{};
        battleUnit.fill(0x5A);
        const auto before = battleUnit;
        Check(ApplyServer(battleUnit.data(), 6, {0, 0},
                          functions, &state) == ApplyResult::Applied &&
                  battleUnit == before && server.finds == 0,
              "Easy preserves the server-native level and optional slot");
    }

    std::printf("BATTLE AI LOADOUT CHECKS %s\n",
                failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
