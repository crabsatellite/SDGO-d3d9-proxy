// pvp_ai - battle server side of the PvP AI players.
//
// A PvP room (every battle mode that the battle-start dispatcher 0x004B9BF0 runs through its
// general path: all but the co-op missions 5/8/10 and the native AI modes 11/12) starts with
// AI pilots on either team when its title asks for them ("!ai e3 a1 lv4"). The AI pilots
// are added exactly the way the AI battle mode adds its own (0x004B79D0), so the battle
// treats them as real participants. The client mod match_ai spawns their units.
//
// The dispatcher's general path and the battle tick are involved:
//   1. Two checks count human players only (AI pilots do not exist yet): 2..12 players in
//      the room (battle.start_size_check, 0x004B9ED5; room +0x71 is the current player
//      count) and the team layout after the per-slot checks (battle.start_team_checks,
//      0x004BA146: equal numbers of humans per team, at least three per team in modes
//      3/6/7/9, or an even player count when room +8 is set). For a room that asks for AI
//      players this mod takes the route the co-op mission rooms take at both points (on to
//      battle.start_size_checked 0x004B9F32 and battle.start_checks_passed 0x004BA2CF).
//      The other checks (requester state, room flags, every human ready) still apply.
//   2. SetupBattlePilots (battle.setup_humans, 0x004B6EB0) clears every pilot record and
//      sets up the human pilots. Right after it succeeds, this mod adds the AI pilots:
//      BuildAiPilot, the slot table, the AI branch of the per-slot setup with the pilot's
//      team, SetupAiPilot. The start announcement that follows includes them.
//   3. The per-room battle tick (battle.room_tick, 0x004884D0) respawns dead human pilots
//      when their respawn time has passed, and dead AI pilots only in modes 11 and 12.
//      After it, the dead AI pilots of a PvP room get the same respawn steps a human gets.
//
// The title is read from the room itself (+0x79 length, +0x7A text).

#include <windows.h>

#include <cstdint>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"
#include "../battle_ai/difficulty.h"
#include "battle_items.h"
#include "sdgo/room_titles_lookup.h"
#include "sdgo/ai_scoring.h"
#include "sdgo/pvp_ai_observer.h"

#include "sdgo/ai_catalog_provider.h"
#include "sdgo/room_commands.h"

namespace
{
    // ---- battle room layout (WaitRoomEx), as 0x004B9BF0, 0x004B6EB0 and 0x004B79D0 use it ----
    constexpr size_t kRoomType = 0x04;          // u8, passed to the pilot setup with the mode
    constexpr size_t kRoomMode = 0x05;          // u8
    constexpr size_t kRoomPlayers = 0x71;       // u32, current player count (humans only)
    constexpr size_t kRoomHost = 0x4CE;         // u32, pilot id of the room host
    constexpr size_t kRoomTitleMax = 20;
    constexpr int kSlots = 12;
    constexpr int kSlotsPerTeam = 6;
    constexpr size_t kSlotInfo = 0x533;         // slot info records, stride 0x48E
    constexpr size_t kSlotInfoStride = 0x48E;
    constexpr size_t kSlotInfoPilotId = 0x429;  // = room + 0x95C + slot * 0x48E
    constexpr size_t kSlotInfoState = 0x48;     // room + 0x57B + slot * 0x48E (set to 0x1E)
    constexpr size_t kSlotInfoTeamA = 0x42D;    // the team the team checks count (1 or 2)
    constexpr size_t kSlotInfoColour = 0x434;   // respawn colour (1, 2); start points go by it
    constexpr size_t kPilots = 0x3CE4;          // BattlePilot records, stride 0x1F54
    constexpr size_t kPilotStride = 0x1F54;
    constexpr size_t kPilotForm = 0x33C;        // u8, selects the unit record
    constexpr size_t kPilotTagCost = 0x1CA0;     // u8, sum of the pilot's unit costs
    constexpr size_t kPilotTagUnits = 0x1CA1;    // u8
    constexpr size_t kPilotUnits = 0x340;       // unit records, stride 0x658
    constexpr size_t kUnitStride = 0x658;
    constexpr size_t kPilotEquippedItem = 0x64;
    constexpr size_t kPilotItemCapacity = 0x6C;
    constexpr size_t kPilotItemUsed = 0x70;
    // Death match lives (RespawnPilot 0x00487030, modes 3/14): the pilot's power (pilot +0x1E,
    // u16, 1000 at the start) pays each life the unit record's cost (+0x08, u16, unit data +9);
    // the life being played costs pilot +0x20 (u16), taken from the team power when it dies.
    constexpr size_t kPilotPower = 0x1E;
    constexpr size_t kPilotLifeCost = 0x20;
    constexpr size_t kUnitLifeCost = 0x08;
    constexpr size_t kUnitMaxHp = 0x04;
    constexpr size_t kUnitHp = 0x34;

    constexpr size_t kRoomScoreTeam1 = 0x2EEE4;
    constexpr size_t kRoomScoreTeam2 = 0x2EEE8;
    constexpr uint32_t kAiPilotBit = 0x80000000u;

    // The dispatcher keeps a copy of the room in its frame (ebp - 0x328AC); its team checks
    // read that copy.
    constexpr size_t kDispatcherRoomCopy = 0x328AC;

    bool IsPvpMode(uint8_t mode)
    {
        return mode != 5 && mode != 8 && mode != 10 && mode != 11 && mode != 12;
    }

    using SetupHumansFn = int(__thiscall*)(void* proc, uint8_t* room, uint32_t* a, uint32_t* b, uint8_t c);
    using RoomTickFn = int(__thiscall*)(uint8_t* room, uint32_t now);
    using BattleEndFn = void(__thiscall*)(uint8_t* room, uint16_t a, uint16_t b, int now);
    using RespawnPilotFn = int(__thiscall*)(uint8_t* room, uint32_t now, uint8_t* pilot, uint32_t id, uint32_t extra);
    using RespawnRefreshFn = void(__thiscall*)(uint8_t* room, int a, uint8_t* pilot, int b, int c);
    using PilotStateRespawnFn = int(__thiscall*)(uint8_t* part);
    using BuildAiPilotFn = int(__thiscall*)(uint8_t* room, uint8_t* pilot, uint8_t* unit, uint32_t npcData);
    using PilotResetFn = void(__thiscall*)(uint8_t* part);
    using PilotStateIdFn = void(__thiscall*)(uint8_t* part, uint32_t id);
    using PilotStateRoomFn = void(__thiscall*)(uint8_t* part, uint8_t* room);
    using PilotStateModeFn = void(__thiscall*)(uint8_t* part, uint8_t a, uint8_t b);
    using SetupAiPilotFn = int(__thiscall*)(void* proc, uint8_t* info, uint8_t* pilot, uint8_t a, uint8_t b);
    using WaitRoomTickFn = int(__thiscall*)(void* proc);
    using DamageFn = uint32_t(__thiscall*)(uint8_t* room, uint32_t now, uint32_t attackerSlot,
                                          uint8_t* attackerPilot, uint8_t* weapon, uint32_t targetState,
                                          uint32_t targetPilotId, uint32_t proposedDamage, uint16_t stun,
                                          uint8_t calculationFlag, uint32_t extra, uint8_t part,
                                          uint32_t attackType);
    using MapFindFn = int(__thiscall*)(void* map, const uint32_t* key, uint32_t* value);
    using MapInsertFn = int(__thiscall*)(void* map, const uint32_t* key, const uint32_t* value);
    using MapEraseFn = int(__thiscall*)(void* map, const uint32_t* key);
    using LockRoomFn = uint8_t*(__thiscall*)(void* proc, uint32_t roomIndex, uint16_t tag);
    using UnlockRoomFn = void(__thiscall*)(void* proc, uint32_t roomIndex);
    using ItemLookupFn = uint8_t*(__thiscall*)(void* table, uint32_t itemId);
    using SetItemFn = int(__thiscall*)(void* proc, const pvp_ai::battle_items::SetItemSource* source,
                                      uint8_t* pilot);
    using UseItemFn = int(__thiscall*)(void* proc, int socketIndex,
                                      pvp_ai::battle_items::UseEquipItemPacket* packet);

    // The title of a PvP room is in the room itself (unlike a co-op mission room, whose
    // title is in the lobby room coop_ai reads): written by the room-settings (0x004BEC60)
    // and title-change (0x004DCA40) handlers, sent to the clients by 0x004DD120.
    constexpr size_t kRoomTitleLength = 0x79;   // u8
    constexpr size_t kRoomTitle = 0x7A;         // up to 20 bytes in the native encoding

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    const SdgoRoomTitleApi* g_titleApi = nullptr;
    SetupHumansFn g_setupHumans = nullptr;
    RoomTickFn g_roomTick = nullptr;
    BattleEndFn g_battleEnd = nullptr;
    RespawnPilotFn g_respawnPilot = nullptr;
    RespawnRefreshFn g_respawnRefresh = nullptr;
    PilotStateRespawnFn g_pilotStateRespawn = nullptr;
    uint8_t** g_aiNpcList = nullptr;
    void* g_waitRoomProc = nullptr;
    BuildAiPilotFn g_buildAiPilot = nullptr;
    using ApplyServerLoadoutFn = int(__cdecl*)(uint8_t*, uint8_t*, uint8_t*, uint32_t);
    thread_local HMODULE g_battleAiServerModule = nullptr;
    thread_local ApplyServerLoadoutFn g_applyServerLoadout = nullptr;
    thread_local std::wstring g_battleAiServerPath;
    volatile LONG g_loadoutBridgeFaultLogged = 0;
    PilotResetFn g_pilotReset = nullptr;
    PilotStateIdFn g_pilotStateId = nullptr;
    PilotStateRoomFn g_pilotStateRoom = nullptr;
    PilotStateModeFn g_pilotStateMode = nullptr;
    SetupAiPilotFn g_setupAiPilot = nullptr;
    WaitRoomTickFn g_waitRoomTick = nullptr;
    DamageFn g_damage = nullptr;
    MapFindFn g_mapFind = nullptr;
    MapInsertFn g_mapInsert = nullptr;
    MapEraseFn g_mapErase = nullptr;
    LockRoomFn g_lockRoom = nullptr;
    UnlockRoomFn g_unlockRoom = nullptr;
    ItemLookupFn g_itemLookup = nullptr;
    void* g_itemTable = nullptr;
    SetItemFn g_setItem = nullptr;
    UseItemFn g_useItem = nullptr;

    struct Settings
    {
        int defaultLevel = 2;
        int fillTeamSize = 3;
        int gradeCost[5] = {}; // [1..4]: death match life cost of a player unit of that grade, 0 = keep
        int logRoom = 1;
        int logBattle = 0;
        std::vector<uint32_t> levels[6]; // [1..5]
    } g_settings;

    std::atomic<unsigned> g_random{0};
    PVOID volatile g_observerCallback = nullptr;
    PVOID volatile g_observerUser = nullptr;
    volatile LONG g_observerRegistration = 0;
    volatile LONG g_observerSequence = 0;
    volatile LONG g_observerFaultLogged = 0;
    PVOID volatile g_observerActiveRooms[64] = {};

    unsigned NextRandom()
    {
        unsigned previous = g_random.load(std::memory_order_relaxed);
        for (;;) {
            unsigned next = previous ? previous : 1u;
            next ^= next << 13;
            next ^= next >> 17;
            next ^= next << 5;
            if (g_random.compare_exchange_weak(previous, next, std::memory_order_relaxed))
                return next;
        }
    }

    void ResolveServerLoadoutBridge()
    {
        if (g_battleAiServerPath.empty() && g_api && g_api->ModDirectory) {
            const wchar_t* directory = g_api->ModDirectory(g_mod);
            if (directory && *directory) {
                g_battleAiServerPath = directory;
                const size_t slash = g_battleAiServerPath.find_last_of(L"\\/");
                if (slash == std::wstring::npos) {
                    g_battleAiServerPath.clear();
                } else {
                    g_battleAiServerPath.resize(slash + 1);
                    g_battleAiServerPath += L"battle_ai_server\\battle_ai_server.dll";
                }
            }
        }
        HMODULE module =
            g_battleAiServerPath.empty() ? nullptr : GetModuleHandleW(g_battleAiServerPath.c_str());
        if (module != g_battleAiServerModule) {
            g_battleAiServerModule = module;
            g_applyServerLoadout = nullptr;
        }
        if (module && !g_applyServerLoadout)
            g_applyServerLoadout = reinterpret_cast<ApplyServerLoadoutFn>(
                GetProcAddress(module, "SdgoBattleAiServerApplyLoadoutV1"));
    }

    void ApplyOptionalServerLoadout(uint8_t* room, uint8_t* pilot, uint8_t* unit, uint32_t npcData)
    {
        ResolveServerLoadoutBridge();
        if (!g_applyServerLoadout)
            return;
        __try {
            g_applyServerLoadout(room, pilot, unit, npcData);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_applyServerLoadout = nullptr;
            if (InterlockedCompareExchange(&g_loadoutBridgeFaultLogged, 1, 0) == 0)
                g_api->Log(g_mod, SDGO_LOG_WARN,
                           "battle AI server loadout bridge faulted; native AI unit retained");
        }
    }

    int BuildAiPilotWithLoadout(uint8_t* room, uint8_t* pilot, uint8_t* unit, uint32_t npcData)
    {
        const int result = g_buildAiPilot(room, pilot, unit, npcData);
        if (result)
            ApplyOptionalServerLoadout(room, pilot, unit, npcData);
        return result;
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
        g_settings.defaultLevel = g_api->SettingInt(g_mod, "DefaultLevel", 2);
        if (g_settings.defaultLevel < 1 || g_settings.defaultLevel > 5)
            g_settings.defaultLevel = 2;
        g_settings.fillTeamSize = g_api->SettingInt(g_mod, "FillTeamSize", 3);
        const int playerCost[5] = {0, 250, 334, 455, 556};
        for (int grade = 1; grade <= 4; ++grade) {
            char key[24];
            sprintf_s(key, "LifeCostGrade%d", grade);
            g_settings.gradeCost[grade] = g_api->SettingInt(g_mod, key, playerCost[grade]);
        }
        g_settings.logRoom = g_api->SettingInt(g_mod, "LogRoom", 1);
        g_settings.logBattle = g_api->SettingInt(g_mod, "LogBattle", 0);
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

    uint8_t SlotTeam(uint8_t* room, int slot)
    {
        return room[kSlotInfo + slot * kSlotInfoStride + kSlotInfoTeamA];
    }

    std::string Printable(const char* text)
    {
        std::string out;
        for (const char* c = text; *c; ++c)
            out += (static_cast<unsigned char>(*c) >= 0x20 && static_cast<unsigned char>(*c) < 0x7F) ? *c : '?';
        return out;
    }

    constexpr int kFill = -1;

    struct Plan
    {
        bool enabled = false;
        int enemies = 0;
        int allies = 0;
        int level = 2;
        battle_ai::Difficulty difficulty = battle_ai::Difficulty::Medium;
    };

    // "!ai" fills both teams to FillTeamSize; "!aiN" asks for N enemies; options after it: eN enemies,
    // aN allies, lvN level. Case-insensitive.
    Plan ParseTitle(const char* title)
    {
        Plan plan;
        plan.level = g_settings.defaultLevel;
        namespace commands=sdgo::room_commands;
        SdgoRoomCommand command{};
        if (!commands::Find(g_api,title,SDGO_ROOM_TITLE_MAX_UTF8_BYTES,"ai",
                            SDGO_ROOM_COMMAND_ALLOW_COUNT,&command))
            return plan;
        int difficulty = battle_ai::RoomDifficulty(std::string_view(title));
        if (difficulty >= 0) plan.difficulty = static_cast<battle_ai::Difficulty>(difficulty);
        plan.enabled = true;
        const int count=command.has_count ? static_cast<int>((std::min)(command.count,uint32_t{kSlotsPerTeam})) : -1;
        int enemies = -1;
        int allies = -1;
        uint32_t cursor=command.arguments_offset;
        commands::Token argument;
        while (commands::Next(title,command.arguments_offset+command.arguments_length,cursor,argument)) {
            const char* token=title+argument.offset;
            const uint32_t length=argument.length;
            const char first=commands::Lower(token[0]);
            if (length == 3 && first == 'l' && commands::Lower(token[1]) == 'v' && token[2] >= '1' && token[2] <= '5') {
                plan.level = token[2] - '0';
            } else if (length >= 2 && (first == 'e' || first == 'a')) {
                uint32_t value=0;
                if(commands::Number(token+1,length-1,value))
                    (first == 'e' ? enemies : allies)=static_cast<int>((std::min)(value,uint32_t{kSlotsPerTeam}));
            }
        }
        if (count < 0 && enemies < 0 && allies < 0) {
            plan.enemies = kFill;
            plan.allies = kFill;
        } else {
            plan.enemies = enemies >= 0 ? enemies : (count >= 0 ? count : 0);
            plan.allies = allies >= 0 ? allies : 0;
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

    // Humans and AI pilots per team (index 1, 2) and the room's title.
    struct RoomView
    {
        int humans[3] = {};
        int ai[3] = {};
        char title[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
        bool titled = false;
        uint8_t hostTeam = 0;
    };

    RoomView ViewRoom(uint8_t* room)
    {
        RoomView view;
        size_t length = room[kRoomTitleLength];
        if (length > kRoomTitleMax)
            length = kRoomTitleMax;
        memcpy(view.title, room + kRoomTitle, length);
        view.title[length] = '\0';
        view.title[strnlen(view.title, length)] = '\0';
        uint32_t host = *reinterpret_cast<uint32_t*>(room + kRoomHost);
        char extended[SDGO_ROOM_TITLE_MAX_UTF8_BYTES + 1] = {};
        const uint32_t extendedLength = g_titleApi ? g_titleApi->QueryForPilot(host, extended, sizeof(extended)) : 0;
        if (extendedLength && extendedLength <= SDGO_ROOM_TITLE_MAX_UTF8_BYTES && extended[extendedLength] == '\0')
            memcpy(view.title, extended, extendedLength + 1);
        view.titled = view.title[0] != '\0';
        for (int slot = 0; slot < kSlots; ++slot) {
            uint32_t id = SlotPilotId(room, slot);
            if (id == 0)
                continue;
            int team = SlotTeam(room, slot) == 1 ? 1 : 2;
            if (id & kAiPilotBit) {
                ++view.ai[team];
                continue;
            }
            ++view.humans[team];
            if (id == host)
                view.hostTeam = static_cast<uint8_t>(team);
        }
        if (view.hostTeam == 0)
            view.hostTeam = view.humans[1] >= view.humans[2] ? 1 : 2;
        return view;
    }

    // ---- 1. player count and team checks of the dispatcher ----------------------------

    bool DecideSkipChecks(uint8_t* frame, bool log)
    {
        uint8_t* copy = frame - kDispatcherRoomCopy;
        if (!IsPvpMode(copy[kRoomMode]))
            return false;
        RoomView view = ViewRoom(copy);
        Plan plan = view.titled ? ParseTitle(view.title) : Plan{};
        if (log && g_settings.logRoom)
            g_api->Log(g_mod, SDGO_LOG_INFO,
                       "battle start: mode %u type %u players %lu, humans %d/%d (team 1/2), title '%s'; AI players %s",
                       copy[kRoomMode], copy[kRoomType],
                       static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(copy + kRoomPlayers)), view.humans[1],
                       view.humans[2], Printable(view.title).c_str(), plan.enabled ? "requested" : "not requested");
        return plan.enabled && (plan.enemies != 0 || plan.allies != 0);
    }

    int GuardedDecide(uint8_t* frame, bool log)
    {
        __try {
            return DecideSkipChecks(frame, log) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
    }

    // ---- 2. AI pilots after the human setup -------------------------------------------

    // Death match lives (modes 3, 14): the AI pilot setup (0x004D7A10) copies the unit's life
    // cost from its unit data (+9, the DB's BattleCost) and pays the first life from the
    // pilot's 1000 power. The AI battle units (tbl_UnitNpc) cost more than player units of the
    // same grade (grade 3: 500 against 455, grade 4: 606 against 556). With LifeCostGradeN set,
    // an AI unit of grade N (unit record +0x0B = unit data +6, the DB's UnitGradeNumber) costs
    // what a player unit of that grade does, and the first life is paid again with it.
    constexpr size_t kUnitGrade = 0x0B;
    constexpr int kPilotStartPower = 1000;

    void ApplyPlayerLifeCost(uint8_t* room, uint8_t* pilot, uint8_t* unit)
    {
        uint8_t mode = room[kRoomMode];
        if (mode != 3 && mode != 14)
            return;
        uint8_t grade = unit[kUnitGrade];
        if (grade < 1 || grade > 4 || g_settings.gradeCost[grade] <= 0 || g_settings.gradeCost[grade] > kPilotStartPower)
            return;
        auto cost = static_cast<uint16_t>(g_settings.gradeCost[grade]);
        auto& unitCost = *reinterpret_cast<uint16_t*>(unit + kUnitLifeCost);
        auto& power = *reinterpret_cast<uint16_t*>(pilot + kPilotPower);
        auto& lifeCost = *reinterpret_cast<uint16_t*>(pilot + kPilotLifeCost);
        if (unitCost == cost)
            return;
        unitCost = cost;
        power = static_cast<uint16_t>(kPilotStartPower - cost);
        lifeCost = cost;
    }

    // Grid and tag match (modes 6, 7): the respawn (0x00487030, cases 6 and 7) passes byte +0x24
    // of the record at pilot +0x50 to the unit change notice for the pilot's own client
    // (0x004797B0), without a NULL check. The human setup sets +0x50 from a global table
    // (0x004D7CA0: 0x0043F340 on slot info +0xEA); an AI pilot has none and its respawn read
    // through NULL, so it never got its next unit. An AI pilot of these modes gets a zero
    // record there (it has no client to notify).
    constexpr size_t kPilotUserRecord = 0x50;
    alignas(16) uint8_t g_aiUserRecord[0x40] = {};
    uint8_t* SlotUnit(uint8_t* room, int slot);

    // ---- Hard battle items -----------------------------------------------------------
    //
    // Native player item requests first resolve pilot id -> battle-room index through
    // CWaitRoomProc's map. The native AI modes populate that map before their pilots are
    // created; PvP AI pilots are added later, so this mod registers and erases their exact
    // entries. A caller-owned generation prevents a delayed cleanup from erasing a reused
    // pilot id in a later battle.
    namespace items = pvp_ai::battle_items;
    constexpr size_t kWaitRoomMap = 0x1C;
    constexpr size_t kWaitRoomMapLock = 0x40;
    constexpr size_t kWaitRoomClock = 0x64;
    constexpr size_t kRoomIndex = 0x00;
    constexpr size_t kRoomBattleActive = 0x21;
    constexpr size_t kItemFeature = 0x67;
    constexpr size_t kItemMissionAllowed = 0x6E;
    constexpr size_t kItemNormalAllowed = 0x6F;
    constexpr size_t kItemDeathAllowed = 0x70;
    constexpr size_t kItemLeaderAllowed = 0x73;
    constexpr uint8_t kTeleportFeature = 7;
    constexpr uint8_t kHidingFeature = 21;
    constexpr int kTrackedRooms = 40;

    struct RoomItems
    {
        bool active;
        uint8_t* room;
        uint32_t roomIndex;
        uint32_t generation;
        battle_ai::Difficulty difficulty;
        bool botActive[kSlots];
        items::BotKey bots[kSlots];
    };

    CRITICAL_SECTION g_itemsLock;
    items::Controller<kTrackedRooms * kSlots> g_itemController;
    items::NativeOps g_itemOps;
    RoomItems g_itemRooms[kTrackedRooms] = {};
    uint32_t g_roomGeneration = 0;
    volatile LONG g_damageReceipt = 0;

    void* PilotRoomMap()
    {
        return static_cast<uint8_t*>(g_waitRoomProc) + kWaitRoomMap;
    }

    CRITICAL_SECTION* PilotRoomMapLock()
    {
        return reinterpret_cast<CRITICAL_SECTION*>(
            static_cast<uint8_t*>(g_waitRoomProc) + kWaitRoomMapLock);
    }

    RoomItems* FindRoomItems(uint8_t* room, uint32_t roomIndex)
    {
        for (RoomItems& state : g_itemRooms)
            if (state.active && state.room == room && state.roomIndex == roomIndex)
                return &state;
        return nullptr;
    }

    RoomItems* FindRoomItems(const items::BotKey& key)
    {
        RoomItems* state = FindRoomItems(reinterpret_cast<uint8_t*>(key.room), key.roomIndex);
        return state && state->generation == key.generation ? state : nullptr;
    }

    int FindPilotSlot(uint8_t* room, uint32_t pilotId)
    {
        for (int slot = 0; slot < kSlots; ++slot)
            if (SlotPilotId(room, slot) == pilotId)
                return slot;
        return -1;
    }

    bool NativeMapMatches(void*, const items::BotKey& key)
    {
        if (!FindRoomItems(key))
            return false;
        uint32_t mappedRoom = 0;
        EnterCriticalSection(PilotRoomMapLock());
        int found = g_mapFind(PilotRoomMap(), &key.pilotId, &mappedRoom);
        LeaveCriticalSection(PilotRoomMapLock());
        return found != 0 && mappedRoom == key.roomIndex;
    }

    bool NativeMapInsert(void*, const items::BotKey& key)
    {
        if (!FindRoomItems(key))
            return false;
        uint32_t mappedRoom = 0;
        EnterCriticalSection(PilotRoomMapLock());
        int found = g_mapFind(PilotRoomMap(), &key.pilotId, &mappedRoom);
        int inserted = found ? (mappedRoom == key.roomIndex)
                             : g_mapInsert(PilotRoomMap(), &key.pilotId, &key.roomIndex);
        if (inserted) {
            mappedRoom = 0;
            inserted = g_mapFind(PilotRoomMap(), &key.pilotId, &mappedRoom) &&
                       mappedRoom == key.roomIndex;
        }
        LeaveCriticalSection(PilotRoomMapLock());
        return inserted != 0;
    }

    bool NativeMapEraseIfExact(void*, const items::BotKey& key)
    {
        if (!FindRoomItems(key))
            return false;
        uint32_t mappedRoom = 0;
        EnterCriticalSection(PilotRoomMapLock());
        int found = g_mapFind(PilotRoomMap(), &key.pilotId, &mappedRoom);
        int erased = !found;
        if (found && mappedRoom == key.roomIndex)
            erased = g_mapErase(PilotRoomMap(), &key.pilotId);
        LeaveCriticalSection(PilotRoomMapLock());
        return erased != 0;
    }

    bool ItemAllowedInMode(uint8_t mode, const uint8_t* item)
    {
        if (!item)
            return false;
        if (mode == 5 || mode == 8 || mode == 10)
            return item[kItemMissionAllowed] != 0;
        if (mode == 3 || mode == 14)
            return item[kItemDeathAllowed] != 0;
        if (mode == 1 || mode == 2 || mode == 13)
            return item[kItemNormalAllowed] != 0;
        if (mode == 9)
            return item[kItemLeaderAllowed] != 0;
        return true;
    }

    bool NativeSnapshot(void*, const items::BotKey& key, uint32_t selectedItemId,
                        items::NativeSnapshot* out)
    {
        if (!out)
            return false;
        *out = {};
        RoomItems* roomState = FindRoomItems(key);
        out->generationCurrent = roomState != nullptr;
        if (!roomState)
            return true;
        uint8_t* room = roomState->room;
        int slot = FindPilotSlot(room, key.pilotId);
        if (slot < 0)
            return true;
        uint8_t* pilot = room + kPilots + slot * kPilotStride;
        out->pilotPresent = SlotTeam(room, slot) == key.team &&
                            *reinterpret_cast<uint32_t*>(pilot) == key.pilotId;
        out->mapMatches = NativeMapMatches(nullptr, key);
        out->hardDifficulty = roomState->difficulty == battle_ai::Difficulty::Hard;
        out->alive = *reinterpret_cast<uint32_t*>(SlotUnit(room, slot) + kUnitHp) != 0;
        uint8_t* item = g_itemLookup(g_itemTable, selectedItemId);
        out->modeAllowed = room[kRoomBattleActive] != 0 &&
                           ItemAllowedInMode(room[kRoomMode], item);
        uint8_t expectedFeature = selectedItemId == items::kTeleportItemId ? kTeleportFeature
                                : selectedItemId == items::kHidingItemId ? kHidingFeature
                                                                         : 0;
        out->selectedItemPlayerLegal =
            expectedFeature != 0 && item && item[kItemFeature] == expectedFeature;
        out->equippedItemId =
            *reinterpret_cast<uint32_t*>(pilot + kPilotEquippedItem);
        out->capacity =
            *reinterpret_cast<int32_t*>(pilot + kPilotItemCapacity);
        out->used = *reinterpret_cast<int32_t*>(pilot + kPilotItemUsed);
        return true;
    }

    bool NativeEquip(void*, const items::BotKey& key,
                     const items::SetItemSource& source, int32_t* nativeCapacity)
    {
        if (!nativeCapacity || !FindRoomItems(key))
            return false;
        uint8_t* room = reinterpret_cast<uint8_t*>(key.room);
        int slot = FindPilotSlot(room, key.pilotId);
        if (slot < 0 || SlotTeam(room, slot) != key.team)
            return false;
        uint8_t* pilot = room + kPilots + slot * kPilotStride;
        if (!g_setItem(g_waitRoomProc, &source, pilot))
            return false;
        *nativeCapacity =
            *reinterpret_cast<int32_t*>(pilot + kPilotItemCapacity);
        return *reinterpret_cast<uint32_t*>(pilot + kPilotEquippedItem) ==
               source.itemId;
    }

    int NativeUse(void*, const items::BotKey& key,
                  const items::UseEquipItemPacket& packet)
    {
        if (!FindRoomItems(key))
            return 0x1393;
        items::UseEquipItemPacket request = packet;
        return g_useItem(g_waitRoomProc, 0, &request);
    }

    void ResetRoomItems(RoomItems& state)
    {
        for (int slot = 0; slot < kSlots; ++slot) {
            if (!state.botActive[slot])
                continue;
            if (!g_itemController.EndMatch(state.bots[slot], g_itemOps))
                g_api->Log(g_mod, SDGO_LOG_WARN,
                           "AI pilot %08lX: exact item-map cleanup was not completed",
                           static_cast<unsigned long>(state.bots[slot].pilotId));
        }
        state = {};
    }

    bool PrepareRoomItems(uint8_t* room, battle_ai::Difficulty difficulty)
    {
        uint32_t roomIndex = *reinterpret_cast<uint32_t*>(room + kRoomIndex);
        EnterCriticalSection(&g_itemsLock);
        RoomItems* state = FindRoomItems(room, roomIndex);
        if (state)
            ResetRoomItems(*state);
        if (!state) {
            for (RoomItems& candidate : g_itemRooms)
                if (!candidate.active) {
                    state = &candidate;
                    break;
                }
        }
        if (!state) {
            LeaveCriticalSection(&g_itemsLock);
            return false;
        }
        uint32_t generation = ++g_roomGeneration;
        if (generation == 0)
            generation = ++g_roomGeneration;
        *state = {};
        state->active = true;
        state->room = room;
        state->roomIndex = roomIndex;
        state->generation = generation;
        state->difficulty = difficulty;
        LeaveCriticalSection(&g_itemsLock);
        return true;
    }

    bool RegisterItemPilot(uint8_t* room, int slot, uint32_t pilotId, uint8_t team)
    {
        uint32_t roomIndex = *reinterpret_cast<uint32_t*>(room + kRoomIndex);
        EnterCriticalSection(&g_itemsLock);
        RoomItems* state = FindRoomItems(room, roomIndex);
        bool registered = false;
        if (state && slot >= 0 && slot < kSlots) {
            items::BotKey key{reinterpret_cast<uintptr_t>(room), roomIndex,
                              state->generation, pilotId, team};
            registered = g_itemController.BeginMatch(key, g_itemOps);
            if (registered) {
                state->botActive[slot] = true;
                state->bots[slot] = key;
            }
        }
        LeaveCriticalSection(&g_itemsLock);
        return registered;
    }

    void EndRoomItems(uint8_t* room)
    {
        uint32_t roomIndex = *reinterpret_cast<uint32_t*>(room + kRoomIndex);
        EnterCriticalSection(&g_itemsLock);
        if (RoomItems* state = FindRoomItems(room, roomIndex))
            ResetRoomItems(*state);
        LeaveCriticalSection(&g_itemsLock);
    }

    // Same steps as the AI battle mode (0x004B79D0) for an AI slot, with the given team.
    bool AddAiPilot(uint8_t* room, int slot, uint32_t npcData, uint8_t team, uint8_t colour)
    {
        uint8_t* pilot = room + kPilots + slot * kPilotStride;
        uint8_t* info = room + kSlotInfo + slot * kSlotInfoStride;
        memset(pilot, 0, kPilotStride);
        uint8_t* unit = pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
        if (!BuildAiPilotWithLoadout(room, pilot, unit, npcData)) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "slot %d: building the AI pilot for NPC %lu failed", slot,
                       static_cast<unsigned long>(npcData));
            memset(pilot, 0, kPilotStride);
            return false;
        }
        uint32_t id = *reinterpret_cast<uint32_t*>(pilot);
        SlotPilotId(room, slot) = id;
        info[kSlotInfoState] = 0x1E;

        *reinterpret_cast<uint32_t*>(pilot + 0x09) = 1;
        pilot[0x0D] = 0;
        pilot[0x04] = team;
        pilot[0x39] = colour;
        // Native AI identity normally starts full, while the player setup
        // starts at zero. Medium/Hard earn SP through the same native damage
        // rewards; respawn deliberately does not reset this match resource.
        auto resourceProfile=ParseTitle(ViewRoom(room).title).difficulty;
        *reinterpret_cast<uint32_t*>(pilot + 0x05) = resourceProfile==battle_ai::Difficulty::Easy?30000:0;
        info[kSlotInfoTeamA] = team;
        info[kSlotInfoColour] = colour;
        *reinterpret_cast<uint16_t*>(pilot + kPilotPower) = 1000;
        pilot[kPilotTagCost] = 0;
        pilot[kPilotTagUnits] = 0;
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
        g_pilotStateMode(pilot + 0x1D90, room[kRoomType], room[kRoomMode]);
        if (!g_setupAiPilot(g_waitRoomProc, info, pilot, room[kRoomType], room[kRoomMode])) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "slot %d: AI pilot setup for NPC %lu failed", slot,
                       static_cast<unsigned long>(npcData));
            SlotPilotId(room, slot) = 0;
            memset(pilot, 0, kPilotStride);
            return false;
        }
        ApplyPlayerLifeCost(room, pilot, unit);
        if (room[kRoomMode] == 6 || room[kRoomMode] == 7)
            *reinterpret_cast<uint8_t**>(pilot + kPilotUserRecord) = g_aiUserRecord;
        if (!RegisterItemPilot(room, slot, id, team))
            g_api->Log(g_mod, SDGO_LOG_WARN,
                       "slot %d: AI pilot %08lX was not registered in the native pilot-room map; Hard items are disabled",
                       slot, static_cast<unsigned long>(id));
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "slot %d: AI pilot %08lX (NPC %lu) joined on team %u, colour %u; unit grade %u, life cost %u, max HP %lu, "
                   "power %u, life cost %u",
                   slot, static_cast<unsigned long>(id), static_cast<unsigned long>(npcData), team, colour, unit[kUnitGrade],
                   *reinterpret_cast<uint16_t*>(unit + kUnitLifeCost),
                   static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(unit + kUnitMaxHp)),
                   *reinterpret_cast<uint16_t*>(pilot + kPilotPower), *reinterpret_cast<uint16_t*>(pilot + kPilotLifeCost));
        return true;
    }

    uint32_t PilotUnitId(uint8_t* room, int slot)
    {
        uint8_t* pilot = room + kPilots + slot * kPilotStride;
        return *reinterpret_cast<uint32_t*>(pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride);
    }

    SdgoPvpAiObserverCallbackV1 ObserverCallback()
    {
        if (InterlockedCompareExchange(&g_observerRegistration, 2, 2) != 2)
            return nullptr;
        return reinterpret_cast<SdgoPvpAiObserverCallbackV1>(
            InterlockedCompareExchangePointer(&g_observerCallback, nullptr, nullptr));
    }

    bool ObserverRequested(uint8_t* room)
    {
        if (!room || !IsPvpMode(room[kRoomMode]))
            return false;
        const RoomView view = ViewRoom(room);
        if (!view.titled)
            return false;
        const Plan plan = ParseTitle(view.title);
        return plan.enabled && (plan.enemies != 0 || plan.allies != 0);
    }

    bool ObserverRoomActive(uint8_t* room)
    {
        if (!room || !ObserverCallback())
            return false;
        for (auto& active : g_observerActiveRooms)
            if (InterlockedCompareExchangePointer(&active, nullptr, nullptr) == room)
                return true;
        return false;
    }

    void ObserverEmit(
        uint32_t type, uint8_t* room, uint32_t status = 0, bool requireRequest = true)
    {
        SdgoPvpAiObserverCallbackV1 callback = ObserverCallback();
        if (!callback || !room || (requireRequest && !ObserverRequested(room)))
            return;

        SdgoPvpAiObserverEventV1 event = {};
        event.size = sizeof(event);
        event.version = SDGO_PVP_AI_OBSERVER_VERSION;
        event.type = type;
        event.status = status;
        event.sequence = static_cast<uint32_t>(InterlockedIncrement(&g_observerSequence));
        LARGE_INTEGER counter = {};
        QueryPerformanceCounter(&counter);
        event.qpc = static_cast<uint64_t>(counter.QuadPart);
        event.thread_id = GetCurrentThreadId();
        event.room_cookie = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(room));
        event.host_pilot_id = *reinterpret_cast<uint32_t*>(room + kRoomHost);
        event.score_team_1 = *reinterpret_cast<int32_t*>(room + kRoomScoreTeam1);
        event.score_team_2 = *reinterpret_cast<int32_t*>(room + kRoomScoreTeam2);
        event.room_type = room[kRoomType];
        event.mode = room[kRoomMode];

        const RoomView view = ViewRoom(room);
        event.humans_team_1 = static_cast<uint8_t>((std::min)(view.humans[1], 255));
        event.humans_team_2 = static_cast<uint8_t>((std::min)(view.humans[2], 255));
        event.ai_team_1 = static_cast<uint8_t>((std::min)(view.ai[1], 255));
        event.ai_team_2 = static_cast<uint8_t>((std::min)(view.ai[2], 255));
        memcpy(event.title, view.title, sizeof(event.title));

        for (int slot = 0; slot < kSlots; ++slot) {
            SdgoPvpAiObserverSlotV1& output = event.slots[slot];
            output.slot = static_cast<uint8_t>(slot);
            output.pilot_id = SlotPilotId(room, slot);
            if (!output.pilot_id)
                continue;
            output.unit_or_npc_id = PilotUnitId(room, slot);
            output.team = SlotTeam(room, slot);
            output.colour = room[kSlotInfo + slot * kSlotInfoStride + kSlotInfoColour];
            output.is_ai = (output.pilot_id & kAiPilotBit) ? 1 : 0;
            ++event.active_slots;
        }

        __try {
            callback(
                &event,
                InterlockedCompareExchangePointer(&g_observerUser, nullptr, nullptr));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            InterlockedCompareExchangePointer(
                &g_observerCallback, nullptr, reinterpret_cast<PVOID>(callback));
            if (InterlockedCompareExchange(&g_observerFaultLogged, 1, 0) == 0)
                g_api->Log(g_mod, SDGO_LOG_ERROR, "disabled PvP observer after its callback raised an exception");
        }
    }

    void ObserverForgetRoom(uint8_t* room)
    {
        if (!ObserverCallback())
            return;
        for (auto& active : g_observerActiveRooms)
            InterlockedCompareExchangePointer(&active, nullptr, room);
    }

    void ObserverMarkBattleActive(uint8_t* room)
    {
        if (!ObserverCallback() || !ObserverRequested(room))
            return;
        bool hasAi = false;
        for (int slot = 0; slot < kSlots && !hasAi; ++slot)
            hasAi = (SlotPilotId(room, slot) & kAiPilotBit) != 0;
        if (!hasAi)
            return;
        for (auto& active : g_observerActiveRooms)
            if (InterlockedCompareExchangePointer(&active, nullptr, nullptr) == room)
                return;
        for (auto& active : g_observerActiveRooms)
            if (InterlockedCompareExchangePointer(&active, room, nullptr) == nullptr) {
                ObserverEmit(SDGO_PVP_AI_EVENT_BATTLE_ACTIVE, room);
                return;
            }
    }

    void GuardedObserverEmit(
        uint32_t type, uint8_t* room, uint32_t status = 0, bool requireRequest = true)
    {
        __try {
            ObserverEmit(type, room, status, requireRequest);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (InterlockedCompareExchange(&g_observerFaultLogged, 1, 0) == 0)
                g_api->Log(g_mod, SDGO_LOG_ERROR, "ignored exception while copying a PvP observer snapshot");
        }
    }

    void GuardedObserverForget(uint8_t* room)
    {
        __try {
            ObserverForgetRoom(room);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void GuardedObserverMarkActive(uint8_t* room)
    {
        __try {
            ObserverMarkBattleActive(room);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            if (InterlockedCompareExchange(&g_observerFaultLogged, 1, 0) == 0)
                g_api->Log(g_mod, SDGO_LOG_ERROR, "ignored exception while observing an active PvP battle");
        }
    }

    // The respawn colour of a team (slot info +0x434, copied to pilot +0x39 by the human setup):
    // the colour of a human on that team, else the other colour than the humans of the other
    // team. The clients pick start points by colour, not by team.
    uint8_t TeamColour(uint8_t* room, uint8_t team)
    {
        uint8_t other = 0;
        for (int slot = 0; slot < kSlots; ++slot) {
            uint32_t id = SlotPilotId(room, slot);
            if (id == 0 || (id & kAiPilotBit))
                continue;
            uint8_t colour = room[kSlotInfo + slot * kSlotInfoStride + kSlotInfoColour];
            if (colour != 1 && colour != 2)
                continue;
            if ((SlotTeam(room, slot) == 1 ? 1 : 2) == team)
                return colour;
            other = colour;
        }
        return other ? static_cast<uint8_t>(3 - other) : team;
    }

    // Adds up to `wanted` AI pilots of `team` in the free slots of that team's half
    // (team 1: slots 0-5, team 2: slots 6-11, as the AI battle modes place theirs).
    // ---- modes 6 and 7: a deck of units per AI pilot ------------------------------------
    //
    // In grid match (6) and tag match (7) a pilot plays a deck of up to 4 units: the player
    // setup (0x004D6F40) builds a unit record per deck unit and adds up their points (unit
    // data +0xB, the DB's Star: C 2, B 3, A 4, S 5; a deck holds up to 10) in pilot +0x1CA0 and
    // their number in +0x1CA1. RespawnPilot (0x00487030) refuses a pilot of either mode without
    // units. In tag match each unit is one life: a kill takes the dead unit's points off its
    // team's gauge and one unit off the pilot (0x0047C240) and the respawn (case 7) takes the
    // next unit still alive. In grid match a kill takes none and the respawn (case 6) moves to
    // the next unit of the deck, round and round (+0x33C). The battle start (0x004B61E0) sends every slot's 4 unit records (first 0x32
    // bytes: NPC data id, HP, cost, points, grade for an AI unit) and the deck totals.
    // An AI pilot gets more units of its level pool (all different) the way its first one was
    // built (AI pilot build 0x00485720, then the AI pilot setup 0x004D7A10 on that unit), as
    // many as fit in 10 points, sampling all eligible candidates each battle.
    constexpr size_t kUnitPoints = 0x0A;       // u8, unit record: points (unit data +0xB)
    constexpr int kDeckPoints = 10;

    bool PlaysDeck(uint8_t mode)
    {
        return mode == 6 || mode == 7;
    }
    constexpr int kDeckUnits = 4;

    // Builds the unit of AI NPC `npc` in a scratch pilot and copies it to `unitOut` (when set);
    // returns its points, 0 when the NPC has no unit.
    uint8_t BuildNpcUnit(uint8_t* room, uint32_t npc, uint8_t* unitOut)
    {
        std::vector<uint8_t> scratch(kPilotStride);
        uint8_t* unit = scratch.data() + kPilotUnits;
        if (!BuildAiPilotWithLoadout(room, scratch.data(), unit, npc))
            return 0;
        if (unitOut)
            memcpy(unitOut, unit, kUnitStride);
        return unit[kUnitPoints];
    }

    // The points of each pool NPC's unit (0: none), built once per battle start.
    std::vector<uint8_t> PoolPoints(uint8_t* room, const std::vector<uint32_t>& pool)
    {
        std::vector<uint8_t> points;
        points.reserve(pool.size());
        for (uint32_t npc : pool)
            points.push_back(BuildNpcUnit(room, npc, nullptr));
        return points;
    }

    void BuildDeck(uint8_t* room, int slot, uint32_t firstNpc, const std::vector<uint32_t>& pool,
                   const std::vector<uint8_t>& poolPoints)
    {
        uint8_t* pilot = room + kPilots + slot * kPilotStride;
        uint8_t* info = room + kSlotInfo + slot * kSlotInfoStride;
        uint8_t* first = pilot + kPilotUnits;
        int points = first[kUnitPoints];
        int units = 1;
        std::vector<uint32_t> candidates;
        std::vector<uint8_t> candidatePoints;
        for (size_t i = 0; i < pool.size() && i < poolPoints.size(); ++i) {
            uint32_t npc = pool[i];
            if (npc == firstNpc || poolPoints[i] == 0 || Contains(candidates, npc))
                continue;
            candidates.push_back(npc);
            candidatePoints.push_back(poolPoints[i]);
        }
        char deck[96];
        int length = sprintf_s(deck, "%lu (%d)", static_cast<unsigned long>(firstNpc), points);
        auto savedPower = *reinterpret_cast<uint16_t*>(pilot + kPilotPower);
        auto savedLifeCost = *reinterpret_cast<uint16_t*>(pilot + kPilotLifeCost);
        while (units < kDeckUnits && !candidates.empty()) {
            int left = kDeckPoints - points;
            int best = -1;
            unsigned eligible = 0;
            for (size_t i = 0; i < candidates.size(); ++i) {
                int p = candidatePoints[i];
                if (p > left)
                    continue;
                // Reservoir sampling avoids preferring the same highest-cost
                // units when a level grows to include many grade variants.
                ++eligible;
                if (NextRandom() % eligible == 0)
                    best = static_cast<int>(i);
            }
            if (best < 0)
                break;
            uint32_t npc = candidates[best];
            uint8_t* unit = pilot + kPilotUnits + units * kUnitStride;
            int p = BuildNpcUnit(room, npc, unit);
            pilot[kPilotForm] = static_cast<uint8_t>(units);
            bool ok = p > 0 && p <= left && g_setupAiPilot(g_waitRoomProc, info, pilot, room[kRoomType], room[kRoomMode]);
            pilot[kPilotForm] = 0;
            candidates.erase(candidates.begin() + best);
            candidatePoints.erase(candidatePoints.begin() + best);
            if (!ok) {
                memset(unit, 0, kUnitStride);
                continue;
            }
            points += p;
            ++units;
            length += sprintf_s(deck + length, sizeof(deck) - length, " + %lu (%d)", static_cast<unsigned long>(npc), p);
        }
        *reinterpret_cast<uint16_t*>(pilot + kPilotPower) = savedPower;
        *reinterpret_cast<uint16_t*>(pilot + kPilotLifeCost) = savedLifeCost;
        pilot[kPilotTagCost] = static_cast<uint8_t>(points);
        pilot[kPilotTagUnits] = static_cast<uint8_t>(units);
        g_api->Log(g_mod, SDGO_LOG_INFO, "slot %d: deck %s = %d points, %d units", slot, deck, points, units);
    }

    int AddTeam(uint8_t* room, uint8_t team, int wanted, const std::vector<uint32_t>& pool,
                const std::vector<uint8_t>& poolPoints)
    {
        std::vector<uint32_t> deckCandidates;
        const std::vector<uint32_t>* choices = &pool;
        if (PlaysDeck(room[kRoomMode])) {
            for (size_t i = 0; i < pool.size() && i < poolPoints.size(); ++i)
                if (poolPoints[i] > 0 && poolPoints[i] <= kDeckPoints && !Contains(deckCandidates, pool[i]))
                    deckCandidates.push_back(pool[i]);
            choices = &deckCandidates;
        }
        if (choices->empty())
            return 0;
        int added = 0;
        uint8_t colour = TeamColour(room, team);
        int first = team == 1 ? 0 : kSlotsPerTeam;
        for (int slot = first; slot < first + kSlotsPerTeam && added < wanted; ++slot) {
            if (SlotPilotId(room, slot) != 0)
                continue;
            uint32_t pick = (*choices)[NextRandom() % choices->size()];
            for (int tries = 0; tries < 20; ++tries) {
                bool used = false;
                for (int other = 0; other < kSlots && !used; ++other)
                    used = (SlotPilotId(room, other) & kAiPilotBit) && PilotUnitId(room, other) == pick;
                if (!used)
                    break;
                pick = (*choices)[NextRandom() % choices->size()];
            }
            if (AddAiPilot(room, slot, pick, team, colour)) {
                if (PlaysDeck(room[kRoomMode]))
                    BuildDeck(room, slot, pick, pool, poolPoints);
                ++added;
            }
        }
        return added;
    }

    // Battle values that count the pilots. Before the pilots are set up, the battle start
    // (0x004B7540) derives them from the room's player count (+0x71, humans only): the team
    // sizes (+0x32114, +0x32115: count / 2; leader election 0x004A0FB0 skips a team of size
    // 0) and, in death match (3, 14), each team's power (+0x2EEEC team 1, +0x2EEF0 team 2:
    // count / 2 * 1000). The human setup (0x004B6EB0) sets the grid match (6) gauges
    // (+0x2EEF4 team 1, +0x2EEF8 team 2) from count / 2, and in tag match (7) adds each
    // pilot's unit costs (pilot +0x1CA0, units at +0x1CA1, 0x004D6F40) to its team's gauge
    // and unit count (+0x32116, +0x32117). Once AI pilots have joined these are set again
    // counting them: per team where the teams are 1 and 2 (the AI battle mode sets the team
    // sizes that way, 0x004B79D0), from the pilot count in a free-for-all room (team 0).
    constexpr size_t kRoomTeamSize1 = 0x32114;   // u8
    constexpr size_t kRoomTeamSize2 = 0x32115;   // u8
    constexpr size_t kRoomPower1 = 0x2EEEC;      // death match
    constexpr size_t kRoomPower2 = 0x2EEF0;
    constexpr size_t kRoomGauge1 = 0x2EEF4;      // grid and tag match
    constexpr size_t kRoomGauge2 = 0x2EEF8;
    constexpr size_t kRoomTagUnits1 = 0x32116;   // u8
    constexpr size_t kRoomTagUnits2 = 0x32117;   // u8

    void RecountBattle(uint8_t* room)
    {
        int count[3] = {};
        for (int slot = 0; slot < kSlots; ++slot) {
            if (SlotPilotId(room, slot) == 0)
                continue;
            uint8_t team = SlotTeam(room, slot);
            ++count[team == 1 || team == 2 ? team : 0];
        }
        int total = count[0] + count[1] + count[2];
        bool teams = count[0] == 0;
        int size1 = teams ? count[1] : total / 2;
        int size2 = teams ? count[2] : total / 2;
        room[kRoomTeamSize1] = static_cast<uint8_t>(size1);
        room[kRoomTeamSize2] = static_cast<uint8_t>(size2);
        uint8_t mode = room[kRoomMode];
        auto field = [room](size_t offset) -> int32_t& { return *reinterpret_cast<int32_t*>(room + offset); };
        if (mode == 3 || mode == 14) {
            field(kRoomPower1) = size1 * 1000;
            field(kRoomPower2) = size2 * 1000;
        } else if (mode == 6) {
            // The client and its HUD have one full-bar value. Different server
            // totals (e.g. 80/60 in a 1v3 room) cannot match that display.
            int gauge = sdgo::ai::GridGaugeForTeams(size1, size2);
            if (gauge > 0) {
                field(kRoomGauge1) = gauge;
                field(kRoomGauge2) = gauge;
            }
        } else if (mode == 7) {
            for (int slot = 0; slot < kSlots; ++slot) {
                if (!(SlotPilotId(room, slot) & kAiPilotBit))
                    continue;
                uint8_t* pilot = room + kPilots + slot * kPilotStride;
                bool team1 = SlotTeam(room, slot) == 1;
                field(team1 ? kRoomGauge1 : kRoomGauge2) += pilot[kPilotTagCost];
                room[team1 ? kRoomTagUnits1 : kRoomTagUnits2] += pilot[kPilotTagUnits];
            }
        }
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "battle values with the AI pilots: team sizes %d/%d, power %ld/%ld, gauges %ld/%ld, tag units %u/%u",
                   size1, size2, static_cast<long>(field(kRoomPower1)), static_cast<long>(field(kRoomPower2)),
                   static_cast<long>(field(kRoomGauge1)), static_cast<long>(field(kRoomGauge2)), room[kRoomTagUnits1],
                   room[kRoomTagUnits2]);
    }

    void AddPvpAi(uint8_t* room)
    {
        if (!IsPvpMode(room[kRoomMode]))
            return;
        RoomView view = ViewRoom(room);
        if (!view.titled)
            return;
        Plan plan = ParseTitle(view.title);
        if (!plan.enabled)
            return;

        // "!ai" fills each team to FillTeamSize players; eN/aN are capped by the six slots
        // of a team.
        int fill = g_settings.fillTeamSize;
        if (fill < 1 || fill > kSlotsPerTeam)
            fill = kSlotsPerTeam;
        uint8_t allyTeam = view.hostTeam;
        uint8_t enemyTeam = allyTeam == 1 ? 2 : 1;
        int allyFree = kSlotsPerTeam - view.humans[allyTeam] - view.ai[allyTeam];
        int enemyFree = kSlotsPerTeam - view.humans[enemyTeam] - view.ai[enemyTeam];
        int allies = plan.allies == kFill ? fill - view.humans[allyTeam] - view.ai[allyTeam] : plan.allies;
        int enemies = plan.enemies == kFill ? fill - view.humans[enemyTeam] - view.ai[enemyTeam] : plan.enemies;
        if (allies > allyFree)
            allies = allyFree;
        if (enemies > enemyFree)
            enemies = enemyFree;
        if (allies <= 0 && enemies <= 0) {
            g_api->Log(g_mod, SDGO_LOG_INFO, "no AI players to add (teams full or none asked for)");
            return;
        }

        std::vector<uint32_t> available = AiNpcIds();
        std::vector<uint32_t> pool;
        const bool catalog = sdgo::AiCatalogPool(g_api, g_mod, plan.level, pool);
        if (!catalog)
            for (uint32_t id : g_settings.levels[plan.level])
                if (Contains(available, id))
                    pool.push_back(id);
        if (pool.empty()) {
            if (catalog) {
                g_api->Log(g_mod, SDGO_LOG_WARN, "AI catalog has no usable Level%d entries; check catalog source-data diagnostics", plan.level);
                return;
            }
            g_api->Log(g_mod, SDGO_LOG_WARN, "no NPC of Level%d is in the server's AI-NPC list (%u entries)",
                       plan.level, static_cast<unsigned>(available.size()));
            return;
        }
        if (!PrepareRoomItems(room, plan.difficulty))
            g_api->Log(g_mod, SDGO_LOG_WARN,
                       "room %lu: no item lifecycle slot; AI players will join without Hard battle items",
                       static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(room + kRoomIndex)));

        std::vector<uint8_t> poolPoints;
        if (PlaysDeck(room[kRoomMode]))
            poolPoints = PoolPoints(room, pool);
        int addedAllies = allies > 0 ? AddTeam(room, allyTeam, allies, pool, poolPoints) : 0;
        int addedEnemies = enemies > 0 ? AddTeam(room, enemyTeam, enemies, pool, poolPoints) : 0;
        const char* difficulty =
            plan.difficulty == battle_ai::Difficulty::Hard ? "Hard"
            : plan.difficulty == battle_ai::Difficulty::Easy ? "Easy"
                                                               : "Medium";
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "%d allies (team %u) and %d enemies (team %u) added, level %d, difficulty %s",
                   addedAllies, allyTeam, addedEnemies, enemyTeam, plan.level,
                   difficulty);
        if (addedAllies + addedEnemies > 0)
            RecountBattle(room);
    }

    bool GuardedAddPvpAi(uint8_t* room, DWORD* code)
    {
        __try {
            AddPvpAi(room);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *code = GetExceptionCode();
            return false;
        }
    }

    int __fastcall SetupHumans(void* proc, void* /*edx*/, uint8_t* room, uint32_t* a, uint32_t* b, uint8_t c)
    {
        int result = g_setupHumans(proc, room, a, b, c);
        if (result == 0 && room) {
            GuardedObserverForget(room);
            GuardedObserverEmit(SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED, room);
            DWORD code = 0;
            if (!GuardedAddPvpAi(room, &code)) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while adding AI players; this battle has none", code);
                GuardedObserverEmit(SDGO_PVP_AI_EVENT_SETUP_FAILED, room, code);
            } else {
                GuardedObserverEmit(SDGO_PVP_AI_EVENT_SETUP_COMPLETE, room);
            }
        }
        return result;
    }

    // ---- 3. respawn ------------------------------------------------------------------
    //
    // The per-room battle tick (battle.room_tick, 0x004884D0) respawns a dead human pilot
    // once its respawn time has passed, but a dead AI pilot only in the AI battle modes 11
    // and 12. Right after it, the AI pilots of a PvP room go through the same steps the tick
    // runs for a human: RespawnPilot (time, mode rules, unit cost from pilot +0x1E, HP), and
    // once the unit is alive again the refresh and pilot-state calls.

    constexpr size_t kSlotInfoRespawnArg = 0x472; // passed to RespawnPilot by the tick

    uint8_t* SlotUnit(uint8_t* room, int slot)
    {
        uint8_t* pilot = room + kPilots + slot * kPilotStride;
        return pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
    }

    // Diagnostic text: the pilot's death match power and life cost, its unit's HP and life
    // cost, and the team powers.
    std::string LivesText(uint8_t* room, uint8_t* pilot)
    {
        uint8_t* unit = pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
        char text[200];
        sprintf_s(text, "power %u, life cost %u, unit HP %lu/%lu, unit life cost %u, team powers %ld/%ld",
                  *reinterpret_cast<uint16_t*>(pilot + kPilotPower), *reinterpret_cast<uint16_t*>(pilot + kPilotLifeCost),
                  static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(unit + kUnitHp)),
                  static_cast<unsigned long>(*reinterpret_cast<uint32_t*>(unit + kUnitMaxHp)),
                  *reinterpret_cast<uint16_t*>(unit + kUnitLifeCost),
                  static_cast<long>(*reinterpret_cast<int32_t*>(room + kRoomPower1)),
                  static_cast<long>(*reinterpret_cast<int32_t*>(room + kRoomPower2)));
        return text;
    }

    struct DeadAi
    {
        uint8_t* room;
        uint32_t id;
        uint32_t since; // tick time of death, for the log
    };
    DeadAi g_dead[64];

    DeadAi* FindDead(uint8_t* room, uint32_t id, bool create, uint32_t now)
    {
        for (DeadAi& d : g_dead)
            if (d.room == room && d.id == id)
                return &d;
        if (!create)
            return nullptr;
        for (DeadAi& d : g_dead)
            if (!d.room) {
                d = {room, id, now};
                return &d;
            }
        return nullptr;
    }

    void RespawnAiPilots(uint8_t* room, uint32_t now)
    {
        if (!IsPvpMode(room[kRoomMode]))
            return;
        for (int slot = 0; slot < kSlots; ++slot) {
            uint32_t id = SlotPilotId(room, slot);
            if (!(id & kAiPilotBit))
                continue;
            if (*reinterpret_cast<uint32_t*>(SlotUnit(room, slot) + kUnitHp) != 0)
                continue;
            DeadAi* dead = g_settings.logBattle ? FindDead(room, id, true, now) : nullptr;
            uint8_t* pilot = room + kPilots + slot * kPilotStride;
            if (dead && dead->since == now)
                g_api->Log(g_mod, SDGO_LOG_INFO, "AI pilot %08lX (slot %d) down at tick %lu; %s", static_cast<unsigned long>(id),
                           slot, static_cast<unsigned long>(now), LivesText(room, pilot).c_str());
            uint8_t* info = room + kSlotInfo + slot * kSlotInfoStride;
            if (!g_respawnPilot(room, now, pilot, id, *reinterpret_cast<uint32_t*>(info + kSlotInfoRespawnArg)))
                continue;
            if (*reinterpret_cast<uint32_t*>(SlotUnit(room, slot) + kUnitHp) == 0)
                continue;
            g_respawnRefresh(room, 0, pilot, 0, 0);
            g_pilotStateRespawn(pilot + 0x1D90);
            if (dead) {
                g_api->Log(g_mod, SDGO_LOG_INFO, "AI pilot %08lX (slot %d) respawned at tick %lu (down for %lu); %s",
                           static_cast<unsigned long>(id), slot, static_cast<unsigned long>(now),
                           static_cast<unsigned long>(now - dead->since), LivesText(room, pilot).c_str());
                dead->room = nullptr;
            }
        }
    }

    // Diagnostic (LogBattle): in a PvP room with AI pilots, the team scores (room +0x2EEE4 team 1, +0x2EEE8
    // team 2; a kill adds to the killer pilot's team in 0x0047BD30) whenever they change, and
    // the human pilots going down and coming back.
    struct RoomWatch
    {
        uint8_t* room;
        int32_t score[2];
        uint16_t humansDown; // bit per slot
    };
    RoomWatch g_watch[32];

    void WatchRoom(uint8_t* room)
    {
        if (!g_settings.logBattle || !IsPvpMode(room[kRoomMode]))
            return;
        bool hasAi = false;
        for (int slot = 0; slot < kSlots && !hasAi; ++slot)
            hasAi = (SlotPilotId(room, slot) & kAiPilotBit) != 0;
        RoomWatch* watch = nullptr;
        for (RoomWatch& w : g_watch)
            if (w.room == room)
                watch = &w;
        if (!hasAi) {
            if (watch)
                watch->room = nullptr;
            return;
        }
        int32_t score1 = *reinterpret_cast<int32_t*>(room + kRoomScoreTeam1);
        int32_t score2 = *reinterpret_cast<int32_t*>(room + kRoomScoreTeam2);
        if (!watch) {
            for (RoomWatch& w : g_watch)
                if (!w.room) {
                    w = {room, {score1, score2}, 0};
                    watch = &w;
                    break;
                }
            if (!watch)
                return;
            g_api->Log(g_mod, SDGO_LOG_INFO, "scores: team 1 %ld, team 2 %ld", static_cast<long>(score1),
                       static_cast<long>(score2));
        }
        if (score1 != watch->score[0] || score2 != watch->score[1]) {
            g_api->Log(g_mod, SDGO_LOG_INFO, "scores: team 1 %ld -> %ld, team 2 %ld -> %ld",
                       static_cast<long>(watch->score[0]), static_cast<long>(score1),
                       static_cast<long>(watch->score[1]), static_cast<long>(score2));
            watch->score[0] = score1;
            watch->score[1] = score2;
        }
        for (int slot = 0; slot < kSlots; ++slot) {
            uint32_t id = SlotPilotId(room, slot);
            if (id == 0 || (id & kAiPilotBit))
                continue;
            bool down = *reinterpret_cast<uint32_t*>(SlotUnit(room, slot) + kUnitHp) == 0;
            bool wasDown = (watch->humansDown >> slot) & 1;
            if (down != wasDown) {
                g_api->Log(g_mod, SDGO_LOG_INFO, "human pilot %08lX (slot %d, team %u) %s; %s", static_cast<unsigned long>(id),
                           slot, SlotTeam(room, slot), down ? "down" : "back",
                           LivesText(room, room + kPilots + slot * kPilotStride).c_str());
                watch->humansDown = static_cast<uint16_t>(watch->humansDown ^ (1u << slot));
            }
        }
    }

    bool GuardedRespawn(uint8_t* room, uint32_t now)
    {
        __try {
            WatchRoom(room);
            RespawnAiPilots(room, now);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    int __fastcall RoomTick(uint8_t* room, void* /*edx*/, uint32_t now)
    {
        int result = g_roomTick(room, now);
        if (room && !GuardedRespawn(room, now))
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception while respawning AI pilots");
        if (room)
            GuardedObserverMarkActive(room);
        return result;
    }

    // ---- 4. accepted damage and deferred Hard item use -----------------------------------

    bool RecordAcceptedDamage(uint8_t* room, int targetSlot, uint32_t now,
                              uint8_t* attackerPilot, uint8_t* weapon,
                              uint32_t hpBefore, uint32_t hpAfter)
    {
        if (!attackerPilot || !weapon || hpAfter >= hpBefore)
            return false;
        uint32_t attackerId = *reinterpret_cast<uint32_t*>(attackerPilot);
        uint8_t attackerTeam = attackerPilot[0x04];
        EnterCriticalSection(&g_itemsLock);
        uint32_t roomIndex = *reinterpret_cast<uint32_t*>(room + kRoomIndex);
        RoomItems* state = FindRoomItems(room, roomIndex);
        bool queued = false;
        if (state && state->difficulty == battle_ai::Difficulty::Hard &&
            targetSlot >= 0 && targetSlot < kSlots &&
            state->botActive[targetSlot]) {
            const items::BotKey& key = state->bots[targetSlot];
            LONG sequence = InterlockedIncrement(&g_damageReceipt);
            if (sequence == 0)
                sequence = InterlockedIncrement(&g_damageReceipt);
            items::DamageReceipt receipt{};
            receipt.target = key;
            receipt.receiptId =
                (static_cast<uint64_t>(now) << 32) |
                static_cast<uint32_t>(sequence);
            receipt.nowSeconds = now;
            receipt.attackerPilotId = attackerId;
            receipt.attackerTeam = attackerTeam;
            // The current server consumes byte +0x38 of its compact weapon
            // record as RangeType; 1 and 5 are its melee categories.
            receipt.weaponRangeType = weapon[0x38];
            receipt.hpBefore = hpBefore;
            receipt.hpAfter = hpAfter;
            queued = g_itemController.OnAcceptedDamage(receipt);
        }
        LeaveCriticalSection(&g_itemsLock);
        return queued;
    }

    uint32_t __fastcall AcceptedDamage(
        uint8_t* room, void* /*edx*/, uint32_t now, uint32_t attackerSlot,
        uint8_t* attackerPilot, uint8_t* weapon, uint32_t targetState,
        uint32_t targetPilotId, uint32_t proposedDamage, uint16_t stun,
        uint8_t calculationFlag, uint32_t extra, uint8_t part,
        uint32_t attackType)
    {
        int targetSlot = -1;
        uint32_t hpBefore = 0;
        uint8_t* targetUnit = nullptr;
        if (room && (targetPilotId & kAiPilotBit)) {
            targetSlot = FindPilotSlot(room, targetPilotId);
            if (targetSlot >= 0) {
                targetUnit = SlotUnit(room, targetSlot);
                hpBefore = *reinterpret_cast<uint32_t*>(targetUnit + kUnitHp);
            }
        }
        uint32_t result =
            g_damage(room, now, attackerSlot, attackerPilot, weapon, targetState,
                     targetPilotId, proposedDamage, stun, calculationFlag,
                     extra, part, attackType);
        if (targetUnit) {
            uint32_t hpAfter =
                *reinterpret_cast<uint32_t*>(targetUnit + kUnitHp);
            RecordAcceptedDamage(room, targetSlot, now, attackerPilot, weapon,
                                 hpBefore, hpAfter);
        }
        return result;
    }

    struct DrainTarget
    {
        uint8_t* room;
        uint32_t roomIndex;
        uint32_t generation;
    };

    void DrainHardItems(void* proc)
    {
        DrainTarget targets[kTrackedRooms] = {};
        int count = 0;
        EnterCriticalSection(&g_itemsLock);
        for (const RoomItems& state : g_itemRooms) {
            if (!state.active ||
                state.difficulty != battle_ai::Difficulty::Hard)
                continue;
            bool hasBot = false;
            for (bool active : state.botActive)
                hasBot = hasBot || active;
            if (hasBot)
                targets[count++] =
                    {state.room, state.roomIndex, state.generation};
        }
        LeaveCriticalSection(&g_itemsLock);

        uint32_t now =
            *reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(proc) +
                                        kWaitRoomClock);
        for (int i = 0; i < count; ++i) {
            const DrainTarget& target = targets[i];
            uint8_t* locked =
                g_lockRoom(proc, target.roomIndex,
                           items::kUseEquipItemOpcode);
            if (!locked)
                continue;
            items::DrainReport report{};
            if (locked == target.room) {
                EnterCriticalSection(&g_itemsLock);
                RoomItems* state =
                    FindRoomItems(target.room, target.roomIndex);
                if (state && state->generation == target.generation)
                    report = g_itemController.DrainRoom(
                        target.roomIndex, now, g_itemOps);
                LeaveCriticalSection(&g_itemsLock);
            }
            g_unlockRoom(proc, target.roomIndex);
            if (g_settings.logBattle &&
                (report.succeeded || report.expired || report.rejected))
                g_api->Log(
                    g_mod, SDGO_LOG_INFO,
                    "room %lu Hard items: %lu used, %lu expired, %lu rejected",
                    static_cast<unsigned long>(target.roomIndex),
                    static_cast<unsigned long>(report.succeeded),
                    static_cast<unsigned long>(report.expired),
                    static_cast<unsigned long>(report.rejected));
        }
    }

    int __fastcall WaitRoomTick(void* proc, void* /*edx*/)
    {
        int result = g_waitRoomTick(proc);
        DrainHardItems(proc);
        return result;
    }

    // ---- 5. battle end: the AI pilots leave the room -------------------------------------
    //
    // The battle end (battle.end, 0x0047E4A0) sends the results (the mode's result routine),
    // sets the room back to waiting (0x004848D0) and releases every pilot's battle state
    // (0x0047DF90). The AI pilots of a PvP room joined for that battle only (the next start
    // adds them again from the title); left in the slots, the clients took them for room
    // members in the waiting room and loaded a 3D model for a pilot without unit data
    // (0x00729C50 read through NULL). Right after the battle end each AI pilot's slot record
    // and pilot record are cleared, as the room delete (0x004849E0) clears a pilot's.

    void RemoveAiPilots(uint8_t* room)
    {
        if (!IsPvpMode(room[kRoomMode]))
            return;
        EndRoomItems(room);
        int removed = 0;
        for (int slot = 0; slot < kSlots; ++slot) {
            uint32_t id = SlotPilotId(room, slot);
            if (!(id & kAiPilotBit))
                continue;
            memset(room + kSlotInfo + slot * kSlotInfoStride, 0, kSlotInfoStride);
            memset(room + kPilots + slot * kPilotStride, 0, kPilotStride);
            if (DeadAi* dead = FindDead(room, id, false, 0))
                dead->room = nullptr;
            ++removed;
        }
        for (RoomWatch& w : g_watch)
            if (w.room == room)
                w.room = nullptr;
        if (removed)
            g_api->Log(g_mod, SDGO_LOG_INFO, "battle end: %d AI pilot(s) left the room", removed);
    }

    bool GuardedRemove(uint8_t* room)
    {
        __try {
            RemoveAiPilots(room);
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    void __fastcall BattleEnd(uint8_t* room, void* /*edx*/, uint16_t a, uint16_t b, int now)
    {
        const bool observed = ObserverRoomActive(room);
        g_battleEnd(room, a, b, now);
        if (observed)
            GuardedObserverEmit(SDGO_PVP_AI_EVENT_BATTLE_END, room, 0, false);
        if (room && !GuardedRemove(room))
            g_api->Log(g_mod, SDGO_LOG_ERROR, "exception while removing the AI pilots at battle end");
        if (room)
            GuardedObserverForget(room);
    }

    template <typename Fn>
    bool Bind(Fn& out, const char* symbol)
    {
        out = reinterpret_cast<Fn>(g_api->Symbol(g_mod, symbol));
        return out != nullptr;
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoPvpAiRegisterObserverV1(
    const SdgoPvpAiObserverSinkV1* sink)
{
    if (!sink || sink->size < sizeof(*sink) ||
        sink->version != SDGO_PVP_AI_OBSERVER_VERSION || !sink->callback)
        return 0;
    if (InterlockedCompareExchange(&g_observerRegistration, 1, 0) != 0)
        return 0;
    InterlockedExchangePointer(&g_observerUser, sink->user);
    InterlockedExchangePointer(
        &g_observerCallback, reinterpret_cast<PVOID>(sink->callback));
    InterlockedExchange(&g_observerRegistration, 2);
    return 1;
}

// The check detours run in the middle of the dispatcher: each keeps every register and the
// flags, asks GuardedDecide with the dispatcher's frame pointer, and then either resumes the
// original instructions (MinHook trampoline) or continues where the co-op mission rooms
// continue. The player count check comes first and logs the room.
static void* g_sizeCheckResume = nullptr;
static void* g_sizeChecked = nullptr;
static void* g_teamChecksResume = nullptr;
static void* g_teamChecksPassed = nullptr;

static int __stdcall SizeCheckDecision(uint8_t* frame)
{
    return GuardedDecide(frame, true);
}

static int __stdcall TeamChecksDecision(uint8_t* frame)
{
    return GuardedDecide(frame, false);
}

static __declspec(naked) void SizeCheckDetour()
{
    __asm {
        pushad
        pushfd
        push ebp
        call SizeCheckDecision
        test eax, eax
        jnz skip_check
        popfd
        popad
        jmp dword ptr [g_sizeCheckResume]
    skip_check:
        popfd
        popad
        jmp dword ptr [g_sizeChecked]
    }
}

static __declspec(naked) void TeamChecksDetour()
{
    __asm {
        pushad
        pushfd
        push ebp
        call TeamChecksDecision
        test eax, eax
        jnz skip_checks
        popfd
        popad
        jmp dword ptr [g_teamChecksResume]
    skip_checks:
        popfd
        popad
        jmp dword ptr [g_teamChecksPassed]
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    g_titleApi = sdgo::FindRoomTitleApi(api->ModDirectory(mod));
    bool bound = Bind(g_sizeChecked, "battle.start_size_checked") &&
                 Bind(g_teamChecksPassed, "battle.start_checks_passed") && Bind(g_aiNpcList, "battle.ai_npc_list") &&
                 Bind(g_waitRoomProc, "battle.waitroom_proc") && Bind(g_buildAiPilot, "battle.build_ai_pilot") &&
                 Bind(g_pilotReset, "battle.pilot_reset") && Bind(g_pilotStateId, "battle.pilot_state_id") &&
                 Bind(g_pilotStateRoom, "battle.pilot_state_room") && Bind(g_pilotStateMode, "battle.pilot_state_mode") &&
                 Bind(g_setupAiPilot, "battle.setup_ai_pilot") && Bind(g_respawnPilot, "battle.respawn_pilot") &&
                 Bind(g_respawnRefresh, "battle.respawn_refresh") &&
                  Bind(g_pilotStateRespawn, "battle.pilot_state_respawn") &&
                  Bind(g_waitRoomTick, "items.waitroom_tick") &&
                  Bind(g_damage, "items.accepted_damage") &&
                  Bind(g_mapFind, "items.pilot_map_find") &&
                  Bind(g_mapInsert, "items.pilot_map_insert") &&
                  Bind(g_mapErase, "items.pilot_map_erase") &&
                  Bind(g_lockRoom, "items.lock_room") &&
                  Bind(g_unlockRoom, "items.unlock_room") &&
                  Bind(g_itemLookup, "items.item_lookup") &&
                  Bind(g_itemTable, "items.item_table") &&
                  Bind(g_setItem, "items.set_item") &&
                  Bind(g_useItem, "items.use_item");
    if (!bound)
        return 0;
    InitializeCriticalSection(&g_itemsLock);
    g_itemOps.mapMatches = &NativeMapMatches;
    g_itemOps.mapInsert = &NativeMapInsert;
    g_itemOps.mapEraseIfExact = &NativeMapEraseIfExact;
    g_itemOps.snapshot = &NativeSnapshot;
    g_itemOps.equip = &NativeEquip;
    g_itemOps.use = &NativeUse;
    LoadSettings();
    LARGE_INTEGER counter = {};
    QueryPerformanceCounter(&counter);
    g_random = static_cast<unsigned>(counter.LowPart ^ GetTickCount() ^ GetCurrentProcessId()) | 1u;
    if (!api->HookCode(mod, "battle.setup_humans", reinterpret_cast<void*>(&SetupHumans),
                       reinterpret_cast<void**>(&g_setupHumans)))
        return 0;
    if (!api->HookCode(mod, "battle.start_size_check", reinterpret_cast<void*>(&SizeCheckDetour),
                       &g_sizeCheckResume))
        return 0;
    if (!api->HookCode(mod, "battle.start_team_checks", reinterpret_cast<void*>(&TeamChecksDetour),
                       &g_teamChecksResume))
        return 0;
    if (!api->HookCode(mod, "battle.end", reinterpret_cast<void*>(&BattleEnd),
                       reinterpret_cast<void**>(&g_battleEnd)))
        return 0;
    if (!api->HookCode(mod, "battle.room_tick", reinterpret_cast<void*>(&RoomTick),
                       reinterpret_cast<void**>(&g_roomTick)))
        return 0;
    if (!api->HookCode(mod, "items.accepted_damage",
                       reinterpret_cast<void*>(&AcceptedDamage),
                       reinterpret_cast<void**>(&g_damage)))
        return 0;
    if (!api->HookCode(mod, "items.waitroom_tick",
                       reinterpret_cast<void*>(&WaitRoomTick),
                       reinterpret_cast<void**>(&g_waitRoomTick)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "ready: PvP rooms whose title has !ai get AI players; default difficulty Medium, Hard enables native battle items");
    return 1;
}
