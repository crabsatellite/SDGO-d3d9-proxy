#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>

#include "mods/battle_rewards/combat_tracker.h"
#include "mods/battle_rewards/native_combat_hook.h"

namespace hook = sdgo::battle_rewards::native_combat_hook;
namespace rewards = sdgo::battle_rewards;

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL line " << line << ": " << expression << "\n";
    }
}

#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

std::vector<uint8_t> room(0x40000);
std::vector<uint8_t> npc_pilot(0x2000);
std::vector<uint8_t> victim_unit(0x100);
void* expected_room = nullptr;
void* expected_victim = nullptr;
void* expected_attacker = nullptr;
void* expected_unit = nullptr;
uint32_t expected_damage = 0;
int32_t expected_death = 0;
DWORD expected_incoming_error = 0;
int original_calls = 0;
bool original_arguments = false;
bool original_last_error = false;
bool mutate_original_inputs = false;
bool gate_open = true;
int gate_calls = 0;
int classifier_calls = 0;
int record_calls = 0;
int diagnostic_calls = 0;
int record_calls_seen_by_original = -1;
hook::DiagnosticReason last_diagnostic = hook::DiagnosticReason::None;
hook::NativeDamageSnapshot last_snapshot;
hook::VictimClassification last_classification;
rewards::CombatTracker combat;

template <typename T>
void Put(void* base, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(value));
}

uint8_t* Pilot(int slot) {
    return room.data() + hook::kBattlePilots +
           static_cast<size_t>(slot) * hook::kBattlePilotStride;
}

void SetParticipant(int slot, uint32_t account, uint8_t team) {
    uint8_t* pilot = Pilot(slot);
    Put(pilot, hook::kPilotEntity, account);
    Put(pilot, hook::kPilotTeam, team);
    Put(room.data(),
        hook::kSlotInfo + static_cast<size_t>(slot) * hook::kSlotInfoStride +
            hook::kSlotAccount,
        account);
}

void SetMissionNpc(int slot, uint32_t entity, uint8_t team) {
    CHECK(slot >= hook::kHumanRoomSlots && slot < hook::kNativePilotSlots);
    uint8_t* pilot = Pilot(slot);
    Put(pilot, hook::kPilotEntity, entity);
    Put(pilot, hook::kPilotTeam, team);
}

uint32_t __fastcall PrivateOriginal(
    void* native_room, void* /*unused_edx*/, void* native_victim,
    void* native_attacker, void* native_unit, uint32_t native_damage,
    int32_t native_death) {
    ++original_calls;
    original_arguments =
        native_room == expected_room && native_victim == expected_victim &&
        native_attacker == expected_attacker && native_unit == expected_unit &&
        native_damage == expected_damage && native_death == expected_death;
    original_last_error = GetLastError() == expected_incoming_error;
    record_calls_seen_by_original = record_calls;
    if (mutate_original_inputs) {
        Put(native_victim, hook::kPilotEntity, 999999u);
        Put(native_victim, hook::kPilotTeam, static_cast<uint8_t>(9));
        if (native_unit) {
            Put(native_unit, hook::kUnitKey, 999999u);
            Put(native_unit, hook::kUnitDataPointer,
                static_cast<uintptr_t>(999999u));
        }
    }
    SetLastError(0x33445566);
    return 0xC0FFEE01u;
}

bool __stdcall EventGate(const hook::NativeDamageSnapshot*) {
    ++gate_calls;
    SetLastError(0x77777777);
    return gate_open;
}

bool __stdcall Classify(
    const hook::NativeDamageSnapshot* snapshot,
    hook::VictimClassification* classification) {
    ++classifier_calls;
    SetLastError(0x88888888);
    if (snapshot->victim_slot >= 0) {
        *classification = {true, false, false};
        return true;
    }
    if (snapshot->mission_id == 700 && snapshot->victim_entity == 42) {
        *classification = {true, true, true};
        return true;
    }
    if (snapshot->victim_entity == 0x80000001u ||
        (snapshot->room_mode == 5 && snapshot->victim_unit == nullptr)) {
        *classification = {true, true, false};
        return true;
    }
    return false;
}

void __stdcall Record(
    const hook::NativeDamageSnapshot* snapshot,
    const hook::VictimClassification* classification) {
    ++record_calls;
    last_snapshot = *snapshot;
    last_classification = *classification;
    SetLastError(0x99999999);
}

void __stdcall RecordCombat(
    const hook::NativeDamageSnapshot* snapshot,
    const hook::VictimClassification* classification) {
    Record(snapshot, classification);
    const rewards::CombatEvent hit{
        snapshot->attacker_account,
        snapshot->victim_entity,
        snapshot->attacker_team,
        snapshot->victim_team,
        snapshot->effective_damage,
        snapshot->death_flag != 0,
        classification->npc,
        classification->boss};
    CHECK(combat.Record(hit, rewards::Mode::Pve));
}

void __stdcall Diagnostic(
    hook::DiagnosticReason reason,
    const hook::NativeDamageSnapshot*) {
    ++diagnostic_calls;
    last_diagnostic = reason;
    SetLastError(0xAAAAAAAA);
}

void Reset() {
    std::fill(room.begin(), room.end(), static_cast<uint8_t>(0));
    std::fill(npc_pilot.begin(), npc_pilot.end(), static_cast<uint8_t>(0));
    std::fill(victim_unit.begin(), victim_unit.end(), static_cast<uint8_t>(0));
    room[hook::kRoomMode] = 1;
    Put(room.data(), hook::kRoomMission, 100u);
    SetParticipant(0, 1001, 1);
    SetParticipant(1, 1002, 2);
    Put(victim_unit.data(), hook::kUnitKey, 4001u);
    Put(victim_unit.data(), hook::kUnitDataPointer,
        static_cast<uintptr_t>(0x12345678u));
    expected_room = room.data();
    expected_victim = Pilot(1);
    expected_attacker = Pilot(0);
    expected_unit = victim_unit.data();
    expected_damage = 1234;
    expected_death = 1;
    expected_incoming_error = 0x11223344;
    original_calls = 0;
    original_arguments = false;
    original_last_error = false;
    mutate_original_inputs = false;
    gate_open = true;
    gate_calls = 0;
    classifier_calls = 0;
    record_calls = 0;
    diagnostic_calls = 0;
    record_calls_seen_by_original = -1;
    last_diagnostic = hook::DiagnosticReason::None;
    last_snapshot = {};
    last_classification = {};
    combat = {};
    const auto fixture_original = &PrivateOriginal;
    static_assert(sizeof(fixture_original) == sizeof(hook::original),
                  "x86 fixture function pointers must have the same size");
    std::memcpy(&hook::original, &fixture_original, sizeof(hook::original));
    hook::event_gate = &EventGate;
    hook::classify_victim = &Classify;
    hook::record_sink = &Record;
    hook::diagnostic_sink = &Diagnostic;
}

uint32_t Call() {
    SetLastError(expected_incoming_error);
    uintptr_t before = 0;
    uintptr_t after = 0;
    __asm mov before, esp
    const uint32_t result = hook::Detour(
        expected_room, nullptr, expected_victim, expected_attacker,
        expected_unit, expected_damage, expected_death);
    __asm mov after, esp
    CHECK(before == after);
    CHECK(result == 0xC0FFEE01u);
    CHECK(GetLastError() == 0x33445566);
    CHECK(original_calls == 1);
    CHECK(original_arguments);
    CHECK(original_last_error);
    return result;
}

void ParticipantAndAbiTest() {
    Reset();
    mutate_original_inputs = true;
    Call();
    CHECK(gate_calls == 1);
    CHECK(classifier_calls == 1);
    CHECK(record_calls == 1);
    CHECK(record_calls_seen_by_original == 1);
    CHECK(diagnostic_calls == 0);
    CHECK(last_snapshot.attacker_account == 1001);
    CHECK(last_snapshot.attacker_entity == 1001);
    CHECK(last_snapshot.victim_account == 1002);
    CHECK(last_snapshot.victim_entity == 1002);
    CHECK(last_snapshot.attacker_slot == 0);
    CHECK(last_snapshot.victim_slot == 1);
    CHECK(last_snapshot.mission_id == 100);
    CHECK(last_snapshot.victim_unit_key == 4001);
    CHECK(last_snapshot.victim_unit_data ==
          static_cast<uintptr_t>(0x12345678u));
    CHECK(last_snapshot.effective_damage == 1234);
    CHECK(last_snapshot.death_flag == 1);
    CHECK(!last_classification.npc && !last_classification.boss);
}

void AiAndNpcClassificationTests() {
    Reset();
    SetParticipant(0, 0x80000005u, 1);
    expected_attacker = Pilot(0);
    expected_victim = npc_pilot.data();
    Put(npc_pilot.data(), hook::kPilotEntity, 42u);
    Put(npc_pilot.data(), hook::kPilotTeam, static_cast<uint8_t>(0));
    Put(room.data(), hook::kRoomMission, 700u);
    Put(victim_unit.data(), hook::kUnitKey, 777u);
    Call();
    CHECK(record_calls == 1);
    CHECK(last_snapshot.attacker_account == 0x80000005u);
    CHECK(!last_snapshot.attacker_native_npc);
    CHECK(last_snapshot.victim_slot == -1);
    CHECK(last_snapshot.victim_entity == 42u);
    CHECK(last_classification.npc && last_classification.boss);

    Reset();
    expected_victim = npc_pilot.data();
    Put(npc_pilot.data(), hook::kPilotEntity, 0x80000001u);
    Put(npc_pilot.data(), hook::kPilotTeam, static_cast<uint8_t>(0));
    Put(victim_unit.data(), hook::kUnitKey, 888u);
    Call();
    CHECK(record_calls == 1);
    CHECK(last_snapshot.victim_entity == 0x80000001u);
    CHECK(last_classification.npc && !last_classification.boss);

    Reset();
    expected_victim = npc_pilot.data();
    Put(npc_pilot.data(), hook::kPilotEntity, 43u);
    Put(npc_pilot.data(), hook::kPilotTeam, static_cast<uint8_t>(0));
    Put(room.data(), hook::kRoomMission, 701u);
    Put(victim_unit.data(), hook::kUnitKey, 777u);
    Call();
    CHECK(classifier_calls == 1);
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::ClassificationRejected);
}

void MissionNpcAttackerTests() {
    Reset();
    room[hook::kRoomMode] = 5;
    Put(room.data(), hook::kRoomMission, 700u);
    SetMissionNpc(12, 0x80000102u, 1);
    expected_attacker = Pilot(12);
    expected_victim = npc_pilot.data();
    expected_damage = 500;
    expected_death = 0;
    Put(npc_pilot.data(), hook::kPilotEntity, 42u);
    Put(npc_pilot.data(), hook::kPilotTeam, static_cast<uint8_t>(2));
    Call();
    CHECK(record_calls == 1);
    CHECK(last_snapshot.attacker_native_npc);
    CHECK(last_snapshot.attacker_slot == 12);
    CHECK(last_snapshot.attacker_entity == 0x80000102u);
    CHECK(last_snapshot.attacker_account == 0x80000102u);
    CHECK(last_classification.npc && last_classification.boss);

    Reset();
    SetMissionNpc(12, 0x80000102u, 1);
    expected_attacker = Pilot(12);
    Call();
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::AttackerNotMember);

    Reset();
    room[hook::kRoomMode] = 5;
    expected_attacker = npc_pilot.data();
    Put(npc_pilot.data(), hook::kPilotEntity, 0x80000102u);
    Put(npc_pilot.data(), hook::kPilotTeam, static_cast<uint8_t>(1));
    Call();
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::AttackerNotMember);

    Reset();
    room[hook::kRoomMode] = 5;
    SetMissionNpc(12, 102u, 1);
    expected_attacker = Pilot(12);
    Call();
    CHECK(record_calls == 0);
    CHECK(classifier_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::AttackerIdentityMismatch);

    Reset();
    room[hook::kRoomMode] = 5;
    Put(room.data(), hook::kRoomMission, 700u);
    hook::record_sink = &RecordCombat;
    expected_victim = npc_pilot.data();
    expected_damage = 500;
    expected_death = 0;
    Put(npc_pilot.data(), hook::kPilotEntity, 42u);
    Put(npc_pilot.data(), hook::kPilotTeam, static_cast<uint8_t>(2));
    Call();
    SetMissionNpc(12, 0x80000102u, 1);
    expected_attacker = Pilot(12);
    expected_death = 1;
    original_calls = 0;
    Call();
    CHECK(combat.BossesDefeated() == 1);
    const auto human = combat.Find(1001u);
    const auto npc = combat.Find(0x80000102u);
    CHECK(human.damage == 500);
    CHECK(npc.damage == 500);
    CHECK(human.boss_credit_micro == rewards::kMicro / 2);
    CHECK(npc.boss_credit_micro == rewards::kMicro / 2);
}

void GuardTests() {
    Reset();
    expected_damage = 0;
    Call();
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::NoEffectiveDamage);

    Reset();
    expected_unit = nullptr;
    Call();
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::NativeGuardRejected);

    Reset();
    room[hook::kRoomMode] = 5;
    expected_victim = npc_pilot.data();
    expected_unit = nullptr;
    Put(npc_pilot.data(), hook::kPilotEntity, 77u);
    Put(npc_pilot.data(), hook::kPilotTeam, static_cast<uint8_t>(0));
    Call();
    CHECK(record_calls == 1);
    CHECK(last_snapshot.victim_unit_key == 0);
    CHECK(last_snapshot.victim_unit_data == 0);
    CHECK(last_classification.npc && !last_classification.boss);

    Reset();
    SetParticipant(1, 1002, 1);
    Call();
    CHECK(record_calls == 0);
    CHECK(classifier_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::FriendlyDamage);

    Reset();
    gate_open = false;
    Call();
    CHECK(gate_calls == 1);
    CHECK(classifier_calls == 0);
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::EventGateRejected);
}

void IdentityAndFaultTests() {
    Reset();
    expected_attacker = npc_pilot.data();
    Call();
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::AttackerNotMember);

    Reset();
    // BP+9 is not read by the verified native HP-loss helper. Different raw
    // values there must not become a guessed lifecycle qualification rule.
    Put(Pilot(0), 0x09, 2u);
    Put(Pilot(1), 0x09, 99u);
    Call();
    CHECK(record_calls == 1);
    CHECK(last_diagnostic == hook::DiagnosticReason::None);

    Reset();
    Put(room.data(),
        hook::kSlotInfo + hook::kSlotAccount, 999u);
    Call();
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::AttackerIdentityMismatch);

    Reset();
    expected_victim = reinterpret_cast<void*>(1);
    Call();
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::SnapshotFault);

    Reset();
    expected_unit = reinterpret_cast<void*>(1);
    Call();
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::SnapshotFault);
}

void MissingOriginalTest() {
    Reset();
    hook::original = nullptr;
    SetLastError(expected_incoming_error);
    const uint32_t result = hook::Detour(
        expected_room, nullptr, expected_victim, expected_attacker,
        expected_unit, expected_damage, expected_death);
    CHECK(result == 0);
    CHECK(original_calls == 0);
    CHECK(record_calls == 0);
    CHECK(last_diagnostic == hook::DiagnosticReason::MissingOriginal);
    CHECK(GetLastError() == expected_incoming_error);
}

}  // namespace

int main() {
    ParticipantAndAbiTest();
    AiAndNpcClassificationTests();
    MissionNpcAttackerTests();
    GuardTests();
    IdentityAndFaultTests();
    MissingOriginalTest();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
