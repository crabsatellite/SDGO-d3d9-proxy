#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <utility>

#include "sdgo/battle_reward_mission.h"
#include "mods/battle_rewards/combat_tracker.h"

namespace {

using namespace sdgo::battle_rewards;
using namespace sdgo::battle_rewards::mission_native;

int failures = 0;

void Check(bool condition, const char* message) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
    if (!condition) ++failures;
}

template <typename T>
void Put(void* base, size_t offset, const T& value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(value));
}

struct Reader {
    const void* fail_base = nullptr;
    size_t fail_offset = 0;

    bool operator()(
        const void* base, size_t offset, void* output, size_t size) const {
        if (!base || !output ||
            (base == fail_base && offset == fail_offset)) {
            return false;
        }
        std::memcpy(
            output, static_cast<const uint8_t*>(base) + offset, size);
        return true;
    }
};

struct PairCatalog {
    std::set<std::pair<uint32_t, uint32_t>> bosses;

    bool IsBoss(uint32_t mission_id, uint32_t npc_data_id) const {
        return bosses.find({mission_id, npc_data_id}) != bosses.end();
    }
};

CombatEvent Event(
    uint32_t attacker, uint32_t victim, uint32_t damage, bool death,
    const RewardClassification& classification) {
    CombatEvent event{};
    event.attacker = attacker;
    event.victim = victim;
    event.attacker_team = 1;
    event.victim_team = 2;
    event.effective_damage = damage;
    event.death = death;
    event.victim_npc = classification.npc;
    event.victim_boss = classification.boss;
    return event;
}

}  // namespace

int main() {
    std::array<uint8_t, 0x31720> room{};
    std::array<uint8_t, 0x1D10> victim_pilot{};
    std::array<uint8_t, 0x20> npc_data{};
    Reader reader{};

    const uint8_t mission_mode = 5;
    const uint32_t mission_id = 88006;
    const uint8_t native_difficulty = 2;
    const uint32_t victim_entity = kNpcEntityBit | 13u;
    const uint32_t npc_data_id = 85054;
    Put(room.data(), kRoomMode, mission_mode);
    Put(room.data(), kRoomMissionId, mission_id);
    Put(room.data(), kRoomMissionDifficulty, native_difficulty);
    Put(victim_pilot.data(), kBattlePilotEntity, victim_entity);
    Put(npc_data.data(), kNpcDataId, npc_data_id);
    const void* npc_data_pointer = npc_data.data();
    Put(victim_pilot.data(), kBattlePilotNpcData, npc_data_pointer);

    MissionVictimIdentity victim{};
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), reader, &victim) ==
            ReadError::None,
        "mission victim reads MissionID and NpcDataID through the native pilot record");
    Check(
        victim.mission_id == mission_id &&
            victim.npc_data_id == npc_data_id &&
            victim.victim_entity == victim_entity &&
            victim.raw_difficulty == native_difficulty &&
            victim.difficulty == Difficulty::Normal,
        "mission victim preserves authoritative identity and raw difficulty");

    for (uint8_t mode : {uint8_t{5}, uint8_t{8}, uint8_t{10}}) {
        Put(room.data(), kRoomMode, mode);
        Check(
            ReadMissionVictim(
                room.data(), victim_pilot.data(), reader, &victim) ==
                ReadError::None,
            "all three native mission room modes are accepted");
    }
    Put(room.data(), kRoomMode, uint8_t{4});
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), reader, &victim) ==
            ReadError::NotMissionMode,
        "non-mission rooms cannot create mission reward identities");
    Put(room.data(), kRoomMode, mission_mode);

    Put(victim_pilot.data(), kBattlePilotEntity, uint32_t{13});
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), reader, &victim) ==
            ReadError::HumanVictim,
        "a human victim cannot be classified from an NPC data pointer");
    Put(victim_pilot.data(), kBattlePilotEntity, victim_entity);

    const void* null_npc_data = nullptr;
    Put(victim_pilot.data(), kBattlePilotNpcData, null_npc_data);
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), reader, &victim) ==
            ReadError::MissingNpcDataPointer,
        "a missing native NpcData record fails closed");
    Put(victim_pilot.data(), kBattlePilotNpcData, npc_data_pointer);

    Put(npc_data.data(), kNpcDataId, uint32_t{0});
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), reader, &victim) ==
            ReadError::MissingNpcDataId,
        "zero NpcDataID fails closed");
    Put(npc_data.data(), kNpcDataId, npc_data_id);

    const MissionVictimIdentity stable_victim = victim;
    Reader fault_reader{room.data(), kRoomMissionId};
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), fault_reader, &victim) ==
            ReadError::MemoryFault,
        "guarded-reader failure propagates without a partial identity");
    Check(
        victim.mission_id == stable_victim.mission_id &&
            victim.npc_data_id == stable_victim.npc_data_id &&
            victim.victim_entity == stable_victim.victim_entity &&
            victim.raw_difficulty == stable_victim.raw_difficulty &&
            victim.difficulty == stable_victim.difficulty,
        "a failed guarded read leaves the caller's previous identity intact");

    Reader pointer_fault{victim_pilot.data(), kBattlePilotNpcData};
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), pointer_fault, &victim) ==
            ReadError::MemoryFault,
        "a guarded failure reading the pilot NpcData pointer fails closed");

    Reader npc_record_fault{npc_data.data(), kNpcDataId};
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), npc_record_fault, &victim) ==
            ReadError::MemoryFault,
        "a guarded failure reading the pointed NpcData record fails closed");

    Difficulty difficulty = Difficulty::Normal;
    Check(
        DecodeDifficulty(1, &difficulty) &&
            difficulty == Difficulty::Normal &&
            DecodeDifficulty(2, &difficulty) &&
            difficulty == Difficulty::Normal,
        "native difficulties 1 and 2 map to the Normal reward tier");
    Check(
        DecodeDifficulty(3, &difficulty) &&
            difficulty == Difficulty::Hard &&
            DecodeDifficulty(4, &difficulty) &&
            difficulty == Difficulty::Hard,
        "native difficulties 3 and 4 map to the Hard reward tier");
    Check(
        DecodeDifficulty(5, &difficulty) &&
            difficulty == Difficulty::Expert &&
            DecodeDifficulty(9, &difficulty) &&
            difficulty == Difficulty::Expert,
        "native difficulties 5 through 9 map to the Expert reward tier");
    Check(
        !DecodeDifficulty(0, &difficulty) &&
            !DecodeDifficulty(10, &difficulty),
        "out-of-contract native difficulty values fail closed");

    Put(room.data(), kRoomMissionState, uint32_t{kMissionStateClear});
    MissionSettlementIdentity settlement{};
    Check(
        ReadMissionSettlement(room.data(), reader, &settlement) ==
                ReadError::None &&
            settlement.outcome == Outcome::Clear &&
            settlement.mission_id == mission_id,
        "final mission state 5 maps to authoritative clear");
    Put(room.data(), kRoomMissionState, uint32_t{kMissionStateFail});
    Check(
        ReadMissionSettlement(room.data(), reader, &settlement) ==
                ReadError::None &&
            settlement.outcome == Outcome::Fail,
        "final mission state 6 maps to authoritative fail");
    Put(room.data(), kRoomMissionState, uint32_t{2});
    Check(
        ReadMissionSettlement(room.data(), reader, &settlement) ==
            ReadError::InvalidOutcome,
        "a non-final mission state cannot be paid as clear or fail");

    Put(room.data(), kRoomMissionState, uint32_t{kMissionStateClear});
    Check(
        ReadMissionVictim(
            room.data(), victim_pilot.data(), reader, &victim) ==
            ReadError::None,
        "valid mission victim can be classified after settlement checks");
    PairCatalog catalog{{{mission_id, npc_data_id}}};
    RewardClassification boss = Classify(victim, catalog);
    Check(
        boss.npc && boss.boss,
        "exact MissionID and NpcDataID registry membership classifies a Boss");

    MissionVictimIdentity other_mission = victim;
    other_mission.mission_id = 88010;
    RewardClassification same_npc_elsewhere =
        Classify(other_mission, catalog);
    MissionVictimIdentity other_npc = victim;
    other_npc.npc_data_id = 85079;
    RewardClassification same_mission_other_npc =
        Classify(other_npc, catalog);
    Check(
        same_npc_elsewhere.npc && !same_npc_elsewhere.boss &&
            same_mission_other_npc.npc && !same_mission_other_npc.boss,
        "Boss classification requires exact pair membership, not an NPC or model heuristic");

    const uint32_t minion_entity = kNpcEntityBit | 14u;
    CombatTracker minion_tracker;
    Check(
        minion_tracker.Record(
            Event(
                2001, minion_entity, 700, false,
                same_mission_other_npc),
            Mode::Pve),
        "a nonlethal minion event records native effective damage");
    Check(
        minion_tracker.Find(2001).damage == 700 &&
            minion_tracker.Find(2001).minions == 0,
        "nonlethal effective damage cannot become a mission kill");
    Check(
        minion_tracker.Record(
            Event(
                2001, minion_entity, 300, true,
                same_mission_other_npc),
            Mode::Pve),
        "the native death event closes the minion life");
    Check(
        minion_tracker.Find(2001).damage == 1000 &&
            minion_tracker.Find(2001).minions == 1,
        "one native death produces exactly one mission minion kill");

    CombatTracker tracker;
    Check(
        tracker.Record(
            Event(1001, victim_entity, 600, false, boss), Mode::Pve),
        "Boss damage is accepted before the native death event");
    Check(
        tracker.BossesDefeated() == 0 &&
            tracker.Find(1001).boss_credit_micro == 0,
        "Boss credit is not issued before actual death");
    Check(
        tracker.Record(
            Event(1002, victim_entity, 400, true, boss), Mode::Pve),
        "native Boss death closes the tracked life");
    const TrackedContribution first = tracker.Find(1001);
    const TrackedContribution second = tracker.Find(1002);
    Check(
        tracker.BossesDefeated() == 1 &&
            first.boss_credit_micro == 600000 &&
            second.boss_credit_micro == 400000 &&
            first.boss_credit_micro + second.boss_credit_micro <= kMicro,
        "Boss credit is proportional and never exceeds one defeated Boss");

    Check(
        tracker.Record(
            Event(
                1001, victim_entity, 100, true,
                same_npc_elsewhere),
            Mode::Pve),
        "a reused native entity starts a new non-Boss life after death");
    Check(
        tracker.BossesDefeated() == 1 &&
            tracker.Find(1001).minions == 1,
        "mission-pair classification prevents stale Boss credit on entity reuse");

    std::printf(
        "%s\n",
        failures ? "MISSION REWARD CHECKS FAILED"
                 : "ALL MISSION REWARD CHECKS PASSED");
    return failures ? 1 : 0;
}
