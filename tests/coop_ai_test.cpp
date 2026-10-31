#include "ai_test_support.h"
#include "../mods/coop_ai/coop_ai.cpp"
#include <algorithm>
#include <array>
#include <map>

namespace fixture {
using ai_test::Put;
std::map<std::string, int> settings;
int Setting(SdgoMod*, const char* key, int def) { auto i = settings.find(key); return i == settings.end() ? def : i->second; }
uint32_t StringSetting(SdgoMod*, const char*, const char*, char* value, uint32_t size) {
    strcpy_s(value, size, "100,101"); return 7;
}
std::vector<uint8_t> room(0x33000), lobby(0x100), lobbyProc(0x60), npcList(0x40);
std::array<uint8_t, 16> sentinel{}, first{}, second{};
uint8_t* listPointer = nullptr;
uint32_t npc1 = 100, nextId = 0x80000010u;
int added = 0, released = 0, started = 0;
void __fastcall Part(uint8_t*, void*) {}
void __fastcall Id(uint8_t*, void*, uint32_t) {}
void __fastcall Room(uint8_t*, void*, uint8_t*) {}
void __fastcall Mode(uint8_t*, void*, uint8_t, uint8_t) {}
int __fastcall Build(uint8_t*, void*, uint8_t* pilot, uint8_t* unit, uint32_t npc) {
    Put(pilot, 0, nextId++); Put(unit, 0, npc); return 1;
}
int __fastcall Setup(void*, void*, uint8_t*, uint8_t*, uint8_t, uint8_t) { ++added; return 1; }
int __fastcall Map(void*, void*, const uint32_t*, int* index) { *index = 1; return 1; }
uint8_t* __fastcall Lobby(void*, void*, int, int) { return lobby.data(); }
void __fastcall Release(void*, void*, int) { ++released; }
void __fastcall Start(void*, void*, uint8_t*, uint32_t, int, uint32_t, uint32_t) { ++started; }
void Reset() {
    std::fill(room.begin(), room.end(), uint8_t{}); std::fill(lobby.begin(), lobby.end(), uint8_t{});
    g_settings = Settings{}; g_settings.levels[2] = {100, 101}; g_random = 123;
    g_api = &ai_test::api; ai_test::api.SettingInt = &Setting; ai_test::api.SettingString = &StringSetting;
    g_buildAiPilot = reinterpret_cast<BuildAiPilotFn>(&Build); g_setupAiPilot = reinterpret_cast<SetupAiPilotFn>(&Setup);
    g_pilotReset = reinterpret_cast<PilotResetFn>(&Part); g_pilotStateId = reinterpret_cast<PilotStateIdFn>(&Id);
    g_pilotStateRoom = reinterpret_cast<PilotStateRoomFn>(&Room); g_pilotStateMode = reinterpret_cast<PilotStateModeFn>(&Mode);
    g_lobbyMapFind = reinterpret_cast<MapFindFn>(&Map); g_lobbyRoom = reinterpret_cast<LobbyRoomFn>(&Lobby);
    g_lobbyRelease = reinterpret_cast<LobbyReleaseFn>(&Release); g_startNotify = reinterpret_cast<StartNotifyFn>(&Start);
    g_lobbyProc = lobbyProc.data(); g_aiNpcList = &listPointer;
    room[kRoomMode] = 5; SlotPilotId(room.data(), 0) = 7;
    lobby[kLobbyTitleLength] = 3; std::memcpy(lobby.data() + kLobbyTitle, "!ai", 3);
    added = released = started = 0; settings.clear();
}
}

int main() {
    using namespace fixture;
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(lobbyProc.data() + kLobbyMapLock));
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(npcList.data() + 0x18));
    Put(npcList.data(), 4, first.data()); Put(npcList.data(), 8, sentinel.data()); Put<int>(npcList.data(), 12, 2);
    Put(first.data(), 8, second.data()); Put(second.data(), 0, &npc1);
    // The native list advances once before reading each payload.
    Put(second.data(), 8, sentinel.data()); listPointer = npcList.data();
    Reset();
    SCENARIO("co-op command semantics");
    CHECK(!ParseTitle("mission").enabled);
    auto p = ParseTitle("!AI3 LV4"); CHECK(p.enabled && p.count == 3 && p.level == 4);
    p = ParseTitle("!ai"); CHECK(p.enabled && p.count == 0 && p.level == 2);
    g_settings.requireRoomCommand = 0; CHECK(ParseTitle("mission").enabled);
    p = ParseTitle("!ai4294967296"); CHECK(p.count == kSlots);
    CHECK(ParseIds("4294967296,100") == (std::vector<uint32_t>{100}));
    SCENARIO("invalid configuration cannot index outside level pools");
    settings["DefaultLevel"] = 99; settings["SquadSize"] = 999; settings["DefaultCount"] = -1;
    LoadSettings(); CHECK(g_settings.defaultLevel == 2); CHECK(g_settings.squadSize == 4); CHECK(g_settings.defaultCount == 0);
    settings["DefaultLevel"] = 5; settings["SquadSize"] = 12; settings["DefaultCount"] = 2;
    LoadSettings(); CHECK(g_settings.defaultLevel == 5 && g_settings.squadSize == 12 && g_settings.defaultCount == 2);
    SCENARIO("wingmen fill only free squad slots and join the human team");
    Reset(); CHECK(AiNpcIds() == (std::vector<uint32_t>{100}));
    StartNotify(nullptr, nullptr, room.data(), 0, 0, 0, 0);
    CHECK(added == 3 && started == 1 && released == 1);
    for (int i = 1; i <= 3; ++i) {
        CHECK(SlotPilotId(room.data(), i) & 0x80000000u);
        CHECK(room[kSlotInfo + i * kSlotInfoStride + kSlotInfoTeamA] == 1);
    }
    AddWingmen(room.data()); CHECK(added == 3);
    SCENARIO("explicit count and no-command mode isolation");
    Reset(); lobby[kLobbyTitleLength] = 4; std::memcpy(lobby.data() + kLobbyTitle, "!ai1", 4);
    AddWingmen(room.data()); CHECK(added == 1);
    Reset(); lobby[kLobbyTitleLength] = 4; std::memcpy(lobby.data() + kLobbyTitle, "test", 4);
    AddWingmen(room.data()); CHECK(added == 0);
    Reset(); room[kRoomMode] = 1; AddWingmen(room.data()); CHECK(added == 0);
    Reset(); g_settings.levels[2] = {999}; AddWingmen(room.data()); CHECK(added == 0);
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(lobbyProc.data() + kLobbyMapLock));
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(npcList.data() + 0x18));
    return ai_test::Finish();
}
