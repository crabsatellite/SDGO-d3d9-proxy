#include <windows.h>
#include <mutex>
#include "sdgo/ai_catalog_format.h"
#include "sdgo/sdgo_api.h"

namespace {
using namespace sdgo::catalog;
using GridFind = int(__thiscall*)(void*, const uint8_t**, uint32_t, int, char);
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
Catalog g_catalog;
GridFind g_find = nullptr;
std::mutex g_lock;
std::vector<uint8_t> g_states; // 0 pending, 1 compatible, 2 native collision, 3 incompatible source

int __fastcall Find(void* self, void*, const uint8_t** output, uint32_t id, int type, char log) {
    const size_t index = type == 41 ? g_catalog.Find(id) : g_catalog.records.size();
    if (index == g_catalog.records.size()) return g_find(self, output, id, type, log);
    if (!output) return 0;
    std::lock_guard<std::mutex> guard(g_lock);
    const auto& row = g_catalog.records[index];
    if (!g_states[index]) {
        const uint8_t* existing = nullptr;
        const uint8_t* unit = nullptr;
        if (g_find(self, &existing, id, type, 0)) {
            g_states[index] = 2;
            g_api->Log(g_mod, SDGO_LOG_ERROR, "NPC %lu already exists; catalog entry rejected", static_cast<unsigned long>(id));
        } else if (g_find(self, &unit, row.unitId, 2, 0) && unit) {
            g_states[index] = Hash64(unit, kClientUnitBytes) == row.clientUnitHash ? 1 : 3;
            if (g_states[index] == 3)
                g_api->Log(g_mod, SDGO_LOG_ERROR, "unit %lu differs from the owner's client data", static_cast<unsigned long>(row.unitId));
        }
    }
    if (g_states[index] == 2) return g_find(self, output, id, type, log);
    if (g_states[index] != 1) return 0;
    *output = row.clientNpc.data();
    return 1;
}
}

SDGO_MOD_ENTRY {
    g_api = api; g_mod = mod;
    std::string error;
    if (!g_catalog.Load(std::wstring(api->ModDirectory(mod)) + L"\\catalog.bin", error)) {
        api->Log(mod, SDGO_LOG_ERROR, "%s", error.c_str()); return 0;
    }
    g_states.resize(g_catalog.records.size());
    if (!api->HookCode(mod, "grid.find", reinterpret_cast<void*>(&Find), reinterpret_cast<void**>(&g_find))) return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: %u owner-provided NPC records; native grid data unchanged", static_cast<unsigned>(g_catalog.records.size()));
    return 1;
}
