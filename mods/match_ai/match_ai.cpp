// match_ai - client side of the PvP AI players.
//
// The battle server mod pvp_ai adds AI pilots to a PvP room (title "!ai e3 a1 lv4") the way
// the server's AI battle mode adds its own. The client puts them into room+0x3C (records of
// 0x344 bytes, after the humans in room+0x1C), but the PvP game modes only spawn the humans:
// CNormalGameMode, CDeathMatchGameMode, CGridMatchGameMode, CLeaderMatchGameMode and
// CTagMatchGameMode (the CNetworkGameMode family; the native AI modes derive from
// CMissionMode instead). Right after each one's Init (vftable slot 4) this spawns the AI
// records with the steps of CAINormalGameMode::Init's AI loop (0x004049B3..0x00404DA5):
//   start point = UM->FindStartPoint(record team, index)           (0x00499930)
//   unit        = mode->CreateNpc(npc data, x, y, z, team, 0, 0, 1)  vt slot 13; for this
//                 family the CBaseGameMode version (0x00ACA0B0): a CNPC that runs the Lua
//                 AI named by its NPC data (InitNPCAI, 0x0065A3A0), the AI battle bots' VSAI_*
//   UM->SetPlayerInfo(slot, 0, unit+4, record+4, record)            (0x0049DFE0)
//   UM->PlayerSlot(slot)+8 = NPC name (npc data +5)                 (0x004039E0, 0x004026F0)
//   UM->AddAiUnit(unit); unit+0x2B68 = record+0 (server id)         (0x0049DB40)
//   UM->CalculateTeam()                                             (0x0049E0F0)
//
// The CBaseGameMode CreateNpc leaves out what CScriptControlMode::CreateNpc (0x00ADF970)
// and the mode's NPC registration (CMissionMode vt slot 24, 0x00ACED20) do for a network
// game, so right after it this does those steps, minus the mission-only ones (objective
// list, the mode as the unit's owner, whose callbacks are mission vftable slots):
//   unit+0x3238 = npc data id; UM->AddObject(unit, 0, 0) (0x0049BA20: object id, world);
//   on the room master: +0x31ED = 0, +0x2B64 = 1, +0x3074 = 1, +0x2B65 = 0, +0x3070 = 2.
//
// A unit's AI runs only once it is switched on (CNPC::EnableAI, unit+0x326C). The game does
// that for every AI unit at game ready, but only in the script modes; for a PvP battle with
// AI players this mod does it at the same moment (GameReady, 0x004B2390). The AI scripts
// need the Lua bindings the script modes register in their Init (0x0050FAB0), and their
// target searches call script mode vftable slots; both are provided for the PvP modes.
//
// The AI runs on the room master, found the way CMissionMode::Init finds it: the local
// player's record (record+0 == room+0x68) has record+0x1C set. The other clients create the
// same units as remote copies (unit+0x2B65 = 1), as the mission NPC registration does on a
// client that is not the master; creating them in the same order gives them the same object
// ids. The start point index of an AI player follows the humans of its team, as the PvP Init
// counts them per team. As in the AI battle mode, the player count (UM+0x8C) then counts the
// AI players too (the HUD sums team scores over the counted slots), and the unit manager's
// object id -> player slot steps answer for them the way that mode's PlayerSlot remap does.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sdgo/sdgo_api.h"

namespace
{
    constexpr size_t kModeRoom = 0x08;          // room data*
    constexpr size_t kRoomHumansBegin = 0x1C;   // vector of 0x344-byte records
    constexpr size_t kRoomHumansEnd = 0x20;
    constexpr size_t kRoomAiBegin = 0x3C;
    constexpr size_t kRoomAiEnd = 0x40;
    constexpr size_t kRoomLocalPlayer = 0x68;   // u32, the local player's id
    constexpr size_t kRecordSize = 0x344;
    constexpr size_t kRecordServerId = 0x00;
    constexpr size_t kRecordConnection = 0x04;
    constexpr size_t kRecordMaster = 0x1C;      // u8, set on the room master's record
    constexpr size_t kRecordTeam = 0x1A;        // u8, team (1, 2) used for start points
    constexpr size_t kRecordNpcData = 0x44;
    constexpr size_t kStartPointPos = 0x24;     // x, y, z floats
    constexpr size_t kUnitObjectId = 0x04;
    constexpr size_t kUnitServerId = 0x2B68;
    constexpr size_t kUnitNpcData = 0x320C;     // npc data*, name (wchar) at +5
    constexpr size_t kUnitNpcDataId = 0x3238;   // u32
    constexpr size_t kUnitLocalControl = 0x2B64; // u8, 0x00670290
    constexpr size_t kUnitRemote = 0x2B65;      // u8, 0x006702A0 (1: no local AI)
    constexpr size_t kUnitNpcState = 0x3070;    // u32, 0x0065EEF0
    constexpr size_t kUnitMasterNpc = 0x3074;   // u8
    constexpr size_t kUnitNpcFlag = 0x31ED;     // u8
    constexpr size_t kSlotName = 0x08;          // std::wstring in the player slot
    constexpr size_t kModeCreateNpcSlot = 13;

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
    using AddObjectFn = void(__thiscall*)(void* um, uint8_t* unit, uint32_t id, int flag);
    using CalculateTeamFn = void(__thiscall*)(void* um);
    using GameReadyFn = void(__thiscall*)(uint8_t* service);
    using EnableAiFn = void(__thiscall*)(uint8_t* unit);
    using ErrorNameFn = const char*(__cdecl*)(int code);
    using ThinkFn = void(__thiscall*)(uint8_t* unit);
    using UdpSetupFn = void(__thiscall*)(uint8_t* waitRoom);
    struct lua_State;
    using LuaOnErrorFn = int(__cdecl*)(lua_State* L);
    using LuaToStringFn = const char*(__cdecl*)(lua_State* L, int index);
    using PacketDispatchFn = int(__thiscall*)(void* self, int a, int b, int c, int opcode, void* packet);
    using RegisterLuaBindingsFn = void(__cdecl*)();
    using GameLogFn = void(__cdecl*)(void* logger, int level, const wchar_t* format, ...);
    using GameAppFn = uint8_t*(__cdecl*)();
    using ModeIsAFn = bool(__thiscall*)(uint8_t* mode, int kind);
    using LockTargetFn = int(__thiscall*)(void* mode, uint32_t objectId, char p2, char p3);
    using RunLuaFunctionFn = void(__thiscall*)(void* mode, int index);
    using LuaLockFn = int(__cdecl*)(uint32_t objectId);
    using LuaLockSelectFn = int(__cdecl*)(uint32_t objectId, int select);
    using LuaRunLuaFunctionFn = void(__cdecl*)(int index);
    using LuaTeamSearchFn = int(__cdecl*)(uint8_t* npc);
    using NatCheckFn = void(__thiscall*)(uint8_t* gameMain);
    using ObjectIdFromPilotFn = uint32_t(__thiscall*)(void* um, uint32_t pilotId);
    using SetSlotColourFn = void(__thiscall*)(void* um, int index, int colour);
    using FindObjectFn = void*(__thiscall*)(void* um, uint32_t objectId);
    using IsPlayerObjectFn = bool(__thiscall*)(void* um, uint32_t objectId);
    using SetPlayerCountFn = void(__thiscall*)(void* um, int count);

    constexpr size_t kAppGameMain = 0x4A4;      // game-main service* of the game application
    constexpr size_t kServiceMode = 0x0C;       // game mode* of the game-main service
    constexpr size_t kUnitOwner = 0x3268;       // owning script mode*, 0 from the CBaseGameMode CreateNpc
    constexpr int kModeIsASlot = 1;
    constexpr int kModeInitSlot = 4;
    constexpr size_t kWaitRoomSlots = 0xA00;    // CServiceWaitRoomMgr: pilot id per slot
    constexpr size_t kWaitRoomSlotStride = 0x48E;
    constexpr uint32_t kAiPilotBit = 0x80000000u;

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;

    struct GameCalls
    {
        void** unitManager;
        IsNetworkFn isNetwork;
        FindStartPointFn findStartPoint;
        IsObserverFn isObserver;
        CreateNpcFn baseCreateNpc;
        SetPlayerInfoFn setPlayerInfo;
        PlayerSlotFn playerSlot;
        WStringAssignFn assign;
        AddAiUnitFn addAiUnit;
        AddObjectFn addObject;
        CalculateTeamFn calculateTeam;
        EnableAiFn enableAi;
    };
    GameCalls g_game = {};
    GameReadyFn g_originalGameReady = nullptr;
    ErrorNameFn g_originalErrorName = nullptr;
    ThinkFn g_originalThink = nullptr;
    UdpSetupFn g_originalUdpSetup = nullptr;
    LuaOnErrorFn g_originalLuaOnError = nullptr;
    LuaToStringFn g_luaToString = nullptr;
    PacketDispatchFn g_originalPacketDispatch = nullptr;
    RegisterLuaBindingsFn g_registerLuaBindings = nullptr;
    GameLogFn g_originalGameLog = nullptr;
    GameAppFn g_gameApp = nullptr;
    LockTargetFn g_lockTarget = nullptr;
    RunLuaFunctionFn g_runLuaFunction = nullptr;
    LuaLockFn g_originalLockPlayerA = nullptr;
    LuaLockFn g_originalLockPlayer = nullptr;
    LuaLockFn g_originalLockPlayerAibs = nullptr;
    LuaLockSelectFn g_originalLockTargetSelect = nullptr;
    LuaRunLuaFunctionFn g_originalRunLuaFunction = nullptr;
    LuaTeamSearchFn g_originalTeamSearch = nullptr;
    NatCheckFn g_originalNatCheck = nullptr;
    bool g_natCheckSkipped = false;
    ObjectIdFromPilotFn g_originalObjectIdFromPilot = nullptr;
    SetSlotColourFn g_originalSetSlotColour = nullptr;
    FindObjectFn g_findObject = nullptr;
    IsPlayerObjectFn g_originalIsPlayerObject = nullptr;
    PlayerSlotFn g_originalPlayerSlot = nullptr;
    SetPlayerCountFn g_setPlayerCount = nullptr;

    // The AI players spawned for the current battle and its game mode; their AI is switched
    // on at game ready.
    std::vector<uint8_t*> g_bots;
    uint8_t* g_botMode = nullptr;
    // The game mode of a battle with AI players whose only human is this client.
    uint8_t* g_soloAiMode = nullptr;

    // The AI players of the battle in g_botMode: server pilot id, object id, player slot.
    struct Bot
    {
        uint32_t pilotId;
        uint32_t objectId;
        int slot;
    };
    std::vector<Bot> g_botTable;
    bool g_logPackets = false; // diagnostics: server packets from the spawn of AI players on

    struct ModeClass
    {
        const char* name;
        const char* slot; // symbol of its vftable Init slot
        ModeInitFn original;
    };
    ModeClass g_classes[] = {
        {"normal", "CNormalGameMode.vtbl.Init", nullptr},
        {"death match", "CDeathMatchGameMode.vtbl.Init", nullptr},
        {"grid match", "CGridMatchGameMode.vtbl.Init", nullptr},
        {"leader match", "CLeaderMatchGameMode.vtbl.Init", nullptr},
        {"tag match", "CTagMatchGameMode.vtbl.Init", nullptr},
    };
    void** g_vtables[5] = {}; // vftable of each class, from its Init slot

    int ModeClassIndex(uint8_t* mode)
    {
        void** vtable = *reinterpret_cast<void***>(mode);
        for (int i = 0; i < 5; ++i)
            if (g_vtables[i] && vtable == g_vtables[i])
                return i;
        return -1;
    }

    // What each PvP Init sets in a player's slot (+0x48, the score the HUD sums per team)
    // right after SetPlayerInfo: death match (0x00AD98B0) 1000, the player's share of the
    // team power; tag match (0x00ADB390: the death match Init, then record +0x30 * 100, at
    // most 1000); grid match (0x00ADB790) 1; normal and leader match nothing.
    constexpr size_t kSlotScore = 0x48;
    constexpr size_t kRecordTagCost = 0x30;

    void InitAiSlot(int classIndex, uint8_t* playerSlot, uint8_t* record)
    {
        auto score = reinterpret_cast<int32_t*>(playerSlot + kSlotScore);
        if (classIndex == 1) {
            *score = 1000;
        } else if (classIndex == 2) {
            *score = 1;
        } else if (classIndex == 4) {
            int32_t cost = *reinterpret_cast<int32_t*>(record + kRecordTagCost);
            *score = cost * 100 < 1001 ? cost * 100 : 1000;
            g_api->Log(g_mod, SDGO_LOG_INFO, "tag match: AI player %08lX record +0x30 = %ld",
                       static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(record)), static_cast<long>(cost));
        }
    }

    int RecordCount(uint8_t* room, size_t begin, size_t end)
    {
        auto first = *reinterpret_cast<uint8_t**>(room + begin);
        auto last = *reinterpret_cast<uint8_t**>(room + end);
        if (!first || last <= first)
            return 0;
        return static_cast<int>((last - first) / kRecordSize);
    }

    bool LocalIsRoomMaster(uint8_t* room)
    {
        uint32_t local = *reinterpret_cast<uint32_t*>(room + kRoomLocalPlayer);
        uint8_t* records = *reinterpret_cast<uint8_t**>(room + kRoomHumansBegin);
        int humans = RecordCount(room, kRoomHumansBegin, kRoomHumansEnd);
        for (int k = 0; k < humans; ++k) {
            uint8_t* record = records + k * kRecordSize;
            if (record[kRecordMaster] != 0 && *reinterpret_cast<uint32_t*>(record + kRecordServerId) == local)
                return true;
        }
        return false;
    }

    void SpawnAiPlayers(uint8_t* mode, const char* className)
    {
        g_bots.clear();
        g_botTable.clear();
        g_botMode = mode;
        g_soloAiMode = nullptr;
        g_natCheckSkipped = false;
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        if (!room)
            return;
        int count = RecordCount(room, kRoomAiBegin, kRoomAiEnd);
        if (count == 0)
            return;
        if (RecordCount(room, kRoomHumansBegin, kRoomHumansEnd) == 1)
            g_soloAiMode = mode;
        if (!g_game.isNetwork()) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "%d AI player(s) in the room, but the battle took the offline path", count);
            return;
        }
        void* um = *g_game.unitManager;
        bool master = LocalIsRoomMaster(room) && !g_game.isObserver(um);
        auto vtable = *reinterpret_cast<void* const* const*>(mode);
        if (vtable[kModeCreateNpcSlot] != reinterpret_cast<void*>(g_game.baseCreateNpc)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "%s: CreateNpc is not the CBaseGameMode one; AI players not spawned",
                       className);
            return;
        }

        int humans = RecordCount(room, kRoomHumansBegin, kRoomHumansEnd);
        uint8_t* humanRecords = *reinterpret_cast<uint8_t**>(room + kRoomHumansBegin);
        int nextIndex[3] = {};
        for (int k = 0; k < humans; ++k) {
            uint8_t* human = humanRecords + k * kRecordSize;
            int team = human[kRecordTeam];
            uint8_t* start = g_game.findStartPoint(um, team, nextIndex[team == 1 || team == 2 ? team : 0]);
            auto hp = start ? reinterpret_cast<float*>(start + kStartPointPos) : nullptr;
            g_api->Log(g_mod, SDGO_LOG_INFO, "human %08lX: team +0x19 %u, colour +0x1A %u, start point (%.0f, %.0f, %.0f)",
                       static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(human)), human[0x19], human[0x1A],
                       hp ? hp[0] : 0.0f, hp ? hp[1] : 0.0f, hp ? hp[2] : 0.0f);
            if (team == 1 || team == 2)
                ++nextIndex[team];
        }

        uint8_t* records = *reinterpret_cast<uint8_t**>(room + kRoomAiBegin);
        int spawned = 0;
        for (int k = 0; k < count; ++k) {
            uint8_t* record = records + k * kRecordSize;
            uint32_t serverId = *reinterpret_cast<uint32_t*>(record + kRecordServerId);
            int team = record[kRecordTeam];
            uint32_t npcData = *reinterpret_cast<uint32_t*>(record + kRecordNpcData);
            int index = (team == 1 || team == 2) ? nextIndex[team]++ : k;
            uint8_t* start = g_game.findStartPoint(um, team, index);
            if (!start)
                start = g_game.findStartPoint(um, team, 0);
            if (!start) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "AI player %08lX: the map has no team %d start point",
                           static_cast<unsigned long>(serverId), team);
                continue;
            }
            auto pos = reinterpret_cast<float*>(start + kStartPointPos);
            g_api->Log(g_mod, SDGO_LOG_INFO, "AI %08lX: team +0x19 %u, colour +0x1A %u, start point %d (%.0f, %.0f, %.0f)",
                       static_cast<unsigned long>(serverId), record[0x19], record[0x1A], index, pos[0], pos[1], pos[2]);
            uint8_t* unit = g_game.baseCreateNpc(mode, npcData, pos[0], pos[1], pos[2], team, 0.0f, 0.0f, 1.0f);
            if (!unit) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "AI player %08lX: creating NPC %lu failed",
                           static_cast<unsigned long>(serverId), static_cast<unsigned long>(npcData));
                continue;
            }
            // what CScriptControlMode::CreateNpc and the mission NPC registration (0x00ACED20)
            // add for a network game: the room master runs the AI, the other clients get a
            // remote copy
            *reinterpret_cast<uint32_t*>(unit + kUnitNpcDataId) = npcData;
            g_game.addObject(um, unit, 0, 0);
            unit[kUnitNpcFlag] = 0;
            if (master) {
                unit[kUnitLocalControl] = 1;
                unit[kUnitMasterNpc] = 1;
                unit[kUnitRemote] = 0;
            } else {
                unit[kUnitRemote] = 1;
            }
            *reinterpret_cast<uint32_t*>(unit + kUnitNpcState) = 2;
            int slot = humans + spawned;
            uint32_t objectId = *reinterpret_cast<uint32_t*>(unit + kUnitObjectId);
            g_game.setPlayerInfo(um, slot, 0, objectId, *reinterpret_cast<uint32_t*>(record + kRecordConnection), record);
            const uint8_t* npc = *reinterpret_cast<uint8_t**>(unit + kUnitNpcData);
            if (npc) {
                auto name = reinterpret_cast<const wchar_t*>(npc + 5);
                g_game.assign(g_game.playerSlot(um, slot) + kSlotName, name, wcslen(name));
            }
            InitAiSlot(ModeClassIndex(mode), g_game.playerSlot(um, slot), record);
            g_game.addAiUnit(um, unit);
            *reinterpret_cast<uint32_t*>(unit + kUnitServerId) = serverId;
            g_bots.push_back(unit);
            g_botTable.push_back({serverId, objectId, slot});
            g_logPackets = true;
            ++spawned;
            g_api->Log(g_mod, SDGO_LOG_INFO, "%s: AI player %08lX joined as object %lu (NPC %lu, team %d, slot %d, %s)",
                       className, static_cast<unsigned long>(serverId), static_cast<unsigned long>(objectId),
                       static_cast<unsigned long>(npcData), team, slot,
                       master ? "AI runs here" : "copy of the room master's");
        }
        if (spawned > 0) {
            // the AI battle mode's player count: humans and AI players (0x0040417C)
            g_setPlayerCount(um, humans + spawned);
            g_api->Log(g_mod, SDGO_LOG_INFO, "player count %d (%d humans, %d AI players)", humans + spawned, humans,
                       spawned);
        }
        g_game.calculateTeam(um);
    }

    bool GuardedSpawn(uint8_t* mode, const char* className, DWORD* code)
    {
        __try {
            SpawnAiPlayers(mode, className);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *code = GetExceptionCode();
            return false;
        }
    }

    // The units' AI scripts call methods on their npc argument (CNPC); the game registers
    // those Lua bindings at the start of every script mode's Init (0x0050FAB0), never in a
    // PvP mode. A PvP battle with AI players does the same before its Init.
    bool RoomHasAiPlayers(uint8_t* mode)
    {
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        return room && RecordCount(room, kRoomAiBegin, kRoomAiEnd) > 0;
    }

    bool GuardedHasAi(uint8_t* mode)
    {
        __try {
            return RoomHasAiPlayers(mode);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template <int I>
    void __fastcall ModeInit(uint8_t* mode, void* /*edx*/)
    {
        if (GuardedHasAi(mode)) {
            g_registerLuaBindings();
            g_api->Log(g_mod, SDGO_LOG_INFO, "%s: Lua bindings registered for the AI players", g_classes[I].name);
        }
        g_classes[I].original(mode);
        DWORD code = 0;
        if (!GuardedSpawn(mode, g_classes[I].name, &code))
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while spawning AI players", code);
    }

    using DetourFn = void(__fastcall*)(uint8_t*, void*);
    const DetourFn g_detours[] = {&ModeInit<0>, &ModeInit<1>, &ModeInit<2>, &ModeInit<3>, &ModeInit<4>};

    // ---- game ready: switch the AI on --------------------------------------------------
    //
    // GameReady (0x004B2390) switches on the AI of the unit manager's AI units only for the
    // script modes (game mode kind 9 or 10, 0x004B2716). For a PvP mode with AI players the
    // same step runs here, right after it: CNPC::EnableAI on each of them.

    void EnableBots(uint8_t* service)
    {
        uint8_t* mode = *reinterpret_cast<uint8_t**>(service + kServiceMode);
        if (!mode || mode != g_botMode || g_bots.empty())
            return;
        for (uint8_t* unit : g_bots)
            g_game.enableAi(unit);
        g_api->Log(g_mod, SDGO_LOG_INFO, "game ready: AI switched on for %u AI player(s)",
                   static_cast<unsigned>(g_bots.size()));
        g_bots.clear();
    }

    bool GuardedEnable(uint8_t* service)
    {
        __try {
            EnableBots(service);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void __fastcall GameReady(uint8_t* service, void* /*edx*/)
    {
        g_originalGameReady(service);
        if (service && !GuardedEnable(service))
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception while switching on the AI players");
    }

    // ---- battle start: no UDP link to AI pilots ------------------------------------------
    //
    // On a normal wait room's battle start (0x00A0FE80) the client asks for a UDP link to the
    // pilot of every other slot and counts the requests against the links that come back.
    // An AI pilot has no client to answer, so for that call its slot reads as empty.

    void __fastcall UdpSetup(uint8_t* waitRoom, void* /*edx*/)
    {
        uint32_t hidden[12] = {};
        int count = 0;
        if (waitRoom) {
            for (int slot = 0; slot < 12; ++slot) {
                auto id = reinterpret_cast<uint32_t*>(waitRoom + kWaitRoomSlots + slot * kWaitRoomSlotStride);
                if (*id & kAiPilotBit) {
                    hidden[slot] = *id;
                    *id = 0;
                    ++count;
                }
            }
        }
        g_originalUdpSetup(waitRoom);
        for (int slot = 0; slot < 12 && count; ++slot)
            if (hidden[slot])
                *reinterpret_cast<uint32_t*>(waitRoom + kWaitRoomSlots + slot * kWaitRoomSlotStride) = hidden[slot];
        if (count)
            g_api->Log(g_mod, SDGO_LOG_INFO, "battle start: no UDP link asked for %d AI pilot(s)", count);
    }

    // ---- NAT check: no peer for the only human ------------------------------------------
    //
    // CGameMain's update (vt slot 3, 0x004BC620) runs a NAT check (0x004B6FD0) in a network
    // battle of every mode but the missions and AI modes (5, 8, 10, 11, 12): once the unit
    // manager's counter +0x82CC reaches 1000 and no message from another player has come in
    // (CGameMain +0xF8 and +0xFC, raised by 0x004C2090 and 0x004CBA90), it shows message
    // 120003 (blocked connection, firewall) and takes the mouse away. The only human of a
    // battle with AI players has no other player, so for that battle the check is skipped,
    // as the game skips it for its AI modes.

    void __fastcall NatCheck(uint8_t* gameMain, void* /*edx*/)
    {
        if (gameMain && g_soloAiMode && *reinterpret_cast<uint8_t**>(gameMain + kServiceMode) == g_soloAiMode) {
            if (!g_natCheckSkipped) {
                g_natCheckSkipped = true;
                g_api->Log(g_mod, SDGO_LOG_INFO, "NAT check skipped: no other player in this battle with AI players");
            }
            return;
        }
        g_originalNatCheck(gameMain);
    }

    // ---- the AI's Lua functions that need a script mode ----------------------------------
    //
    // The AI scripts aim through SearchLockPlayer & co. (0x00500D50, 0x00500E00, 0x00501120,
    // 0x00501270), which call vftable slot 18 of the current game mode, and through
    // GameInterfaceRunLuafunction (0x00502550, slot 22). Only the script modes have those
    // slots; on CNormalGameMode slot 18 reads 0 and the call jumps to address 0. In a PvP mode
    // these hooks run what the AI battle mode runs there (CAINormalGameMode slots 18 and 22,
    // 0x00AE35B0 and 0x00ACBC60). Of the mode, those two read only its objective list
    // (+0x108/+0x10C, the object ids 0x00AE0160 collects) and its counter list (+0x60C/+0x610);
    // a PvP battle has neither, so they get a stand-in with both lists empty.
    // OurTeamTargetSearch (0x0050AA90) reads the NPC's owning script mode, which the AI
    // players have not (+0x3268 = 0); it returns 0 there, its result for a mode +0xE4 <= 1
    // (the script mode default, 0x00AE6DF2).

    alignas(16) uint8_t g_scriptModeStandIn[0x620] = {};
    int g_lockFaults = 0;

    bool IsPvpMode(uint8_t* mode)
    {
        if (!mode)
            return false;
        void** vtable = *reinterpret_cast<void***>(mode);
        for (void** known : g_vtables)
            if (known && vtable == known)
                return true;
        return false;
    }

    // The current game mode when it is one of the PvP modes, else NULL (the way the Lua
    // functions find it: GameApp +0x4A4 -> service +0x0C).
    uint8_t* CurrentPvpMode()
    {
        __try {
            uint8_t* app = g_gameApp();
            uint8_t* service = app ? *reinterpret_cast<uint8_t**>(app + kAppGameMain) : nullptr;
            uint8_t* mode = service ? *reinterpret_cast<uint8_t**>(service + kServiceMode) : nullptr;
            return IsPvpMode(mode) ? mode : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    bool ModeIsA(uint8_t* mode, int kind)
    {
        auto isA = reinterpret_cast<ModeIsAFn>((*reinterpret_cast<void***>(mode))[kModeIsASlot]);
        return isA(mode, kind);
    }

    int LockFault(DWORD code, void* address)
    {
        if (g_lockFaults < 10) {
            ++g_lockFaults;
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX at %p in the AI's target search", code, address);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    int LockTarget(uint32_t objectId, char p2, char p3)
    {
        __try {
            return g_lockTarget(g_scriptModeStandIn, objectId, p2, p3);
        } __except (LockFault(GetExceptionCode(), (GetExceptionInformation())->ExceptionRecord->ExceptionAddress)) {
            return 0;
        }
    }

    int __cdecl LockPlayerA(uint32_t objectId)
    {
        if (CurrentPvpMode())
            return LockTarget(objectId, 0, 0);
        return g_originalLockPlayerA(objectId);
    }

    int __cdecl LockPlayer(uint32_t objectId)
    {
        uint8_t* mode = CurrentPvpMode();
        if (mode && !ModeIsA(mode, 0))
            return LockTarget(objectId, 1, 0);
        return g_originalLockPlayer(objectId);
    }

    int __cdecl LockPlayerAibs(uint32_t objectId)
    {
        if (CurrentPvpMode())
            return LockTarget(objectId, 1, 0);
        return g_originalLockPlayerAibs(objectId);
    }

    int __cdecl LockTargetSelect(uint32_t objectId, int select)
    {
        uint8_t* mode = CurrentPvpMode();
        if (mode && !ModeIsA(mode, 0))
            return LockTarget(objectId, 1, static_cast<char>(select));
        return g_originalLockTargetSelect(objectId, select);
    }

    void __cdecl RunLuaFunction(int index)
    {
        if (CurrentPvpMode()) {
            g_runLuaFunction(g_scriptModeStandIn, index);
            return;
        }
        g_originalRunLuaFunction(index);
    }

    int __cdecl TeamSearch(uint8_t* npc)
    {
        if (npc && *reinterpret_cast<void**>(npc + kUnitOwner) == nullptr)
            return 0;
        return g_originalTeamSearch(npc);
    }

    // ---- server pilot ids of the AI players ----------------------------------------------
    //
    // The battle packets name a player by server pilot id; UM::ObjectIdFromPilotId (0x0049BF00)
    // finds the object id among the player slots below the player count (UM +0x8C), then
    // among the live AI units (UM +0x70). The AI battle mode counts its AI players in UM +0x8C
    // (0x0040417C); here that count stays at the humans, so a dead AI player was not found
    // and its respawn (PT_NTF_PLAYBACK_UNIT) was dropped. The lookup now answers for the AI
    // players of this battle the way their slot would.
    // The respawn handler then calls UM::SetSlotColour(object id - 1, colour) (0x004C1DD0),
    // which maps NPC object ids to their slot only in the AI modes; for a PvP mode it gets
    // the AI player's slot.

    const Bot* BotByPilot(uint32_t pilotId)
    {
        if (!(pilotId & kAiPilotBit) || g_botTable.empty() || !g_botMode || CurrentPvpMode() != g_botMode)
            return nullptr;
        for (const Bot& bot : g_botTable)
            if (bot.pilotId == pilotId)
                return &bot;
        return nullptr;
    }

    uint32_t __fastcall ObjectIdFromPilot(void* um, void* /*edx*/, uint32_t pilotId)
    {
        if (const Bot* bot = BotByPilot(pilotId))
            return bot->objectId;
        return g_originalObjectIdFromPilot(um, pilotId);
    }

    // Kills, deaths and score: the PT_NTF_DEAD_UNIT and PT_NTF_MAP_DAMAGE handlers credit them
    // to UM::PlayerSlot(object id - 1) only when UM::IsPlayerObject(object id) (1..12); the AI
    // battle mode credits an NPC killer through PlayerSlot, which there maps object id - 1 >=
    // 1000 to the AI players' slots. For a PvP mode both answer for this battle's AI players
    // the same way; the score sync (0x004C5140) and the other PlayerSlot(object id - 1) users
    // then reach the AI player's slot instead of memory far past the unit manager.

    const Bot* BotByObject(uint32_t objectId)
    {
        if (objectId < 1001 || g_botTable.empty() || !g_botMode || CurrentPvpMode() != g_botMode)
            return nullptr;
        for (const Bot& bot : g_botTable)
            if (bot.objectId == objectId)
                return &bot;
        return nullptr;
    }

    bool __fastcall IsPlayerObject(void* um, void* /*edx*/, uint32_t objectId)
    {
        return g_originalIsPlayerObject(um, objectId) || BotByObject(objectId) != nullptr;
    }

    uint8_t* __fastcall PlayerSlot(void* um, void* /*edx*/, int index)
    {
        if (index >= 1000)
            if (const Bot* bot = BotByObject(static_cast<uint32_t>(index) + 1))
                return g_originalPlayerSlot(um, bot->slot);
        return g_originalPlayerSlot(um, index);
    }

    void __fastcall SetSlotColour(void* um, void* /*edx*/, int index, int colour)
    {
        if (index >= 1000 && CurrentPvpMode()) {
            for (const Bot& bot : g_botTable) {
                if (bot.objectId != static_cast<uint32_t>(index + 1))
                    continue;
                g_api->Log(g_mod, SDGO_LOG_INFO, "AI player %08lX respawns: object %lu %s, slot %d, colour %d",
                           static_cast<unsigned long>(bot.pilotId), static_cast<unsigned long>(bot.objectId),
                           g_findObject(um, bot.objectId) ? "present" : "MISSING", bot.slot, colour);
                g_originalSetSlotColour(um, bot.slot, colour);
                return;
            }
            g_api->Log(g_mod, SDGO_LOG_WARN, "respawn colour for object %d not set: no AI player slot", index + 1);
            return;
        }
        g_originalSetSlotColour(um, index, colour);
    }

    // ---- diagnostics: who runs CNPC::Think ------------------------------------------------

    void* g_thinkCallers[8] = {};
    int g_thinkSamples = 0;

    void __fastcall Think(uint8_t* unit, void* /*edx*/)
    {
        if (g_thinkSamples >= 5000) {
            g_originalThink(unit);
            return;
        }
        ++g_thinkSamples;
        void* frames[4] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
        if (n > 0) {
            for (void*& seen : g_thinkCallers) {
                if (seen == frames[0])
                    break;
                if (!seen) {
                    seen = frames[0];
                    g_api->Log(g_mod, SDGO_LOG_INFO, "CNPC::Think on object %lu, called from %p %p %p %p",
                               static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(unit + kUnitObjectId)),
                               frames[0], n > 1 ? frames[1] : nullptr, n > 2 ? frames[2] : nullptr,
                               n > 3 ? frames[3] : nullptr);
                    break;
                }
            }
        }
        g_originalThink(unit);
    }

    // ---- diagnostics: Lua errors of unit AI and the server's packets ----------------------

    int g_luaErrors = 0;

    int __cdecl LuaOnError(lua_State* L)
    {
        if (g_luaErrors < 40) {
            ++g_luaErrors;
            const char* message = g_luaToString ? g_luaToString(L, -1) : nullptr;
            g_api->Log(g_mod, SDGO_LOG_WARN, "Lua error: %s", message ? message : "(not a string)");
        }
        return g_originalLuaOnError(L);
    }

    int g_lastOpcode = -1;
    int g_opcodeRepeat = 0;
    int g_packetLines = 0;

    // The game's own log (disabled in this client), copied into sdgo.log from the spawn of
    // AI players on: network and battle state messages.
    int g_gameLogLines = 0;

    void __cdecl GameLog(void* logger, int level, const wchar_t* format, ...)
    {
        // the game's logger writes only while enabled (+4) and level <= its level (+8)
        auto fields = reinterpret_cast<const int*>(logger);
        bool enabled = fields && fields[1] != 0 && level <= fields[2];
        bool copy = g_logPackets && g_gameLogLines < 6000;
        if (!format || (!enabled && !copy))
            return;
        wchar_t text[1024];
        va_list args;
        va_start(args, format);
        _vsnwprintf_s(text, _TRUNCATE, format, args);
        va_end(args);
        if (copy) {
            char utf8[2048];
            if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), nullptr, nullptr) > 0) {
                ++g_gameLogLines;
                g_api->Log(g_mod, SDGO_LOG_INFO, "game[%d]: %s", level, utf8);
            }
        }
        if (enabled)
            g_originalGameLog(logger, level, L"%s", text);
    }

    int __fastcall PacketDispatch(void* self, void* /*edx*/, int a, int b, int c, int opcode, void* packet)
    {
        int op = opcode & 0xFFFF;
        if (g_logPackets && g_packetLines < 3000) {
            if (op == g_lastOpcode) {
                ++g_opcodeRepeat;
            } else {
                if (g_opcodeRepeat > 0)
                    g_api->Log(g_mod, SDGO_LOG_INFO, "  (opcode %04X repeated %d more)", g_lastOpcode, g_opcodeRepeat);
                g_api->Log(g_mod, SDGO_LOG_INFO, "packet %04X", op);
                g_lastOpcode = op;
                g_opcodeRepeat = 0;
                ++g_packetLines;
            }
        }
        return g_originalPacketDispatch(self, a, b, c, opcode, packet);
    }

    // ---- diagnostics: every error the client names ------------------------------------
    //
    // The server error popup (0x006FD480) and the client error popups name the error code
    // through ErrorName (0x006DFC60); logging the code with the callers shows where an error
    // comes from.

    const char* __cdecl ErrorName(int code)
    {
        const char* name = g_originalErrorName(code);
        if (code == 0)
            return name; // ERR_NULL: named for every server message, not an error
        void* frames[4] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
        g_api->Log(g_mod, SDGO_LOG_INFO, "error %d (%s), called from %p %p %p %p", code, name ? name : "?", n > 0 ? frames[0] : nullptr,
                   n > 1 ? frames[1] : nullptr, n > 2 ? frames[2] : nullptr, n > 3 ? frames[3] : nullptr);
        return name;
    }

    // ---- diagnostics: access violations ------------------------------------------------
    //
    // The game's crash log (zoa_log\Exp_*.txt) has no faulting address and often no call
    // stack. This logs the first access violations with the address, the registers and the
    // stack words that point into GOnline.exe (return address candidates).

    int g_faults = 0;
    uintptr_t g_imageBegin = 0;
    uintptr_t g_imageEnd = 0;

    LONG CALLBACK FaultLogger(EXCEPTION_POINTERS* info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_faults >= 8)
            return EXCEPTION_CONTINUE_SEARCH;
        ++g_faults;
        const CONTEXT* c = info->ContextRecord;
        const ULONG_PTR* params = info->ExceptionRecord->ExceptionInformation;
        g_api->Log(g_mod, SDGO_LOG_WARN,
                   "access violation at %p (%s %08lX): eax %08lX ebx %08lX ecx %08lX edx %08lX esi %08lX edi %08lX "
                   "ebp %08lX esp %08lX",
                   info->ExceptionRecord->ExceptionAddress, params[0] ? "write" : "read",
                   static_cast<unsigned long>(params[1]), c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
        auto tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
        auto top = reinterpret_cast<uintptr_t>(tib->StackBase);
        char line[512];
        int length = 0;
        int found = 0;
        for (uintptr_t at = c->Esp; at + 4 <= top && at < c->Esp + 0x800 && found < 16; at += 4) {
            uintptr_t value = *reinterpret_cast<const uintptr_t*>(at);
            if (value > g_imageBegin && value < g_imageEnd) {
                length += sprintf_s(line + length, sizeof(line) - length, " %08lX", static_cast<unsigned long>(value));
                ++found;
            }
        }
        g_api->Log(g_mod, SDGO_LOG_WARN, "  stack code addresses:%s", found ? line : " none");
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void InstallFaultLogger()
    {
        auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        g_imageBegin = reinterpret_cast<uintptr_t>(base) + 0x1000;
        g_imageEnd = reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage;
        if (!AddVectoredExceptionHandler(1, &FaultLogger))
            g_api->Log(g_mod, SDGO_LOG_WARN, "access violation logging not available");
    }

    template <typename Fn>
    bool Bind(Fn& out, const char* symbol)
    {
        out = reinterpret_cast<Fn>(g_api->Symbol(g_mod, symbol));
        return out != nullptr;
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    bool bound = Bind(g_game.unitManager, "game.unit_manager") && Bind(g_game.isNetwork, "game.is_network") &&
                 Bind(g_game.findStartPoint, "um.find_start_point") && Bind(g_game.isObserver, "um.is_observer") &&
                 Bind(g_game.baseCreateNpc, "mode.base_create_npc") && Bind(g_game.setPlayerInfo, "um.set_player_info") &&
                 Bind(g_game.playerSlot, "um.player_slot") && Bind(g_game.assign, "wstring.assign") &&
                 Bind(g_game.addAiUnit, "um.add_ai_unit") && Bind(g_game.addObject, "um.add_object") &&
                 Bind(g_game.calculateTeam, "um.calculate_team") && Bind(g_game.enableAi, "npc.enable_ai") &&
                 Bind(g_registerLuaBindings, "lua.register_bindings") && Bind(g_gameApp, "game.app") &&
                 Bind(g_lockTarget, "ainormal.lock_target") && Bind(g_runLuaFunction, "ainormal.run_lua_function") &&
                 Bind(g_findObject, "um.find_object") && Bind(g_setPlayerCount, "um.set_player_count");
    if (!bound)
        return 0;
    // The AI players' scripts call these; without them they would jump to address 0.
    if (!api->HookCode(mod, "lua.search_lock_player_a", reinterpret_cast<void*>(&LockPlayerA),
                       reinterpret_cast<void**>(&g_originalLockPlayerA)) ||
        !api->HookCode(mod, "lua.search_lock_player", reinterpret_cast<void*>(&LockPlayer),
                       reinterpret_cast<void**>(&g_originalLockPlayer)) ||
        !api->HookCode(mod, "lua.search_lock_player_aibs", reinterpret_cast<void*>(&LockPlayerAibs),
                       reinterpret_cast<void**>(&g_originalLockPlayerAibs)) ||
        !api->HookCode(mod, "lua.search_lock_target_select", reinterpret_cast<void*>(&LockTargetSelect),
                       reinterpret_cast<void**>(&g_originalLockTargetSelect)) ||
        !api->HookCode(mod, "lua.game_interface_run_lua_function", reinterpret_cast<void*>(&RunLuaFunction),
                       reinterpret_cast<void**>(&g_originalRunLuaFunction)) ||
        !api->HookCode(mod, "lua.our_team_target_search", reinterpret_cast<void*>(&TeamSearch),
                       reinterpret_cast<void**>(&g_originalTeamSearch)))
        return 0;
    // Without these an AI player that died does not come back, and SetSlotColour would write
    // out of bounds for it.
    if (!api->HookCode(mod, "um.object_id_from_pilot", reinterpret_cast<void*>(&ObjectIdFromPilot),
                       reinterpret_cast<void**>(&g_originalObjectIdFromPilot)) ||
        !api->HookCode(mod, "um.set_slot_colour", reinterpret_cast<void*>(&SetSlotColour),
                       reinterpret_cast<void**>(&g_originalSetSlotColour)) ||
        !api->HookCode(mod, "um.is_player_object", reinterpret_cast<void*>(&IsPlayerObject),
                       reinterpret_cast<void**>(&g_originalIsPlayerObject)) ||
        !api->HookCode(mod, "um.player_slot", reinterpret_cast<void*>(&PlayerSlot),
                       reinterpret_cast<void**>(&g_originalPlayerSlot)))
        return 0;
    if (!api->HookCode(mod, "game.nat_check", reinterpret_cast<void*>(&NatCheck),
                       reinterpret_cast<void**>(&g_originalNatCheck)))
        api->Log(mod, SDGO_LOG_WARN, "NAT check not hooked; a battle with AI players and one human shows a blocked connection");
    if (!api->HookCode(mod, "game.service_game_ready", reinterpret_cast<void*>(&GameReady),
                       reinterpret_cast<void**>(&g_originalGameReady)))
        return 0;
    if (!api->HookCode(mod, "waitroom.battle_udp_setup", reinterpret_cast<void*>(&UdpSetup),
                       reinterpret_cast<void**>(&g_originalUdpSetup)))
        return 0;
    if (api->SettingInt(mod, "LogThinkCallers", 1) != 0 &&
        !api->HookCode(mod, "npc.think", reinterpret_cast<void*>(&Think), reinterpret_cast<void**>(&g_originalThink)))
        api->Log(mod, SDGO_LOG_WARN, "Think logging not available");
    if (api->SettingInt(mod, "LogDiagnostics", 1) != 0) {
        InstallFaultLogger();
        Bind(g_luaToString, "lua.tostring");
        if (!api->HookCode(mod, "game.log", reinterpret_cast<void*>(&GameLog), reinterpret_cast<void**>(&g_originalGameLog)))
            api->Log(mod, SDGO_LOG_WARN, "game log copy not available");
        if (!api->HookCode(mod, "lua_tinker.on_error", reinterpret_cast<void*>(&LuaOnError),
                           reinterpret_cast<void**>(&g_originalLuaOnError)))
            api->Log(mod, SDGO_LOG_WARN, "Lua error logging not available");
        if (!api->HookCode(mod, "game.packet_dispatch", reinterpret_cast<void*>(&PacketDispatch),
                           reinterpret_cast<void**>(&g_originalPacketDispatch)))
            api->Log(mod, SDGO_LOG_WARN, "packet logging not available");
    }
    if (api->SettingInt(mod, "LogErrors", 1) != 0 &&
        !api->HookCode(mod, "game.error_name", reinterpret_cast<void*>(&ErrorName),
                       reinterpret_cast<void**>(&g_originalErrorName)))
        api->Log(mod, SDGO_LOG_WARN, "error logging not available");
    int hooked = 0;
    for (int i = 0; i < 5; ++i) {
        if (auto slot = reinterpret_cast<void**>(api->Symbol(mod, g_classes[i].slot)))
            g_vtables[i] = slot - kModeInitSlot;
        if (api->HookSlot(mod, g_classes[i].slot, reinterpret_cast<void*>(g_detours[i]),
                          reinterpret_cast<void**>(&g_classes[i].original)))
            ++hooked;
        else
            api->Log(mod, SDGO_LOG_WARN, "%s mode not hooked; it gets no AI players", g_classes[i].name);
    }
    if (hooked == 0)
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: PvP battles spawn the AI players the server adds (room title !ai)");
    return 1;
}
