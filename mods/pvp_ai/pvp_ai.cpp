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
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"

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
    constexpr size_t kPilotUnits = 0x340;       // unit records, stride 0x658
    constexpr size_t kUnitStride = 0x658;
    // Death match lives (RespawnPilot 0x00487030, modes 3/14): the pilot's power (pilot +0x1E,
    // u16, 1000 at the start) pays each life the unit record's cost (+0x08, u16, unit data +9);
    // the life being played costs pilot +0x20 (u16), taken from the team power when it dies.
    constexpr size_t kPilotPower = 0x1E;
    constexpr size_t kPilotLifeCost = 0x20;
    constexpr size_t kUnitLifeCost = 0x08;
    constexpr size_t kUnitMaxHp = 0x04;
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
    using RespawnPilotFn = int(__thiscall*)(uint8_t* room, uint32_t now, uint8_t* pilot, uint32_t id, uint32_t extra);
    using RespawnRefreshFn = void(__thiscall*)(uint8_t* room, int a, uint8_t* pilot, int b, int c);
    using PilotStateRespawnFn = int(__thiscall*)(uint8_t* part);
    using BuildAiPilotFn = int(__thiscall*)(uint8_t* room, uint8_t* pilot, uint8_t* unit, uint32_t npcData);
    using PilotResetFn = void(__thiscall*)(uint8_t* part);
    using PilotStateIdFn = void(__thiscall*)(uint8_t* part, uint32_t id);
    using PilotStateRoomFn = void(__thiscall*)(uint8_t* part, uint8_t* room);
    using PilotStateModeFn = void(__thiscall*)(uint8_t* part, uint8_t a, uint8_t b);
    using SetupAiPilotFn = int(__thiscall*)(void* proc, uint8_t* info, uint8_t* pilot, uint8_t a, uint8_t b);

    // The title of a PvP room is in the room itself (unlike a co-op mission room, whose
    // title is in the lobby room coop_ai reads): written by the room-settings (0x004BEC60)
    // and title-change (0x004DCA40) handlers, sent to the clients by 0x004DD120.
    constexpr size_t kRoomTitleLength = 0x79;   // u8
    constexpr size_t kRoomTitle = 0x7A;         // up to 20 bytes, CP949

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    SetupHumansFn g_setupHumans = nullptr;
    RoomTickFn g_roomTick = nullptr;
    RespawnPilotFn g_respawnPilot = nullptr;
    RespawnRefreshFn g_respawnRefresh = nullptr;
    PilotStateRespawnFn g_pilotStateRespawn = nullptr;
    uint8_t** g_aiNpcList = nullptr;
    void* g_waitRoomProc = nullptr;
    BuildAiPilotFn g_buildAiPilot = nullptr;
    PilotResetFn g_pilotReset = nullptr;
    PilotStateIdFn g_pilotStateId = nullptr;
    PilotStateRoomFn g_pilotStateRoom = nullptr;
    PilotStateModeFn g_pilotStateMode = nullptr;
    SetupAiPilotFn g_setupAiPilot = nullptr;

    struct Settings
    {
        int defaultLevel = 2;
        int fillTeamSize = 3;
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
            while (*p >= '0' && *p <= '9')
                value = value * 10 + static_cast<uint32_t>(*p++ - '0');
            ids.push_back(value);
        }
        return ids;
    }

    void LoadSettings()
    {
        g_settings.defaultLevel = g_api->SettingInt(g_mod, "DefaultLevel", 2);
        g_settings.fillTeamSize = g_api->SettingInt(g_mod, "FillTeamSize", 3);
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
    };

    int ReadNumber(const char*& p)
    {
        int value = 0;
        while (*p >= '0' && *p <= '9')
            value = value * 10 + (*p++ - '0');
        return value;
    }

    // "!ai" fills both teams to FillTeamSize; "!aiN" asks for N enemies; options after it: eN enemies,
    // aN allies, lvN level. Case-insensitive.
    Plan ParseTitle(const char* title)
    {
        Plan plan;
        plan.level = g_settings.defaultLevel;
        char lower[kRoomTitleMax + 1];
        size_t n = 0;
        for (; title[n] && n < kRoomTitleMax; ++n)
            lower[n] = (title[n] >= 'A' && title[n] <= 'Z') ? static_cast<char>(title[n] - 'A' + 'a') : title[n];
        lower[n] = '\0';
        const char* command = strstr(lower, "!ai");
        if (!command)
            return plan;
        plan.enabled = true;
        const char* p = command + 3;
        int count = -1;
        if (*p >= '0' && *p <= '9')
            count = ReadNumber(p);
        int enemies = -1;
        int allies = -1;
        while (*p) {
            while (*p == ' ')
                ++p;
            const char* token = p;
            while (*p && *p != ' ')
                ++p;
            size_t length = static_cast<size_t>(p - token);
            if (length == 3 && token[0] == 'l' && token[1] == 'v' && token[2] >= '1' && token[2] <= '5') {
                plan.level = token[2] - '0';
            } else if (length >= 2 && (token[0] == 'e' || token[0] == 'a') && token[1] >= '0' && token[1] <= '9') {
                const char* digits = token + 1;
                int value = ReadNumber(digits);
                if (digits == p)
                    (token[0] == 'e' ? enemies : allies) = value;
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
        char title[kRoomTitleMax + 1] = {};
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
        view.titled = view.title[0] != '\0';
        uint32_t host = *reinterpret_cast<uint32_t*>(room + kRoomHost);
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
        return plan.enabled;
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

    // Same steps as the AI battle mode (0x004B79D0) for an AI slot, with the given team.
    bool AddAiPilot(uint8_t* room, int slot, uint32_t npcData, uint8_t team, uint8_t colour)
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

        *reinterpret_cast<uint32_t*>(pilot + 0x09) = 1;
        pilot[0x0D] = 0;
        pilot[0x04] = team;
        pilot[0x39] = colour;
        *reinterpret_cast<uint32_t*>(pilot + 0x05) = 30000;
        info[kSlotInfoTeamA] = team;
        info[kSlotInfoColour] = colour;
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
        g_pilotStateMode(pilot + 0x1D90, room[kRoomType], room[kRoomMode]);
        if (!g_setupAiPilot(g_waitRoomProc, info, pilot, room[kRoomType], room[kRoomMode])) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "slot %d: AI pilot setup for NPC %lu failed", slot,
                       static_cast<unsigned long>(npcData));
            SlotPilotId(room, slot) = 0;
            memset(pilot, 0, kPilotStride);
            return false;
        }
        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "slot %d: AI pilot %08lX (NPC %lu) joined on team %u, colour %u; unit life cost %u, max HP %lu, power %u, "
                   "life cost %u",
                   slot, static_cast<unsigned long>(id), static_cast<unsigned long>(npcData), team, colour,
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
    int AddTeam(uint8_t* room, uint8_t team, int wanted, const std::vector<uint32_t>& pool)
    {
        int added = 0;
        uint8_t colour = TeamColour(room, team);
        int first = team == 1 ? 0 : kSlotsPerTeam;
        for (int slot = first; slot < first + kSlotsPerTeam && added < wanted; ++slot) {
            if (SlotPilotId(room, slot) != 0)
                continue;
            uint32_t pick = pool[NextRandom() % pool.size()];
            for (int tries = 0; tries < 20; ++tries) {
                bool used = false;
                for (int other = 0; other < kSlots && !used; ++other)
                    used = (SlotPilotId(room, other) & kAiPilotBit) && PilotUnitId(room, other) == pick;
                if (!used)
                    break;
                pick = pool[NextRandom() % pool.size()];
            }
            if (AddAiPilot(room, slot, pick, team, colour))
                ++added;
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
    constexpr size_t kPilotTagCost = 0x1CA0;     // u8, sum of the pilot's unit costs
    constexpr size_t kPilotTagUnits = 0x1CA1;    // u8
    constexpr size_t kUnitCost = 0x0A;           // u8, unit record: cost (kill points)

    int GridGauge(int perTeam)
    {
        switch (perTeam) {
        case 1: case 2: case 4: return 0x50;
        case 3: return 0x3C;
        case 5: return 0x78;
        case 6: return 0xA0;
        default: return -1;
        }
    }

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
            if (GridGauge(size1) > 0)
                field(kRoomGauge1) = GridGauge(size1);
            if (GridGauge(size2) > 0)
                field(kRoomGauge2) = GridGauge(size2);
        } else if (mode == 7) {
            for (int slot = 0; slot < kSlots; ++slot) {
                if (!(SlotPilotId(room, slot) & kAiPilotBit))
                    continue;
                uint8_t* pilot = room + kPilots + slot * kPilotStride;
                uint8_t cost = pilot[kPilotUnits + pilot[kPilotForm] * kUnitStride + kUnitCost];
                pilot[kPilotTagCost] = cost;
                pilot[kPilotTagUnits] = 1;
                bool team1 = SlotTeam(room, slot) == 1;
                field(team1 ? kRoomGauge1 : kRoomGauge2) += cost;
                ++room[team1 ? kRoomTagUnits1 : kRoomTagUnits2];
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
        for (uint32_t id : g_settings.levels[plan.level])
            if (Contains(available, id))
                pool.push_back(id);
        if (pool.empty()) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "no NPC of Level%d is in the server's AI-NPC list (%u entries)",
                       plan.level, static_cast<unsigned>(available.size()));
            return;
        }

        int addedAllies = allies > 0 ? AddTeam(room, allyTeam, allies, pool) : 0;
        int addedEnemies = enemies > 0 ? AddTeam(room, enemyTeam, enemies, pool) : 0;
        g_api->Log(g_mod, SDGO_LOG_INFO, "%d allies (team %u) and %d enemies (team %u) added, level %d",
                   addedAllies, allyTeam, addedEnemies, enemyTeam, plan.level);
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
            DWORD code = 0;
            if (!GuardedAddPvpAi(room, &code))
                g_api->Log(g_mod, SDGO_LOG_ERROR, "exception %08lX while adding AI players; this battle has none", code);
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
    constexpr size_t kUnitHp = 0x34;              // unit record: current HP

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
            DeadAi* dead = FindDead(room, id, true, now);
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
            g_api->Log(g_mod, SDGO_LOG_INFO, "AI pilot %08lX (slot %d) respawned at tick %lu (down for %lu); %s",
                       static_cast<unsigned long>(id), slot, static_cast<unsigned long>(now),
                       static_cast<unsigned long>(dead ? now - dead->since : 0), LivesText(room, pilot).c_str());
            if (dead)
                dead->room = nullptr;
        }
    }

    // Diagnostic: in a PvP room with AI pilots, the team scores (room +0x2EEE4 team 1, +0x2EEE8
    // team 2; a kill adds to the killer pilot's team in 0x0047BD30) whenever they change, and
    // the human pilots going down and coming back.
    constexpr size_t kRoomScoreTeam1 = 0x2EEE4;
    constexpr size_t kRoomScoreTeam2 = 0x2EEE8;

    struct RoomWatch
    {
        uint8_t* room;
        int32_t score[2];
        uint16_t humansDown; // bit per slot
    };
    RoomWatch g_watch[32];

    void WatchRoom(uint8_t* room)
    {
        if (!g_settings.logRoom || !IsPvpMode(room[kRoomMode]))
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
        return result;
    }

    template <typename Fn>
    bool Bind(Fn& out, const char* symbol)
    {
        out = reinterpret_cast<Fn>(g_api->Symbol(g_mod, symbol));
        return out != nullptr;
    }
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
    bool bound = Bind(g_sizeChecked, "battle.start_size_checked") &&
                 Bind(g_teamChecksPassed, "battle.start_checks_passed") && Bind(g_aiNpcList, "battle.ai_npc_list") &&
                 Bind(g_waitRoomProc, "battle.waitroom_proc") && Bind(g_buildAiPilot, "battle.build_ai_pilot") &&
                 Bind(g_pilotReset, "battle.pilot_reset") && Bind(g_pilotStateId, "battle.pilot_state_id") &&
                 Bind(g_pilotStateRoom, "battle.pilot_state_room") && Bind(g_pilotStateMode, "battle.pilot_state_mode") &&
                 Bind(g_setupAiPilot, "battle.setup_ai_pilot") && Bind(g_respawnPilot, "battle.respawn_pilot") &&
                 Bind(g_respawnRefresh, "battle.respawn_refresh") &&
                 Bind(g_pilotStateRespawn, "battle.pilot_state_respawn");
    if (!bound)
        return 0;
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
    if (!api->HookCode(mod, "battle.room_tick", reinterpret_cast<void*>(&RoomTick),
                       reinterpret_cast<void**>(&g_roomTick)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: PvP rooms whose title has !ai get AI players");
    return 1;
}
