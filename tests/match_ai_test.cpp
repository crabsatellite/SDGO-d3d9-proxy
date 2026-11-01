#include "ai_test_support.h"
#include "../mods/match_ai/match_ai.cpp"
#include <algorithm>
#include <array>
#include <limits>

namespace fixture {
using ai_test::Put;
using ai_test::Get;
std::array<uint8_t, 0x100> room{}, service{};
std::array<uint8_t, 0x500> mode{}, app{};
std::array<uint8_t, 0x400> camera{};
std::array<uint8_t, 0x40> hud{}, start{};
std::array<uint8_t, 12 * kRecordSize> humans{}, ais{};
std::array<std::array<uint8_t, 0x3400>, 12> units{};
std::array<std::array<uint8_t, 0xA0>, 12> nodes{};
std::array<std::array<uint8_t, 0x200>, 12> npcs{};
std::array<std::array<uint8_t, 0x70>, 12> slots{};
std::array<std::array<uint8_t, 4 * kUnitDeckEntry>, 12> decks{};
std::array<void*, 32> vtable{}, hudVtable{};
std::array<uint8_t, 0xCD4> packet{};
int manager = 0;
void* um = &manager;
bool network = true, observer = false, failAppend = false, throwCamera = false, throwUdp = false;
int created = 0, playerCount = 0, enabled = 0, calculated = 0, hudGauge = 0, hudCalls = 0;
int switchCalls = 0, switchedForm = -1, udpCalls = 0, natCalls = 0, originalPlayback = 0;
int slotColourIndex = -1, slotColourValue = -1, originalLookups = 0, nativeSlotCalls = 0;
float seenYaw = 0, seenPitch = 0;
std::vector<std::pair<int, int>> starts;
std::vector<void*> allocations;
int allocationBudget = 12;
void* inboundAllocation = nullptr;
LONG sequenceInsideInbound = -1;
int __fastcall Inbound(void*, void*, int, int, int, int, void*) {
    sequenceInsideInbound = InterlockedCompareExchange(&g_createReplySequence, 0, 0);
    if (inboundAllocation) {
        CHECK(VirtualFree(inboundAllocation, 0, MEM_RELEASE) != 0);
        inboundAllocation = nullptr;
    }
    return 73;
}

uint8_t* __cdecl App() { return app.data(); }
bool __cdecl Network() { return network; }
bool __fastcall Observer(void*, void*) { return observer; }
uint8_t* __fastcall Start(void*, void*, int colour, int index) { starts.emplace_back(colour, index); return start.data(); }
uint8_t* __fastcall Create(uint8_t*, void*, uint32_t npc, float, float, float, int, float, float, float) {
    if (created >= 12) return nullptr;
    auto unit = units[created].data();
    Put(unit, kUnitNpcData, npcs[created].data());
    Put(unit, kUnitNode, nodes[created].data());
    Put(unit, kUnitDeckBegin, decks[created].data());
    Put(unit, kUnitDeckEnd, decks[created].data() + kUnitDeckEntry);
    Put(unit, kUnitObjectId, 1001u + created);
    Put(unit, kUnitNpcDataId, npc);
    ++created;
    return unit;
}
void __fastcall AddObject(void*, void*, uint8_t*, uint32_t, int) {}
void __fastcall SetInfo(void*, void*, int slot, int, uint32_t object, uint32_t, uint8_t* record) {
    CHECK(slot >= 0 && slot < 12);
    if (slot < 0 || slot >= 12) return;
    Put(slots[slot].data(), kSlotObjectId, object);
    Put(slots[slot].data(), kSlotRecord, record);
}
uint8_t* __fastcall Slot(void*, void*, int slot) {
    ++nativeSlotCalls;
    CHECK(slot >= 0 && slot < 12);
    return slot >= 0 && slot < 12 ? slots[slot].data() : nullptr;
}
void __fastcall Assign(void*, void*, const wchar_t*, size_t) {}
void __fastcall AddAi(void*, void*, uint8_t*) {}
void __fastcall Calculate(void*, void*) { ++calculated; }
void __fastcall Count(void*, void*, int count) { playerCount = count; }
void __fastcall Enable(uint8_t*, void*) { ++enabled; }
void __fastcall Gauge(uint8_t*, void*, int value) { hudGauge = value; ++hudCalls; }
void* __fastcall Find(void*, void*, uint32_t id) {
    for (int i = 0; i < created; ++i)
        if (Get<uint32_t>(units[i].data(), kUnitObjectId) == id) return units[i].data();
    return nullptr;
}
uint32_t __fastcall Lookup(void*, void*, uint32_t) { ++originalLookups; return 42; }
bool __fastcall IsPlayer(void*, void*, uint32_t id) { return id >= 1 && id <= 12; }
void* __fastcall FindHuman(void*, void*, uint32_t) { return nullptr; }
void __fastcall Colour(void*, void*, int index, int colour) { slotColourIndex = index; slotColourValue = colour; }
void* __cdecl Grid() { return &manager; }
int __fastcall GridFind(void*, void*, const uint8_t** record, uint32_t id, int, char) {
    if (id < 100 || id >= 112) return 0;
    *record = npcs[id - 100].data(); return 1;
}
void __fastcall Append(uint8_t* unit, void*, uint32_t) {
    if (!failAppend) Put(unit, kUnitDeckEnd, Get<uint8_t*>(unit, kUnitDeckEnd) + kUnitDeckEntry);
}
void __fastcall Switch(uint8_t*, void*, int form, char) { ++switchCalls; switchedForm = form; }
int __fastcall Playback(void*, void*, uint8_t*) { ++originalPlayback; return 17; }
void __fastcall Udp(uint8_t* wait, void*) {
    ++udpCalls;
    CHECK(Get<uint32_t>(wait, kWaitRoomSlots) == 7);
    CHECK(Get<uint32_t>(wait, kWaitRoomSlots + kWaitRoomSlotStride) == 0);
    if (throwUdp) RaiseException(0xE0000002, 0, 0, nullptr);
}
bool CallUdp(uint8_t* wait) {
    __try { UdpSetup(wait, nullptr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __fastcall Nat(uint8_t*, void*) { ++natCalls; }
void* __cdecl Allocate(size_t size) {
    if (allocationBudget-- <= 0) return nullptr;
    void* p = std::malloc(size); allocations.push_back(p); return p;
}
void __fastcall Camera(uint8_t* cam, void*) {
    auto target = Get<uint8_t*>(cam, kCameraTarget);
    seenYaw = Get<float>(target, kUnitAimYaw); seenPitch = Get<float>(target, kUnitAimPitch);
    if (throwCamera) RaiseException(0xE0000001, 0, 0, nullptr);
}
bool CallCamera() {
    __try { TrackingUpdate(camera.data(), nullptr); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Records(int humanCount, int aiCount) {
    Put(room.data(), kRoomHumansBegin, humans.data());
    Put(room.data(), kRoomHumansEnd, humans.data() + humanCount * kRecordSize);
    Put(room.data(), kRoomAiBegin, ais.data());
    Put(room.data(), kRoomAiEnd, ais.data() + aiCount * kRecordSize);
    for (int i = 0; i < humanCount; ++i) {
        auto rec = humans.data() + i * kRecordSize;
        Put(rec, kRecordServerId, 7u + i); rec[kRecordTeam] = 1; rec[kRecordColour] = 2;
    }
    for (int i = 0; i < aiCount; ++i) {
        auto rec = ais.data() + i * kRecordSize;
        Put(rec, kRecordServerId, kAiPilotBit | (100u + i));
        Put(rec, kRecordNpcData, 100u + i);
        rec[kRecordTeam] = 2; rec[kRecordColour] = 1;
    }
    Put(room.data(), kRoomLocalPlayer, 7u);
    humans[kRecordMaster] = 1;
}
void Reset(int classIndex = 0, int roomMode = 1) {
    for (void* p : allocations) std::free(p);
    allocations.clear(); allocationBudget = 12;
    room.fill(0); service.fill(0); mode.fill(0); app.fill(0); camera.fill(0); hud.fill(0);
    humans.fill(0); ais.fill(0); packet.fill(0); units = {}; slots = {}; decks = {}; nodes = {}; npcs = {};
    for (int i = 0; i < 12; ++i) { Put<int32_t>(npcs[i].data(), kNpcPoints, 4); Put<uint32_t>(npcs[i].data(), kNpcUnitDataId, 2000u + i); }
    vtable.fill(nullptr); hudVtable.fill(nullptr);
    vtable[kModeCreateNpcSlot] = reinterpret_cast<void*>(&Create);
    hudVtable[kInterfaceSetGaugeSlot] = reinterpret_cast<void*>(&Gauge);
    Put(mode.data(), 0, vtable.data()); Put(mode.data(), kModeRoom, room.data());
    Put(mode.data(), kModeInterface, hud.data()); Put(hud.data(), 0, hudVtable.data());
    Put(service.data(), kServiceMode, mode.data()); Put(app.data(), kAppGameMain, service.data());
    Put<uint16_t>(room.data(), kModeRoomMode, static_cast<uint16_t>(roomMode));
    std::fill(std::begin(g_vtables), std::end(g_vtables), nullptr); g_vtables[classIndex] = vtable.data();
    g_api = &ai_test::api; g_gameApp = &App; g_game.unitManager = &um;
    g_game.isNetwork = &Network; g_game.isObserver = reinterpret_cast<IsObserverFn>(&Observer);
    g_game.findStartPoint = reinterpret_cast<FindStartPointFn>(&Start);
    g_game.baseCreateNpc = reinterpret_cast<CreateNpcFn>(&Create);
    g_game.setPlayerInfo = reinterpret_cast<SetPlayerInfoFn>(&SetInfo);
    g_game.playerSlot = reinterpret_cast<PlayerSlotFn>(&Slot);
    g_game.assign = reinterpret_cast<WStringAssignFn>(&Assign);
    g_game.addAiUnit = reinterpret_cast<AddAiUnitFn>(&AddAi);
    g_game.addObject = reinterpret_cast<AddObjectFn>(&AddObject);
    g_game.calculateTeam = reinterpret_cast<CalculateTeamFn>(&Calculate);
    g_game.enableAi = reinterpret_cast<EnableAiFn>(&Enable);
    g_setPlayerCount = reinterpret_cast<SetPlayerCountFn>(&Count);
    g_originalPlayerSlot = reinterpret_cast<PlayerSlotFn>(&Slot);
    g_findObject = reinterpret_cast<FindObjectFn>(&Find);
    g_originalObjectIdFromPilot = reinterpret_cast<ObjectIdFromPilotFn>(&Lookup);
    g_originalIsPlayerObject = reinterpret_cast<IsPlayerObjectFn>(&IsPlayer);
    g_originalFindPlayer = reinterpret_cast<FindPlayerFn>(&FindHuman);
    g_originalSetSlotColour = reinterpret_cast<SetSlotColourFn>(&Colour);
    g_gridData = &Grid; g_gridFind = reinterpret_cast<GridFindFn>(&GridFind);
    g_addDeckUnit = reinterpret_cast<AddDeckUnitFn>(&Append); g_switchDeck = reinterpret_cast<SwitchDeckFn>(&Switch);
    g_originalPlaybackUnit = reinterpret_cast<PlaybackUnitFn>(&Playback);
    g_originalUdpSetup = reinterpret_cast<UdpSetupFn>(&Udp); g_originalNatCheck = reinterpret_cast<NatCheckFn>(&Nat);
    g_gameNew = &Allocate; g_originalTrackingUpdate = reinterpret_cast<CameraUpdateFn>(&Camera);
    g_botTable.clear(); g_botMode = nullptr; g_cameraAim = {}; g_soloAiMode = nullptr;
    std::memset(g_startDecks, 0, sizeof(g_startDecks));
    created = playerCount = enabled = calculated = hudCalls = hudGauge = 0;
    switchCalls = udpCalls = natCalls = originalPlayback = originalLookups = nativeSlotCalls = 0;
    switchedForm = slotColourIndex = slotColourValue = -1;
    g_aiEnabled = false; g_logDiagnostics = false; g_viewStateLogs = g_deadViewLogs = 0;
    network = true; observer = failAppend = throwCamera = throwUdp = false; starts.clear();
}
void StartEntry(int slot, uint32_t id, int count, std::initializer_list<uint32_t> ids) {
    auto entry = packet.data() + kStartPacketSlots + slot * kStartSlotStride;
    Put(entry, 0, id); entry[kStartSlotUnits] = static_cast<uint8_t>(count); entry[kStartSlotPoints] = 9;
    int i = 0; for (auto npc : ids) {
        auto block = entry + kStartSlotUnitBlocks + i * kStartUnitBlock;
        Put(block, 0, npc); Put<uint16_t>(block, kStartUnitLifeCost, static_cast<uint16_t>(455 + i)); ++i;
    }
}
void Spawn(int classIndex = 0, int roomMode = 1, int humanCount = 1, int aiCount = 2) {
    Reset(classIndex, roomMode); Records(humanCount, aiCount); SpawnAiPlayers(mode.data(), "test");
}
bool Near(float a, float b) { return std::fabs(a - b) < 0.0001f; }
}

int main() {
    using namespace fixture;
    SCENARIO("master, remote, slot cap and AI enable once");
    Spawn(); CHECK(g_botTable.size() == 2 && playerCount == 3 && calculated == 1);
    CHECK(g_soloAiMode == mode.data());
    CHECK(units[0][kUnitRemote] == 0 && units[0][kUnitLocalControl] == 1 && units[0][kUnitMasterNpc] == 1);
    CHECK(starts[0] == std::make_pair(1, 0) && starts[1] == std::make_pair(1, 1));
    EnableBots(service.data()); EnableBots(service.data()); CHECK(enabled == 2);
    Reset(); Records(2, 1); humans[kRecordMaster] = 0; SpawnAiPlayers(mode.data(), "remote");
    CHECK(units[0][kUnitRemote] == 1 && units[0][kUnitLocalControl] == 0);
    Spawn(0, 1, 11, 3); CHECK(g_botTable.size() == 1 && playerCount == 12);
    Reset(); Records(1, 2); network = false; SpawnAiPlayers(mode.data(), "offline"); CHECK(created == 0);
    Reset(); Records(1, 0); SpawnAiPlayers(mode.data(), "none"); CHECK(g_botTable.empty());

    SCENARIO("record and deck ranges reject partial records and impossible counts");
    Reset(); Records(1, 0);
    Put(room.data(), kRoomHumansEnd, humans.data() + kRecordSize + 1);
    CHECK(RecordCount(room.data(), kRoomHumansBegin, kRoomHumansEnd) == 0);
    Put(room.data(), kRoomHumansEnd, humans.data() + humans.size());
    CHECK(RecordCount(room.data(), kRoomHumansBegin, kRoomHumansEnd) == 12);
    Put(room.data(), kRoomHumansEnd, reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(humans.data()) + 13 * kRecordSize));
    CHECK(RecordCount(room.data(), kRoomHumansBegin, kRoomHumansEnd) == 0);
    Put(units[0].data(), kUnitDeckBegin, decks[0].data());
    Put(units[0].data(), kUnitDeckEnd, decks[0].data() + kUnitDeckEntry + 1);
    CHECK(DeckSize(units[0].data()) == 0);

    SCENARIO("identity mappings survive death but reject reused slots");
    Spawn(); auto bot = g_botTable.front();
    CHECK(ObjectIdFromPilot(um, nullptr, bot.pilotId) == bot.objectId);
    CHECK(IsPlayerObject(um, nullptr, bot.objectId));
    CHECK(PlayerSlot(um, nullptr, static_cast<int>(bot.objectId - 1)) == slots[bot.slot].data());
    CHECK(FindPlayer(um, nullptr, bot.objectId) == bot.unit);
    Put<uint32_t>(slots[bot.slot].data(), kSlotObjectId, 9);
    CHECK(BotByObject(um, bot.objectId) == nullptr);
    CHECK(ObjectIdFromPilot(um, nullptr, bot.pilotId) == 42 && originalLookups == 1);
    const int slotCalls = nativeSlotCalls;
    CHECK(PlayerSlot(um, nullptr, 9999) == g_scratchSlot);
    CHECK(nativeSlotCalls <= slotCalls + static_cast<int>(g_botTable.size()));
    SetSlotColour(um, nullptr, 9999, 2); CHECK(slotColourIndex == -1);

    SCENARIO("grid HUD gauge includes human and AI teams");
    const int expected[] = {0, 80, 80, 60, 80, 120, 160};
    for (int n = 1; n <= 6; ++n) {
        Reset(2, 6); Records(1, n); SetGridGauge(mode.data());
        CHECK(Get<int32_t>(mode.data(), kModeGridGauge) == expected[n]);
        CHECK(hudGauge == expected[n] && hudCalls == 1);
    }
    Reset(2, 6); Records(1, 1); Put<uint8_t*>(mode.data(), kModeInterface, nullptr);
    SetGridGauge(mode.data()); CHECK(Get<int32_t>(mode.data(), kModeGridGauge) == 80 && hudCalls == 0);

    SCENARIO("slot scores and tag divisor");
    for (int index : {1, 2, 4}) {
        Spawn(index, index == 4 ? 7 : 1); int score = Get<int32_t>(slots[1].data(), kSlotScore);
        CHECK(score == (index == 1 ? 1000 : index == 2 ? 1 : 400));
        if (index == 4) CHECK(Get<int32_t>(ais.data(), kRecordTagCost) == 4);
    }
    SCENARIO("tag score always has a positive divisor and cannot overflow");
    Reset(4, 7); Put(slots[0].data(), kSlotRecordCopy, ais.data());
    InitAiSlot(4, slots[0].data(), units[0].data());
    CHECK(Get<int32_t>(ais.data(), kRecordTagCost) > 0);
    CHECK(Get<int32_t>(slots[0].data(), kSlotScore) > 0);
    Put<int32_t>(ais.data(), kRecordTagCost, 0);
    Put(units[0].data(), kUnitNpcData, npcs[0].data());
    Put<int32_t>(npcs[0].data(), kNpcPoints, INT32_MAX);
    InitAiSlot(4, slots[0].data(), units[0].data());
    CHECK(Get<int32_t>(slots[0].data(), kSlotScore) == 1000);

    SCENARIO("battle start packet replaces prior decks and catches unreadable input");
    Reset(); StartEntry(2, kAiPilotBit | 100, 3, {100, 101, 102}); ReadStartDecks(packet.data());
    const StartDeck* deck = FindStartDeck(kAiPilotBit | 100);
    CHECK(deck && deck->units == 3 && deck->npc[2] == 102 && deck->lifeCost[2] == 457);
    CHECK(!FindStartDeck(7)); packet.fill(0); ReadStartDecks(packet.data()); CHECK(!FindStartDeck(kAiPilotBit | 100));
    CHECK(!GuardedReadStartDecks(nullptr)); CHECK(!FindStartDeck(kAiPilotBit | 100));

    SCENARIO("deck respects declared unit count, not unused packet blocks");
    Spawn(4, 7, 1, 1); StartEntry(0, g_botTable[0].pilotId, 2, {100, 101, 102, 103}); ReadStartDecks(packet.data());
    BuildAiDeck(g_botTable[0], 100); CHECK(g_botTable[0].units == 2 && DeckSize(g_botTable[0].unit) == 2);

    SCENARIO("missing NPC does not compact server deck indices");
    Spawn(4, 7, 1, 1); StartEntry(0, g_botTable[0].pilotId, 3, {100, 99999, 102}); ReadStartDecks(packet.data());
    BuildAiDeck(g_botTable[0], 100); CHECK(g_botTable[0].units == 1 && DeckSize(g_botTable[0].unit) == 1);

    SCENARIO("failed native deck append does not invent metadata");
    Spawn(4, 7, 1, 1); StartEntry(0, g_botTable[0].pilotId, 2, {100, 101}); ReadStartDecks(packet.data());
    failAppend = true; BuildAiDeck(g_botTable[0], 100); CHECK(g_botTable[0].units == 1);

    SCENARIO("respawn sets NPC before native playback and switches deck afterwards");
    Spawn(4, 7, 1, 1); StartEntry(0, g_botTable[0].pilotId, 3, {100, 101, 102}); ReadStartDecks(packet.data());
    BuildAiDeck(g_botTable[0], 100);
    auto& b = g_botTable[0]; SetSlotColour(um, nullptr, static_cast<int>(b.objectId - 1), 2);
    CHECK(b.pendingForm == 2 && b.form == 0 && Get<uint32_t>(b.unit, kUnitNpcDataId) == 102);
    CHECK(slotColourIndex == b.slot && slotColourValue == 2);
    CHECK(PlaybackUnit(nullptr, nullptr, nullptr) == 17 && originalPlayback == 1);
    CHECK(b.form == 2 && b.pendingForm == -1 && switchCalls == 1 && switchedForm == 2);
    CHECK(AiDeathCost(b.unit, 606) == 457);
    GuardedSwitchDecks(); CHECK(switchCalls == 1);
    SetSlotColour(um, nullptr, static_cast<int>(b.objectId - 1), 4); CHECK(b.pendingForm == -1);
    Put<uint8_t*>(service.data(), kServiceMode, nullptr); CHECK(AiDeathCost(b.unit, 606) == 606);

    SCENARIO("UDP temporarily hides bots; solo NAT exception is scoped");
    Spawn(); std::vector<uint8_t> wait(0xA00 + 12 * kWaitRoomSlotStride);
    Put<uint32_t>(wait.data(), kWaitRoomSlots, 7);
    Put<uint32_t>(wait.data(), kWaitRoomSlots + kWaitRoomSlotStride, kAiPilotBit | 100);
    UdpSetup(wait.data(), nullptr); CHECK(udpCalls == 1);
    CHECK(Get<uint32_t>(wait.data(), kWaitRoomSlots + kWaitRoomSlotStride) == (kAiPilotBit | 100));
    throwUdp = true; CHECK(!CallUdp(wait.data()));
    CHECK(Get<uint32_t>(wait.data(), kWaitRoomSlots + kWaitRoomSlotStride) == (kAiPilotBit | 100));
    NatCheck(service.data(), nullptr); CHECK(natCalls == 0);
    g_soloAiMode = nullptr; NatCheck(service.data(), nullptr); CHECK(natCalls == 1);

    SCENARIO("dead-view linked list remains valid and duplicate-free");
    Spawn(); ListNode head{nullptr, nullptr, nullptr}; head.next = head.prev = &head; UnitList list{&head, 0};
    AddAiToDeadView(&list); CHECK(list.size == 2);
    CHECK(head.next->prev == &head && head.prev->next == &head && head.next->next == head.prev);
    AddAiToDeadView(&list); CHECK(list.size == 2);
    Reset(); Records(1, 2); SpawnAiPlayers(mode.data(), "list"); allocationBudget = 1;
    head.next = head.prev = &head; list = {&head, 0}; AddAiToDeadView(&list);
    CHECK(list.size == 1 && head.next == head.prev && head.next->next == &head);

    SCENARIO("camera yaw, height, clamping, easing and restoring actual unit aim");
    Spawn(); auto u = units[0].data();
    Put<uint32_t>(u, kUnitLockTarget, 1002); Put<float>(u, kUnitAimYaw, 2); Put<float>(u, kUnitAimPitch, 0.4f);
    Put<float>(nodes[1].data(), kNodePosition, 10); Put(camera.data(), kCameraTarget, u);
    TrackingUpdate(camera.data(), nullptr); CHECK(Near(seenYaw, kPi * 0.5f) && Near(seenPitch, 0));
    CHECK(Near(Get<float>(u, kUnitAimYaw), 2) && Near(Get<float>(u, kUnitAimPitch), 0.4f));
    Put<float>(nodes[1].data(), kNodePosition + 4, 10000); g_cameraAim = {};
    TrackingUpdate(camera.data(), nullptr); CHECK(Near(seenPitch, -0.87f));
    throwCamera = true; CHECK(!CallCamera()); CHECK(Near(Get<float>(u, kUnitAimYaw), 2) && Near(Get<float>(u, kUnitAimPitch), 0.4f));
    CHECK(Near(AngleDelta(-3.0f, 3.0f), 2 * kPi - 6.0f));
    CHECK(Near(AngleDelta(3.0f, -3.0f), 6.0f - 2 * kPi));
    CHECK(std::isfinite(AngleDelta(std::numeric_limits<float>::quiet_NaN(), 0)));
    CHECK(AngleDelta(std::numeric_limits<float>::infinity(), 0) == 0);
    CHECK(std::fabs(AngleDelta(100000000.0f, 0)) <= kPi);
    CHECK(std::fabs(AngleDelta(std::numeric_limits<float>::max(), -std::numeric_limits<float>::max())) <= kPi);
    SCENARIO("room rejection observation reads only the native header and preserves dispatch ownership");
    g_originalServicePacket = reinterpret_cast<PacketDispatchFn>(&Inbound);
    InterlockedExchange(&g_createReplyVersion, 0); InterlockedExchange(&g_createReplySequence, 0);
    SYSTEM_INFO system{}; GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    inboundAllocation = VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(inboundAllocation != nullptr);
    DWORD previousProtection = 0;
    CHECK(VirtualProtect(static_cast<uint8_t*>(inboundAllocation) + page, page, PAGE_NOACCESS, &previousProtection) != 0);
    uint8_t* rejection = static_cast<uint8_t*>(inboundAllocation) + page - 8;
    Put<uint16_t>(rejection, 6, 0x0BC2);
    CHECK(ServicePacket(nullptr, nullptr, 0, 0, 0, 0x0A0A, rejection) == 73);
    CHECK(sequenceInsideInbound == 0 && g_createReplySequence == 1);
    CHECK((static_cast<uint32_t>(g_createReplyDetails) >> 16) == 0x0BC2 && g_createReplyRoom == 0);
    inboundAllocation = VirtualAlloc(nullptr, page * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(inboundAllocation != nullptr);
    CHECK(VirtualProtect(static_cast<uint8_t*>(inboundAllocation) + page, page, PAGE_NOACCESS, &previousProtection) != 0);
    uint8_t* accepted = static_cast<uint8_t*>(inboundAllocation) + page - 10;
    Put<uint32_t>(accepted, 6, 37);
    CHECK(ServicePacket(nullptr, nullptr, 0, 0, 0, 0x0918, accepted) == 73);
    CHECK(sequenceInsideInbound == 1 && g_createReplySequence == 2 && g_createReplyRoom == 37);
    CHECK(static_cast<uint32_t>(g_createReplyDetails) == 0x0918);
    CHECK(ServicePacket(nullptr, nullptr, 0, 0, 0, 0x1234, nullptr) == 73);
    CHECK(g_createReplySequence == 2);
    for (void* p : allocations) std::free(p); allocations.clear();
    return ai_test::Finish();
}
