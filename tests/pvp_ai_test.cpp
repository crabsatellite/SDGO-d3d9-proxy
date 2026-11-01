#include "ai_test_support.h"
#include "../mods/pvp_ai/pvp_ai.cpp"
#include <algorithm>
#include <limits>
#include <set>
#include <thread>

namespace fixture {
using ai_test::Put;
using ai_test::Get;
std::vector<uint8_t> room(0x33000);
uint32_t nextId = kAiPilotBit | 100;
uint32_t failBuild = 0;
uint32_t failSetup = 0;
int buildCalls = 0, setupCalls = 0, respawns = 0, refreshes = 0, stateRespawns = 0;
int originals = 0;
bool revive = true;
bool clearIdentityAtEnd = false;
bool originalReturnedBeforeObserver = false;
std::vector<SdgoPvpAiObserverEventV1> observerEvents;
std::vector<uint32_t> npcIds = {100, 101, 102, 103, 104};
std::vector<uint8_t> points = {4, 3, 2, 5, 0};

void Human(int slot, int team, int colour = 0) {
    SlotPilotId(room.data(), slot) = 10u + slot;
    room[kSlotInfo + slot * kSlotInfoStride + kSlotInfoTeamA] = static_cast<uint8_t>(team);
    room[kSlotInfo + slot * kSlotInfoStride + kSlotInfoColour] = static_cast<uint8_t>(colour ? colour : team);
}
uint8_t* Pilot(int slot) { return room.data() + kPilots + slot * kPilotStride; }
uint8_t* Unit(int slot, int form = 0) { return Pilot(slot) + kPilotUnits + form * kUnitStride; }
void Title(const char* title) {
    const auto n = (std::min)(std::strlen(title), kRoomTitleMax);
    room[kRoomTitleLength] = static_cast<uint8_t>(n);
    std::memcpy(room.data() + kRoomTitle, title, n);
}
int __fastcall Build(uint8_t*, void*, uint8_t* pilot, uint8_t* unit, uint32_t npc) {
    ++buildCalls;
    if (npc == failBuild) return 0;
    auto it = std::find(npcIds.begin(), npcIds.end(), npc);
    if (it == npcIds.end()) return 0;
    Put(pilot, 0, nextId++);
    Put(unit, 0, npc);
    unit[kUnitPoints] = points[static_cast<size_t>(it - npcIds.begin())];
    unit[kUnitGrade] = 3;
    Put<uint16_t>(unit, kUnitLifeCost, 500);
    Put<uint32_t>(unit, kUnitMaxHp, 15000);
    Put<uint32_t>(unit, kUnitHp, 15000);
    return 1;
}
int __fastcall Setup(void*, void*, uint8_t*, uint8_t* pilot, uint8_t, uint8_t mode) {
    ++setupCalls;
    uint8_t* unit = pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride;
    if (Get<uint32_t>(unit, 0) == failSetup) return 0;
    if (mode == 3 || mode == 14) {
        auto cost = Get<uint16_t>(unit, kUnitLifeCost);
        Put<uint16_t>(pilot, kPilotPower, static_cast<uint16_t>(1000 - cost));
        Put(pilot, kPilotLifeCost, cost);
    }
    return 1;
}
void __fastcall ResetPart(uint8_t*, void*) {}
void __fastcall SetId(uint8_t*, void*, uint32_t) {}
void __fastcall SetRoom(uint8_t*, void*, uint8_t*) {}
void __fastcall SetMode(uint8_t*, void*, uint8_t, uint8_t) {}
int __fastcall Respawn(uint8_t*, void*, uint32_t, uint8_t* pilot, uint32_t, uint32_t) {
    ++respawns;
    if (revive) Put<uint32_t>(pilot + kPilotUnits + pilot[kPilotForm] * kUnitStride, kUnitHp, 9000);
    return revive ? 1 : 0;
}
void __fastcall Refresh(uint8_t*, void*, int, uint8_t*, int, int) { ++refreshes; }
int __fastcall StateRespawn(uint8_t*, void*) { ++stateRespawns; return 1; }
void __fastcall End(uint8_t* endRoom, void*, uint16_t, uint16_t, int) {
    ++originals;
    if (clearIdentityAtEnd) {
        Put(endRoom, kRoomHost, 0u);
        endRoom[kRoomMode] = 0;
        endRoom[kRoomTitleLength] = 0;
        memset(endRoom + kRoomTitle, 0, kRoomTitleMax);
    }
    originalReturnedBeforeObserver = true;
}
void __cdecl Observe(const SdgoPvpAiObserverEventV1* event, void*) {
    CHECK(originalReturnedBeforeObserver);
    observerEvents.push_back(*event);
}

void Reset(int mode = 1) {
    std::fill(room.begin(), room.end(), uint8_t{});
    room[kRoomMode] = static_cast<uint8_t>(mode);
    g_settings = Settings{};
    g_settings.gradeCost[3] = 455;
    g_settings.logRoom = 0;
    g_random = 1234567;
    g_api = &ai_test::api;
    g_buildAiPilot = reinterpret_cast<BuildAiPilotFn>(&Build);
    g_setupAiPilot = reinterpret_cast<SetupAiPilotFn>(&Setup);
    g_pilotReset = reinterpret_cast<PilotResetFn>(&ResetPart);
    g_pilotStateId = reinterpret_cast<PilotStateIdFn>(&SetId);
    g_pilotStateRoom = reinterpret_cast<PilotStateRoomFn>(&SetRoom);
    g_pilotStateMode = reinterpret_cast<PilotStateModeFn>(&SetMode);
    g_respawnPilot = reinterpret_cast<RespawnPilotFn>(&Respawn);
    g_respawnRefresh = reinterpret_cast<RespawnRefreshFn>(&Refresh);
    g_pilotStateRespawn = reinterpret_cast<PilotStateRespawnFn>(&StateRespawn);
    g_battleEnd = reinterpret_cast<BattleEndFn>(&End);
    std::memset(g_dead, 0, sizeof(g_dead));
    std::memset(g_watch, 0, sizeof(g_watch));
    for (auto& active : g_observerActiveRooms)
        InterlockedExchangePointer(&active, nullptr);
    InterlockedExchangePointer(&g_observerCallback, nullptr);
    InterlockedExchangePointer(&g_observerUser, nullptr);
    InterlockedExchange(&g_observerRegistration, 0);
    InterlockedExchange(&g_observerSequence, 0);
    InterlockedExchange(&g_observerFaultLogged, 0);
    buildCalls = setupCalls = respawns = refreshes = stateRespawns = originals = 0;
    failBuild = failSetup = 0;
    revive = true;
    clearIdentityAtEnd = false;
    originalReturnedBeforeObserver = false;
    observerEvents.clear();
}
}

int main() {
    using namespace fixture;
    // The production ModLoad initializes the new item lifecycle lock. This
    // fixture calls the included implementation directly instead of ModLoad.
    InitializeCriticalSection(&g_itemsLock);
    Reset();
    SCENARIO("room commands and bounded numbers");
    CHECK(!ParseTitle("normal room").enabled);
    auto p = ParseTitle("!AI e3 a2 LV4");
    CHECK(p.enabled && p.enemies == 3 && p.allies == 2 && p.level == 4);
    p = ParseTitle("!ai"); CHECK(p.enemies == kFill && p.allies == kFill);
    p = ParseTitle("!ai3 a1"); CHECK(p.enemies == 3 && p.allies == 1);
    p = ParseTitle("!ai0"); CHECK(p.enemies == 0 && p.allies == 0);
    p = ParseTitle("!ai e2x a3 lv9"); CHECK(p.enemies == 0 && p.allies == 3 && p.level == 2);
    p = ParseTitle("!ai4294967296"); CHECK(p.enemies == kSlotsPerTeam && p.allies == 0);
    p = ParseTitle("!ai e99999999999999"); CHECK(p.enemies == kSlotsPerTeam);
    p = ParseTitle("!ai a99999999999999"); CHECK(p.allies == kSlotsPerTeam);
    SCENARIO("PvP AI and random commands compose in either order with scoped arguments");
    for(const char* title:{"!rand !ai3 a1 lv4","!ai3 a1 lv4 !rand","!rand !ai e3 a1 lv4","!AI e3 a1 LV4 !RaNd"}) {
        p=ParseTitle(title);CHECK(p.enabled && p.enemies==3 && p.allies==1 && p.level==4);
    }
    for(const char* title:{"!rand !ai","!ai !rand lv5 e2 a2","!ai !future lv5 e2"}) {
        p=ParseTitle(title);CHECK(p.enabled && p.enemies==kFill && p.allies==kFill && p.level==2);
    }
    p=ParseTitle("!ai e2 !rand a3 lv5");CHECK(p.enemies==2 && p.allies==0 && p.level==2);
    p=ParseTitle("!rand !ai0");CHECK(p.enabled && p.enemies==0 && p.allies==0);
    for(const char* title:{"!aim","prefix!ai3","!ai3x","!ai!rand"}) CHECK(!ParseTitle(title).enabled);
    auto parserApi=ai_test::api;parserApi.FindRoomCommand=&sdgo::room_commands::Parse;
    g_api=&parserApi;
    p=ParseTitle("!rand !AI3 a1 LV4");CHECK(p.enabled && p.enemies==3 && p.allies==1 && p.level==4);
    g_api=&ai_test::api;
    CHECK(ParseIds("1, 2 3,,4294967295") == (std::vector<uint32_t>{1, 2, 3, UINT32_MAX}));
    CHECK(ParseIds("4294967296, 100") == (std::vector<uint32_t>{100}));
    CHECK(ParseIds("18446744073709551616100,101") == (std::vector<uint32_t>{101}));
    CHECK(ParseIds("").empty());

    SCENARIO("room view, host team and spawn colours");
    Human(0, 2, 1); Human(1, 1, 2); Put(room.data(), kRoomHost, 10u); Title("!ai e2 a1");
    auto v = ViewRoom(room.data());
    CHECK(v.humans[1] == 1 && v.humans[2] == 1 && v.hostTeam == 2);
    CHECK(std::strcmp(v.title, "!ai e2 a1") == 0);
    CHECK(TeamColour(room.data(), 1) == 2 && TeamColour(room.data(), 2) == 1);
    Human(1, 2, 1); CHECK(TeamColour(room.data(), 1) == 2);
    room[kRoomTitleLength] = 255; v = ViewRoom(room.data()); CHECK(std::strlen(v.title) <= kRoomTitleMax);

    SCENARIO("explicit zero bots retain native start checks");
    std::vector<uint8_t> frame(kDispatcherRoomCopy + 16);
    auto copy = frame.data(); copy[kRoomMode] = 1; copy[kRoomTitleLength] = 4;
    std::memcpy(copy + kRoomTitle, "!ai0", 4);
    CHECK(!DecideSkipChecks(frame.data() + kDispatcherRoomCopy, false));
    copy[kRoomTitleLength] = 3; CHECK(DecideSkipChecks(frame.data() + kDispatcherRoomCopy, false));
    for (int mode : {5, 8, 10, 11, 12}) { copy[kRoomMode] = static_cast<uint8_t>(mode); CHECK(!DecideSkipChecks(frame.data() + kDispatcherRoomCopy, false)); }

    SCENARIO("life cost and first-life power");
    for (int mode : {3, 14}) {
        Reset(mode); CHECK(AddAiPilot(room.data(), 6, 100, 2, 1));
        CHECK(Get<uint16_t>(Unit(6), kUnitLifeCost) == 455);
        CHECK(Get<uint16_t>(Pilot(6), kPilotPower) == 545);
        CHECK(Get<uint16_t>(Pilot(6), kPilotLifeCost) == 455);
        CHECK(Pilot(6)[4] == 2 && Pilot(6)[0x39] == 1);
    }
    for (int bad : {0, -1, 1001}) {
        Reset(3); g_settings.gradeCost[3] = bad; CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
        CHECK(Get<uint16_t>(Unit(6), kUnitLifeCost) == 500);
    }
    Reset(1); CHECK(AddAiPilot(room.data(), 0, 100, 1, 1)); CHECK(Get<uint16_t>(Unit(0), kUnitLifeCost) == 500);
    Reset(); failBuild = 100; CHECK(!AddAiPilot(room.data(), 6, 100, 2, 2)); CHECK(SlotPilotId(room.data(), 6) == 0);
    Reset(); failSetup = 100; CHECK(!AddAiPilot(room.data(), 6, 100, 2, 2)); CHECK(SlotPilotId(room.data(), 6) == 0);
    CHECK(std::all_of(Pilot(6), Pilot(6) + kPilotStride, [](uint8_t b) { return b == 0; }));

    SCENARIO("deck budget, unique units and setup failure");
    Reset(7); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    Put<uint16_t>(Pilot(6), kPilotPower, 321); Put<uint16_t>(Pilot(6), kPilotLifeCost, 654);
    BuildDeck(room.data(), 6, 100, npcIds, points);
    CHECK(Pilot(6)[kPilotTagCost] == 9 && Pilot(6)[kPilotTagUnits] >= 2 && Pilot(6)[kPilotTagUnits] <= 3);
    CHECK(Get<uint32_t>(Unit(6, 1), 0) != 100);
    CHECK(Get<uint16_t>(Pilot(6), kPilotPower) == 321 && Get<uint16_t>(Pilot(6), kPilotLifeCost) == 654);
    Reset(6); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2)); failSetup = 103;
    BuildDeck(room.data(), 6, 100, {100, 101, 102, 103, 101, 104}, {4, 3, 2, 5, 3, 0});
    CHECK(Pilot(6)[kPilotTagCost] == 9 && Pilot(6)[kPilotTagUnits] == 3 && Pilot(6)[kPilotForm] == 0);
    CHECK((std::set<uint32_t>{Get<uint32_t>(Unit(6, 1), 0), Get<uint32_t>(Unit(6, 2), 0)}) == (std::set<uint32_t>{101, 102}));

    SCENARIO("all eligible deck variants remain reachable across battles");
    std::set<uint32_t> laterUnits;
    for (unsigned seed = 1; seed <= 64; ++seed) {
        Reset(7); g_random = seed;
        CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
        BuildDeck(room.data(), 6, 100, npcIds, points);
        std::set<uint32_t> unique;
        int total = 0;
        for (int i = 0; i < Pilot(6)[kPilotTagUnits]; ++i) {
            auto id = Get<uint32_t>(Unit(6, i), 0);
            CHECK(unique.insert(id).second);
            total += Unit(6, i)[kUnitPoints];
            if (i) laterUnits.insert(id);
        }
        CHECK(total <= 10 && unique.size() <= 4);
    }
    CHECK(laterUnits.count(101) && laterUnits.count(102) && laterUnits.count(103));

    SCENARIO("team capacity and human preservation");
    Reset(); for (int i = 0; i < 5; ++i) Human(i, 1);
    auto before = std::vector<uint8_t>(Pilot(0), Pilot(0) + kPilotStride);
    CHECK(AddTeam(room.data(), 1, 100, {100}, {}) == 1);
    CHECK(SlotPilotId(room.data(), 5) & kAiPilotBit);
    CHECK(std::equal(before.begin(), before.end(), Pilot(0)));
    CHECK(SlotPilotId(room.data(), 6) == 0);

    SCENARIO("server gauges use the HUD's shared scale, including asymmetric teams");
    const int gauge[] = {0, 80, 80, 60, 80, 120, 160};
    for (int n = 1; n <= 6; ++n) {
        Reset(6); for (int i = 0; i < n; ++i) Human(i, 1);
        Human(6, 2); RecountBattle(room.data());
        CHECK(Get<int32_t>(room.data(), kRoomGauge1) == gauge[n]);
        CHECK(Get<int32_t>(room.data(), kRoomGauge2) == gauge[n]);
        CHECK(room[kRoomTeamSize1] == n && room[kRoomTeamSize2] == 1);
    }
    Reset(3); Human(0, 1); Human(1, 1); Human(6, 2); RecountBattle(room.data());
    CHECK(Get<int32_t>(room.data(), kRoomPower1) == 2000 && Get<int32_t>(room.data(), kRoomPower2) == 1000);
    Reset(7); Human(0, 1); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    Pilot(6)[kPilotTagCost] = 9; Pilot(6)[kPilotTagUnits] = 3;
    Put<int32_t>(room.data(), kRoomGauge1, 4); room[kRoomTagUnits1] = 1;
    RecountBattle(room.data());
    CHECK(Get<int32_t>(room.data(), kRoomGauge1) == 4 && Get<int32_t>(room.data(), kRoomGauge2) == 9);
    CHECK(room[kRoomTagUnits1] == 1 && room[kRoomTagUnits2] == 3);

    SCENARIO("only dead PvP AI respawns, unsuccessful native respawn stays dead");
    Reset(); Human(0, 1); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    RespawnAiPilots(room.data(), 99); CHECK(respawns == 0);
    Put<uint32_t>(Unit(6), kUnitHp, 0); revive = false;
    RespawnAiPilots(room.data(), 100); CHECK(respawns == 1 && refreshes == 0);
    revive = true; RespawnAiPilots(room.data(), 101); CHECK(respawns == 2 && refreshes == 1 && stateRespawns == 1);
    room[kRoomMode] = 11; Put<uint32_t>(Unit(6), kUnitHp, 0); RespawnAiPilots(room.data(), 102); CHECK(respawns == 2);

    SCENARIO("battle end removes AI, preserves humans and releases diagnostic state");
    room[kRoomMode] = 1; before.assign(Pilot(0), Pilot(0) + kPilotStride);
    FindDead(room.data(), SlotPilotId(room.data(), 6), true, 77);
    g_watch[0].room = room.data();
    BattleEnd(room.data(), nullptr, 0, 0, 100);
    CHECK(originals == 1 && SlotPilotId(room.data(), 6) == 0 && SlotPilotId(room.data(), 0) == 10);
    CHECK(std::equal(before.begin(), before.end(), Pilot(0)));
    CHECK(g_dead[0].room == nullptr && g_watch[0].room == nullptr);
    RemoveAiPilots(room.data()); CHECK(SlotPilotId(room.data(), 0) == 10);

    SCENARIO("battle-end observer preserves the real battle identity and publishes only after the native end returns");
    Reset(1);
    Human(0, 1);
    Put(room.data(), kRoomHost, 10u);
    Title("T7x !ai");
    CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    const SdgoPvpAiObserverSinkV1 sink = {
        sizeof(sink), SDGO_PVP_AI_OBSERVER_VERSION, &Observe, nullptr};
    CHECK(SdgoPvpAiRegisterObserverV1(&sink) == 1);
    InterlockedExchangePointer(&g_observerActiveRooms[0], room.data());
    clearIdentityAtEnd = true;
    BattleEnd(room.data(), nullptr, 0, 0, 100);
    CHECK(originals == 1);
    CHECK(observerEvents.size() == 1);
    if (observerEvents.size() == 1) {
        const auto& end = observerEvents.front();
        CHECK(end.type == SDGO_PVP_AI_EVENT_BATTLE_END);
        CHECK(end.room_cookie ==
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(room.data())));
        CHECK(end.host_pilot_id == 10);
        CHECK(end.mode == 1);
        CHECK(std::strcmp(end.title, "T7x !ai") == 0);
        CHECK(end.active_slots == 2);
        CHECK(end.ai_team_2 == 1);
        CHECK(end.slots[0].pilot_id == 10);
        CHECK(end.slots[6].pilot_id & kAiPilotBit);
        CHECK(end.sequence == 1);
        CHECK(end.qpc != 0);
        CHECK(end.thread_id == GetCurrentThreadId());
    }
    CHECK(Get<uint32_t>(room.data(), kRoomHost) == 0);
    CHECK(room[kRoomTitleLength] == 0);
    CHECK(SlotPilotId(room.data(), 6) == 0);
    CHECK(!ObserverRoomActive(room.data()));

    SCENARIO("expanded eligible pools are sampled afresh in both deck modes");
    auto originalIds = npcIds; auto originalPoints = points;
    for (int generation = 1; generation <= 2; ++generation)
        for (int level = 1; level <= 2; ++level)
            for (int i = 0; i < 60; ++i) {
                npcIds.push_back(static_cast<uint32_t>(1000 * generation + 100 * (level - 1) + i));
                points.push_back(static_cast<uint8_t>(level + 1));
            }
    std::set<uint32_t> expandedSeen;
    for (int mode : {6, 7}) {
        for (int level : {1, 2}) {
            for (int generation : {1, 2}) {
                std::vector<uint32_t> pool;
                for (int i = 0; i < 60; ++i)
                    pool.push_back(static_cast<uint32_t>(1000 * generation + 100 * (level - 1) + i));
                for (unsigned seed = 1; seed <= 24; ++seed) {
                    Reset(mode); g_random = seed;
                    CHECK(AddTeam(room.data(), 2, 3, pool, PoolPoints(room.data(), pool)) == 3);
                    for (int slot = 6; slot < 9; ++slot) {
                        std::set<uint32_t> unique;
                        int total = 0;
                        for (int i = 0; i < Pilot(slot)[kPilotTagUnits]; ++i) {
                            uint32_t id = Get<uint32_t>(Unit(slot, i), 0);
                            uint32_t first = static_cast<uint32_t>(1000 * generation + 100 * (level - 1));
                            CHECK(id >= first && id < first + 60);
                            CHECK(unique.insert(id).second);
                            total += Unit(slot, i)[kUnitPoints]; expandedSeen.insert(id);
                        }
                        CHECK(total <= 10 && unique.size() <= 4 && unique.size() >= 2);
                    }
                }
            }
        }
    }
    CHECK(expandedSeen.size() >= 160);
    npcIds = originalIds; points = originalPoints;

    SCENARIO("deck selection excludes zero-cost and over-budget first units");
    npcIds.push_back(105); points.push_back(11);
    for (int mode : {6, 7}) {
        for (unsigned seed = 1; seed <= 24; ++seed) {
            Reset(mode); g_random = seed;
            std::vector<uint32_t> candidate{104, 105, 100};
            CHECK(AddTeam(room.data(), 2, 1, candidate, PoolPoints(room.data(), candidate)) == 1);
            CHECK(Get<uint32_t>(Unit(6), 0) == 100);
            CHECK(Pilot(6)[kPilotTagCost] == 4);
        }
        Reset(mode);
        CHECK(AddTeam(room.data(), 2, 1, {104, 105}, {0, 11}) == 0);
    }
    SCENARIO("actual built points cannot exceed the remaining deck budget");
    npcIds.push_back(106); points.push_back(8);
    Reset(7); CHECK(AddAiPilot(room.data(), 6, 100, 2, 2));
    BuildDeck(room.data(), 6, 100, {100, 106}, {4, 1});
    CHECK(Pilot(6)[kPilotTagCost] == 4 && Pilot(6)[kPilotTagUnits] == 1);
    npcIds = originalIds; points = originalPoints;
    SCENARIO("concurrent rooms do not lose random-state updates");
    g_random = 1234567;
    std::vector<unsigned> draws(4096);
    std::vector<std::thread> threads;
    for (size_t worker = 0; worker < 4; ++worker)
        threads.emplace_back([&, worker] {
            for (size_t i = worker * 1024; i < (worker + 1) * 1024; ++i) draws[i] = NextRandom();
        });
    for (auto& thread : threads) thread.join();
    CHECK(std::set<unsigned>(draws.begin(), draws.end()).size() == draws.size());
    CHECK(std::find(draws.begin(), draws.end(), 0u) == draws.end());
    DeleteCriticalSection(&g_itemsLock);
    return ai_test::Finish();
}
