// coop_ai - battle server side of the co-op mission wingmen.
//
// Wingmen join a co-op mission as AI pilots, exactly the way the server's own AI battle
// modes add theirs (FUN_004B79D0 for mode 11, FUN_004B88E0 for mode 12, where slots 4 and
// 5 are AI pilots on the human team). Being real room participants, their hits, kills,
// deaths and explosions all go through the game's own code on server and clients.
//
// The battle-start dispatcher (0x004B9BF0) sets up every human pilot of a co-op mission
// room (modes 5, 8 and 10: the modes it exempts from team checks, and the ones the client
// runs as CMissionMode), then announces the start with every slot that has a pilot id
// (battle.start_notify, 0x004B61E0; for these modes only that path calls it, since
// SDBattlePlus.dll through its own detour stub). This mod runs right before that
// announcement and,
// when the room title asks for wingmen ("!aiN lvN"), adds team-1 AI pilots to free slots
// with the same calls and field writes the AI modes use. The clients learn about them
// from the announcement; the client mod mission_ai spawns their units.
//
// The battle room does not carry the title; the lobby room does (WaitRoom: +0x53 length,
// +0x54 title, written by the create-room and room-settings handlers). It is found the
// way those handlers find it: the lobby proc maps a user id (a human pilot id of the
// battle) to the room index under its lock, then the record is locked by index.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"
#include "sdgo/room_commands.h"

namespace
{
    // ---- battle room layout (WaitRoomEx), as read and written by 0x004B79D0/0x004B88E0 ----
    constexpr size_t kRoomMode = 0x05;          // u8, 9 = co-op mission
    constexpr size_t kRoomTitleMax = 20;
    constexpr int kSlots = 12;
    constexpr size_t kSlotInfo = 0x533;         // slot info records, stride 0x48E
    constexpr size_t kSlotInfoStride = 0x48E;
    constexpr size_t kSlotInfoPilotId = 0x429;  // = room + 0x95C + slot * 0x48E
    constexpr size_t kSlotInfoState = 0x48;     // room + 0x57B + slot * 0x48E (set to 0x1E)
    constexpr size_t kSlotInfoTeamA = 0x42D;
    constexpr size_t kSlotInfoTeamB = 0x434;
    constexpr size_t kPilots = 0x3CE4;          // BattlePilot records, stride 0x1F54
    constexpr size_t kPilotStride = 0x1F54;
    constexpr size_t kPilotForm = 0x33C;        // u8, selects the unit record
    constexpr size_t kPilotUnits = 0x340;       // unit records, stride 0x658
    constexpr size_t kUnitStride = 0x658;
    bool IsMissionMode(uint8_t mode)
    {
        return mode == 5 || mode == 8 || mode == 10;
    }
    constexpr uint8_t kHumanTeam = 1;

    using StartNotifyFn = void(__thiscall*)(void* proc, uint8_t* room, uint32_t key, int index, uint32_t a, uint32_t b);
    using BuildAiPilotFn = int(__thiscall*)(uint8_t* room, uint8_t* pilot, uint8_t* unit, uint32_t npcData);
    using PilotResetFn = void(__thiscall*)(uint8_t* part);
    using PilotStateIdFn = void(__thiscall*)(uint8_t* part, uint32_t id);
    using PilotStateRoomFn = void(__thiscall*)(uint8_t* part, uint8_t* room);
    using PilotStateModeFn = void(__thiscall*)(uint8_t* part, uint8_t a, uint8_t b);
    using SetupAiPilotFn = int(__thiscall*)(void* proc, uint8_t* info, uint8_t* pilot, uint8_t a, uint8_t b);
    using MapFindFn = int(__thiscall*)(void* map, const uint32_t* key, int* value);
    using LobbyRoomFn = uint8_t*(__thiscall*)(void* proc, int index, int tag);
    using LobbyReleaseFn = void(__thiscall*)(void* proc, int index);

    // ---- lobby room layout (WaitRoom), as the create-room handler writes it ----------
    constexpr size_t kLobbyMapLock = 0x38;      // CRITICAL_SECTION in the lobby proc
    constexpr size_t kLobbyMap = 0x14;          // user id -> room index
    constexpr size_t kLobbyTitleLength = 0x53;  // u8
    constexpr size_t kLobbyTitle = 0x54;        // up to 20 bytes, CP949

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    StartNotifyFn g_startNotify = nullptr;
    uint8_t** g_aiNpcList = nullptr;
    void* g_waitRoomProc = nullptr;
    BuildAiPilotFn g_buildAiPilot = nullptr;
    PilotResetFn g_pilotReset = nullptr;
    PilotStateIdFn g_pilotStateId = nullptr;
    PilotStateRoomFn g_pilotStateRoom = nullptr;
    PilotStateModeFn g_pilotStateMode = nullptr;
    SetupAiPilotFn g_setupAiPilot = nullptr;
    uint8_t* g_lobbyProc = nullptr;
    MapFindFn g_lobbyMapFind = nullptr;
    LobbyRoomFn g_lobbyRoom = nullptr;
    LobbyReleaseFn g_lobbyRelease = nullptr;

    struct Settings
    {
        int requireRoomCommand = 1;
        int defaultCount = 0; // 0 = fill the free player slots
        int squadSize = 4;
        int defaultLevel = 2;
        int logRoom = 1;
        std::vector<uint32_t> levels[6]; // [1..5]
    } g_settings;

    unsigned g_random = 0;

    unsigned NextRandom()
    {
        g_random ^= g_random << 13;
        g_random ^= g_random >> 17;
        g_random ^= g_random << 5;
        return g_random;
    }

    std::vector<uint32_t> ParseIds(const char* text)
    {
        std::vector<uint32_t> ids;
        const char* p = text;
        while (*p) {
            while (*p == ' ' || *p == ',')
                ++p;
            if (*p < '0' || *p > '9')
                break;
            uint32_t value = 0;
            bool overflow = false;
            while (*p >= '0' && *p <= '9') {
                uint32_t digit = static_cast<uint32_t>(*p++ - '0');
                if (value > (UINT32_MAX - digit) / 10)
                    overflow = true;
                if (!overflow)
                    value = value * 10 + digit;
            }
            if (!overflow)
                ids.push_back(value);
        }
        return ids;
    }

    void LoadSettings()
    {
        g_settings.requireRoomCommand = g_api->SettingInt(g_mod, "RequireRoomCommand", 1);
        g_settings.defaultCount = g_api->SettingInt(g_mod, "DefaultCount", 0);
        g_settings.squadSize = g_api->SettingInt(g_mod, "SquadSize", 4);
        g_settings.defaultLevel = g_api->SettingInt(g_mod, "DefaultLevel", 2);
        if (g_settings.defaultCount < 0 || g_settings.defaultCount > kSlots)
            g_settings.defaultCount = 0;
        if (g_settings.squadSize < 1 || g_settings.squadSize > kSlots)
            g_settings.squadSize = 4;
        if (g_settings.defaultLevel < 1 || g_settings.defaultLevel > 5)
            g_settings.defaultLevel = 2;
        g_settings.logRoom = g_api->SettingInt(g_mod, "LogRoom", 1);
        for (int level = 1; level <= 5; ++level) {
            char key[16];
            char value[1024];
            sprintf_s(key, "Level%d", level);
            g_api->SettingString(g_mod, key, "", value, sizeof(value));
            g_settings.levels[level] = ParseIds(value);
        }
    }

    // ---- room ------------------------------------------------------------------

    uint32_t& SlotPilotId(uint8_t* room, int slot)
    {
        return *reinterpret_cast<uint32_t*>(room + kSlotInfo + slot * kSlotInfoStride + kSlotInfoPilotId);
    }

    std::string Printable(const char* text)
    {
        std::string out;
        for (const char* c = text; *c; ++c)
            out += (static_cast<unsigned char>(*c) >= 0x20 && static_cast<unsigned char>(*c) < 0x7F) ? *c : '?';
        return out;
    }

    struct Plan
    {
        bool enabled = false;
        int count = 0;
        int level = 2;
    };

    // "!ai", "!ai3", options after it: lvN. Case-insensitive.
    Plan ParseTitle(const char* title)
    {
        Plan plan;
        plan.count = g_settings.defaultCount;
        plan.level = g_settings.defaultLevel;
        namespace commands=sdgo::room_commands;
        SdgoRoomCommand command{};
        if (!commands::Find(g_api,title,static_cast<uint32_t>(kRoomTitleMax),"ai",
                            SDGO_ROOM_COMMAND_ALLOW_COUNT,&command)) {
            plan.enabled = g_settings.requireRoomCommand == 0;
            return plan;
        }
        plan.enabled = true;
        if(command.has_count) plan.count=static_cast<int>((std::min)(command.count,uint32_t{kSlots}));
        uint32_t cursor=command.arguments_offset;
        commands::Token argument;
        while(commands::Next(title,command.arguments_offset+command.arguments_length,cursor,argument)) {
            const char* token=title+argument.offset;
            if(argument.length==3 && commands::Lower(token[0])=='l' && commands::Lower(token[1])=='v'
               && token[2]>='1' && token[2]<='5') plan.level=token[2]-'0';
        }
        return plan;
    }

    // ---- AI NPC list ---------------------------------------------------------------

    // The NPC data ids in the server's AI-NPC list, walked like 0x00428820 does.
    std::vector<uint32_t> AiNpcIds()
    {
        std::vector<uint32_t> ids;
        uint8_t* list = *g_aiNpcList;
        if (!list)
            return ids;
        auto lock = reinterpret_cast<CRITICAL_SECTION*>(list + 0x18);
        EnterCriticalSection(lock);
        uint8_t* end = *reinterpret_cast<uint8_t**>(list + 0x8);
        uint8_t* node = *reinterpret_cast<uint8_t**>(list + 0x4);
        int count = *reinterpret_cast<int*>(list + 0xC);
        for (int i = 0; i < count && node; ++i) {
            node = *reinterpret_cast<uint8_t**>(node + 0x8);
            if (!node || node == end)
                break;
            auto data = *reinterpret_cast<uint32_t**>(node);
            if (data)
                ids.push_back(*data);
        }
        LeaveCriticalSection(lock);
        return ids;
    }

    bool Contains(const std::vector<uint32_t>& ids, uint32_t id)
    {
        for (uint32_t x : ids)
            if (x == id)
                return true;
        return false;
    }

    // ---- adding one AI pilot (same steps as the AI battle modes) ----------------------

    bool AddAiPilot(uint8_t* room, int slot, uint32_t npcData)
    {
        uint8_t* pilot = room + kPilots + slot * kPilotStride;
        uint8_t* info = room + kSlotInfo + slot * kSlotInfoStride;
        memset(pilot, 0, kPilotStride);
        uint8_t* unit = pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
        if (!g_buildAiPilot(room, pilot, unit, npcData)) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "slot %d: building the AI pilot for NPC %lu failed", slot,
                       static_cast<unsigned long>(npcData));
            memset(pilot, 0, kPilotStride);
            return false;
        }
        uint32_t id = *reinterpret_cast<uint32_t*>(pilot);
        SlotPilotId(room, slot) = id;
        info[kSlotInfoState] = 0x1E;

        // per-slot battle setup, AI branch, human team (0x004B95D8..0x004B9861)
        *reinterpret_cast<uint32_t*>(pilot + 0x09) = 1;
        pilot[0x0D] = 0;
        pilot[0x04] = kHumanTeam;
        pilot[0x39] = kHumanTeam;
        *reinterpret_cast<uint32_t*>(pilot + 0x05) = 30000;
        info[kSlotInfoTeamA] = kHumanTeam;
        info[kSlotInfoTeamB] = kHumanTeam;
        *reinterpret_cast<uint16_t*>(pilot + 0x1E) = 1000;
        pilot[0x1CA0] = 0;
        pilot[0x1CA1] = 0;
        pilot[0x1CAA] = 0;
        pilot[0x1CAB] = 0;
        pilot[0x1CA8] = 0;
        pilot[0x1CAC] = 0;
        pilot[0x1CAD] = 0;
        *reinterpret_cast<uint32_t*>(pilot + 0x1CB0) = 0;
        g_pilotReset(pilot + 0x1CE0);
        *reinterpret_cast<uint32_t*>(pilot + 0x1F44) = 1;
        pilot[0x1F48] = 0;
        g_pilotStateId(pilot + 0x1D90, id);
        pilot[0x1D93] = static_cast<uint8_t>(pilot[0x1CAC] + 1);
        g_pilotStateRoom(pilot + 0x1D90, room);
        g_pilotStateMode(pilot + 0x1D90, room[4], room[5]);
        if (!g_setupAiPilot(g_waitRoomProc, info, pilot, room[4], room[5])) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "slot %d: AI pilot setup for NPC %lu failed", slot,
                       static_cast<unsigned long>(npcData));
            SlotPilotId(room, slot) = 0;
            memset(pilot, 0, kPilotStride);
            return false;
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "slot %d: AI pilot %08lX (NPC %lu) joined on team %u", slot,
                   static_cast<unsigned long>(id), static_cast<unsigned long>(npcData), kHumanTeam);
        return true;
    }

    // Title of the lobby room of a user; false when the user is in none.
    bool LobbyTitle(uint32_t userId, char* title, int* index)
    {
        *index = 0;
        auto lock = reinterpret_cast<CRITICAL_SECTION*>(g_lobbyProc + kLobbyMapLock);
        EnterCriticalSection(lock);
        g_lobbyMapFind(g_lobbyProc + kLobbyMap, &userId, index);
        LeaveCriticalSection(lock);
        if (*index == 0)
            return false;
        uint8_t* lobby = g_lobbyRoom(g_lobbyProc, *index, 0);
        if (!lobby)
            return false;
        size_t length = lobby[kLobbyTitleLength];
        if (length > kRoomTitleMax)
            length = kRoomTitleMax;
        memcpy(title, lobby + kLobbyTitle, length);
        title[length] = '\0';
        title[strnlen(title, length)] = '\0';
        g_lobbyRelease(g_lobbyProc, *index);
        return true;
    }

    void AddWingmen(uint8_t* room)
    {
        if (!IsMissionMode(room[kRoomMode]))
            return;
        int humans = 0;
        int ai = 0;
        char title[kRoomTitleMax + 1] = {};
        bool titled = false;
        uint32_t titleOwner = 0;
        int lobbyIndex = 0;
        for (int slot = 0; slot < kSlots; ++slot) {
            uint32_t id = SlotPilotId(room, slot);
            if (id == 0)
                continue;
            if (id & 0x80000000u) {
                ++ai;
                continue;
            }
            ++humans;
            if (!titled && LobbyTitle(id, title, &lobbyIndex)) {
                titled = true;
                titleOwner = id;
            }
        }
        if (!titled) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "mission start: none of the %d human(s) is in a lobby room; no title", humans);
            return;
        }
        Plan plan = ParseTitle(title);
        if (g_settings.logRoom)
            g_api->Log(g_mod, SDGO_LOG_INFO, "mission start: lobby room %d (user %lu) title '%s', %d human(s), %d AI; wingmen %s",
                       lobbyIndex, static_cast<unsigned long>(titleOwner), Printable(title).c_str(), humans, ai,
                       plan.enabled ? "requested" : "not requested");
        if (!plan.enabled || humans == 0)
            return;

        int wanted = g_settings.squadSize - humans - ai;
        if (plan.count > 0 && plan.count < wanted)
            wanted = plan.count;
        if (wanted <= 0)
            return;

        std::vector<uint32_t> available = AiNpcIds();
        std::vector<uint32_t> pool;
        for (uint32_t id : g_settings.levels[plan.level])
            if (Contains(available, id))
                pool.push_back(id);
        if (pool.empty()) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "no NPC of Level%d is in the server's AI-NPC list (%u entries)",
                       plan.level, static_cast<unsigned>(available.size()));
            return;
        }

        int added = 0;
        for (int slot = 0; slot < kSlots && added < wanted; ++slot) {
            if (SlotPilotId(room, slot) != 0)
                continue;
            uint32_t pick = pool[NextRandom() % pool.size()];
            if (pool.size() > static_cast<size_t>(wanted)) {
                // no duplicate units in one squad while the pool allows it
                for (int tries = 0; tries < 20; ++tries) {
                    bool used = false;
                    for (int other = 0; other < kSlots; ++other) {
                        uint32_t id = SlotPilotId(room, other);
                        if (id & 0x80000000u) {
                            uint8_t* p = room + kPilots + other * kPilotStride;
                            used = used || *reinterpret_cast<uint32_t*>(p + kPilotUnits + p[kPilotForm] * kUnitStride) ==
                                               pick;
                        }
                    }
                    if (!used)
                        break;
                    pick = pool[NextRandom() % pool.size()];
                }
            }
            if (AddAiPilot(room, slot, pick))
                ++added;
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "%d wingmen added (level %d)", added, plan.level);
    }

    bool GuardedAddWingmen(uint8_t* room, DWORD* code)
    {
        __try {
            AddWingmen(room);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *code = GetExceptionCode();
            return false;
        }
    }

    void __fastcall StartNotify(void* proc, void* /*edx*/, uint8_t* room, uint32_t key, int index, uint32_t a,
                                uint32_t b)
    {
        if (room && IsMissionMode(room[kRoomMode])) {
            DWORD code = 0;
            if (!GuardedAddWingmen(room, &code))
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while adding wingmen; this battle has none", code);
        }
        g_startNotify(proc, room, key, index, a, b);
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
    bool bound = Bind(g_aiNpcList, "battle.ai_npc_list") && Bind(g_waitRoomProc, "battle.waitroom_proc") &&
                 Bind(g_buildAiPilot, "battle.build_ai_pilot") && Bind(g_pilotReset, "battle.pilot_reset") &&
                 Bind(g_pilotStateId, "battle.pilot_state_id") && Bind(g_pilotStateRoom, "battle.pilot_state_room") &&
                 Bind(g_pilotStateMode, "battle.pilot_state_mode") && Bind(g_setupAiPilot, "battle.setup_ai_pilot") &&
                 Bind(g_lobbyProc, "battle.lobby_proc") &&
                 Bind(g_lobbyMapFind, "battle.lobby_map_find") && Bind(g_lobbyRoom, "battle.lobby_room") &&
                 Bind(g_lobbyRelease, "battle.lobby_release");
    if (!bound)
        return 0;
    LoadSettings();
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    g_random = static_cast<unsigned>(counter.LowPart ^ GetTickCount() ^ GetCurrentProcessId()) | 1u;
    if (!api->HookCode(mod, "battle.start_notify", reinterpret_cast<void*>(&StartNotify),
                       reinterpret_cast<void**>(&g_startNotify)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: co-op missions whose room title has !ai get AI wingmen");
    return 1;
}
