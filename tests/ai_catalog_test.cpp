// Real core + shipped catalog DLL, synthetic native callbacks and owned records.
// Each invocation is a fresh process: no unsupported live mod unloading.
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "sdgo/ai_catalog_format.h"
#include "sdgo/ai_catalog_provider.h"

using namespace sdgo::catalog;
namespace fs = std::filesystem;
namespace {
int g_checks = 0, g_nativeCalls = 0, g_logFlag = -1;
bool g_throw = false, g_missing = false;
bool g_nested = false;
uint8_t* g_nestedUnit = nullptr;
int g_npcManager = 1, g_unitManager = 2;
std::wstring g_consumer;
std::map<uint32_t, std::array<uint8_t, kClientUnitBytes>> g_units;
std::map<uint32_t, std::array<uint8_t, 0x7c>> g_nativeUnits;
std::map<uint32_t, std::array<uint8_t, 0x30>> g_nativeNpcs;
std::array<uint8_t, kClientNpcBytes> g_nativeClient{};
void Check(bool value, const char* text) {
    ++g_checks;
    if (!value) { fprintf(stderr, "FAIL: %s\n", text); ExitProcess(2); }
}
__declspec(noinline) int __fastcall NativeGrid(void*, void*, const uint8_t** out, uint32_t id, int type, char log) {
    ++g_nativeCalls; g_logFlag = log;
    if (!out) return 0;
    if (type == 2 && g_units.count(id) && !(g_missing && id == 10001)) { *out = g_units.at(id).data(); return 1; }
    if (type == 41 && U32(g_nativeClient.data()) == id) { *out = g_nativeClient.data(); return 1; }
    return 0;
}
__declspec(noinline) int __fastcall NativeNpc(void*, void*, uint32_t id, uint8_t** out) {
    ++g_nativeCalls;
    if (!out || !g_nativeNpcs.count(id)) return 0;
    *out = g_nativeNpcs.at(id).data(); return 1;
}
__declspec(noinline) uint8_t* __fastcall NativeUnit(void*, void*, uint32_t id) {
    ++g_nativeCalls;
    return g_nativeUnits.count(id) && !(g_missing && id == 10001) ? g_nativeUnits.at(id).data() : nullptr;
}
__declspec(noinline) int __fastcall NativeBuild(void*, void*, uint8_t*, uint8_t* out, uint32_t id) {
    ++g_nativeCalls;
    uint8_t* npc = nullptr;
    if (!NativeNpc(&g_npcManager, nullptr, id, &npc)) return 0;
    uint8_t* unit = NativeUnit(&g_unitManager, nullptr, U32(npc + 4));
    if (!unit) return 0;
    if (U32(unit + 0x45) < 1 || U32(unit + 0x45) > 3) Put32(unit + 0x45, 1);
    if (g_throw) RaiseException(0xE013A1C0, 0, 0, nullptr);
    if (g_nested && id == 3604036) {
        uint8_t nestedPilot[32]{}, nestedUnit[32]{};
        g_nested = false;
        Check(NativeBuild(nullptr, nullptr, nestedPilot, nestedUnit, 3604001) == 1, "nested native builder succeeds");
        g_nestedUnit = reinterpret_cast<uint8_t*>(U32(nestedUnit + 16));
        g_nested = true;
    }
    Put32(out, id); Put32(out + 4, U32(unit)); Put32(out + 8, U32(npc + 8));
    Put32(out + 12, U32(unit + 0x45)); Put32(out + 16, reinterpret_cast<uint32_t>(unit));
    return 1;
}
bool Raises(uint32_t id) {
    uint8_t pilot[32]{}, unit[32]{};
    __try { NativeBuild(nullptr, nullptr, pilot, unit, id); return false; }
    __except(GetExceptionCode() == 0xE013A1C0 ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return true; }
}
void Log(SdgoMod*, int, const char*, ...) {}
const wchar_t* ModDir(SdgoMod*) { return g_consumer.c_str(); }

std::vector<uint8_t> MakeCatalog() {
    std::vector<Record> rows(150);
    for (size_t i = 0; i < rows.size(); ++i) {
        auto& row = rows[i];
        row.npcId = 3604031 + static_cast<uint32_t>(i); row.unitId = 10001 + static_cast<uint32_t>(i);
        row.level = static_cast<uint32_t>(i % 5) + 1; row.templateNpcId = 3604001;
        row.preferredWeapon = 2;
        auto& source = g_units[row.unitId];
        Put32(source.data(), row.unitId); source[100] = static_cast<uint8_t>(i);
        row.clientUnitHash = Hash64(source.data(), source.size());
        Put32(row.nativeUnitPrefix.data(), row.unitId);
        Put32(row.nativeUnitPrefix.data() + 0x10, 12000);
        Put32(row.nativeUnitPrefix.data() + 0x49, 101);
        Put32(row.nativeUnitPrefix.data() + 0x4d, 102);
        Put32(row.nativeUnitPrefix.data() + 0x51, 103);
        if (i == 5) Put32(row.nativeUnitPrefix.data() + 0x45, 1);
        memcpy(g_nativeUnits[row.unitId].data(), row.nativeUnitPrefix.data(), kNativeUnitPrefixBytes);
        Put32(row.nativeNpc.data(), row.npcId); Put32(row.nativeNpc.data() + 4, row.unitId);
        Put32(row.nativeNpc.data() + 8, 12000); Put32(row.nativeNpc.data() + 12, 4);
        Put32(row.clientNpc.data(), row.npcId); Put32(row.clientNpc.data() + 0x69, row.unitId);
        Put32(row.clientNpc.data() + 0xe5, 12000);
        memcpy(row.clientNpc.data() + 0x6d, L"VSAI_Gunfighter_01", 36);
    }
    Header header{}; memcpy(header.magic, "SDGOAI1", 8); header.version = 1;
    header.recordBytes = sizeof(Record); header.count = static_cast<uint32_t>(rows.size());
    header.payloadHash = Hash32(rows.data(), rows.size() * sizeof(Record));
    std::vector<uint8_t> data(sizeof(header) + rows.size() * sizeof(Record));
    memcpy(data.data(), &header, sizeof(header)); memcpy(data.data() + sizeof(header), rows.data(), rows.size() * sizeof(Record));
    return data;
}
void PutFile(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path()); std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Check(static_cast<bool>(file), "fixture file written");
}
void Symbol(std::ofstream& ini, const char* name, void* address, bool code) {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    ini << "[" << name << "]\nkind=" << (code ? "code" : "data") << "\nrva=" << std::dec << (reinterpret_cast<uintptr_t>(address) - base) << "\n";
    if (code) {
        ini << "expect=";
        for (int i = 0; i < 12; ++i) { char value[4]; sprintf_s(value, "%02X ", static_cast<uint8_t*>(address)[i]); ini << value; }
        ini << "\n";
    }
}
}

int wmain(int argc, wchar_t** argv) {
    Check(argc == 3, "role and scenario supplied");
    const bool server = wcscmp(argv[1], L"server") == 0;
    const std::wstring scenario = argv[2];
    const bool off = scenario == L"off", conflict = scenario == L"conflict", changed = scenario == L"changed";
    const bool corrupt = scenario == L"corrupt", missing = scenario == L"missing", deferred = scenario == L"deferred";
    const bool rollback = scenario == L"rollback";
    wchar_t executable[MAX_PATH * 2]{}; GetModuleFileNameW(nullptr, executable, MAX_PATH * 2);
    const fs::path root = fs::path(executable).parent_path();
    const fs::path mod = root / L"sdgo/mods/ai_catalog";
    g_consumer = (root / L"sdgo/mods/coop_ai").wstring();
    auto bytes = MakeCatalog();
    Put32(g_nativeNpcs[3604001].data(), 3604001);
    Put32(g_nativeNpcs[3604001].data() + 4, 10006);
    Put32(g_nativeNpcs[3604001].data() + 8, 12000);
    Catalog parsed; std::string error;
    Check(parsed.Parse(bytes, error), "production catalog parser accepts fixture");
    Check(Hash32("hello", 5) == 0x4f9f2cabu && Hash64("hello", 5) == 0xa430d84680aabd0bull,
          "catalog hashes match independent FNV test vectors");
    for (size_t n : {size_t(0), sizeof(Header) - 1, bytes.size() - 1}) {
        std::vector<uint8_t> truncated(bytes.begin(), bytes.begin() + n);
        Check(!parsed.Parse(truncated, error), "truncated catalog rejected atomically");
        Check(parsed.records.empty(), "failed parse retains no prior records");
    }
    Check(parsed.Parse(bytes, error), "catalog reload after rejection");
    for (int mutation = 0; mutation < 7; ++mutation) {
        auto invalid = bytes;
        auto* header = reinterpret_cast<Header*>(invalid.data());
        auto* rows = reinterpret_cast<Record*>(invalid.data() + sizeof(Header));
        if (mutation == 0) rows[0].level = 0;
        if (mutation == 1) rows[1].npcId = rows[0].npcId;
        if (mutation == 2) rows[1].unitId = rows[0].unitId;
        if (mutation == 3) rows[0].templateNpcId = rows[0].npcId;
        if (mutation == 4) Put32(rows[0].clientNpc.data() + 0xe5, 0);
        if (mutation == 5) rows[0].preferredWeapon = 4;
        if (mutation == 6) memset(rows[0].clientNpc.data() + 0x114, 1, 100);
        header->payloadHash = Hash32(invalid.data() + sizeof(Header), invalid.size() - sizeof(Header));
        Check(!parsed.Parse(invalid, error) && parsed.records.empty(), "malformed semantic record rejected after valid checksum");
    }
    Check(parsed.Parse(bytes, error), "valid catalog survives semantic rejection cases");
    if (conflict) {
        Put32(g_nativeClient.data(), 3604031); Put32(g_nativeClient.data() + 0x69, 99999);
        Put32(g_nativeNpcs[3604031].data(), 3604031); Put32(g_nativeNpcs[3604031].data() + 4, 10001);
    }
    if (changed) { g_units[10001][10] ^= 1; g_nativeUnits[10001][10] ^= 1; }
    if (deferred) g_missing = true;
    if (corrupt) bytes.back() ^= 1;
    if (!missing) PutFile(mod / L"catalog.bin", bytes);
    std::ofstream manifest(mod / L"mod.ini");
    manifest << "[mod]\nid=ai_catalog\nname=catalog integration\napi=1\norder=50\nhosts=*\ndll=ai_catalog.dll\nenabled=" << (!off) << "\nrequires="
             << (server ? "catalog.npc_manager,catalog.unit_manager,catalog.find_ai_npc,catalog.find_unit,battle.build_ai_pilot" : "grid.find") << "\n";
    manifest.close();
    std::ofstream global(root / L"sdgo/sdgo.ini"); global << "[mods]\nunrelated_feature=0\n"; global.close();
    const auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    fs::create_directories(root / L"sdgo/symbols");
    char key[100]; sprintf_s(key, "ai_catalog_test-%08X-%08X", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    std::ofstream symbols(root / L"sdgo/symbols" / (std::string(key) + ".ini"));
    symbols << "[host]\ntimestamp=" << nt->FileHeader.TimeDateStamp << "\nsize_of_image=" << nt->OptionalHeader.SizeOfImage << "\n";
    Symbol(symbols, "grid.find", reinterpret_cast<void*>(&NativeGrid), true);
    Symbol(symbols, "catalog.find_ai_npc", reinterpret_cast<void*>(&NativeNpc), true);
    Symbol(symbols, "catalog.find_unit", reinterpret_cast<void*>(&NativeUnit), true);
    Symbol(symbols, "battle.build_ai_pilot", reinterpret_cast<void*>(&NativeBuild), true);
    Symbol(symbols, "catalog.npc_manager", &g_npcManager, false);
    Symbol(symbols, "catalog.unit_manager", &g_unitManager, false); symbols.close();
    std::array<uint8_t, 12> originalGrid{}, originalNpc{}, originalUnit{}, originalBuild{};
    memcpy(originalGrid.data(), reinterpret_cast<void*>(&NativeGrid), 12);
    memcpy(originalNpc.data(), reinterpret_cast<void*>(&NativeNpc), 12);
    memcpy(originalUnit.data(), reinterpret_cast<void*>(&NativeUnit), 12);
    memcpy(originalBuild.data(), reinterpret_cast<void*>(&NativeBuild), 12);
    const auto beforeUnits = g_units; const auto beforeNativeUnits = g_nativeUnits; const auto beforeNpcs = g_nativeNpcs;
    HMODULE core = LoadLibraryW((root / L"sdgo/core.dll").c_str()); Check(core != nullptr, "real core loaded");
    auto init = reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core, "SdgoCoreInit")); Check(init != nullptr, "core init found");
    SdgoBoot boot{sizeof(SdgoBoot), "catalog-test", GetModuleHandleW(nullptr)};
    Check(init(&boot) != 0, "real core initialized");
    const bool active = !off && !corrupt && !missing && !rollback;
    const bool loaded = GetModuleHandleW((mod / L"ai_catalog.dll").c_str()) != nullptr;
    Check(loaded == active, "DLL activation matches manifest and data validity");
    if (!active) {
        if (!rollback || server)
            Check(!memcmp(originalGrid.data(), reinterpret_cast<void*>(&NativeGrid), 12), "disabled/rejected client has original code bytes");
        Check(!memcmp(originalNpc.data(), reinterpret_cast<void*>(&NativeNpc), 12), "disabled/rejected NPC lookup original");
        Check(!memcmp(originalUnit.data(), reinterpret_cast<void*>(&NativeUnit), 12), "disabled/rejected unit lookup original");
        if (!rollback || !server)
            Check(!memcmp(originalBuild.data(), reinterpret_cast<void*>(&NativeBuild), 12), "disabled/rejected builder original");
        if (rollback) {
            Check(GetModuleHandleW((root / L"sdgo/mods/other_feature/catalog_conflict_mod.dll").c_str()) != nullptr,
                  "existing hook owner stays loaded while catalog rolls back");
            const uint8_t* value = nullptr;
            Check(NativeGrid(nullptr, nullptr, &value, 10001, 2, 7) && value == g_units.at(10001).data(), "existing feature still forwards native lookups");
        }
    } else if (server) {
        SdgoApi api{}; api.ModDirectory = &ModDir; api.Log = &Log;
        std::vector<uint32_t> ids;
        Check(sdgo::AiCatalogPool(&api, nullptr, 1, ids), "consumer finds active provider");
        Check(ids.size() == ((conflict || changed || deferred) ? size_t(29) : size_t(30)), "pool validates every candidate");
        if (deferred) { g_missing = false; Check(sdgo::AiCatalogPool(&api, nullptr, 1, ids) && ids.size() == 30, "deferred native tables recover"); }
        for (const auto& row : parsed.records) {
            if ((conflict || changed) && row.unitId == 10001) continue;
            uint8_t pilot[32]{}, result[32]{};
            Check(NativeBuild(nullptr, nullptr, pilot, result, row.npcId) == 1, "native builder can use expanded NPC");
            Check(U32(result + 4) == row.unitId && U32(result + 8) == 12000 && U32(result + 12) == 2, "AI uses private data and valid starting weapon");
            Check(NativeUnit(&g_unitManager, nullptr, row.unitId) == g_nativeUnits.at(row.unitId).data(), "unit override is scoped to the expanded build");
        }
        g_throw = true; Check(Raises(3604036), "native exception propagated"); g_throw = false;
        Check(NativeUnit(&g_unitManager, nullptr, 10006) == g_nativeUnits.at(10006).data(), "TLS scope restored after exception");
        g_nested = true;
        uint8_t nestedPilot[32]{}, nestedResult[32]{};
        Check(NativeBuild(nullptr, nullptr, nestedPilot, nestedResult, 3604036) == 1, "outer catalog builder survives nested native build");
        g_nested = false;
        Check(g_nestedUnit == g_nativeUnits.at(10006).data() &&
              reinterpret_cast<uint8_t*>(U32(nestedResult + 16)) != g_nestedUnit,
              "nested native builder uses original data; outer catalog context is restored");
        uint8_t* sentinel = reinterpret_cast<uint8_t*>(1);
        Check(NativeNpc(&g_npcManager, nullptr, 9999999, &sentinel) == 0 && sentinel == reinterpret_cast<uint8_t*>(1), "unknown NPC preserves native failure semantics");
        if (conflict) Check(NativeNpc(&g_npcManager, nullptr, 3604031, &sentinel) && sentinel == g_nativeNpcs.at(3604031).data(), "catalog collision preserves original NPC");
    } else {
        for (const auto& row : parsed.records) {
            const uint8_t* value = reinterpret_cast<const uint8_t*>(1);
            const bool excluded = row.unitId == 10001 && (changed || deferred);
            Check(NativeGrid(nullptr, nullptr, &value, row.npcId, 41, 1) == (excluded ? 0 : 1), "client NPC lookup result");
            if (!excluded && !(conflict && row.unitId == 10001))
                Check(U32(value) == row.npcId && U32(value + 0x69) == row.unitId && U32(value + 0xe5) == 12000, "client/server identity and HP agree");
            if (excluded) Check(value == reinterpret_cast<const uint8_t*>(1), "client failure leaves output unchanged");
            const uint8_t* source = nullptr;
            if (!deferred || row.unitId != 10001)
                Check(NativeGrid(nullptr, nullptr, &source, row.unitId, 2, 7) == 1 && source == g_units.at(row.unitId).data() && g_logFlag == 7, "normal client unit and flags unchanged");
        }
        if (deferred) { g_missing = false; const uint8_t* row = nullptr; Check(NativeGrid(nullptr, nullptr, &row, 3604031, 41, 0) == 1, "client deferred unit lookup recovers"); }
        if (conflict) { const uint8_t* row = nullptr; Check(NativeGrid(nullptr, nullptr, &row, 3604031, 41, 9) && row == g_nativeClient.data() && g_logFlag == 9, "native client collision and flags preserved"); }
    }
    Check(g_units == beforeUnits && g_nativeUnits == beforeNativeUnits && g_nativeNpcs == beforeNpcs, "all original records byte-identical after catalog operations");
    Check(GetPrivateProfileIntW(L"mods", L"unrelated_feature", 1, (root / L"sdgo/sdgo.ini").c_str()) == 0, "unrelated global mod setting preserved");
    printf("ALL CATALOG CHECKS PASSED: %s scenario, %d checks, source data unchanged\n", server ? "server" : "client", g_checks);
    return 0;
}
