#include <windows.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "sdgo/sdgo_api.h"

static_assert(sizeof(void*) == 4,
              "the native battle reward runtime fixture must be built for x86");

namespace {
namespace fs = std::filesystem;

constexpr size_t kRoomSize = 0x33000;
constexpr size_t kRoomMode = 0x05;
constexpr size_t kRoomMission = 0x1B;
constexpr size_t kRoomPilotBase = 0x533;
constexpr size_t kRoomPilotStride = 0x48E;
constexpr size_t kRoomPilotAccount = 0x429;
constexpr size_t kRoomPilotTeam = 0x42D;
constexpr size_t kRoomPilotAgent = 0x476;
constexpr size_t kBattlePilotBase = 0x3CE4;
constexpr size_t kBattlePilotStride = 0x1F54;
constexpr size_t kBattlePilotEntity = 0x00;
constexpr size_t kBattlePilotTeam = 0x04;
constexpr size_t kBattlePilotKills = 0x12;
constexpr size_t kBattlePilotState = 0x22;
constexpr size_t kBattlePilotNpcData = 0x1CEC;
constexpr size_t kBattlePilotOutcome = 0x1D28;
constexpr size_t kRoomStartedAt = 0x2EEB4;
constexpr size_t kRoomMissionState = 0x316A4;
constexpr size_t kRoomMissionDifficulty = 0x316F0;

struct FrameContract {
    ptrdiff_t room;
    ptrdiff_t duration;
    ptrdiff_t room_pilot;
    ptrdiff_t battle_pilot;
    ptrdiff_t physical_slot;
    ptrdiff_t rank;
    ptrdiff_t eligibility;
    ptrdiff_t mission_outcomes;
};

constexpr std::array<FrameContract, 10> kFrames = {{
    {-0xB48, -0x880, -0x244, -0x7E0, -0x88C, -0x914, -0x7B8, 0},
    {-0xBBC, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {-0xBBC, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {-0xBBC, -0x840, -0x23C, -0x7C8, -0x8FC, -0x984, -0x7A0, 0},
    {-0xBD0, -0x850, -0x23C, -0x7CC, -0x90C, -0x994, -0x7A4, 0},
    {-0xBD0, -0x840, -0x234, -0x7C8, -0x8FC, -0x990, -0x7A0, 0},
    {-0xBD0, -0x840, -0x234, -0x7C8, -0x8FC, -0x990, -0x7A0, 0},
    {-0xBC0, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {-0xBC8, -0x838, -0x234, -0x7BC, -0x8FC, -0x984, -0x798, 0},
    {-0xB58, -0x844, -0x234, -0x7C8, -0x854, -0x8D8, -0x7A0, -0x828},
}};

// Independent copies of the shipped daily-reward settlement fields:
// pilot EXP, G, unit EXP, then their legacy event buckets.
constexpr std::array<std::array<ptrdiff_t, 6>, 10> kDailyFields = {{
    {{-0x770, -0x28, -0x89C, -0x18, -0x87C, -0x844}},
    {{-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
    {{-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
    {{-0x760, -0x24, -0x908, -0x14, -0x83C, -0x834}},
    {{-0x764, -0x24, -0x918, -0x14, -0x844, -0x83C}},
    {{-0x760, -0x20, -0x90C, -0x10, -0x83C, -0x834}},
    {{-0x760, -0x20, -0x90C, -0x10, -0x83C, -0x834}},
    {{-0x758, -0x20, -0x904, -0x10, -0x834, -0x82C}},
    {{-0x758, -0x20, -0x90C, -0x10, -0x834, -0x82C}},
    {{-0x760, -0x20, -0x860, -0x10, -0x840, -0x834}},
}};

// Independently traced EBP sources for packet +1E, +24, +2A and +30.
// Packet +36 is the event bucket in kDailyFields. Only these GB rows change.
constexpr std::array<std::array<ptrdiff_t, 4>, 10> kGDisplayFields = {{
    {{-0x758, -0x10, -0x76C, -0x744}},
    {{-0x744, -0x08, -0x754, -0x734}},
    {{-0x744, -0x08, -0x754, -0x734}},
    {{-0x74C, -0x08, -0x75C, -0x73C}},
    {{-0x74C, -0x08, -0x760, -0x73C}},
    {{-0x74C, -0x08, -0x75C, -0x73C}},
    {{-0x74C, -0x08, -0x75C, -0x73C}},
    {{-0x744, -0x08, -0x754, -0x734}},
    {{-0x744, -0x08, -0x754, -0x734}},
    {{-0x74C, -0x08, -0x75C, -0x73C}},
}};

constexpr std::array<const char*, 10> kAugmentSymbols = {{
    "battle.rewards.augment.personal",
    "battle.rewards.augment.team",
    "battle.rewards.augment.leader",
    "battle.rewards.augment.ai",
    "battle.rewards.augment.ai_ship",
    "battle.rewards.augment.mode13",
    "battle.rewards.augment.mode14",
    "battle.rewards.augment.tag",
    "battle.rewards.augment.death_grid",
    "battle.rewards.augment.mission",
}};

constexpr std::array<const char*, 10> kBattleLayoutSymbols = {{
    "battle.rewards.layout.personal",
    "battle.rewards.layout.team",
    "battle.rewards.layout.leader",
    "battle.rewards.layout.ai",
    "battle.rewards.layout.ai_ship",
    "battle.rewards.layout.mode13",
    "battle.rewards.layout.mode14",
    "battle.rewards.layout.tag",
    "battle.rewards.layout.death_grid",
    "battle.rewards.layout.mission",
}};

constexpr std::array<const char*, 10> kDailyFinalSymbols = {{
    "battle.reward.final.personal",
    "battle.reward.final.team",
    "battle.reward.final.leader",
    "battle.reward.final.ai",
    "battle.reward.final.ai_ship",
    "battle.reward.final.mode13",
    "battle.reward.final.mode14",
    "battle.reward.final.tag",
    "battle.reward.final.death_grid",
    "battle.reward.final.mission",
}};

constexpr std::array<const char*, 10> kDailyLayoutSymbols = {{
    "battle.reward.layout.personal",
    "battle.reward.layout.team",
    "battle.reward.layout.leader",
    "battle.reward.layout.ai",
    "battle.reward.layout.ai_ship",
    "battle.reward.layout.mode13",
    "battle.reward.layout.mode14",
    "battle.reward.layout.tag",
    "battle.reward.layout.death_grid",
    "battle.reward.layout.mission",
}};

#define SDGO_FIXTURE_SITE(N)                                                   \
    extern "C" __declspec(noinline) __declspec(naked) void FixtureSite##N() { \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { nop }                                                          \
        __asm { mov eax, N }                                                   \
        __asm { ret }                                                          \
    }
SDGO_FIXTURE_SITE(0)
SDGO_FIXTURE_SITE(1)
SDGO_FIXTURE_SITE(2)
SDGO_FIXTURE_SITE(3)
SDGO_FIXTURE_SITE(4)
SDGO_FIXTURE_SITE(5)
SDGO_FIXTURE_SITE(6)
SDGO_FIXTURE_SITE(7)
SDGO_FIXTURE_SITE(8)
SDGO_FIXTURE_SITE(9)
#undef SDGO_FIXTURE_SITE

extern "C" __declspec(noinline) __declspec(naked) void FixtureDamageTarget() {
    __asm { mov eax, 0C0FFEE01h }
    __asm { ret 14h }
}

using SiteFn = void(__cdecl*)();
using DamageFn = uint32_t(__thiscall*)(
    void*, void*, void*, void*, uint32_t, int32_t);

const std::array<SiteFn, 10> kSiteFunctions = {{
    &FixtureSite0, &FixtureSite1, &FixtureSite2, &FixtureSite3, &FixtureSite4,
    &FixtureSite5, &FixtureSite6, &FixtureSite7, &FixtureSite8, &FixtureSite9,
}};

__declspec(naked) void __cdecl InvokeSite(void*, BYTE*) {
    __asm { push ebp }
    __asm { mov ebp, [esp + 0Ch] }
    __asm { call dword ptr [esp + 08h] }
    __asm { pop ebp }
    __asm { ret }
}

struct PacketRecord {
    uint32_t target = 0;
    uint32_t account = 0;
    uint32_t current_mb = 0;
};

std::array<BYTE, 0x200> g_net_frame{};
void* g_net_frame_proc = g_net_frame.data();
std::mutex g_packet_mutex;
std::vector<PacketRecord> g_packets;
std::array<BYTE, 20> g_layout_guards{};
int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL line " << line << ": " << expression << "\n";
    }
}

#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

template <typename T>
void Put(BYTE* base, ptrdiff_t offset, const T& value) {
    std::memcpy(base + offset, &value, sizeof(value));
}

template <typename T>
T Get(const BYTE* base, ptrdiff_t offset) {
    T value{};
    std::memcpy(&value, base + offset, sizeof(value));
    return value;
}

extern "C" __declspec(noinline) int __fastcall FixtureNetSend(
    void* self, void*, uint32_t target, const void* packet, int length,
    int flags) {
    if (self != g_net_frame.data() || !packet || length != 10 || flags != 0 ||
        target == 0) {
        return -1;
    }
    const BYTE* bytes = static_cast<const BYTE*>(packet);
    uint16_t packet_id = 0;
    uint32_t account = 0;
    uint32_t current_mb = 0;
    std::memcpy(&packet_id, bytes, sizeof(packet_id));
    std::memcpy(&account, bytes + 2, sizeof(account));
    std::memcpy(&current_mb, bytes + 6, sizeof(current_mb));
    if (packet_id != 0x064B || account == 0) return -1;
    std::lock_guard<std::mutex> lock(g_packet_mutex);
    g_packets.push_back({target, account, current_mb});
    return 0;
}

struct MatchFixture {
    std::vector<BYTE> room = std::vector<BYTE>(kRoomSize);
    uint32_t base_account = 0;

    BYTE* RoomPilot(unsigned slot) {
        return room.data() + kRoomPilotBase + slot * kRoomPilotStride;
    }
    BYTE* BattlePilot(unsigned slot) {
        return room.data() + kBattlePilotBase + slot * kBattlePilotStride;
    }
};

struct FrameFixture {
    std::array<BYTE, 0xD00> storage{};
    BYTE* Frame() { return storage.data() + 0xC80; }
};

std::string HexBytes(const BYTE* bytes, size_t count) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (size_t i = 0; i < count; ++i) {
        if (i) out << ' ';
        out << std::setw(2) << static_cast<unsigned>(bytes[i]);
    }
    return out.str();
}

std::string HostKey(HMODULE host, uint32_t* timestamp, uint32_t* image_size) {
    BYTE* base = reinterpret_cast<BYTE*>(host);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    *timestamp = nt->FileHeader.TimeDateStamp;
    *image_size = nt->OptionalHeader.SizeOfImage;
    wchar_t path[32768] = {};
    GetModuleFileNameW(host, path, static_cast<DWORD>(std::size(path)));
    std::ostringstream suffix;
    suffix << fs::path(path).stem().string() << '-' << std::uppercase
           << std::hex << std::setw(8) << std::setfill('0') << *timestamp
           << '-' << std::setw(8) << *image_size;
    return suffix.str();
}

uint32_t Rva(const void* address, HMODULE host, uint32_t image_size) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(host);
    const uintptr_t value = reinterpret_cast<uintptr_t>(address);
    CHECK(value >= base && value - base < image_size);
    return static_cast<uint32_t>(value - base);
}

void WriteSymbol(std::ofstream& out, const char* name, const char* kind,
                 uint32_t rva, const BYTE* expected = nullptr,
                 size_t expected_count = 0) {
    out << "\n[" << name << "]\nkind=" << kind << "\nrva=0x" << std::hex
        << std::uppercase << rva << std::dec << "\n";
    if (expected && expected_count)
        out << "expect=" << HexBytes(expected, expected_count) << "\n";
}

bool GenerateSymbols(const fs::path& root) {
    HMODULE host = GetModuleHandleW(nullptr);
    uint32_t timestamp = 0;
    uint32_t image_size = 0;
    const std::string key = HostKey(host, &timestamp, &image_size);
    fs::create_directories(root / L"sdgo" / L"symbols");
    const fs::path path = root / L"sdgo" / L"symbols" /
                          fs::path(std::wstring(key.begin(), key.end()) + L".ini");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << "[host]\ntimestamp=0x" << std::hex << std::uppercase << timestamp
        << "\nsize_of_image=0x" << image_size << std::dec << "\n";
    for (size_t i = 0; i < kSiteFunctions.size(); ++i) {
        const BYTE* first =
            reinterpret_cast<const BYTE*>(kSiteFunctions[i]);
        WriteSymbol(out, kDailyFinalSymbols[i], "site",
                    Rva(first, host, image_size), first, 6);
        WriteSymbol(out, kAugmentSymbols[i], "site",
                    Rva(first + 6, host, image_size), first + 6, 6);
        WriteSymbol(out, kDailyLayoutSymbols[i], "data",
                    Rva(&g_layout_guards[i], host, image_size));
        WriteSymbol(out, kBattleLayoutSymbols[i], "data",
                    Rva(&g_layout_guards[i + 10], host, image_size));
    }
    const BYTE* damage =
        reinterpret_cast<const BYTE*>(&FixtureDamageTarget);
    WriteSymbol(out, "battle.rewards.record_damage", "code",
                Rva(damage, host, image_size), damage, 5);
    const BYTE* sender = reinterpret_cast<const BYTE*>(&FixtureNetSend);
    WriteSymbol(out, "battle.rewards.net_send", "code",
                Rva(sender, host, image_size), sender, 5);
    WriteSymbol(out, "battle.rewards.net_frame_proc", "data",
                Rva(&g_net_frame_proc, host, image_size));
    out.flush();
    return out.good();
}

std::string ReadText(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

template <typename Predicate>
bool WaitUntil(Predicate predicate, DWORD timeout_ms) {
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    do {
        if (predicate()) return true;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return predicate();
}

bool WaitStatus(const fs::path& runtime, const std::string& needle,
                DWORD timeout_ms = 8000) {
    const fs::path status =
        runtime / L"status" / L"battle-fixture.json";
    return WaitUntil(
        [&] {
            if (!fs::is_regular_file(status)) return false;
            const std::string text = ReadText(status);
            return text.find("\"settlement_ready\": true") !=
                       std::string::npos &&
                   text.find(needle) != std::string::npos;
        },
        timeout_ms);
}

void PrepareMatch(MatchFixture* match, uint32_t room_id, uint32_t started,
                  uint32_t base_account) {
    match->base_account = base_account;
    Put(match->room.data(), 0, room_id);
    Put(match->room.data(), kRoomMode, static_cast<uint8_t>(4));
    Put(match->room.data(), kRoomMission, static_cast<uint32_t>(0));
    Put(match->room.data(), kRoomStartedAt, started);
    for (unsigned slot = 0; slot < 4; ++slot) {
        const uint32_t account = base_account + slot;
        const uint8_t team = slot < 2 ? 1 : 2;
        BYTE* room_pilot = match->RoomPilot(slot);
        BYTE* battle_pilot = match->BattlePilot(slot);
        Put(room_pilot, kRoomPilotAccount, account);
        Put(room_pilot, kRoomPilotTeam, team);
        Put(room_pilot, kRoomPilotAgent, 900000u + account);
        Put(battle_pilot, kBattlePilotEntity, account);
        Put(battle_pilot, kBattlePilotTeam, team);
        Put(battle_pilot, kBattlePilotState, static_cast<uint8_t>(3));
        Put(battle_pilot, kBattlePilotOutcome, static_cast<uint8_t>(1));
    }
}

void RecordCombat(MatchFixture* match, unsigned attacker_slot) {
    static const uint32_t damage[] = {2000, 2000, 4000};
    static const unsigned victims[] = {2, 3, 2};
    DamageFn target = reinterpret_cast<DamageFn>(&FixtureDamageTarget);
    std::array<BYTE, 0x100> victim_unit{};
    Put(victim_unit.data(), 0, static_cast<uint32_t>(0xABCDEF01));
    Put(victim_unit.data(), 0x6C, static_cast<uintptr_t>(0x12345678));
    for (size_t i = 0; i < std::size(damage); ++i) {
        const uint32_t result = target(
            match->room.data(), match->BattlePilot(victims[i]),
            match->BattlePilot(attacker_slot), victim_unit.data(), damage[i], 1);
        CHECK(result == 0xC0FFEE01u);
    }
    Put(match->BattlePilot(attacker_slot), kBattlePilotKills,
        static_cast<uint32_t>(3));
}

void PrepareMission(MatchFixture* match, uint32_t room_id, uint32_t started,
                    uint32_t base_account) {
    PrepareMatch(match, room_id, started, base_account);
    Put(match->room.data(), kRoomMode, static_cast<uint8_t>(5));
    Put(match->room.data(), kRoomMission, static_cast<uint32_t>(10));
    Put(match->room.data(), kRoomMissionState, static_cast<uint32_t>(5));
    Put(match->room.data(), kRoomMissionDifficulty, static_cast<uint8_t>(2));
}

void RecordMissionCombat(MatchFixture* match) {
    DamageFn target = reinterpret_cast<DamageFn>(&FixtureDamageTarget);
    BYTE* victim = match->BattlePilot(12);
    std::array<BYTE, 0x100> victim_unit{};
    // unit+0 is deliberately unrelated to the mission identity. Production
    // classification must use BP+1CEC -> NpcData[0].
    Put(victim_unit.data(), 0, static_cast<uint32_t>(999));
    Put(victim_unit.data(), 0x6C, static_cast<uintptr_t>(0x87654321));
    std::array<BYTE, 0x40> minion_data{};
    Put(minion_data.data(), 0, static_cast<uint32_t>(99));
    for (uint32_t i = 0; i < 15; ++i) {
        Put(victim, kBattlePilotEntity, 0x80001000u + i);
        Put(victim, kBattlePilotTeam, static_cast<uint8_t>(0));
        Put(victim, kBattlePilotNpcData,
            reinterpret_cast<uintptr_t>(minion_data.data()));
        const uint32_t result =
            target(match->room.data(), victim, match->BattlePilot(0),
                   victim_unit.data(), 100, 1);
        CHECK(result == 0xC0FFEE01u);
    }
    std::array<BYTE, 0x40> boss_data{};
    Put(boss_data.data(), 0, static_cast<uint32_t>(20));
    Put(victim, kBattlePilotEntity, static_cast<uint32_t>(0x80002000u));
    Put(victim, kBattlePilotTeam, static_cast<uint8_t>(0));
    Put(victim, kBattlePilotNpcData,
        reinterpret_cast<uintptr_t>(boss_data.data()));
    const uint32_t human_result =
        target(match->room.data(), victim, match->BattlePilot(0),
               victim_unit.data(), 3250, 0);
    CHECK(human_result == 0xC0FFEE01u);
    BYTE* wingman = match->BattlePilot(13);
    Put(wingman, kBattlePilotEntity, static_cast<uint32_t>(0x80003000u));
    Put(wingman, kBattlePilotTeam, static_cast<uint8_t>(1));
    const uint32_t wingman_result =
        target(match->room.data(), victim, wingman,
               victim_unit.data(), 3250, 1);
    CHECK(wingman_result == 0xC0FFEE01u);
}

std::unique_ptr<FrameFixture> PrepareFrame(MatchFixture* match, unsigned site,
                                           unsigned slot, int32_t rank) {
    auto result = std::make_unique<FrameFixture>();
    BYTE* frame = result->Frame();
    const FrameContract& contract = kFrames[site];
    Put(frame, contract.room,
        reinterpret_cast<uintptr_t>(match->room.data()));
    Put(frame, contract.duration, static_cast<uint32_t>(180));
    Put(frame, contract.room_pilot,
        reinterpret_cast<uintptr_t>(match->RoomPilot(slot)));
    Put(frame, contract.battle_pilot,
        reinterpret_cast<uintptr_t>(match->BattlePilot(slot)));
    Put(frame, contract.physical_slot, static_cast<int32_t>(slot));
    Put(frame, contract.rank, rank);
    Put(frame, contract.eligibility + rank * 4,
        static_cast<uint32_t>(0));
    if (contract.mission_outcomes)
        Put(frame, contract.mission_outcomes + rank * 4,
            static_cast<uint32_t>(1));
    const auto& fields = kDailyFields[site];
    Put(frame, fields[0], static_cast<uint16_t>(11));
    Put(frame, fields[1], static_cast<uint16_t>(22));
    Put(frame, fields[2], static_cast<uint16_t>(33));
    Put(frame, fields[3], static_cast<uint16_t>(2));
    Put(frame, fields[4], static_cast<uint16_t>(3));
    Put(frame, fields[5], static_cast<uint16_t>(4));
    for (unsigned i = 0; i < 4; ++i)
        Put(frame, kGDisplayFields[site][i], static_cast<uint16_t>(16 + i));
    return result;
}

void CheckFrame(const FrameFixture& frame_fixture, unsigned site,
                uint16_t expected_g, bool daily) {
    const BYTE* frame = frame_fixture.storage.data() + 0xC80;
    const auto& fields = kDailyFields[site];
    CHECK(Get<uint16_t>(frame, fields[0]) == (daily ? 22 : 11));
    CHECK(Get<uint16_t>(frame, fields[1]) == expected_g);
    CHECK(Get<uint16_t>(frame, fields[2]) == (daily ? 66 : 33));
    CHECK(Get<uint16_t>(frame, fields[3]) == (daily ? 13 : 2));
    CHECK(Get<uint16_t>(frame, fields[4]) == 0);
    CHECK(Get<uint16_t>(frame, fields[5]) == (daily ? 37 : 4));
    CHECK(Get<uint16_t>(frame, kGDisplayFields[site][0]) == expected_g);
    for (unsigned i = 1; i < 4; ++i)
        CHECK(Get<uint16_t>(frame, kGDisplayFields[site][i]) == 0);
}

bool HasPacket(uint32_t account, uint32_t current_mb) {
    std::lock_guard<std::mutex> lock(g_packet_mutex);
    for (const PacketRecord& packet : g_packets) {
        if (packet.account == account && packet.current_mb == current_mb &&
            packet.target == 900000u + account) {
            return true;
        }
    }
    return false;
}

bool WaitPacket(uint32_t account, uint32_t current_mb,
                DWORD timeout_ms = 8000) {
    return WaitUntil([&] { return HasPacket(account, current_mb); },
                     timeout_ms);
}

bool ReplacePolicyWithBaseFive(const fs::path& runtime) {
    const fs::path path = runtime / L"reward-policy.ini";
    std::string text = ReadText(path);
    const std::string base = "Base=10\r\n";
    const size_t base_pos = text.find(base);
    const std::string label = "PolicyRevision=";
    const size_t revision_pos = text.find(label);
    if (base_pos == std::string::npos || revision_pos == std::string::npos ||
        revision_pos + label.size() + 64 > text.size()) {
        return false;
    }
    text.replace(base_pos, base.size(), "Base=5\r\n");
    text.replace(revision_pos + label.size(), 64, 64, 'c');
    const fs::path temporary = path.parent_path() / L"reward-policy.fixture.tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.flush();
        if (!output) return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

void VerifyPatchedSites(bool daily) {
    for (SiteFn function : kSiteFunctions) {
        const BYTE* first = reinterpret_cast<const BYTE*>(function);
        CHECK(first[6] == 0xE9);
        CHECK(!daily || first[0] == 0xE9);
        CHECK(daily || first[0] == 0x90);
    }
    CHECK(*reinterpret_cast<const BYTE*>(&FixtureDamageTarget) == 0xE9);
}

bool LoadRealCore(const fs::path& root) {
    HMODULE core = LoadLibraryW((root / L"sdgo" / L"core.dll").c_str());
    if (!core) {
        std::cerr << "LoadLibrary(core.dll) failed: " << GetLastError() << "\n";
        return false;
    }
    auto init = reinterpret_cast<SdgoCoreInitFn>(
        GetProcAddress(core, "SdgoCoreInit"));
    if (!init) return false;
    SdgoBoot boot{sizeof(SdgoBoot), "battle_rewards_runtime_test",
                  GetModuleHandleW(nullptr)};
    return init(&boot) != 0;
}

void BasicCases(const fs::path& runtime) {
    std::vector<std::unique_ptr<MatchFixture>> matches;
    std::vector<std::unique_ptr<FrameFixture>> frames;
    for (unsigned site = 0; site < 10; ++site) {
        auto match = std::make_unique<MatchFixture>();
        const uint32_t base_account = 1001u + site * 100u;
        PrepareMatch(match.get(), 0x1000u + site, 1700000000u + site,
                     base_account);
        RecordCombat(match.get(), 0);
        if (site == 0) RecordCombat(match.get(), 1);
        auto frame = PrepareFrame(match.get(), site, 0, 0);
        InvokeSite(reinterpret_cast<void*>(kSiteFunctions[site]),
                   frame->Frame());
        CheckFrame(*frame, site, 13310, false);
        matches.push_back(std::move(match));
        frames.push_back(std::move(frame));
    }
    for (unsigned site = 0; site < 10; ++site)
        CHECK(WaitPacket(matches[site]->base_account, 1125));

    auto mission = std::make_unique<MatchFixture>();
    PrepareMission(mission.get(), 0x1800, 1700000500, 2501);
    RecordMissionCombat(mission.get());
    auto mission_frame = PrepareFrame(mission.get(), 9, 0, 0);
    InvokeSite(reinterpret_cast<void*>(kSiteFunctions[9]),
               mission_frame->Frame());
    CheckFrame(*mission_frame, 9, 2092, false);
    CHECK(WaitPacket(2501, 1003));

    CHECK(ReplacePolicyWithBaseFive(runtime));
    CHECK(WaitStatus(runtime, std::string(64, 'c')));

    auto frozen_frame = PrepareFrame(matches[0].get(), 0, 1, 1);
    InvokeSite(reinterpret_cast<void*>(kSiteFunctions[0]),
               frozen_frame->Frame());
    CheckFrame(*frozen_frame, 0, 10648, false);
    CHECK(WaitPacket(matches[0]->base_account + 1, 1045));

    auto next_match = std::make_unique<MatchFixture>();
    PrepareMatch(next_match.get(), 0x2000, 1700001000, 3001);
    RecordCombat(next_match.get(), 0);
    auto next_frame = PrepareFrame(next_match.get(), 1, 0, 0);
    InvokeSite(reinterpret_cast<void*>(kSiteFunctions[1]),
               next_frame->Frame());
    CheckFrame(*next_frame, 1, 6655, false);
    CHECK(WaitPacket(3001, 1062));
}

void DailyCase() {
    auto match = std::make_unique<MatchFixture>();
    PrepareMatch(match.get(), 0x3000, 1700002000, 4001);
    RecordCombat(match.get(), 0);
    auto frame = PrepareFrame(match.get(), 0, 0, 0);
    InvokeSite(reinterpret_cast<void*>(kSiteFunctions[0]), frame->Frame());
    CheckFrame(*frame, 0, 26620, true);
    CHECK(WaitPacket(4001, 1250));

    auto mission = std::make_unique<MatchFixture>();
    PrepareMission(mission.get(), 0x4000, 1700003000, 5001);
    RecordMissionCombat(mission.get());
    auto mission_frame = PrepareFrame(mission.get(), 9, 0, 0);
    InvokeSite(reinterpret_cast<void*>(kSiteFunctions[9]),
               mission_frame->Frame());
    CheckFrame(*mission_frame, 9, 4185, true);
    CHECK(WaitPacket(5001, 1006));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 3) {
        std::cerr << "usage: battle_rewards_runtime_test <runtime> <basic|daily>\n";
        return 2;
    }
    const fs::path runtime = fs::absolute(argv[1]);
    const std::wstring mode = argv[2];
    const bool daily = mode == L"daily";
    if (!daily && mode != L"basic") return 2;

    wchar_t executable[32768] = {};
    GetModuleFileNameW(nullptr, executable,
                       static_cast<DWORD>(std::size(executable)));
    const fs::path root = fs::path(executable).parent_path();
    void* transport = g_net_frame.data();
    Put(g_net_frame.data(), 0xAC, transport);
    for (size_t i = 0; i < g_layout_guards.size(); ++i)
        g_layout_guards[i] = static_cast<BYTE>(0x40 + i);

    CHECK(GenerateSymbols(root));
    CHECK(LoadRealCore(root));
    CHECK(GetModuleHandleW(L"battle_rewards.dll") != nullptr);
    CHECK((GetModuleHandleW(L"daily_rewards.dll") != nullptr) == daily);
    VerifyPatchedSites(daily);
    CHECK(WaitStatus(runtime, "\"policy_loaded\": true"));

    if (daily)
        DailyCase();
    else
        BasicCases(runtime);

    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    if (!g_failures)
        std::cout << "ALL REAL CORE BATTLE REWARD RUNTIME CASES PASSED\n";
    return g_failures ? 1 : 0;
}
