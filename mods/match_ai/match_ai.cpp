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
// AI players this mod does it at the same moment (GameReady, 0x004B2390).
//
// The AI runs on the room master, found the way CMissionMode::Init finds it: the local
// player's record (record+0 == room+0x68) has record+0x1C set. The start point index of an
// AI player follows the humans of its team, as the PvP Init counts them per team. As in
// mission_ai, the player count (UM+0x8C) stays at the humans: the PvP game keeps a UDP
// connection to every counted player.
//
// Version 0.1 spawns the AI players on the room master only; other clients log them.

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

    constexpr size_t kServiceMode = 0x0C;       // game mode* of the game-main service

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

    // The AI players spawned for the current battle and its game mode; their AI is switched
    // on at game ready.
    std::vector<uint8_t*> g_bots;
    uint8_t* g_botMode = nullptr;

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
        g_botMode = mode;
        uint8_t* room = *reinterpret_cast<uint8_t**>(mode + kModeRoom);
        if (!room)
            return;
        int count = RecordCount(room, kRoomAiBegin, kRoomAiEnd);
        if (count == 0)
            return;
        if (!g_game.isNetwork()) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "%d AI player(s) in the room, but the battle took the offline path", count);
            return;
        }
        void* um = *g_game.unitManager;
        if (!LocalIsRoomMaster(room) || g_game.isObserver(um)) {
            g_api->Log(g_mod, SDGO_LOG_INFO, "%s: %d AI player(s) run on the room master; not shown here (version 0.1)",
                       className, count);
            return;
        }
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
            int team = humanRecords[k * kRecordSize + kRecordTeam];
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
            uint8_t* unit = g_game.baseCreateNpc(mode, npcData, pos[0], pos[1], pos[2], team, 0.0f, 0.0f, 1.0f);
            if (!unit) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "AI player %08lX: creating NPC %lu failed",
                           static_cast<unsigned long>(serverId), static_cast<unsigned long>(npcData));
                continue;
            }
            // what CScriptControlMode::CreateNpc and the master branch of the mission NPC
            // registration add for a network game
            *reinterpret_cast<uint32_t*>(unit + kUnitNpcDataId) = npcData;
            g_game.addObject(um, unit, 0, 0);
            unit[kUnitNpcFlag] = 0;
            unit[kUnitLocalControl] = 1;
            unit[kUnitMasterNpc] = 1;
            unit[kUnitRemote] = 0;
            *reinterpret_cast<uint32_t*>(unit + kUnitNpcState) = 2;
            int slot = humans + spawned;
            uint32_t objectId = *reinterpret_cast<uint32_t*>(unit + kUnitObjectId);
            g_game.setPlayerInfo(um, slot, 0, objectId, *reinterpret_cast<uint32_t*>(record + kRecordConnection), record);
            const uint8_t* npc = *reinterpret_cast<uint8_t**>(unit + kUnitNpcData);
            if (npc) {
                auto name = reinterpret_cast<const wchar_t*>(npc + 5);
                g_game.assign(g_game.playerSlot(um, slot) + kSlotName, name, wcslen(name));
            }
            g_game.addAiUnit(um, unit);
            *reinterpret_cast<uint32_t*>(unit + kUnitServerId) = serverId;
            g_bots.push_back(unit);
            ++spawned;
            g_api->Log(g_mod, SDGO_LOG_INFO, "%s: AI player %08lX joined as object %lu (NPC %lu, team %d, slot %d)",
                       className, static_cast<unsigned long>(serverId), static_cast<unsigned long>(objectId),
                       static_cast<unsigned long>(npcData), team, slot);
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

    template <int I>
    void __fastcall ModeInit(uint8_t* mode, void* /*edx*/)
    {
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

    // ---- diagnostics: every error the client names ------------------------------------
    //
    // The server error popup (0x006FD480) and the client error popups name the error code
    // through ErrorName (0x006DFC60); logging the code with the callers shows where an error
    // comes from.

    const char* __cdecl ErrorName(int code)
    {
        const char* name = g_originalErrorName(code);
        void* frames[4] = {};
        USHORT n = RtlCaptureStackBackTrace(1, 4, frames, nullptr);
        g_api->Log(g_mod, SDGO_LOG_INFO, "error %d (%s), called from %p %p %p %p", code, name ? name : "?", n > 0 ? frames[0] : nullptr,
                   n > 1 ? frames[1] : nullptr, n > 2 ? frames[2] : nullptr, n > 3 ? frames[3] : nullptr);
        return name;
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
                 Bind(g_game.calculateTeam, "um.calculate_team") && Bind(g_game.enableAi, "npc.enable_ai");
    if (!bound)
        return 0;
    if (!api->HookCode(mod, "game.service_game_ready", reinterpret_cast<void*>(&GameReady),
                       reinterpret_cast<void**>(&g_originalGameReady)))
        return 0;
    if (api->SettingInt(mod, "LogErrors", 1) != 0 &&
        !api->HookCode(mod, "game.error_name", reinterpret_cast<void*>(&ErrorName),
                       reinterpret_cast<void**>(&g_originalErrorName)))
        api->Log(mod, SDGO_LOG_WARN, "error logging not available");
    int hooked = 0;
    for (int i = 0; i < 5; ++i) {
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
