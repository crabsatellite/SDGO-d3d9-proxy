#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "sdgo/battle_reward_stats.h"

namespace {
using namespace sdgo::battle_rewards::native_stats;

int failures = 0;

void Check(bool ok, const char* message) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++failures;
}

template <typename T>
void Put(BYTE* base, ptrdiff_t offset, const T& value) {
    memcpy(base + offset, &value, sizeof(value));
}

void PopulateSlot(BYTE* room, uint32_t slot, uint32_t id, uint8_t team,
                  uint8_t state) {
    BYTE* roomPilot = room + kRoomPilotBase + slot * kRoomPilotStride;
    BYTE* battlePilot = room + kBattlePilotBase + slot * kBattlePilotStride;
    Put(roomPilot, kRoomPilotId, id);
    Put(roomPilot, kRoomPilotTeam, team);
    Put(battlePilot, kBattlePilotState, state);
}

void PopulateFrame(BYTE* frame, const SettlementFrameContract& contract,
                   BYTE* room, BYTE* roomPilot, BYTE* battlePilot,
                   int32_t slot, int32_t rank, uint32_t duration) {
    Put(frame, contract.room, reinterpret_cast<uintptr_t>(room));
    Put(frame, contract.room_pilot, reinterpret_cast<uintptr_t>(roomPilot));
    Put(frame, contract.battle_pilot, reinterpret_cast<uintptr_t>(battlePilot));
    Put(frame, contract.physical_slot, slot);
    Put(frame, contract.native_rank, rank);
    Put(frame, contract.duration_seconds, duration);
    if (rank >= 0 && rank < static_cast<int32_t>(kParticipantSlots)) {
        const uint32_t flags = 0;
        Put(frame, contract.eligibility_flags + rank * static_cast<ptrdiff_t>(sizeof(uint32_t)),
            flags);
        if (contract.mission_outcomes != 0) {
            const uint32_t cleared = 1;
            Put(frame, contract.mission_outcomes + rank * static_cast<ptrdiff_t>(sizeof(uint32_t)),
                cleared);
        }
    }
}
}

int main() {
    alignas(16) std::array<BYTE, 0x33000> roomBytes{};
    alignas(16) std::array<BYTE, 0xC80> frameBytes{};
    BYTE* room = roomBytes.data();
    BYTE* frame = frameBytes.data() + 0xC40;
    const uint32_t roomId = 0x12345678;
    const uint32_t startedAt = 1730000000;
    const uint8_t roomMode = 4;
    Put(room, 0, roomId);
    Put(room, kRoomMode, roomMode);
    Put(room, kRoomStartedAtUnix, startedAt);

    PopulateSlot(room, 0, 1001, 1, 3);
    PopulateSlot(room, 1, 1002, 1, 3);
    PopulateSlot(room, 2, 1003, 2, 3);
    PopulateSlot(room, 3, 1004, 2, 2);
    PopulateSlot(room, 4, kAiIdBit | 9u, 2, 3);

    BYTE* currentRoomPilot = room + kRoomPilotBase + 2 * kRoomPilotStride;
    BYTE* currentBattlePilot = room + kBattlePilotBase + 2 * kBattlePilotStride;
    const uint32_t stars = 321;
    const uint32_t kills = 7;
    const uint32_t deaths = 2;
    const uint8_t outcome = static_cast<uint8_t>(NativeOutcome::WinOrClear);
    Put(currentBattlePilot, kBattlePilotStars, stars);
    Put(currentBattlePilot, kBattlePilotKills, kills);
    Put(currentBattlePilot, kBattlePilotDeaths, deaths);
    Put(currentBattlePilot, kBattlePilotOutcome, outcome);

    for (unsigned site = 0; site < kSettlementFrames.size(); ++site) {
        memset(frameBytes.data(), 0, frameBytes.size());
        const SettlementFrameContract& contract = kSettlementFrames[site];
        PopulateFrame(frame, contract, room, currentRoomPilot, currentBattlePilot,
                      2, 0, 180);

        NativeFrame snapshot{};
        CaptureError error = CaptureError::None;
        Check(ReadSettlementFrame(site, frame, &snapshot, &error),
              "all ten frame contracts capture");
        Check(error == CaptureError::None && snapshot.room == room &&
              snapshot.room_pilot == currentRoomPilot &&
              snapshot.battle_pilot == currentBattlePilot,
              "room and participant pointers agree with native layout");
        Check(snapshot.room_id == roomId && snapshot.room_mode == roomMode &&
              snapshot.room_started_at_unix == startedAt &&
              snapshot.account == 1003 && snapshot.native_id == 1003 &&
              snapshot.physical_slot == 2,
              "room identity, account, and physical slot are preserved");
        Check(snapshot.duration_seconds == 180 && snapshot.native_rank == 0 &&
              snapshot.native_rank_first,
              "duration and native rank survive settlement");
        Check(snapshot.stars == stars && snapshot.kills == kills &&
              snapshot.deaths == deaths,
              "stars, kills, and deaths remain distinct");
        Check(snapshot.outcome == NativeOutcome::WinOrClear &&
              snapshot.kind == ParticipantKind::Human && snapshot.native_eligible,
              "current outcome, participant kind, and eligibility are preserved");
    }

    std::array<Participant, kParticipantSlots> participants{};
    unsigned participantCount = 0;
    CaptureError error = CaptureError::None;
    Check(ReadParticipants(room, participants.data(),
                           static_cast<unsigned>(participants.size()),
                           &participantCount, &error) &&
          participantCount == 5,
          "occupied participant slots are enumerated once");
    Check(CountActiveHumans(participants.data(), participantCount) == 3,
          "human presence excludes AI and inactive observer slots");
    Check(participants[2].physical_slot == 2 && participants[2].account == 1003 &&
          participants[2].kills == kills,
          "participant enumeration retains physical identity and final stats");
    Check(participants[4].kind == ParticipantKind::Ai &&
          participants[4].account == 9 && participants[4].active,
          "AI identity remains explicit while its high marker bit is stripped");

    const SettlementFrameContract& team = kSettlementFrames[1];
    memset(frameBytes.data(), 0, frameBytes.size());
    PopulateFrame(frame, team, room,
                  room + kRoomPilotBase + 3 * kRoomPilotStride,
                  currentBattlePilot, 2, 0, 180);
    NativeFrame snapshot{};
    Check(!ReadSettlementFrame(1, frame, &snapshot, &error) &&
          error == CaptureError::PointerMismatch,
          "mismatched frame pointers fail closed");

    PopulateFrame(frame, team, room, currentRoomPilot, currentBattlePilot,
                  12, 0, 180);
    Check(!ReadSettlementFrame(1, frame, &snapshot, &error) &&
          error == CaptureError::InvalidSlot,
          "out-of-range slots fail closed");
    PopulateFrame(frame, team, room, currentRoomPilot, currentBattlePilot,
                  2, 12, 180);
    Check(!ReadSettlementFrame(1, frame, &snapshot, &error) &&
          error == CaptureError::InvalidRank,
          "out-of-range native ranks fail closed");
    Check(!ReadSettlementFrame(10, frame, &snapshot, &error) &&
          error == CaptureError::InvalidSite,
          "unknown settlement sites fail closed");

    PopulateFrame(frame, team, room, currentRoomPilot, currentBattlePilot,
                  2, 0, 180);
    const uint32_t rejected = 4;
    Put(frame, team.eligibility_flags, rejected);
    Check(!ReadSettlementFrame(1, frame, &snapshot, &error) &&
          error == CaptureError::NativeIneligiblePath,
          "a native-ineligible frame cannot enter the reward hook");

    const uint32_t clearFlags = 0;
    Put(frame, team.eligibility_flags, clearFlags);
    const uint8_t invalidOutcome = 3;
    Put(currentBattlePilot, kBattlePilotOutcome, invalidOutcome);
    Check(!ReadSettlementFrame(1, frame, &snapshot, &error) &&
          error == CaptureError::InvalidOutcome,
          "invalid native outcomes fail closed");
    Put(currentBattlePilot, kBattlePilotOutcome, outcome);

    const SettlementFrameContract& mission = kSettlementFrames[9];
    PopulateFrame(frame, mission, room, currentRoomPilot, currentBattlePilot,
                  2, 0, 180);
    const uint8_t staleMissionBpOutcome = static_cast<uint8_t>(NativeOutcome::Draw);
    Put(currentBattlePilot, kBattlePilotOutcome, staleMissionBpOutcome);
    Check(ReadSettlementFrame(9, frame, &snapshot, &error) &&
          snapshot.outcome == NativeOutcome::WinOrClear,
          "mission outcome comes from the pre-write frame array");
    Put(currentBattlePilot, kBattlePilotOutcome, outcome);

    participantCount = 99;
    Check(!ReadParticipants(room, participants.data(), 11, &participantCount, &error) &&
          error == CaptureError::BufferTooSmall && participantCount == 0,
          "participant collection never returns a partial undersized snapshot");
    Check(SlotFromBattlePilot(room, currentBattlePilot) == 2 &&
          SlotFromBattlePilot(room, currentBattlePilot + 1) == -1,
          "battle-pilot pointers map only on exact native strides");

    SYSTEM_INFO systemInfo{};
    GetSystemInfo(&systemInfo);
    BYTE* pages = static_cast<BYTE*>(VirtualAlloc(
        nullptr, systemInfo.dwPageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    Check(pages != nullptr, "guard-test pages allocate");
    if (pages) {
        DWORD oldProtect = 0;
        const BOOL protectedPage = VirtualProtect(
            pages + systemInfo.dwPageSize, systemInfo.dwPageSize,
            PAGE_NOACCESS, &oldProtect);
        uint32_t guardedValue = 0;
        Check(protectedPage &&
              !Read(pages + systemInfo.dwPageSize - 2, 0, &guardedValue),
              "native reads reject ranges crossing into PAGE_NOACCESS");
        VirtualFree(pages, 0, MEM_RELEASE);
    }

    Check(EffectiveDamageHookContract::address == 0x0047DA90 &&
          EffectiveDamageHookContract::end_exclusive == 0x0047DD2B &&
          EffectiveDamageHookContract::argument_bytes == 0x14,
          "effective-damage hook byte contract remains pinned");
    printf("%s\n", failures ? "NATIVE STATS CHECKS FAILED"
                            : "ALL NATIVE STATS CHECKS PASSED");
    return failures ? 1 : 0;
}
