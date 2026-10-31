// Optional integration with a caller-supplied BattleServer image, mapped without
// its entry point. Uses owned maps, records and buffers; no server or SQL starts.
// Never distributed with game files. The real core/DLL tests are separate.
#include <windows.h>
#include <MinHook.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include "../mods/ai_catalog/server.cpp"

namespace native_test {
namespace fs = std::filesystem;
uint8_t* image = nullptr;
std::wstring directory;
int checks = 0;
void Check(bool value, const char* message) {
    ++checks;
    if (!value) { fprintf(stderr, "FAIL: %s\n", message); ExitProcess(2); }
}
uint32_t __cdecl Hash(const uint32_t* key) { return *key; }
int __cdecl Compare(const uint32_t* key, const uint8_t* node) { return *key == sdgo::catalog::U32(node) ? 0 : 1; }
struct Map {
    std::vector<std::unique_ptr<std::array<uint8_t, 0x2c>>> nodes;
    uint32_t bucket = 0;
    void Add(uint32_t id, void* record) {
        auto node = std::make_unique<std::array<uint8_t, 0x2c>>();
        auto* p = node->data();
        sdgo::catalog::Put32(p, id);
        sdgo::catalog::Put32(p + 0x20, reinterpret_cast<uint32_t>(p + 0x24));
        sdgo::catalog::Put32(p + 0x24, reinterpret_cast<uint32_t>(record));
        sdgo::catalog::Put32(p + 0x28, bucket);
        bucket = reinterpret_cast<uint32_t>(p);
        nodes.push_back(std::move(node));
    }
    void Bind(uint8_t* target) {
        memset(target, 0, 0x24);
        sdgo::catalog::Put32(target, 4); sdgo::catalog::Put32(target + 4, 4); sdgo::catalog::Put32(target + 8, 4);
        sdgo::catalog::Put32(target + 0x14, reinterpret_cast<uint32_t>(&Hash));
        sdgo::catalog::Put32(target + 0x18, reinterpret_cast<uint32_t>(&Compare));
        sdgo::catalog::Put32(target + 0x1c, 1);
        sdgo::catalog::Put32(target + 0x20, reinterpret_cast<uint32_t>(&bucket));
    }
};
void Log(SdgoMod*, int level, const char* message, ...) {
    if (level >= SDGO_LOG_ERROR) fprintf(stderr, "catalog: %s\n", message);
}
const wchar_t* ModDirectory(SdgoMod*) { return directory.c_str(); }
void* Symbol(SdgoMod*, const char* name) {
    if (!strcmp(name, "catalog.npc_manager")) return image + 0x4359d0;
    if (!strcmp(name, "catalog.unit_manager")) return image + 0x42aab8;
    return nullptr;
}
int Hook(SdgoMod*, const char* name, void* detour, void** original) {
    uint32_t rva = !strcmp(name, "catalog.find_ai_npc") ? 0x36860 :
                   !strcmp(name, "catalog.find_unit") ? 0x64ed0 :
                   !strcmp(name, "battle.build_ai_pilot") ? 0x85720 : 0;
    if (!rva) return 0;
    return MH_CreateHook(image + rva, detour, original) == MH_OK && MH_EnableHook(image + rva) == MH_OK;
}
}

int wmain(int argc, wchar_t** argv) {
    using namespace native_test;
    using namespace sdgo::catalog;
    Check(argc == 4, "supplied image, catalog and isolated output directory required");
    const fs::path root = fs::absolute(argv[3]);
    Check(!fs::exists(root), "native test output must be new");
    fs::create_directories(root / L"sdgo/mods/ai_catalog");
    fs::copy_file(argv[1], root / L"BattleServer.exe");
    fs::copy_file(argv[2], root / L"sdgo/mods/ai_catalog/catalog.bin");
    directory = (root / L"sdgo/mods/ai_catalog").wstring();
    // DONT_RESOLVE_DLL_REFERENCES never executes the host CRT/main or starts services.
    HMODULE module = LoadLibraryExW((root / L"BattleServer.exe").c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    Check(module != nullptr, "owned native image mapped");
    image = reinterpret_cast<uint8_t*>(module);
    const auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(image + reinterpret_cast<IMAGE_DOS_HEADER*>(image)->e_lfanew);
    Check(nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386 && nt->FileHeader.TimeDateStamp == 0x548a891d &&
          nt->OptionalHeader.SizeOfImage == 0x46c000, "supported native image identity");
    const uint8_t findPrefix[]{0x55,0x8b,0xec,0x51,0x89,0x4d,0xfc,0x8b,0x45,0x0c};
    const uint8_t buildPrefix[]{0x55,0x8b,0xec,0x83,0xec,0x1c,0x89,0x4d,0xe4};
    Check(!memcmp(image + 0x36860, findPrefix, sizeof(findPrefix)) &&
          !memcmp(image + 0x85720, buildPrefix, sizeof(buildPrefix)), "native function signatures");
    // The sole imported operation reached by BuildAiPilot in this isolated test.
    DWORD protection = 0;
    Check(VirtualProtect(image + 0x43a46c, 4, PAGE_READWRITE, &protection) != 0, "owned import writable");
    Put32(image + 0x43a46c, reinterpret_cast<uint32_t>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "InterlockedIncrement")));
    VirtualProtect(image + 0x43a46c, 4, protection, &protection);
    Put32(image + 0x435d5c, 0x80000000u);
    Catalog catalog; std::string error;
    Check(catalog.Load(directory + L"\\catalog.bin", error), "owner-generated catalog parsed");
    std::map<uint32_t, std::array<uint8_t, 0x7c>> units;
    std::map<uint32_t, std::array<uint8_t, 0x94>> weapons;
    Map unitMap, npcMap, skillMap, weaponMap;
    for (const auto& row : catalog.records) {
        auto& unit = units[row.unitId]; memcpy(unit.data(), row.nativeUnitPrefix.data(), kNativeUnitPrefixBytes);
        unitMap.Add(row.unitId, unit.data());
        for (int i = 0; i < 4; ++i) {
            const uint32_t id = U32(unit.data() + 0x49 + i * 4);
            if (id && !weapons.count(id)) { Put32(weapons[id].data(), id); weaponMap.Add(id, weapons[id].data()); }
        }
    }
    unitMap.Bind(image + 0x42aab8 + 0x18);
    npcMap.Bind(image + 0x4359d0 + 0x84);
    skillMap.Bind(image + 0x42b3ac + 0x14);
    weaponMap.Bind(image + 0x42aa2c + 0x18);
    const auto before = units;
    auto nativeBuild = reinterpret_cast<BuildPilot>(image + 0x85720);
    auto nativeUnit = reinterpret_cast<FindUnit>(image + 0x64ed0);
    auto nativeNpc = reinterpret_cast<FindNpc>(image + 0x36860);
    uint8_t* found = reinterpret_cast<uint8_t*>(1);
    Check(!nativeNpc(image + 0x4359d0, catalog.records[0].npcId, &found) && found == reinterpret_cast<uint8_t*>(1), "native baseline has no generated NPC");
    Check(nativeUnit(image + 0x42aab8, catalog.records[0].unitId) == units.at(catalog.records[0].unitId).data(), "native hash map finds original unit");
    Check(MH_Initialize() == MH_OK, "hook engine initialized");
    SdgoApi api{}; api.size = sizeof(api); api.version = 1;
    api.Symbol = &native_test::Symbol; api.HookCode = &native_test::Hook;
    api.Log = &native_test::Log; api.ModDirectory = &native_test::ModDirectory;
    Check(SdgoModLoad(&api, nullptr) != 0, "production catalog server initialized");
    std::vector<uint32_t> ids(3000);
    size_t count = 0;
    for (int level = 1; level <= 5; ++level) {
        const int added = SdgoAiCatalogPoolV1(level, ids.data(), static_cast<uint32_t>(ids.size()));
        Check(added >= 0, "native catalog pool available"); count += static_cast<size_t>(added);
    }
    Check(count == catalog.records.size(), "all generated units pass native source validation");
    std::array<uint8_t, 0x1f54> pilot{};
    std::array<uint8_t, 0x658> battleUnit{};
    std::array<uint8_t, 32> room{};
    for (const auto& row : catalog.records) {
        pilot.fill(0); battleUnit.fill(0);
        Check(nativeBuild(room.data(), pilot.data(), battleUnit.data(), row.npcId) == 1, "actual BuildAiPilot succeeds");
        Check(U32(battleUnit.data()) == row.npcId && U32(battleUnit.data() + 0x64) == row.unitId &&
              U32(battleUnit.data() + 0x34) == U32(row.nativeNpc.data() + 8), "actual pilot NPC, unit and HP agree");
        Check(U32(battleUnit.data() + 0x3c) == row.preferredWeapon - 1, "actual native builder selects a usable initial weapon");
        Check((U32(pilot.data()) & 0x80000000u) != 0, "actual native AI pilot identity");
        Check(nativeUnit(image + 0x42aab8, row.unitId) == units.at(row.unitId).data(), "actual native unit lookup restored after build");
    }
    Check(units == before, "all original native unit records byte-identical");
    Check(MH_DisableHook(MH_ALL_HOOKS) == MH_OK, "test hook teardown succeeds");
    Check(!nativeNpc(image + 0x4359d0, catalog.records[0].npcId, &found), "hook teardown restores missing-NPC baseline without database rollback");
    Check(units == before && npcMap.nodes.empty(), "no native unit or NPC collection mutations");
    Check(MH_Uninitialize() == MH_OK, "test hook engine released");
    printf("ALL NATIVE CATALOG CHECKS PASSED: %u units, %d checks; no entry point, network or SQL executed\n", static_cast<unsigned>(count), checks);
    return 0;
}
