// mission_ai - client side of the co-op mission wingmen.
//
// Wingmen are AI pilots that the battle server mod coop_ai adds to a co-op mission room
// (room title "!aiN lvN"), the way the server's AI battle modes add their AI players.
// Native side:
//   * Detours CMissionMode::Init and spawns the room's AI participants with the steps the
//     AI battle mode (CAINormalGameMode) uses, so they are real players of the battle:
//     the server knows their hits, kills and deaths, and they run the AI battle bots' own
//     Lua AI (VSAI_* scripts, loaded by the game at start-up).
//   * Detours CMissionMode::Update through its vftable slot and ticks mission_ai.lua in
//     the game's Lua 5.0 state; the script lets mission enemies pick wingmen as targets
//     and logs their status. Edits to mission_ai.lua or mod.ini apply within 2 seconds.

#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sdgo/mission_ai_client_observer.h"
#include "sdgo/sdgo_api.h"

namespace
{
    // ---- the game's Lua 5.0 C API (statically linked into GOnline.exe) -------

    struct lua_State;
    constexpr int kLuaGlobals = -10001; // LUA_GLOBALSINDEX in Lua 5.0
    constexpr int kLuaFunction = 6;     // LUA_TFUNCTION in Lua 5.0
    using LuaCFunction = int(__cdecl*)(lua_State*);
    using LuaReader = const char*(__cdecl*)(lua_State*, void*, size_t*);

    struct GameLua
    {
        lua_State*(__cdecl* state)(lua_State* set);
        void(__cdecl* pushMode)(lua_State*, void* mode);
        int(__cdecl* gettop)(lua_State*);
        void(__cdecl* settop)(lua_State*, int);
        int(__cdecl* type)(lua_State*, int);
        double(__cdecl* tonumber)(lua_State*, int);
        const char*(__cdecl* tostring)(lua_State*, int);
        void(__cdecl* pushnumber)(lua_State*, double);
        void(__cdecl* pushstring)(lua_State*, const char*);
        void(__cdecl* pushcclosure)(lua_State*, LuaCFunction, int);
        void(__cdecl* gettable)(lua_State*, int);
        void(__cdecl* settable)(lua_State*, int);
        int(__cdecl* pcall)(lua_State*, int, int, int);
        int(__cdecl* load)(lua_State*, LuaReader, void*, const char*);
    };

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    GameLua g_lua = {};

    using MissionUpdateFn = void(__thiscall*)(void* mode);
    MissionUpdateFn g_originalUpdate = nullptr;

    std::string g_configChunk; // SDGO_MissionAI_Config = {...}, built from mod.ini
    std::string g_scriptPath;  // absolute path of mission_ai.lua (ANSI, for the chunk name)
    std::wstring g_scriptPathW;
    std::wstring g_manifestPathW;

    lua_State* g_loadedState = nullptr;
    DWORD g_lastLoadAttempt = 0;
    int g_loadFailures = 0;
    int g_tickErrors = 0;
    bool g_disabled = false;
    volatile LONG g_battleGeneration = 0;
    volatile LONG g_battlePcallErrors = 0;
    volatile LONG g_battleTickPcallErrors = 0;
    volatile LONG g_battleLoadPcallErrors = 0;
    volatile LONG g_battleNestedPcallErrors = 0;
    volatile LONG g_battleTickCount = 0;
    volatile LONG g_battleSuccessfulTickCount = 0;
    volatile LONG g_nativeExceptionCount = 0;
    volatile LONG g_lastNativeException = 0;

    FILETIME g_loadedScriptTime = {};
    FILETIME g_loadedManifestTime = {};
    DWORD g_lastReloadCheck = 0;

    void* g_lastMode = nullptr;
    DWORD g_lastTick = 0;
    double g_session = 0;

    template <typename Fn>
    bool Bind(Fn& out, const char* symbol)
    {
        out = reinterpret_cast<Fn>(g_api->Symbol(g_mod, symbol));
        return out != nullptr;
    }

    FILETIME WriteTime(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            return FILETIME{};
        return data.ftLastWriteTime;
    }

    bool SameTime(const FILETIME& a, const FILETIME& b)
    {
        return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
    }

    // ---- AI participants of a co-op mission --------------------------------
    //
    // The battle server mod coop_ai adds AI pilots to a co-op mission room the way the AI
    // battle modes do. The client puts them into room+0x3C (records of 0x344 bytes, after
    // the humans in room+0x1C), but CMissionMode::Init only spawns the humans. Right after
    // it, this spawns every AI record with the same steps CAINormalGameMode::Init uses
    // (its AI loop at 0x004049B3..0x00404DA5, thiscall throughout):
    //   start point  = UM->FindStartPoint(team, index)               (0x00499930)
    //   unit         = room master and not observer -> mode->CreateNpc   (0x00ADF970, CNPC
    //                  running the Lua AI named by its NPC data: the AI battle bots' VSAI_*),
    //                  else mode->vt[0x1C]() ? CreateRemoteNpc (0x00ACF030) : CreateNpc
    //   UM->SetPlayerInfo(slot, 0, unit+4, record+4, record)            (0x0049DFE0)
    //   UM->PlayerSlot(slot)+8 = NPC name (npc data +5)                (0x004039E0, 0x004026F0)
    //   UM->AddAiUnit(unit); unit+0x2B68 = record+0 (server id)        (0x0049DB40)
    //   UM->CalculateTeam() once after the loop                        (0x0049E0F0)
    // The player count (UM+0x8C) stays at the humans, unlike AI battle mode: mission
    // scripts loop GetPlayer(1..GetNumPlayer()) and GetPlayer only knows humans.

    constexpr size_t kModeRoom = 0x08;          // room data*
    constexpr size_t kModeRoomMaster = 0x61C;   // u8, local player is room master
    constexpr size_t kModeRoomMode = 0x66;      // u16, room data: battle mode
    constexpr size_t kRoomHumansBegin = 0x1C;   // vector of 0x344-byte records
    constexpr size_t kRoomHumansEnd = 0x20;
    constexpr size_t kRoomAiBegin = 0x3C;
    constexpr size_t kRoomAiEnd = 0x40;
    constexpr size_t kRecordSize = 0x344;
    constexpr size_t kRecordServerId = 0x00;
    constexpr size_t kRecordConnection = 0x04;
    constexpr size_t kRecordTeam = 0x1A;
    constexpr size_t kRecordNpcData = 0x44;
    constexpr size_t kStartPointPos = 0x24;     // x, y, z floats
    constexpr size_t kUnitObjectId = 0x04;
    constexpr size_t kUnitNode = 0x08;
    constexpr size_t kUnitTeam = 0x20;
    constexpr size_t kObjectHp = 0x38;
    constexpr size_t kUnitServerId = 0x2B68;
    constexpr size_t kUnitLocalControl = 0x2B64;
    constexpr size_t kUnitRemote = 0x2B65;
    constexpr size_t kUnitMasterNpc = 0x3074;
    constexpr size_t kUnitTargetObject = 0x3130;
    constexpr size_t kUnitNpcData = 0x320C;     // npc data*, name (wchar) at +5
    constexpr size_t kUnitNpcDataId = 0x3238;
    constexpr size_t kUnitAiEnabled = 0x326C;
    constexpr size_t kNodePosition = 0x88;
    constexpr size_t kUnitManagerPlayerCount = 0x8C;
    constexpr size_t kSlotName = 0x08;          // std::wstring in the player slot
    constexpr size_t kModeIsNetworkSlot = 0x1C / sizeof(void*);

    using ModeInitFn = void(__thiscall*)(uint8_t* mode);
    using IsNetworkFn = bool(__cdecl*)();
    using FindStartPointFn = uint8_t*(__thiscall*)(void* um, int team, int index);
    using IsObserverFn = bool(__thiscall*)(void* um);
    using CreateNpcFn = uint8_t*(__thiscall*)(uint8_t* mode, uint32_t npcData, float x, float y, float z, int team,
                                              float dx, float dy, float dz);
    using SetPlayerInfoFn = void(__thiscall*)(void* um, int slot, int isLocal, uint32_t objectId, uint32_t connection,
                                              uint8_t* record);
    using PlayerSlotFn = uint8_t*(__thiscall*)(void* um, int slot);
    using WStringAssignFn = void(__thiscall*)(void* string, const wchar_t* text, size_t length);
    using AddAiUnitFn = void(__thiscall*)(void* um, uint8_t* unit);
    using CalculateTeamFn = void(__thiscall*)(void* um);
    using ModeVirtualFn = bool(__thiscall*)(uint8_t* mode);
    using FindObjectFn = void*(__thiscall*)(void* um, uint32_t objectId);

    struct GameCalls
    {
        ModeInitFn originalInit;
        const void* missionVtable;
        void** unitManager;
        IsNetworkFn isNetwork;
        FindStartPointFn findStartPoint;
        IsObserverFn isObserver;
        CreateNpcFn createNpc;
        CreateNpcFn createRemoteNpc;
        SetPlayerInfoFn setPlayerInfo;
        PlayerSlotFn playerSlot;
        WStringAssignFn assign;
        AddAiUnitFn addAiUnit;
        CalculateTeamFn calculateTeam;
        FindObjectFn findObject;
    };
    GameCalls g_game = {};

    std::vector<uint32_t> g_wingmen; // object ids of the AI participants of this mission
    std::vector<uint32_t> g_localWingmen; // only this client's simulated participants

    struct ObservedWingman
    {
        uint32_t recordIndex = 0;
        uint32_t pilotId = 0;
        uint32_t objectId = 0;
        uint8_t* spawnedUnit = nullptr;
        bool local = false;
        bool hasPosition = false;
        bool moved = false;
        float lastX = 0.0f;
        float lastY = 0.0f;
        float lastZ = 0.0f;
    };

    struct RecordSpan
    {
        uint8_t* first = nullptr;
        uint32_t count = 0;
        bool valid = false;
    };

    std::vector<ObservedWingman> g_observedWingmen;
    uint8_t* g_observedMode = nullptr;
    SdgoMissionAiClientSnapshotV1 g_observerSnapshot = {};
    volatile LONG g_observerSnapshotVersion = 0;
    SRWLOCK g_observerSnapshotLock = SRWLOCK_INIT;

    uint32_t PointerValue(const void* pointer)
    {
        return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(pointer));
    }

    RecordSpan ReadRecordSpan(uint8_t* room, size_t begin, size_t end)
    {
        RecordSpan span;
        if (!room)
            return span;
        uint8_t* first = *reinterpret_cast<uint8_t**>(room + begin);
        uint8_t* last = *reinterpret_cast<uint8_t**>(room + end);
        if (!first && !last) {
            span.valid = true;
            return span;
        }
        const uintptr_t firstAddress = reinterpret_cast<uintptr_t>(first);
        const uintptr_t lastAddress = reinterpret_cast<uintptr_t>(last);
        if (!first || !last || lastAddress < firstAddress ||
            (lastAddress - firstAddress) % kRecordSize != 0)
            return span;
        const uintptr_t count = (lastAddress - firstAddress) / kRecordSize;
        if (count > 12)
            return span;
        span.first = first;
        span.count = static_cast<uint32_t>(count);
        span.valid = true;
        return span;
    }

    int RecordCount(uint8_t* room, size_t begin, size_t end)
    {
        auto first = reinterpret_cast<uintptr_t>(*reinterpret_cast<uint8_t**>(room + begin));
        auto last = reinterpret_cast<uintptr_t>(*reinterpret_cast<uint8_t**>(room + end));
        if (!first || last <= first || (last - first) % kRecordSize != 0 || (last - first) / kRecordSize > 12)
            return 0;
        return static_cast<int>((last - first) / kRecordSize);
    }

    ObservedWingman* FindObservedWingman(
        uint32_t recordIndex,
        uint32_t pilotId)
    {
        for (ObservedWingman& wingman : g_observedWingmen)
            if (wingman.recordIndex == recordIndex &&
                wingman.pilotId == pilotId)
                return &wingman;
        return nullptr;
    }

    void PublishObserverSnapshot(const SdgoMissionAiClientSnapshotV1& snapshot)
    {
        AcquireSRWLockExclusive(&g_observerSnapshotLock);
        InterlockedIncrement(&g_observerSnapshotVersion);
        g_observerSnapshot = snapshot;
        InterlockedIncrement(&g_observerSnapshotVersion);
        ReleaseSRWLockExclusive(&g_observerSnapshotLock);
    }

    void ResetBattleCounters()
    {
        InterlockedExchange(&g_battlePcallErrors, 0);
        InterlockedExchange(&g_battleTickPcallErrors, 0);
        InterlockedExchange(&g_battleLoadPcallErrors, 0);
        InterlockedExchange(&g_battleNestedPcallErrors, 0);
        InterlockedExchange(&g_battleTickCount, 0);
        InterlockedExchange(&g_battleSuccessfulTickCount, 0);
        InterlockedExchange(&g_nativeExceptionCount, 0);
        InterlockedExchange(&g_lastNativeException, 0);
    }

    void BeginMissionObservation(uint8_t* mode)
    {
        g_observedMode = mode;
        g_observedWingmen.clear();
        ResetBattleCounters();
        InterlockedIncrement(&g_battleGeneration);
    }

    void CountPcallError(bool tick)
    {
        if (InterlockedCompareExchange(&g_battleGeneration, 0, 0) == 0)
            return;
        InterlockedIncrement(&g_battlePcallErrors);
        InterlockedIncrement(tick ? &g_battleTickPcallErrors
                                  : &g_battleLoadPcallErrors);
    }

    void CountNestedPcallError()
    {
        if (InterlockedCompareExchange(
                &g_battleGeneration,
                0,
                0) == 0)
            return;
        InterlockedIncrement(&g_battlePcallErrors);
        InterlockedIncrement(&g_battleNestedPcallErrors);
    }

    void RecordNativeException(DWORD code)
    {
        InterlockedIncrement(&g_nativeExceptionCount);
        InterlockedExchange(
            &g_lastNativeException,
            static_cast<LONG>(code));
    }

    SdgoMissionAiClientSnapshotV1 BuildObserverSnapshot(uint8_t* mode)
    {
        SdgoMissionAiClientSnapshotV1 snapshot = {};
        snapshot.size = sizeof(snapshot);
        snapshot.version = SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION;
        snapshot.battle_generation = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_battleGeneration, 0, 0));
        snapshot.last_native_update_tick = GetTickCount();
        snapshot.lua_pcall_error_count = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_battlePcallErrors, 0, 0));
        snapshot.lua_tick_pcall_error_count = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_battleTickPcallErrors, 0, 0));
        snapshot.lua_load_pcall_error_count = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_battleLoadPcallErrors, 0, 0));
        snapshot.lua_nested_pcall_error_count = static_cast<uint32_t>(
            InterlockedCompareExchange(
                &g_battleNestedPcallErrors,
                0,
                0));
        snapshot.lua_tick_count = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_battleTickCount, 0, 0));
        snapshot.lua_successful_tick_count = static_cast<uint32_t>(
            InterlockedCompareExchange(
                &g_battleSuccessfulTickCount,
                0,
                0));
        snapshot.native_exception_count = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_nativeExceptionCount, 0, 0));
        snapshot.last_native_exception = static_cast<uint32_t>(
            InterlockedCompareExchange(&g_lastNativeException, 0, 0));
        snapshot.mission_vtable = PointerValue(g_game.missionVtable);
        snapshot.find_object_available = g_game.findObject ? 1 : 0;
        snapshot.mission_ai_disabled = g_disabled ? 1 : 0;
        if (!mode)
            return snapshot;

        snapshot.mode_object = PointerValue(mode);
        const void* modeVtable =
            *reinterpret_cast<const void* const*>(mode);
        snapshot.mode_vtable = PointerValue(modeVtable);
        snapshot.exact_plain_mission =
            modeVtable == g_game.missionVtable ? 1 : 0;
        snapshot.mode_class = snapshot.exact_plain_mission
                                  ? SDGO_MISSION_AI_MODE_CLASS_PLAIN_MISSION
                                  : SDGO_MISSION_AI_MODE_CLASS_OTHER;
        if (!snapshot.exact_plain_mission ||
            mode != g_observedMode)
            return snapshot;

        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        void* unitManager =
            g_game.unitManager ? *g_game.unitManager : nullptr;
        snapshot.room_object = PointerValue(room);
        snapshot.unit_manager_object = PointerValue(unitManager);
        snapshot.local_is_host = mode[kModeRoomMaster] != 0 ? 1 : 0;
        snapshot.network_game =
            g_game.isNetwork && g_game.isNetwork() ? 1 : 0;
        snapshot.local_is_observer =
            unitManager && g_game.isObserver &&
                    g_game.isObserver(unitManager)
                ? 1
                : 0;
        if (!room || !unitManager || !g_game.findObject)
            return snapshot;

        const RecordSpan humans =
            ReadRecordSpan(room, kRoomHumansBegin, kRoomHumansEnd);
        const RecordSpan ai =
            ReadRecordSpan(room, kRoomAiBegin, kRoomAiEnd);
        snapshot.room_records_valid =
            humans.valid && ai.valid ? 1 : 0;
        if (!snapshot.room_records_valid)
            return snapshot;

        snapshot.room_mode =
            *reinterpret_cast<const uint16_t*>(room + kModeRoomMode);
        snapshot.human_participant_count = humans.count;
        snapshot.ai_record_count = ai.count;
        snapshot.spawned_wingman_count =
            static_cast<uint32_t>(g_observedWingmen.size());
        snapshot.unit_manager_player_count =
            *reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(unitManager) +
                kUnitManagerPlayerCount);
        for (const ObservedWingman& wingman : g_observedWingmen)
            if (wingman.local)
                ++snapshot.local_wingman_count;
        if (snapshot.room_mode != 5 &&
            snapshot.room_mode != 8 &&
            snapshot.room_mode != 10)
            return snapshot;

        for (uint32_t index = 0; index < ai.count; ++index) {
            uint8_t* record = ai.first + index * kRecordSize;
            SdgoMissionAiParticipantSnapshotV1& copied =
                snapshot.participants[index];
            copied.pilot_id =
                *reinterpret_cast<const uint32_t*>(
                    record + kRecordServerId);
            copied.connection_id =
                *reinterpret_cast<const uint32_t*>(
                    record + kRecordConnection);
            copied.record_npc_id =
                *reinterpret_cast<const uint32_t*>(
                    record + kRecordNpcData);
            copied.record_team = record[kRecordTeam];

            ObservedWingman* observed =
                FindObservedWingman(index, copied.pilot_id);
            if (!observed)
                continue;
            copied.spawned = 1;
            copied.object_id = observed->objectId;

            uint8_t* unit = static_cast<uint8_t*>(
                g_game.findObject(
                    unitManager,
                    observed->objectId));
            copied.valid_registered_object =
                unit &&
                        observed->objectId != 0 &&
                        unit == observed->spawnedUnit &&
                        *reinterpret_cast<const uint32_t*>(
                            unit + kUnitObjectId) ==
                            observed->objectId
                    ? 1
                    : 0;
            if (!copied.valid_registered_object)
                continue;

            copied.unit_server_id =
                *reinterpret_cast<const uint32_t*>(
                    unit + kUnitServerId);
            copied.native_npc_id =
                *reinterpret_cast<const uint32_t*>(
                    unit + kUnitNpcDataId);
            copied.target_object_id =
                *reinterpret_cast<const uint32_t*>(
                    unit + kUnitTargetObject);
            copied.current_hp =
                *reinterpret_cast<const int32_t*>(
                    unit + kObjectHp);
            copied.unit_team = unit[kUnitTeam];
            copied.local_control =
                unit[kUnitLocalControl] != 0 ? 1 : 0;
            copied.remote_copy =
                unit[kUnitRemote] != 0 ? 1 : 0;
            copied.master_npc =
                unit[kUnitMasterNpc] != 0 ? 1 : 0;
            copied.ai_enabled =
                unit[kUnitAiEnabled] != 0 ? 1 : 0;
            copied.mapping_valid =
                copied.unit_server_id == copied.pilot_id &&
                        copied.native_npc_id ==
                            copied.record_npc_id &&
                        copied.unit_team == copied.record_team
                    ? 1
                    : 0;

            const uint8_t* node =
                *reinterpret_cast<uint8_t* const*>(
                    unit + kUnitNode);
            if (node) {
                const float* position =
                    reinterpret_cast<const float*>(
                        node + kNodePosition);
                const float x = position[0];
                const float y = position[1];
                const float z = position[2];
                if (std::isfinite(x) &&
                    std::isfinite(y) &&
                    std::isfinite(z)) {
                    copied.position_valid = 1;
                    copied.position_x = x;
                    copied.position_y = y;
                    copied.position_z = z;
                    if (observed->hasPosition) {
                        const float dx = x - observed->lastX;
                        const float dy = y - observed->lastY;
                        const float dz = z - observed->lastZ;
                        if (dx * dx + dy * dy + dz * dz >
                            0.0001f)
                            observed->moved = true;
                    }
                    observed->hasPosition = true;
                    observed->lastX = x;
                    observed->lastY = y;
                    observed->lastZ = z;
                }
            }
            copied.movement_observed =
                observed->moved ? 1 : 0;

            if (copied.target_object_id) {
                const uint8_t* target =
                    static_cast<const uint8_t*>(
                        g_game.findObject(
                            unitManager,
                            copied.target_object_id));
                if (target &&
                    *reinterpret_cast<const uint32_t*>(
                        target + kUnitObjectId) ==
                        copied.target_object_id) {
                    copied.target_valid = 1;
                    copied.target_hp =
                        *reinterpret_cast<const int32_t*>(
                            target + kObjectHp);
                }
            }
        }
        snapshot.active = 1;
        return snapshot;
    }

    bool GuardedRefreshObserver(uint8_t* mode, DWORD* code)
    {
        __try {
            PublishObserverSnapshot(
                BuildObserverSnapshot(mode));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *code = GetExceptionCode();
            return false;
        }
    }

    bool CopyObserverSnapshot(
        SdgoMissionAiClientSnapshotV1* output)
    {
        if (!output ||
            output->size < sizeof(*output) ||
            output->version !=
                SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION)
            return false;
        {
            // The export may be read off-thread. A sequence retry around a
            // non-atomic struct copy is still a C++ data race; synchronize the
            // copy itself, and never hold this lock while reading game memory.
            AcquireSRWLockShared(&g_observerSnapshotLock);
            SdgoMissionAiClientSnapshotV1 snapshot =
                g_observerSnapshot;
            ReleaseSRWLockShared(&g_observerSnapshotLock);
            if (snapshot.size == 0) {
                snapshot.size = sizeof(snapshot);
                snapshot.version =
                    SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION;
            }
            if (snapshot.active &&
                GetTickCount() -
                        snapshot.last_native_update_tick >
                    3000)
                snapshot.active = 0;
            *output = snapshot;
            return true;
        }
    }

    void SpawnAiParticipants(uint8_t* mode)
    {
        g_wingmen.clear();
        g_localWingmen.clear();
        g_observedWingmen.clear();
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        if (!room)
            return;
        int count = RecordCount(room, kRoomAiBegin, kRoomAiEnd);
        if (count == 0)
            return;
        if (!g_game.isNetwork()) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "%d AI participant(s) in the room, but the mission took the offline path", count);
            return;
        }
        void* um = *g_game.unitManager;
        int humans = RecordCount(room, kRoomHumansBegin, kRoomHumansEnd);
        uint8_t* records = *reinterpret_cast<uint8_t**>(room + kRoomAiBegin);
        bool localAi = mode[kModeRoomMaster] != 0 && !g_game.isObserver(um);
        auto modeVirtual = (*reinterpret_cast<ModeVirtualFn* const*>(mode))[kModeIsNetworkSlot];
        for (int k = 0; k < count && humans + k < 12; ++k) {
            uint8_t* record = records + k * kRecordSize;
            int team = record[kRecordTeam];
            uint32_t npcData = *reinterpret_cast<uint32_t*>(record + kRecordNpcData);
            // team-1 start points 0..humans-1 belong to the humans; take the next ones
            uint8_t* start = g_game.findStartPoint(um, team, humans + k);
            if (!start)
                start = g_game.findStartPoint(um, team, k);
            if (!start) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "AI participant %08lX: the map has no team %d start point",
                           static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(record)), team);
                continue;
            }
            auto pos = reinterpret_cast<float*>(start + kStartPointPos);
            bool local = localAi || !modeVirtual(mode);
            CreateNpcFn create = local ? g_game.createNpc : g_game.createRemoteNpc;
            uint8_t* unit = create(mode, npcData, pos[0], pos[1], pos[2], team, 0.0f, 0.0f, 1.0f);
            if (!unit) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "AI participant %08lX: creating NPC %lu failed",
                           static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(record)),
                           static_cast<unsigned long>(npcData));
                continue;
            }
            int slot = humans + k;
            uint32_t objectId = *reinterpret_cast<uint32_t*>(unit + kUnitObjectId);
            g_game.setPlayerInfo(um, slot, 0, objectId, *reinterpret_cast<uint32_t*>(record + kRecordConnection), record);
            const uint8_t* npc = *reinterpret_cast<uint8_t**>(unit + kUnitNpcData);
            if (npc) {
                auto name = reinterpret_cast<const wchar_t*>(npc + 5);
                g_game.assign(g_game.playerSlot(um, slot) + kSlotName, name, wcslen(name));
            }
            g_game.addAiUnit(um, unit);
            *reinterpret_cast<uint32_t*>(unit + kUnitServerId) = *reinterpret_cast<uint32_t*>(record + kRecordServerId);
            g_wingmen.push_back(objectId);
            if (local)
                g_localWingmen.push_back(objectId);
            ObservedWingman observed;
            observed.recordIndex = static_cast<uint32_t>(k);
            observed.pilotId =
                *reinterpret_cast<uint32_t*>(
                    record + kRecordServerId);
            observed.objectId = objectId;
            observed.spawnedUnit = unit;
            observed.local = local;
            g_observedWingmen.push_back(observed);
            g_api->Log(g_mod, SDGO_LOG_INFO, "AI participant %08lX joined as object %lu (NPC %lu, team %d, slot %d, %s)",
                       static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(record)),
                       static_cast<unsigned long>(objectId), static_cast<unsigned long>(npcData), team, slot,
                       local ? "AI runs here" : "remote");
        }
        g_game.calculateTeam(um);
    }

    bool GuardedSpawn(uint8_t* mode, DWORD* code)
    {
        __try {
            SpawnAiParticipants(mode);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *code = GetExceptionCode();
            return false;
        }
    }

    void __fastcall MissionInit(uint8_t* mode, void* /*edx*/)
    {
        // Rule initialization may warp humans before spawning the new wingmen.
        // Never expose the previous mission's reused object ids to that callback.
        const bool exactMission =
            *reinterpret_cast<const void* const*>(mode) ==
            g_game.missionVtable;
        if (exactMission) {
            g_wingmen.clear();
            g_localWingmen.clear();
            BeginMissionObservation(mode);
        }
        g_game.originalInit(mode);
        // CAINormalGameMode and the other AI modes derive from CMissionMode and spawn
        // their AI participants themselves: only a plain co-op mission is handled here.
        if (!exactMission)
            return;
        DWORD code = 0;
        if (!GuardedSpawn(mode, &code)) {
            RecordNativeException(code);
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while spawning AI participants", code);
        }
        if (!GuardedRefreshObserver(mode, &code)) {
            RecordNativeException(code);
            g_api->Log(g_mod, SDGO_LOG_WARN, "exception %08lX while observing mission participants", code);
        }
    }

    // ---- functions exposed to mission_ai.lua -------------------------------

    int __cdecl Lua_Log(lua_State* L)
    {
        const char* text = g_lua.tostring(L, 1);
        g_api->Log(g_mod, SDGO_LOG_INFO, "%s", text ? text : "(nil)");
        return 0;
    }

    // SDGO_Now() -> milliseconds (GetTickCount)
    int __cdecl Lua_Now(lua_State* L)
    {
        g_lua.pushnumber(L, static_cast<double>(GetTickCount()));
        return 1;
    }

    // SDGO_Wingmen() -> object ids of this mission's AI participants (multiple results)
    int __cdecl Lua_Wingmen(lua_State* L)
    {
        for (uint32_t id : g_wingmen)
            g_lua.pushnumber(L, static_cast<double>(id));
        return static_cast<int>(g_wingmen.size());
    }

    int __cdecl Lua_LocalWingmen(lua_State* L)
    {
        for (uint32_t id : g_localWingmen)
            g_lua.pushnumber(L, static_cast<double>(id));
        return static_cast<int>(g_localWingmen.size());
    }

    // Called by the Lua wrapper only after one of its nested protected calls
    // returns an error. This records the real pcall outcome; it does not create
    // or alter any game state.
    int __cdecl Lua_PcallError(lua_State*)
    {
        CountNestedPcallError();
        return 0;
    }

    // ---- loading Lua source into the game's state --------------------------

    struct Chunk
    {
        const char* data;
        size_t size;
    };

    const char* __cdecl ReadChunk(lua_State*, void* user, size_t* size)
    {
        auto* chunk = static_cast<Chunk*>(user);
        if (chunk->size == 0)
            return nullptr;
        *size = chunk->size;
        chunk->size = 0;
        return chunk->data;
    }

    bool RunChunk(lua_State* L, const std::string& code, const char* name)
    {
        int top = g_lua.gettop(L);
        Chunk chunk = {code.data(), code.size()};
        int status = g_lua.load(L, ReadChunk, &chunk, name);
        if (status == 0) {
            status = g_lua.pcall(L, 0, 0, 0);
            if (status != 0)
                CountPcallError(false);
        }
        if (status != 0) {
            const char* message = g_lua.tostring(L, -1);
            g_api->Log(g_mod, SDGO_LOG_ERROR, "%s: %s", name, message ? message : "unknown Lua error");
        }
        g_lua.settop(L, top);
        return status == 0;
    }

    bool ReadWholeFile(const std::wstring& path, std::string& out)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size = {};
        bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 4 * 1024 * 1024;
        if (ok) {
            out.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok = ::ReadFile(file, &out[0], static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size();
        }
        CloseHandle(file);
        return ok;
    }

    void SetGlobalFunction(lua_State* L, const char* name, LuaCFunction fn)
    {
        g_lua.pushstring(L, name);
        g_lua.pushcclosure(L, fn, 0);
        g_lua.settable(L, kLuaGlobals);
    }

    bool BuildConfig();

    bool LoadScripts(lua_State* L)
    {
        std::string script;
        FILETIME scriptTime = WriteTime(g_scriptPathW);
        FILETIME manifestTime = WriteTime(g_manifestPathW);
        if (!ReadWholeFile(g_scriptPathW, script)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "cannot read %s", g_scriptPath.c_str());
            return false;
        }
        if (!BuildConfig())
            return false;
        SetGlobalFunction(L, "SDGO_Log", Lua_Log);
        SetGlobalFunction(L, "SDGO_Now", Lua_Now);
        SetGlobalFunction(L, "SDGO_Wingmen", Lua_Wingmen);
        SetGlobalFunction(L, "SDGO_LocalWingmen", Lua_LocalWingmen);
        SetGlobalFunction(
            L,
            "SDGO_MissionAI_PcallError",
            Lua_PcallError);
        if (!RunChunk(L, g_configChunk, "=mission_ai config"))
            return false;
        std::string chunkName = "@" + g_scriptPath;
        if (!RunChunk(L, script, chunkName.c_str()))
            return false;
        g_loadedScriptTime = scriptTime;
        g_loadedManifestTime = manifestTime;
        g_api->Log(g_mod, SDGO_LOG_INFO, "mission_ai.lua loaded into Lua state %p", L);
        return true;
    }

    // Leaves the global on the stack; true when it is a function.
    bool PushGlobalFunction(lua_State* L, const char* name)
    {
        g_lua.pushstring(L, name);
        g_lua.gettable(L, kLuaGlobals);
        return g_lua.type(L, -1) == kLuaFunction;
    }

    // Edits to mission_ai.lua or mod.ini take effect within two seconds, mid-mission.
    void ReloadIfChanged(lua_State* L, DWORD now)
    {
        if (L != g_loadedState || now - g_lastReloadCheck < 2000)
            return;
        g_lastReloadCheck = now;
        if (SameTime(WriteTime(g_scriptPathW), g_loadedScriptTime) &&
            SameTime(WriteTime(g_manifestPathW), g_loadedManifestTime))
            return;
        g_api->Log(g_mod, SDGO_LOG_INFO, "mission_ai.lua or mod.ini changed; reloading");
        g_loadFailures = 0;
        g_tickErrors = 0;
        if (!LoadScripts(L)) {
            // Keep the previous definitions running; retry after the next edit.
            g_loadedScriptTime = WriteTime(g_scriptPathW);
            g_loadedManifestTime = WriteTime(g_manifestPathW);
        }
    }

    // ---- per-frame tick ----------------------------------------------------

    void Tick(void* mode)
    {
        lua_State* L = g_lua.state(nullptr);
        if (!L)
            return;

        // A new CMissionMode object, or a gap between updates, is a new mission.
        DWORD now = GetTickCount();
        if (mode != g_lastMode || now - g_lastTick > 3000)
            g_session += 1;
        g_lastMode = mode;
        g_lastTick = now;

        ReloadIfChanged(L, now);

        int top = g_lua.gettop(L);
        if (!PushGlobalFunction(L, "SDGO_MissionAI_Tick")) {
            g_lua.settop(L, top);
            if (L == g_loadedState && now - g_lastLoadAttempt < 5000)
                return;
            if (g_loadFailures >= 3)
                return;
            g_lastLoadAttempt = now;
            g_loadedState = L;
            if (!LoadScripts(L)) {
                if (++g_loadFailures >= 3)
                    g_api->Log(g_mod, SDGO_LOG_ERROR, "mission_ai.lua failed to load 3 times; fix it and save to retry");
                return;
            }
            if (!PushGlobalFunction(L, "SDGO_MissionAI_Tick")) {
                g_lua.settop(L, top);
                return;
            }
        }

        g_lua.pushMode(L, mode);
        g_lua.pushnumber(L, static_cast<double>(now));
        g_lua.pushnumber(L, g_session);
        InterlockedIncrement(&g_battleTickCount);
        int status = g_lua.pcall(L, 3, 0, 0);
        if (status != 0) {
            CountPcallError(true);
            const char* message = g_lua.tostring(L, -1);
            g_api->Log(g_mod, SDGO_LOG_ERROR, "SDGO_MissionAI_Tick: %s", message ? message : "unknown Lua error");
            if (++g_tickErrors >= 30) {
                g_disabled = true;
                g_api->Log(g_mod, SDGO_LOG_ERROR, "too many Lua errors; mission AI disabled until restart");
            }
        } else {
            g_tickErrors = 0;
            InterlockedIncrement(
                &g_battleSuccessfulTickCount);
        }
        g_lua.settop(L, top);
    }

    bool GuardedTick(void* mode, DWORD* exceptionCode)
    {
        __try {
            Tick(mode);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *exceptionCode = GetExceptionCode();
            return false;
        }
    }

    void __fastcall MissionUpdate(void* mode, void* /*edx*/)
    {
        g_originalUpdate(mode);
        DWORD code = 0;
        if (!g_disabled) {
            if (!GuardedTick(mode, &code)) {
                g_disabled = true;
                RecordNativeException(code);
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX in mission tick; mission AI disabled until restart", code);
            }
        }
        if (!GuardedRefreshObserver(
                static_cast<uint8_t*>(mode),
                &code)) {
            RecordNativeException(code);
            g_api->Log(g_mod, SDGO_LOG_WARN, "exception %08lX while observing mission update", code);
        }
    }

    // ---- configuration -----------------------------------------------------

    std::string Trim(const std::string& text)
    {
        size_t first = text.find_first_not_of(" \t\r");
        if (first == std::string::npos)
            return "";
        return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
    }

    bool IsInteger(const std::string& text)
    {
        size_t start = (!text.empty() && text[0] == '-') ? 1 : 0;
        return text.size() > start && text.size() - start < 10 &&
               text.find_first_not_of("0123456789", start) == std::string::npos;
    }

    // "1,2,3" -> "{ 1, 2, 3 }"; empty when any item is not an integer.
    std::string IntegerList(const std::string& text)
    {
        std::string out = "{";
        size_t start = 0;
        while (start <= text.size()) {
            size_t comma = text.find(',', start);
            std::string item = Trim(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!IsInteger(item))
                return "";
            out += (start ? ", " : " ") + item;
            if (comma == std::string::npos)
                break;
            start = comma + 1;
        }
        return out + " }";
    }

    // Lua source for a [settings] value: integer, integer list, or quoted string.
    std::string LuaValue(const std::string& value)
    {
        if (IsInteger(value))
            return value;
        if (value.find(',') != std::string::npos) {
            std::string list = IntegerList(value);
            if (!list.empty())
                return list;
        }
        std::string quoted = "\"";
        for (char c : value) {
            if (c == '"' || c == '\\')
                quoted += '\\';
            if (static_cast<unsigned char>(c) >= 0x20)
                quoted += c;
        }
        return quoted + "\"";
    }

    // Every [settings] key becomes a field of SDGO_MissionAI_Config, so new settings need
    // no native change.
    bool BuildConfig()
    {
        std::vector<wchar_t> section(32 * 1024);
        DWORD length = GetPrivateProfileSectionW(L"settings", section.data(), static_cast<DWORD>(section.size()),
                                                 g_manifestPathW.c_str());
        std::string chunk = "SDGO_MissionAI_Config = {\n";
        for (const wchar_t* line = section.data(); line < section.data() + length && *line; line += wcslen(line) + 1) {
            std::string text;
            for (const wchar_t* c = line; *c; ++c)
                text.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
            size_t equals = text.find('=');
            if (text.empty() || text[0] == ';' || equals == std::string::npos)
                continue;
            std::string key = Trim(text.substr(0, equals));
            std::string value = Trim(text.substr(equals + 1));
            if (key.empty() || key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos ||
                (key[0] >= '0' && key[0] <= '9')) {
                g_api->Log(g_mod, SDGO_LOG_WARN, "ignoring setting '%s'", key.c_str());
                continue;
            }
            chunk += "  " + key + " = " + LuaValue(value) + ",\n";
        }
        chunk += "}\n";
        g_configChunk = chunk;
        return true;
    }
}

extern "C" __declspec(dllexport) int __cdecl
SdgoMissionAiClientGetSnapshotV1(
    SdgoMissionAiClientSnapshotV1* snapshot)
{
    __try {
        return CopyObserverSnapshot(snapshot) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;

    bool bound = Bind(g_lua.state, "lua_tinker.state") && Bind(g_lua.pushMode, "lua_tinker.push_mode") &&
                 Bind(g_lua.gettop, "lua.gettop") && Bind(g_lua.settop, "lua.settop") &&
                 Bind(g_lua.type, "lua.type") && Bind(g_lua.tonumber, "lua.tonumber") &&
                 Bind(g_lua.tostring, "lua.tostring") && Bind(g_lua.pushnumber, "lua.pushnumber") &&
                 Bind(g_lua.pushstring, "lua.pushstring") && Bind(g_lua.pushcclosure, "lua.pushcclosure") &&
                 Bind(g_lua.gettable, "lua.gettable") && Bind(g_lua.settable, "lua.settable") &&
                 Bind(g_lua.pcall, "lua.pcall") && Bind(g_lua.load, "lua.load") &&
                 Bind(g_game.missionVtable, "CMissionMode.vtbl") && Bind(g_game.unitManager, "game.unit_manager") &&
                 Bind(g_game.isNetwork, "game.is_network") && Bind(g_game.findStartPoint, "um.find_start_point") &&
                 Bind(g_game.isObserver, "um.is_observer") && Bind(g_game.createNpc, "mode.create_npc") &&
                 Bind(g_game.createRemoteNpc, "mode.create_remote_npc") &&
                 Bind(g_game.setPlayerInfo, "um.set_player_info") && Bind(g_game.playerSlot, "um.player_slot") &&
                 Bind(g_game.assign, "wstring.assign") && Bind(g_game.addAiUnit, "um.add_ai_unit") &&
                 Bind(g_game.calculateTeam, "um.calculate_team");
    if (!bound)
        return 0;
    g_game.findObject = reinterpret_cast<FindObjectFn>(
        api->Symbol(mod, "um.find_object"));
    if (!g_game.findObject)
        api->Log(
            mod,
            SDGO_LOG_WARN,
            "read-only mission GameTest object validation is unavailable");
    PublishObserverSnapshot(
        BuildObserverSnapshot(nullptr));

    std::wstring directory = api->ModDirectory(mod);
    g_scriptPathW = directory + L"\\mission_ai.lua";
    g_manifestPathW = directory + L"\\mod.ini";
    for (wchar_t c : g_scriptPathW)
        g_scriptPath.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    if (GetFileAttributesW(g_scriptPathW.c_str()) == INVALID_FILE_ATTRIBUTES) {
        api->Log(mod, SDGO_LOG_ERROR, "missing %s", g_scriptPath.c_str());
        return 0;
    }
    if (!BuildConfig())
        return 0;

    if (!api->HookCode(mod, "CMissionMode.Init", reinterpret_cast<void*>(&MissionInit),
                       reinterpret_cast<void**>(&g_game.originalInit)))
        return 0;
    if (!api->HookSlot(mod, "CMissionMode.vtbl.Update", reinterpret_cast<void*>(&MissionUpdate),
                       reinterpret_cast<void**>(&g_originalUpdate)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: co-op missions spawn the AI participants the server adds (room title !ai)");
    return 1;
}
