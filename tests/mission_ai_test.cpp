#include "ai_test_support.h"
#include "../mods/mission_ai/mission_ai.cpp"
#include <array>

namespace fixture {
using ai_test::Put;
std::array<uint8_t, 0x640> mode{};
std::array<uint8_t, 0x80> room{};
std::array<uint8_t, 12 * kRecordSize> humans{}, ais{};
std::array<std::array<uint8_t, 0x3300>, 12> units{};
std::array<uint8_t, 0x40> start{};
std::array<ModeVirtualFn, 8> vtable{};
int manager = 0;
void* um = &manager;
int created = 0, remote = 0, assigned = 0, calculated = 0, initialized = 0;
bool observer = false;
std::vector<int> slots;
bool __cdecl Network() { return true; }
bool __fastcall Observer(void*, void*) { return observer; }
int __fastcall Virtual(uint8_t*, void*) { return 1; }
uint8_t* __fastcall Start(void*, void*, int, int) { return start.data(); }
uint8_t* __fastcall Create(uint8_t*, void*, uint32_t, float, float, float, int, float, float, float) {
    if (created >= 12) return nullptr;
    auto u = units[created].data(); Put(u, kUnitObjectId, 1001u + created); ++created; return u;
}
uint8_t* __fastcall Remote(uint8_t* m, void*, uint32_t npc, float x, float y, float z, int team, float dx, float dy, float dz) {
    ++remote; return Create(m, nullptr, npc, x, y, z, team, dx, dy, dz);
}
void __fastcall Info(void*, void*, int slot, int, uint32_t, uint32_t, uint8_t*) {
    CHECK(slot >= 0 && slot < 12); slots.push_back(slot); ++assigned;
}
void __fastcall Add(void*, void*, uint8_t*) {}
void __fastcall Calculate(void*, void*) { ++calculated; }
void __fastcall Init(uint8_t*, void*) { ++initialized; }
void Reset(int h, int a, bool master) {
    mode.fill(0); room.fill(0); humans.fill(0); ais.fill(0); units = {}; slots.clear();
    created = remote = assigned = calculated = initialized = 0; observer = false;
    g_api = &ai_test::api;
    vtable.fill(nullptr); vtable[kModeIsNetworkSlot] = reinterpret_cast<ModeVirtualFn>(&Virtual);
    Put(mode.data(), 0, vtable.data()); Put(mode.data(), kModeRoom, room.data());
    mode[kModeRoomMaster] = master ? 1 : 0;
    Put(room.data(), kRoomHumansBegin, humans.data()); Put(room.data(), kRoomHumansEnd, humans.data() + h * kRecordSize);
    Put(room.data(), kRoomAiBegin, ais.data()); Put(room.data(), kRoomAiEnd, ais.data() + a * kRecordSize);
    for (int i = 0; i < a; ++i) {
        auto r = ais.data() + i * kRecordSize; Put(r, kRecordServerId, 0x80000010u + i); r[kRecordTeam] = 1;
    }
    g_game.unitManager = &um; g_game.isNetwork = &Network;
    g_game.isObserver = reinterpret_cast<IsObserverFn>(&Observer);
    g_game.findStartPoint = reinterpret_cast<FindStartPointFn>(&Start);
    g_game.createNpc = reinterpret_cast<CreateNpcFn>(&Create); g_game.createRemoteNpc = reinterpret_cast<CreateNpcFn>(&Remote);
    g_game.setPlayerInfo = reinterpret_cast<SetPlayerInfoFn>(&Info);
    g_game.addAiUnit = reinterpret_cast<AddAiUnitFn>(&Add); g_game.calculateTeam = reinterpret_cast<CalculateTeamFn>(&Calculate);
    g_game.originalInit = reinterpret_cast<ModeInitFn>(&Init); g_game.missionVtable = vtable.data();
}
}

int main() {
    g_api = &ai_test::api;
    SCENARIO("configuration values remain data when converted to Lua");
    CHECK(Trim(" \t hi\r ") == "hi"); CHECK(Trim(" \t\r").empty());
    CHECK(IsInteger("-42") && IsInteger("0") && !IsInteger("-") && !IsInteger("1e9"));
    CHECK(!IsInteger("1; error('injected')") && !IsInteger("9999999999"));
    CHECK(IntegerList("1, -2, 3") == "{ 1, -2, 3 }");
    CHECK(IntegerList("1,,3").empty() && IntegerList("1,2,").empty());
    CHECK(LuaValue("-42") == "-42"); CHECK(LuaValue("1,2") == "{ 1, 2 }");
    CHECK(LuaValue("1,boom()") == "\"1,boom()\"");
    CHECK(LuaValue("a\"\\b\n") == "\"a\\\"\\\\b\"");
    CHECK(LuaValue("") == "\"\"");
    SCENARIO("INI settings become escaped Lua fields, invalid keys are ignored");
    wchar_t dir[MAX_PATH], file[MAX_PATH];
    CHECK(GetTempPathW(MAX_PATH, dir) > 0);
    CHECK(GetTempFileNameW(dir, L"sdt", 0, file) != 0);
    g_manifestPathW = file;
    CHECK(WritePrivateProfileStringW(L"settings", L"EnemiesTargetWingmen", L"1", file) != 0);
    CHECK(WritePrivateProfileStringW(L"settings", L"Label", L"x\"y\\z", file) != 0);
    CHECK(WritePrivateProfileStringW(L"settings", L"bad-key", L"1", file) != 0);
    CHECK(BuildConfig());
    CHECK(g_configChunk.find("EnemiesTargetWingmen = 1") != std::string::npos);
    CHECK(g_configChunk.find("Label = \"x\\\"y\\\\z\"") != std::string::npos);
    CHECK(g_configChunk.find("bad-key") == std::string::npos);
    CHECK(DeleteFileW(file) != 0);

    SCENARIO("mission master runs AI, other clients create remote copies");
    fixture::Reset(1, 3, true); MissionInit(fixture::mode.data(), nullptr);
    CHECK(fixture::initialized == 1 && fixture::created == 3 && fixture::remote == 0 && fixture::calculated == 1);
    CHECK(g_wingmen == (std::vector<uint32_t>{1001, 1002, 1003}));
    CHECK(g_localWingmen == g_wingmen);
    CHECK(fixture::slots == (std::vector<int>{1, 2, 3}));
    fixture::Reset(1, 2, false); MissionInit(fixture::mode.data(), nullptr);
    CHECK(fixture::remote == 2);
    CHECK(g_localWingmen.empty());
    fixture::Reset(1, 2, true); fixture::observer = true; MissionInit(fixture::mode.data(), nullptr);
    CHECK(fixture::remote == 2);
    CHECK(g_localWingmen.empty());

    SCENARIO("mission slot limit and derived mode isolation");
    fixture::Reset(11, 3, true); MissionInit(fixture::mode.data(), nullptr);
    CHECK(fixture::created == 1 && fixture::assigned == 1 && g_wingmen.size() == 1);
    fixture::Reset(1, 3, true); g_game.missionVtable = nullptr; MissionInit(fixture::mode.data(), nullptr);
    CHECK(fixture::initialized == 1 && fixture::created == 0);
    return ai_test::Finish();
}
