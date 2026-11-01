// Optional read-only snapshot ABI for a normally running co-op mission client.
//
// mission_ai publishes state already produced by the original client and the
// production wingman spawn path. The export never creates a room, participant,
// object, packet, result, or state transition. Call it on the GOnline main
// thread. The export copies a versioned cache and never dereferences game
// objects; the native update hook is the only code that reads them.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION 1u
#define SDGO_MISSION_AI_CLIENT_SNAPSHOT_EXPORT \
    "SdgoMissionAiClientGetSnapshotV1"

enum SdgoMissionAiModeClassV1 : uint16_t {
    SDGO_MISSION_AI_MODE_CLASS_NONE = 0,
    SDGO_MISSION_AI_MODE_CLASS_PLAIN_MISSION = 1,
    SDGO_MISSION_AI_MODE_CLASS_OTHER = 2,
};

#pragma pack(push, 8)

struct SdgoMissionAiParticipantSnapshotV1 {
    uint32_t pilot_id;
    uint32_t connection_id;
    uint32_t record_npc_id;
    uint32_t object_id;
    uint32_t unit_server_id;
    uint32_t native_npc_id;
    uint32_t target_object_id;
    int32_t current_hp;
    int32_t target_hp;
    float position_x;
    float position_y;
    float position_z;
    uint8_t record_team;
    uint8_t unit_team;
    uint8_t local_control;
    uint8_t remote_copy;
    uint8_t master_npc;
    uint8_t ai_enabled;
    uint8_t valid_registered_object;
    uint8_t position_valid;
    uint8_t target_valid;
    uint8_t mapping_valid;
    uint8_t movement_observed;
    uint8_t spawned;
    uint8_t reserved[4];
};

struct SdgoMissionAiClientSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t battle_generation;
    uint32_t last_native_update_tick;
    uint32_t mode_object;
    uint32_t mode_vtable;
    uint32_t mission_vtable;
    uint32_t room_object;
    uint32_t unit_manager_object;
    uint32_t lua_pcall_error_count;
    uint32_t lua_tick_pcall_error_count;
    uint32_t lua_load_pcall_error_count;
    uint32_t lua_nested_pcall_error_count;
    uint32_t lua_tick_count;
    uint32_t lua_successful_tick_count;
    uint32_t native_exception_count;
    uint32_t last_native_exception;
    uint32_t human_participant_count;
    uint32_t ai_record_count;
    uint32_t spawned_wingman_count;
    uint32_t local_wingman_count;
    uint32_t unit_manager_player_count;
    uint16_t room_mode;
    uint16_t mode_class;
    uint8_t active;
    uint8_t exact_plain_mission;
    uint8_t local_is_host;
    uint8_t local_is_observer;
    uint8_t network_game;
    uint8_t mission_ai_disabled;
    uint8_t find_object_available;
    uint8_t room_records_valid;
    SdgoMissionAiParticipantSnapshotV1 participants[12];
};

#pragma pack(pop)

static_assert(sizeof(void*) == 4,
              "the mission AI client observer ABI is Win32-only");
static_assert(sizeof(SdgoMissionAiParticipantSnapshotV1) == 64,
              "unexpected mission participant snapshot layout");
static_assert(
    offsetof(SdgoMissionAiParticipantSnapshotV1, record_team) == 48,
    "unexpected mission participant flag offset");
static_assert(
    offsetof(SdgoMissionAiParticipantSnapshotV1, movement_observed) == 58,
    "unexpected mission participant movement offset");
static_assert(sizeof(SdgoMissionAiClientSnapshotV1) == 868,
              "unexpected mission AI client snapshot layout");
static_assert(offsetof(SdgoMissionAiClientSnapshotV1, room_mode) == 88,
              "unexpected mission room-mode offset");
static_assert(offsetof(SdgoMissionAiClientSnapshotV1, participants) == 100,
              "unexpected mission participant array offset");

typedef int(__cdecl* SdgoMissionAiClientGetSnapshotV1Fn)(
    SdgoMissionAiClientSnapshotV1* snapshot);
