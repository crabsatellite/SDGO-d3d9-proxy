#pragma once

#include <cstddef>
#include <cstdint>

#include "sdgo/battle_rewards_policy.h"

namespace sdgo::battle_rewards::mission_native {

// Verified against BattleServer.exe SHA-256
// f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4.
//
// Room construction at 0x004AF370 copies the selected mission key to
// room+0x1B and stores its resolved mission-data pointer at room+0x316A0.
// Mission initialization at 0x0047F0E0 copies the selected raw difficulty to
// room+0x316F0. The ordinary mission spawn loop at 0x00487A1F stores the
// selected NpcData pointer at BattlePilot+0x1CEC, and native mission event
// serialization at 0x004A0519, 0x004A053A, and 0x004A0738 reads NpcDataID
// from the first field of that record. The broad database BossFlag and
// model/unit identities are deliberately absent from this contract.
constexpr size_t kRoomMode = 0x05;
constexpr size_t kRoomMissionId = 0x1B;
constexpr size_t kRoomMissionData = 0x316A0;
constexpr size_t kRoomMissionState = 0x316A4;
constexpr size_t kRoomMissionDifficulty = 0x316F0;
constexpr size_t kRoomMissionDifficultyIndex = 0x316F1;
constexpr size_t kBattlePilotEntity = 0x00;
constexpr size_t kBattlePilotNpcData = 0x1CEC;
constexpr size_t kNpcDataId = 0x00;
constexpr uint32_t kNpcEntityBit = 0x80000000u;
constexpr uint32_t kMissionStateClear = 5;
constexpr uint32_t kMissionStateFail = 6;

enum class ReadError : uint8_t {
    None,
    MissingRoom,
    MissingVictimPilot,
    MemoryFault,
    NotMissionMode,
    MissingMissionId,
    HumanVictim,
    MissingNpcDataPointer,
    MissingNpcDataId,
    InvalidDifficulty,
    InvalidOutcome
};

struct MissionVictimIdentity {
    uint32_t mission_id = 0;
    uint32_t npc_data_id = 0;
    uint32_t victim_entity = 0;
    uint8_t raw_difficulty = 0;
    Difficulty difficulty = Difficulty::Normal;
};

struct MissionSettlementIdentity {
    uint32_t mission_id = 0;
    uint32_t mission_state = 0;
    uint8_t raw_difficulty = 0;
    Difficulty difficulty = Difficulty::Normal;
    Outcome outcome = Outcome::Fail;
};

struct RewardClassification {
    bool npc = false;
    bool boss = false;
};

inline bool IsMissionMode(uint8_t mode) {
    return mode == 5 || mode == 8 || mode == 10;
}

// The native format supports nine mission-difficulty entries. The current
// owner database uses raw values 1..5. The three reward tiers intentionally
// combine adjacent native levels while preserving the raw value for audit:
// 1..2 Normal, 3..4 Hard, 5..9 Expert.
inline bool DecodeDifficulty(uint8_t raw, Difficulty* difficulty) {
    if (!difficulty || raw == 0 || raw > 9) return false;
    if (raw <= 2) {
        *difficulty = Difficulty::Normal;
    } else if (raw <= 4) {
        *difficulty = Difficulty::Hard;
    } else {
        *difficulty = Difficulty::Expert;
    }
    return true;
}

inline bool DecodeOutcome(uint32_t state, Outcome* outcome) {
    if (!outcome) return false;
    if (state == kMissionStateClear) {
        *outcome = Outcome::Clear;
        return true;
    }
    if (state == kMissionStateFail) {
        *outcome = Outcome::Fail;
        return true;
    }
    return false;
}

namespace detail {

// Reader must have the shape:
//   bool reader(const void* base, size_t offset, void* output, size_t size)
// This keeps platform-specific guarded reads in the native adapter while the
// mission contract remains independently testable.
template <typename Reader, typename T>
inline bool ReadField(Reader& reader, const void* base, size_t offset, T* value) {
    return value && reader(base, offset, static_cast<void*>(value), sizeof(T));
}

}  // namespace detail

template <typename Reader>
inline ReadError ReadMissionVictim(
    const void* room, const void* victim_pilot, Reader& reader,
    MissionVictimIdentity* identity) {
    if (!room) return ReadError::MissingRoom;
    if (!victim_pilot) return ReadError::MissingVictimPilot;
    if (!identity) return ReadError::MemoryFault;

    uint8_t mode = 0;
    MissionVictimIdentity result{};
    if (!detail::ReadField(reader, room, kRoomMode, &mode) ||
        !detail::ReadField(reader, room, kRoomMissionId, &result.mission_id) ||
        !detail::ReadField(
            reader, room, kRoomMissionDifficulty, &result.raw_difficulty) ||
        !detail::ReadField(
            reader, victim_pilot, kBattlePilotEntity,
            &result.victim_entity)) {
        return ReadError::MemoryFault;
    }
    if (!IsMissionMode(mode)) return ReadError::NotMissionMode;
    if (result.mission_id == 0) return ReadError::MissingMissionId;
    if ((result.victim_entity & kNpcEntityBit) == 0) {
        return ReadError::HumanVictim;
    }
    if (!DecodeDifficulty(result.raw_difficulty, &result.difficulty)) {
        return ReadError::InvalidDifficulty;
    }

    const void* npc_data = nullptr;
    if (!detail::ReadField(
            reader, victim_pilot, kBattlePilotNpcData, &npc_data)) {
        return ReadError::MemoryFault;
    }
    if (!npc_data) return ReadError::MissingNpcDataPointer;
    if (!detail::ReadField(
            reader, npc_data, kNpcDataId, &result.npc_data_id)) {
        return ReadError::MemoryFault;
    }
    if (result.npc_data_id == 0) return ReadError::MissingNpcDataId;

    *identity = result;
    return ReadError::None;
}

template <typename Reader>
inline ReadError ReadMissionSettlement(
    const void* room, Reader& reader, MissionSettlementIdentity* identity) {
    if (!room) return ReadError::MissingRoom;
    if (!identity) return ReadError::MemoryFault;

    uint8_t mode = 0;
    MissionSettlementIdentity result{};
    if (!detail::ReadField(reader, room, kRoomMode, &mode) ||
        !detail::ReadField(reader, room, kRoomMissionId, &result.mission_id) ||
        !detail::ReadField(
            reader, room, kRoomMissionDifficulty, &result.raw_difficulty) ||
        !detail::ReadField(
            reader, room, kRoomMissionState, &result.mission_state)) {
        return ReadError::MemoryFault;
    }
    if (!IsMissionMode(mode)) return ReadError::NotMissionMode;
    if (result.mission_id == 0) return ReadError::MissingMissionId;
    if (!DecodeDifficulty(result.raw_difficulty, &result.difficulty)) {
        return ReadError::InvalidDifficulty;
    }
    if (!DecodeOutcome(result.mission_state, &result.outcome)) {
        return ReadError::InvalidOutcome;
    }
    *identity = result;
    return ReadError::None;
}

template <typename BossCatalog>
inline RewardClassification Classify(
    const MissionVictimIdentity& identity, const BossCatalog& catalog) {
    return RewardClassification{
        true, catalog.IsBoss(identity.mission_id, identity.npc_data_id)};
}

}  // namespace sdgo::battle_rewards::mission_native
