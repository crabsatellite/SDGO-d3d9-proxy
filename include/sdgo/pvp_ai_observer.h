// Optional read-only observation ABI for a normally running PvP AI battle server.
//
// The observer never creates rooms, pilots or packets. pvp_ai emits snapshots
// only from its existing native hook boundaries after a real client requested a
// battle. A development Mod may register one process-lifetime sink.
#pragma once

#include <stddef.h>
#include <stdint.h>

#define SDGO_PVP_AI_OBSERVER_VERSION 1
#define SDGO_PVP_AI_OBSERVER_EXPORT "SdgoPvpAiRegisterObserverV1"
#define SDGO_PVP_AI_OBSERVER_SLOT_V1_SIZE 12u
#define SDGO_PVP_AI_OBSERVER_EVENT_V1_SIZE 228u

enum SdgoPvpAiObserverEventTypeV1 {
    SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED = 1,
    SDGO_PVP_AI_EVENT_SETUP_COMPLETE = 2,
    SDGO_PVP_AI_EVENT_BATTLE_ACTIVE = 3,
    SDGO_PVP_AI_EVENT_BATTLE_END = 4,
    SDGO_PVP_AI_EVENT_SETUP_FAILED = 5,
};

#pragma pack(push, 4)

typedef struct SdgoPvpAiObserverSlotV1 {
    uint32_t pilot_id;
    uint32_t unit_or_npc_id; // human Unit id; AI NPC data id
    uint8_t slot;
    uint8_t team;
    uint8_t colour;
    uint8_t is_ai;
} SdgoPvpAiObserverSlotV1;

typedef struct SdgoPvpAiObserverEventV1 {
    uint32_t size;
    uint32_t version;
    uint32_t type;
    uint32_t status; // zero on success; native exception code for SETUP_FAILED
    uint64_t sequence;
    uint64_t qpc;
    uint32_t thread_id;
    uint32_t room_cookie; // process-local correlation only; never dereference it
    uint32_t host_pilot_id;
    int32_t score_team_1;
    int32_t score_team_2;
    uint8_t room_type;
    uint8_t mode;
    uint8_t humans_team_1;
    uint8_t humans_team_2;
    uint8_t ai_team_1;
    uint8_t ai_team_2;
    uint8_t active_slots;
    uint8_t reserved;
    char title[21]; // native room title, NUL terminated
    uint8_t reserved2[3];
    SdgoPvpAiObserverSlotV1 slots[12]; // fixed native slot order
} SdgoPvpAiObserverEventV1;

// Called synchronously on the BattleServer thread that reached the native hook
// boundary. The event pointer is valid only for this call; copy it before
// returning and do not call back into pvp_ai or block the battle thread.
typedef void (__cdecl* SdgoPvpAiObserverCallbackV1)(
    const SdgoPvpAiObserverEventV1* event,
    void* user);

typedef struct SdgoPvpAiObserverSinkV1 {
    uint32_t size;
    uint32_t version;
    SdgoPvpAiObserverCallbackV1 callback;
    void* user;
} SdgoPvpAiObserverSinkV1;

#pragma pack(pop)

typedef int (__cdecl* SdgoPvpAiRegisterObserverV1Fn)(
    const SdgoPvpAiObserverSinkV1* sink);

#ifdef __cplusplus
static_assert(sizeof(SdgoPvpAiObserverSlotV1) == SDGO_PVP_AI_OBSERVER_SLOT_V1_SIZE,
              "PvP AI observer slot ABI changed");
static_assert(offsetof(SdgoPvpAiObserverEventV1, sequence) == 16,
              "PvP AI observer sequence offset changed");
static_assert(offsetof(SdgoPvpAiObserverEventV1, title) == 60,
              "PvP AI observer title offset changed");
static_assert(offsetof(SdgoPvpAiObserverEventV1, slots) == 84,
              "PvP AI observer slots offset changed");
static_assert(sizeof(SdgoPvpAiObserverEventV1) == SDGO_PVP_AI_OBSERVER_EVENT_V1_SIZE,
              "PvP AI observer event ABI changed");
static_assert(offsetof(SdgoPvpAiObserverSinkV1, callback) == 8,
              "PvP AI observer callback offset changed");
static_assert(offsetof(SdgoPvpAiObserverSinkV1, user) ==
                  8 + sizeof(SdgoPvpAiObserverCallbackV1),
              "PvP AI observer user offset changed");
static_assert(sizeof(SdgoPvpAiObserverSinkV1) ==
                  8 + sizeof(SdgoPvpAiObserverCallbackV1) + sizeof(void*),
              "PvP AI observer sink ABI changed");
#endif
