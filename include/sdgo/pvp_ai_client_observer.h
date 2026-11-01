// Optional read-only snapshot ABI for a normally running PvP AI client.
//
// The export observes state already produced by the original client and
// match_ai. It never creates a room, participant, unit, packet, or state
// transition. Call it on the GOnline main thread after the native app update.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define SDGO_PVP_AI_CLIENT_OBSERVER_VERSION 1
#define SDGO_PVP_AI_CLIENT_SNAPSHOT_EXPORT "SdgoPvpAiClientGetSnapshotV1"

#pragma pack(push, 8)

struct SdgoPvpAiClientBotSnapshotV1 {
    uint32_t pilot_id;
    uint32_t object_id;
    uint32_t npc_id;
    uint32_t target_object_id;
    int32_t target_hp;
    uint8_t team;
    uint8_t colour;
    uint8_t deck_count;
    uint8_t ai_enabled;
    uint8_t valid_registered_object;
    uint8_t reserved[3];
    uint8_t local_control;
    uint8_t remote_copy;
    uint8_t master_npc;
    uint8_t position_valid;
    float position_x;
    float position_y;
    float position_z;
    uint32_t raw_deck_count;
    uint32_t native_deck_unit_data_ids[4];
    uint8_t native_deck_count_valid;
    uint8_t reserved2[3];
};

struct SdgoPvpAiClientStartDeckSnapshotV1 {
    uint32_t pilot_id;
    uint32_t npc_ids[4];
    int32_t points;
    uint8_t unit_count;
    uint8_t unit_count_valid;
    uint8_t reserved[2];
};

struct SdgoPvpAiClientSnapshotV1 {
    uint32_t size;
    uint32_t version;
    uint32_t native_app_state;
    uint32_t profile_pilot_id;
    uint16_t room_mode;
    uint16_t room_title_length;
    uint32_t human_participant_count;
    uint32_t ai_participant_count;
    uint32_t bot_count;
    uint32_t unit_manager_player_count;
    uint32_t lua_error_count;
    uint8_t lua_error_observer_available;
    uint8_t local_team;
    uint8_t reserved[2];
    uint32_t received_battle_start_count;
    uint16_t received_battle_start_mode;
    uint16_t received_ai_deck_count;
    wchar_t room_title[32];
    SdgoPvpAiClientStartDeckSnapshotV1 received_ai_decks[12];
    SdgoPvpAiClientBotSnapshotV1 bots[12];
    uint32_t create_reply_sequence;
    uint32_t create_reply_room_id;
    uint16_t create_reply_opcode;
    uint16_t create_reply_error;
};

#pragma pack(pop)

static_assert(sizeof(void*) == 4, "the PvP AI client observer ABI is Win32-only");
static_assert(sizeof(wchar_t) == 2, "the PvP AI client observer ABI requires Windows wchar_t");
static_assert(sizeof(SdgoPvpAiClientBotSnapshotV1) == 68,
              "unexpected PvP AI bot snapshot layout");
static_assert(offsetof(SdgoPvpAiClientBotSnapshotV1, local_control) == 28,
              "unexpected PvP AI local-control offset");
static_assert(offsetof(SdgoPvpAiClientBotSnapshotV1, position_x) == 32,
              "unexpected PvP AI position offset");
static_assert(offsetof(SdgoPvpAiClientBotSnapshotV1, raw_deck_count) == 44,
              "unexpected PvP AI raw deck-count offset");
static_assert(offsetof(SdgoPvpAiClientBotSnapshotV1, native_deck_unit_data_ids) == 48,
              "unexpected PvP AI native deck-data offset");
static_assert(offsetof(SdgoPvpAiClientBotSnapshotV1, native_deck_count_valid) == 64,
              "unexpected PvP AI native deck-validity offset");
static_assert(sizeof(SdgoPvpAiClientStartDeckSnapshotV1) == 28,
              "unexpected PvP AI deck snapshot layout");
static_assert(offsetof(SdgoPvpAiClientStartDeckSnapshotV1, unit_count_valid) == 25,
              "unexpected PvP AI packet deck-validity offset");
static_assert(sizeof(SdgoPvpAiClientSnapshotV1) == 1280,
              "unexpected PvP AI client snapshot layout");
static_assert(offsetof(SdgoPvpAiClientSnapshotV1, lua_error_observer_available) == 40,
              "unexpected PvP AI Lua observer offset");
static_assert(offsetof(SdgoPvpAiClientSnapshotV1, local_team) == 41,
              "unexpected PvP AI local-team offset");
static_assert(offsetof(SdgoPvpAiClientSnapshotV1, received_battle_start_count) == 44,
              "unexpected PvP AI battle-start offset");
static_assert(offsetof(SdgoPvpAiClientSnapshotV1, room_title) == 52,
              "unexpected PvP AI room-title offset");
static_assert(offsetof(SdgoPvpAiClientSnapshotV1, received_ai_decks) == 116,
              "unexpected PvP AI deck array offset");
static_assert(offsetof(SdgoPvpAiClientSnapshotV1, bots) == 452,
              "unexpected PvP AI bot array offset");
static_assert(offsetof(SdgoPvpAiClientSnapshotV1, create_reply_sequence) == 1268,
              "unexpected native room reply offset");

typedef int(__cdecl* SdgoPvpAiClientGetSnapshotV1Fn)(
    SdgoPvpAiClientSnapshotV1* snapshot);

// Counters survive normal scene teardown. A separate ABI preserves the
// published 1280-byte room snapshot for existing consumers.
#define SDGO_PVP_AI_CLIENT_LUA_STATUS_EXPORT "SdgoPvpAiClientGetLuaStatusV1"
struct SdgoPvpAiClientLuaStatusV1 {
    uint32_t size;
    uint32_t version;
    uint32_t battle_start_sequence;
    uint32_t error_count;
    uint32_t observer_available;
};
static_assert(sizeof(SdgoPvpAiClientLuaStatusV1) == 20, "unexpected Lua status ABI");
typedef int(__cdecl* SdgoPvpAiClientGetLuaStatusV1Fn)(SdgoPvpAiClientLuaStatusV1* status);
