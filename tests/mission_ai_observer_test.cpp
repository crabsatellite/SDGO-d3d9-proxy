#include "ai_test_support.h"
#include "../mods/mission_ai/mission_ai.cpp"

#include <array>

namespace {
volatile LONG readerStop=0,readerFailures=0,readerCopies=0;
DWORD WINAPI ReadMissionCache(void*) {
    while(!InterlockedCompareExchange(&readerStop,0,0)) {
        SdgoMissionAiClientSnapshotV1 s{};s.size=sizeof(s);s.version=1;
        if(SdgoMissionAiClientGetSnapshotV1(&s)) {
            if(s.human_participant_count!=(s.battle_generation^0x12345678u))InterlockedIncrement(&readerFailures);
            InterlockedIncrement(&readerCopies);
        }
    }
    return 0;
}
}

namespace fixture {
using ai_test::Get;
using ai_test::Put;

std::array<uint8_t, 0x700> mode{};
std::array<uint8_t, 0x100> room{};
std::array<uint8_t, 12 * kRecordSize> humans{};
std::array<uint8_t, 12 * kRecordSize> ais{};
std::array<std::array<uint8_t, 0x3300>, 12> units{};
std::array<std::array<uint8_t, 0x100>, 12> nodes{};
std::array<uint8_t, 0x100> manager{};
std::array<uint8_t, 0x40> start{};
std::array<ModeVirtualFn, 8> vtable{};

void* um = manager.data();
std::array<uint8_t, 1> stateStorage{};
int created = 0;
int initialized = 0;
int updated = 0;
int calculated = 0;
int pcallResult = 0;
bool observer = false;
bool creatingRemote = false;
bool network = true;
bool objectLookup = true;
uint32_t replacementObjectId = 0;
uint8_t* replacementObject = nullptr;
int findCalls = 0;

bool __cdecl Network() { return network; }
bool __fastcall Observer(void*, void*) { return observer; }
int __fastcall Virtual(uint8_t*, void*) { return 1; }
uint8_t* __fastcall Start(void*, void*, int, int) {
    return start.data();
}
uint8_t* __fastcall Create(
    uint8_t*,
    void*,
    uint32_t npc,
    float,
    float,
    float,
    int team,
    float,
    float,
    float)
{
    if (created >= 12)
        return nullptr;
    uint8_t* unit = units[created].data();
    Put(unit, kUnitObjectId, 1001u + created);
    Put(unit, kUnitNpcDataId, npc);
    Put(unit, kUnitNode, nodes[created].data());
    Put(unit, kObjectHp, 1000 - created * 100);
    unit[kUnitTeam] = static_cast<uint8_t>(team);
    unit[kUnitLocalControl] = creatingRemote ? 0 : 1;
    unit[kUnitRemote] = creatingRemote ? 1 : 0;
    unit[kUnitMasterNpc] = creatingRemote ? 0 : 1;
    unit[kUnitAiEnabled] = 1;
    float* position = reinterpret_cast<float*>(
        nodes[created].data() + kNodePosition);
    position[0] = static_cast<float>(created * 10);
    position[1] = 2.0f;
    position[2] = static_cast<float>(created * -5);
    ++created;
    return unit;
}
uint8_t* __fastcall Remote(
    uint8_t* modeObject,
    void*,
    uint32_t npc,
    float x,
    float y,
    float z,
    int team,
    float dx,
    float dy,
    float dz)
{
    creatingRemote = true;
    uint8_t* unit = Create(
        modeObject,
        nullptr,
        npc,
        x,
        y,
        z,
        team,
        dx,
        dy,
        dz);
    creatingRemote = false;
    return unit;
}
void __fastcall Info(
    void*,
    void*,
    int,
    int,
    uint32_t,
    uint32_t,
    uint8_t*)
{
}
void __fastcall Add(void*, void*, uint8_t*) {}
void __fastcall Calculate(void*, void*) { ++calculated; }
void __fastcall Init(uint8_t*, void*) { ++initialized; }
void __fastcall Update(void*, void*) { ++updated; }
void* __fastcall Find(void*, void*, uint32_t objectId)
{
    ++findCalls;
    if (!objectLookup)
        return nullptr;
    if (objectId == replacementObjectId &&
        replacementObject)
        return replacementObject;
    for (auto& unit : units)
        if (Get<uint32_t>(unit.data(), kUnitObjectId) ==
            objectId)
            return unit.data();
    return nullptr;
}

lua_State* State()
{
    return reinterpret_cast<lua_State*>(
        stateStorage.data());
}
lua_State* __cdecl LuaState(lua_State*) { return State(); }
void __cdecl PushMode(lua_State*, void*) {}
int __cdecl GetTop(lua_State*) { return 0; }
void __cdecl SetTop(lua_State*, int) {}
int __cdecl Type(lua_State*, int) { return kLuaFunction; }
double __cdecl ToNumber(lua_State*, int) { return 0.0; }
const char* __cdecl ToString(lua_State*, int) {
    return "fixture pcall error";
}
void __cdecl PushNumber(lua_State*, double) {}
void __cdecl PushString(lua_State*, const char*) {}
void __cdecl PushClosure(lua_State*, LuaCFunction, int) {}
void __cdecl GetTable(lua_State*, int) {}
void __cdecl SetTable(lua_State*, int) {}
int __cdecl Pcall(lua_State*, int, int, int) {
    return pcallResult;
}
int __cdecl Load(
    lua_State*,
    LuaReader,
    void*,
    const char*)
{
    return 0;
}

void Reset(int humanCount, int aiCount, bool master, uint16_t modeId)
{
    mode.fill(0);
    room.fill(0);
    humans.fill(0);
    ais.fill(0);
    units = {};
    nodes = {};
    manager.fill(0);
    start.fill(0);
    vtable.fill(nullptr);
    created = initialized = updated = calculated = 0;
    pcallResult = 0;
    observer = false;
    creatingRemote = false;
    network = true;
    objectLookup = true;
    replacementObjectId = 0;
    replacementObject = nullptr;
    findCalls = 0;

    g_api = &ai_test::api;
    g_game = {};
    g_lua = {};
    g_wingmen.clear();
    g_localWingmen.clear();
    g_observedWingmen.clear();
    g_observedMode = nullptr;
    g_observerSnapshot = {};
    g_observerSnapshotVersion = 0;
    g_battleGeneration = 0;
    ResetBattleCounters();
    g_disabled = false;
    g_tickErrors = 0;
    g_loadFailures = 0;
    g_lastMode = nullptr;
    g_lastTick = 0;
    g_session = 0;

    vtable[kModeIsNetworkSlot] =
        reinterpret_cast<ModeVirtualFn>(&Virtual);
    Put(mode.data(), 0, vtable.data());
    Put(mode.data(), kModeRoom, room.data());
    mode[kModeRoomMaster] = master ? 1 : 0;
    Put(room.data(), kModeRoomMode, modeId);
    Put(
        room.data(),
        kRoomHumansBegin,
        humans.data());
    Put(
        room.data(),
        kRoomHumansEnd,
        humans.data() + humanCount * kRecordSize);
    Put(room.data(), kRoomAiBegin, ais.data());
    Put(
        room.data(),
        kRoomAiEnd,
        ais.data() + aiCount * kRecordSize);
    Put(
        manager.data(),
        kUnitManagerPlayerCount,
        static_cast<uint32_t>(humanCount));
    for (int i = 0; i < aiCount; ++i) {
        uint8_t* record =
            ais.data() + i * kRecordSize;
        Put(
            record,
            kRecordServerId,
            0x80000010u + i);
        Put(
            record,
            kRecordConnection,
            0x100u + i);
        Put(
            record,
            kRecordNpcData,
            70001u + i);
        record[kRecordTeam] = 1;
    }

    g_game.originalInit =
        reinterpret_cast<ModeInitFn>(&Init);
    g_game.missionVtable = vtable.data();
    g_game.unitManager = &um;
    g_game.isNetwork = &Network;
    g_game.findStartPoint =
        reinterpret_cast<FindStartPointFn>(&Start);
    g_game.isObserver =
        reinterpret_cast<IsObserverFn>(&Observer);
    g_game.createNpc =
        reinterpret_cast<CreateNpcFn>(&Create);
    g_game.createRemoteNpc =
        reinterpret_cast<CreateNpcFn>(&Remote);
    g_game.setPlayerInfo =
        reinterpret_cast<SetPlayerInfoFn>(&Info);
    g_game.addAiUnit =
        reinterpret_cast<AddAiUnitFn>(&Add);
    g_game.calculateTeam =
        reinterpret_cast<CalculateTeamFn>(&Calculate);
    g_game.findObject =
        reinterpret_cast<FindObjectFn>(&Find);
    g_originalUpdate =
        reinterpret_cast<MissionUpdateFn>(&Update);

    g_lua.state =
        reinterpret_cast<decltype(g_lua.state)>(&LuaState);
    g_lua.pushMode = &PushMode;
    g_lua.gettop = &GetTop;
    g_lua.settop = &SetTop;
    g_lua.type = &Type;
    g_lua.tonumber = &ToNumber;
    g_lua.tostring = &ToString;
    g_lua.pushnumber = &PushNumber;
    g_lua.pushstring = &PushString;
    g_lua.pushcclosure = &PushClosure;
    g_lua.gettable = &GetTable;
    g_lua.settable = &SetTable;
    g_lua.pcall = &Pcall;
    g_lua.load = &Load;
    g_loadedState =
        State();
    g_lastReloadCheck = GetTickCount();
}

SdgoMissionAiClientSnapshotV1 Snapshot()
{
    SdgoMissionAiClientSnapshotV1 snapshot = {};
    snapshot.size = sizeof(snapshot);
    snapshot.version =
        SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION;
    CHECK(
        SdgoMissionAiClientGetSnapshotV1(&snapshot) ==
        1);
    return snapshot;
}
}

int main()
{
    using ai_test::Put;

    SCENARIO("an unready observer returns an inactive versioned snapshot");
    fixture::Reset(1, 1, true, 5);
    auto snapshot = fixture::Snapshot();
    CHECK(snapshot.size == sizeof(snapshot));
    CHECK(
        snapshot.version ==
        SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION);
    CHECK(snapshot.battle_generation == 0);
    CHECK(snapshot.active == 0);

    SCENARIO("observer ABI rejects incompatible callers");
    fixture::Reset(1, 1, true, 5);
    SdgoMissionAiClientSnapshotV1 invalid = {};
    invalid.size = sizeof(invalid) - 1;
    invalid.version =
        SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION;
    CHECK(
        SdgoMissionAiClientGetSnapshotV1(&invalid) ==
        0);
    invalid.size = sizeof(invalid);
    invalid.version =
        SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION + 1;
    CHECK(
        SdgoMissionAiClientGetSnapshotV1(&invalid) ==
        0);
    CHECK(
        SdgoMissionAiClientGetSnapshotV1(
            reinterpret_cast<
                SdgoMissionAiClientSnapshotV1*>(1)) ==
        0);

    SCENARIO("real mission Init generation maps native wingmen read-only");
    fixture::Reset(1, 3, true, 5);
    MissionInit(fixture::mode.data(), nullptr);
    CHECK(fixture::initialized == 1);
    CHECK(fixture::created == 3);
    CHECK(fixture::calculated == 1);
    const auto modeBefore = fixture::mode;
    const auto roomBefore = fixture::room;
    const auto unitsBefore = fixture::units;
    const auto managerBefore = fixture::manager;
    snapshot = fixture::Snapshot();
    CHECK(fixture::mode == modeBefore);
    CHECK(fixture::room == roomBefore);
    CHECK(fixture::units == unitsBefore);
    CHECK(fixture::manager == managerBefore);
    CHECK(snapshot.size == sizeof(snapshot));
    CHECK(
        snapshot.version ==
        SDGO_MISSION_AI_CLIENT_OBSERVER_VERSION);
    CHECK(snapshot.battle_generation == 1);
    CHECK(snapshot.active == 1);
    CHECK(snapshot.exact_plain_mission == 1);
    CHECK(
        snapshot.mode_class ==
        SDGO_MISSION_AI_MODE_CLASS_PLAIN_MISSION);
    CHECK(snapshot.room_mode == 5);
    CHECK(snapshot.local_is_host == 1);
    CHECK(snapshot.local_is_observer == 0);
    CHECK(snapshot.network_game == 1);
    CHECK(snapshot.find_object_available == 1);
    CHECK(snapshot.room_records_valid == 1);
    CHECK(snapshot.human_participant_count == 1);
    CHECK(snapshot.ai_record_count == 3);
    CHECK(snapshot.spawned_wingman_count == 3);
    CHECK(snapshot.local_wingman_count == 3);
    CHECK(snapshot.unit_manager_player_count == 1);
    for (unsigned i = 0; i < 3; ++i) {
        const auto& participant =
            snapshot.participants[i];
        CHECK(
            participant.pilot_id ==
            0x80000010u + i);
        CHECK(participant.connection_id == 0x100u + i);
        CHECK(participant.record_npc_id == 70001u + i);
        CHECK(participant.object_id == 1001u + i);
        CHECK(participant.unit_server_id == participant.pilot_id);
        CHECK(participant.native_npc_id == participant.record_npc_id);
        CHECK(participant.record_team == 1);
        CHECK(participant.unit_team == 1);
        CHECK(participant.local_control == 1);
        CHECK(participant.remote_copy == 0);
        CHECK(participant.master_npc == 1);
        CHECK(participant.ai_enabled == 1);
        CHECK(participant.valid_registered_object == 1);
        CHECK(participant.position_valid == 1);
        CHECK(participant.mapping_valid == 1);
        CHECK(participant.movement_observed == 0);
        CHECK(participant.spawned == 1);
    }
    const int callsBeforeExport = fixture::findCalls;
    snapshot = fixture::Snapshot();
    CHECK(fixture::findCalls == callsBeforeExport);

    SCENARIO("duplicate pilot ids remain bound to their native record slots");
    fixture::Reset(1, 2, true, 5);
    Put(
        fixture::ais.data() + kRecordSize,
        kRecordServerId,
        0x80000010u);
    MissionInit(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.participants[0].spawned == 1);
    CHECK(snapshot.participants[1].spawned == 1);
    CHECK(snapshot.participants[0].object_id == 1001);
    CHECK(snapshot.participants[1].object_id == 1002);
    CHECK(snapshot.participants[0].mapping_valid == 1);
    CHECK(snapshot.participants[1].mapping_valid == 1);

    SCENARIO("native updates retain movement target HP and cumulative pcall errors");
    float* movedPosition = reinterpret_cast<float*>(
        fixture::nodes[0].data() + kNodePosition);
    movedPosition[0] += 25.0f;
    Put(
        fixture::units[0].data(),
        kUnitTargetObject,
        1002u);
    fixture::pcallResult = 1;
    MissionUpdate(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(fixture::updated == 1);
    CHECK(snapshot.lua_tick_count == 1);
    CHECK(snapshot.lua_successful_tick_count == 0);
    CHECK(snapshot.lua_pcall_error_count == 1);
    CHECK(snapshot.lua_tick_pcall_error_count == 1);
    CHECK(snapshot.lua_load_pcall_error_count == 0);
    CHECK(snapshot.participants[0].movement_observed == 1);
    CHECK(snapshot.participants[0].target_object_id == 1002);
    CHECK(snapshot.participants[0].target_valid == 1);
    CHECK(snapshot.participants[0].target_hp == 900);
    fixture::pcallResult = 0;
    MissionUpdate(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.lua_tick_count == 2);
    CHECK(snapshot.lua_successful_tick_count == 1);
    CHECK(snapshot.lua_pcall_error_count == 1);
    CHECK(g_tickErrors == 0);
    fixture::pcallResult = 1;
    MissionUpdate(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.lua_tick_count == 3);
    CHECK(snapshot.lua_successful_tick_count == 1);
    CHECK(snapshot.lua_pcall_error_count == 2);
    CHECK(snapshot.lua_tick_pcall_error_count == 2);

    SCENARIO("load and nested pcall errors remain cumulative until the next Init");
    fixture::pcallResult = 1;
    CHECK(
        !RunChunk(
            fixture::State(),
            "return true",
            "=observer load fixture"));
    Lua_PcallError(fixture::State());
    fixture::pcallResult = 0;
    MissionUpdate(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.lua_pcall_error_count == 4);
    CHECK(snapshot.lua_tick_pcall_error_count == 2);
    CHECK(snapshot.lua_load_pcall_error_count == 1);
    CHECK(snapshot.lua_nested_pcall_error_count == 1);

    SCENARIO("new native mission Init advances generation and resets battle counters");
    MissionInit(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.battle_generation == 2);
    CHECK(snapshot.lua_tick_count == 0);
    CHECK(snapshot.lua_successful_tick_count == 0);
    CHECK(snapshot.lua_pcall_error_count == 0);
    CHECK(snapshot.lua_tick_pcall_error_count == 0);
    CHECK(snapshot.lua_load_pcall_error_count == 0);
    CHECK(snapshot.lua_nested_pcall_error_count == 0);

    SCENARIO("remote and observer clients never claim local authority");
    fixture::Reset(1, 2, false, 8);
    MissionInit(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.room_mode == 8);
    CHECK(snapshot.active == 1);
    CHECK(snapshot.local_is_host == 0);
    CHECK(snapshot.local_wingman_count == 0);
    CHECK(snapshot.participants[0].local_control == 0);
    CHECK(snapshot.participants[0].remote_copy == 1);
    fixture::Reset(1, 2, true, 10);
    fixture::observer = true;
    MissionInit(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.room_mode == 10);
    CHECK(snapshot.active == 1);
    CHECK(snapshot.local_is_host == 1);
    CHECK(snapshot.local_is_observer == 1);
    CHECK(snapshot.local_wingman_count == 0);
    CHECK(snapshot.participants[0].remote_copy == 1);

    SCENARIO("missing optional object lookup disables proof without changing the mission");
    fixture::Reset(1, 1, true, 5);
    g_game.findObject = nullptr;
    MissionInit(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.find_object_available == 0);
    CHECK(snapshot.active == 0);
    CHECK(snapshot.battle_generation == 1);
    CHECK(fixture::created == 1);

    SCENARIO("missing native registration and identity mismatches remain visible");
    fixture::Reset(1, 1, true, 5);
    MissionInit(fixture::mode.data(), nullptr);
    Put(fixture::units[0].data(), kUnitNpcDataId, 99999u);
    MissionUpdate(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.participants[0].mapping_valid == 0);
    fixture::objectLookup = true;
    fixture::units[11] = fixture::units[0];
    fixture::replacementObjectId = 1001;
    fixture::replacementObject =
        fixture::units[11].data();
    MissionUpdate(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.participants[0].spawned == 1);
    CHECK(snapshot.participants[0].valid_registered_object == 0);
    CHECK(snapshot.participants[0].mapping_valid == 0);
    fixture::objectLookup = false;
    MissionUpdate(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.participants[0].spawned == 1);
    CHECK(snapshot.participants[0].valid_registered_object == 0);
    CHECK(snapshot.participants[0].mapping_valid == 0);

    SCENARIO("derived modes and stale generations cannot appear active");
    fixture::Reset(1, 2, true, 5);
    g_game.missionVtable = nullptr;
    MissionInit(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.battle_generation == 0);
    CHECK(snapshot.active == 0);
    CHECK(snapshot.exact_plain_mission == 0);
    fixture::Reset(1, 1, true, 7);
    MissionInit(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.battle_generation == 1);
    CHECK(snapshot.room_mode == 7);
    CHECK(snapshot.active == 0);
    fixture::Reset(1, 1, true, 5);
    Put(
        fixture::room.data(),
        kRoomAiEnd,
        fixture::ais.data() + 1);
    MissionInit(fixture::mode.data(), nullptr);
    snapshot = fixture::Snapshot();
    CHECK(snapshot.room_records_valid == 0);
    CHECK(snapshot.active == 0);
    fixture::Reset(1, 0, true, 5);
    MissionInit(fixture::mode.data(), nullptr);
    g_observerSnapshot.last_native_update_tick =
        GetTickCount() - 4001;
    snapshot = fixture::Snapshot();
    CHECK(snapshot.battle_generation == 1);
    CHECK(snapshot.active == 0);
    CHECK(snapshot.ai_record_count == 0);
    CHECK(snapshot.room_records_valid == 1);

    SCENARIO("concurrent cache readers never see a torn native-frame snapshot");
    SdgoMissionAiClientSnapshotV1 cached{};cached.size=sizeof(cached);cached.version=1;
    cached.human_participant_count=0x12345678u;PublishObserverSnapshot(cached);
    HANDLE reader=CreateThread(nullptr,0,&ReadMissionCache,nullptr,0,nullptr);
    CHECK(reader!=nullptr);
    if(reader) {
        for(uint32_t i=1;i<10000;++i) {
            cached.battle_generation=i;cached.human_participant_count=i^0x12345678u;
            PublishObserverSnapshot(cached);
        }
        const DWORD deadline=GetTickCount()+1000;
        while(!InterlockedCompareExchange(&readerCopies,0,0) && static_cast<LONG>(deadline-GetTickCount())>0)Sleep(1);
        InterlockedExchange(&readerStop,1);
        CHECK(WaitForSingleObject(reader,2000)==WAIT_OBJECT_0);CloseHandle(reader);
        CHECK(InterlockedCompareExchange(&readerCopies,0,0)>0);
        CHECK(InterlockedCompareExchange(&readerFailures,0,0)==0);
    }
    return ai_test::Finish();
}
