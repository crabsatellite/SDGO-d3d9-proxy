// Optional actual-owner-data test of the production client resolver. The game,
// rendering engine, accounts and network are not started.
#include <windows.h>
#include <fstream>
#include <map>
#include "../mods/ai_catalog/client.cpp"
namespace data_test {
std::wstring directory;
std::map<uint32_t, std::array<uint8_t, sdgo::catalog::kClientUnitBytes>> units;
int checks = 0;
void Check(bool value, const char* text) {
    ++checks;
    if (!value) { fprintf(stderr, "FAIL: %s\n", text); ExitProcess(2); }
}
int __fastcall NativeGrid(void*, void*, const uint8_t** output, uint32_t id, int type, char) {
    if (type != 2 || !output || !units.count(id)) return 0;
    *output = units.at(id).data(); return 1;
}
int Hook(SdgoMod*, const char* name, void*, void** original) {
    if (strcmp(name, "grid.find")) return 0;
    *original = reinterpret_cast<void*>(&NativeGrid); return 1;
}
void Log(SdgoMod*, int, const char*, ...) {}
const wchar_t* ModDir(SdgoMod*) { return directory.c_str(); }
}
int wmain(int argc, wchar_t** argv) {
    using namespace data_test;
    using namespace sdgo::catalog;
    Check(argc == 3, "catalog directory and owner unit fixture supplied");
    directory = argv[1];
    std::ifstream input(argv[2], std::ios::binary);
    char magic[8]{}; uint32_t count = 0;
    input.read(magic, 8); input.read(reinterpret_cast<char*>(&count), 4);
    Check(input && !memcmp(magic, "SDGOUN1", 8) && count > 0 && count <= 3000, "owner fixture header");
    for (uint32_t i = 0; i < count; ++i) {
        std::array<uint8_t, kClientUnitBytes> row{};
        input.read(reinterpret_cast<char*>(row.data()), row.size());
        Check(input.good() && units.emplace(U32(row.data()), row).second, "owner source unit read without duplicate");
    }
    const auto before = units;
    SdgoApi api{}; api.size = sizeof(api); api.version = 1;
    api.ModDirectory = &ModDir; api.HookCode = &Hook; api.Log = &data_test::Log;
    Check(SdgoModLoad(&api, nullptr) != 0, "production client catalog initialized");
    Check(g_catalog.records.size() == units.size(), "client owner source count matches catalog");
    for (const auto& row : g_catalog.records) {
        const uint8_t* npc = nullptr;
        Check(Find(nullptr, nullptr, &npc, row.npcId, 41, 1) == 1, "actual owner unit fingerprint accepted by production client resolver");
        Check(U32(npc) == row.npcId && U32(npc + 0x69) == row.unitId &&
              U32(npc + 0xe5) == U32(row.nativeNpc.data() + 8), "actual client/server NPC identity and HP agree");
        const uint8_t* unit = nullptr;
        Check(Find(nullptr, nullptr, &unit, row.unitId, 2, 1) == 1 && unit == units.at(row.unitId).data(), "original client unit lookup unchanged");
    }
    Check(units == before, "every actual owner unit byte unchanged");
    printf("ALL CLIENT DATA CHECKS PASSED: %u units, %d checks; no game, account or network started\n", count, checks);
    return 0;
}
