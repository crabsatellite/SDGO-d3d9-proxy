// An optional, process-local AI roster. No SQL writes and no native list insertion.
#include <windows.h>
#include <array>
#include <mutex>
#include "sdgo/ai_catalog_format.h"
#include "sdgo/sdgo_api.h"

namespace {
using namespace sdgo::catalog;
using FindNpc = int(__thiscall*)(void*, uint32_t, uint8_t**);
using FindUnit = uint8_t*(__thiscall*)(void*, uint32_t);
using BuildPilot = int(__thiscall*)(void*, uint8_t*, uint8_t*, uint32_t);
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
Catalog g_catalog;
FindNpc g_findNpc = nullptr;
FindUnit g_findUnit = nullptr;
BuildPilot g_buildPilot = nullptr;
void* g_npcManager = nullptr;
void* g_unitManager = nullptr;
std::mutex g_lock;
bool g_active = false;
struct Prepared {
    std::array<uint8_t, 0x30> npc{};  // native record plus null linked-list fields
    std::array<uint8_t, 0x7c> unit{};
    bool ready = false;
    bool rejected = false;
};
std::vector<Prepared> g_prepared;
thread_local Prepared* g_building = nullptr;

Prepared* Prepare(size_t index) {
    if (index >= g_catalog.records.size()) return nullptr;
    std::lock_guard<std::mutex> guard(g_lock);
    auto& item = g_prepared[index];
    const auto& row = g_catalog.records[index];
    if (item.rejected) return nullptr;
    if (item.ready) return &item;
    uint8_t* existing = nullptr;
    if (g_findNpc(g_npcManager, row.npcId, &existing)) {
        item.rejected = true;
        g_api->Log(g_mod, SDGO_LOG_ERROR, "NPC %lu already exists; catalog entry rejected", static_cast<unsigned long>(row.npcId));
        return nullptr;
    }
    const uint8_t* source = g_findUnit(g_unitManager, row.unitId);
    if (!source) return nullptr; // native tables may not have arrived yet; retry next battle
    if (memcmp(source, row.nativeUnitPrefix.data(), kNativeUnitPrefixBytes)) {
        item.rejected = true;
        g_api->Log(g_mod, SDGO_LOG_ERROR, "unit %lu differs from the owner's catalog; entry rejected", static_cast<unsigned long>(row.unitId));
        return nullptr;
    }
    memcpy(item.npc.data(), row.nativeNpc.data(), kNativeNpcBytes);
    memcpy(item.unit.data(), source, kNativeUnitBytes);
    // The native builder may correct this field in place. Keep that correction
    // private to AI pilots; never let it write into a player's original unit data.
    Put32(item.unit.data() + 0x45, row.preferredWeapon);
    item.ready = true;
    return &item;
}

int __fastcall LookupNpc(void* self, void*, uint32_t id, uint8_t** output) {
    const size_t index = g_catalog.Find(id);
    if (self != g_npcManager || index == g_catalog.records.size()) return g_findNpc(self, id, output);
    if (!output) return 0;
    if (auto* item = Prepare(index)) { *output = item->npc.data(); return 1; }
    return g_findNpc(self, id, output); // preserve a native record even on a catalog collision
}
uint8_t* __fastcall LookupUnit(void* self, void*, uint32_t id) {
    if (self == g_unitManager && g_building && U32(g_building->unit.data()) == id)
        return g_building->unit.data();
    return g_findUnit(self, id);
}
int CallBuilder(void* self, uint8_t* pilot, uint8_t* unit, uint32_t id, Prepared* item) {
    Prepared* previous = g_building;
    g_building = item;
    __try { return g_buildPilot(self, pilot, unit, id); }
    __finally { g_building = previous; }
}
int __fastcall Build(void* self, void*, uint8_t* pilot, uint8_t* unit, uint32_t id) {
    const size_t index = g_catalog.Find(id);
    if (index == g_catalog.records.size()) return CallBuilder(self, pilot, unit, id, nullptr);
    auto* item = Prepare(index);
    if (item) return CallBuilder(self, pilot, unit, id, item);
    uint8_t* existing = nullptr;
    return g_findNpc(g_npcManager, id, &existing) ? CallBuilder(self, pilot, unit, id, nullptr) : 0;
}
template<class T> bool Bind(T& target, const char* name) {
    target = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return target != nullptr;
}
}

extern "C" __declspec(dllexport) int __cdecl SdgoAiCatalogPoolV1(int level, uint32_t* output, uint32_t capacity) {
    if (!g_active || level < 1 || level > 5 || !output || capacity < g_catalog.records.size()) return -1;
    int count = 0;
    for (size_t i = 0; i < g_catalog.records.size(); ++i)
        if (g_catalog.records[i].level == static_cast<uint32_t>(level) && Prepare(i))
            output[count++] = g_catalog.records[i].npcId;
    return count;
}

SDGO_MOD_ENTRY {
    g_api = api; g_mod = mod;
    std::string error;
    if (!g_catalog.Load(std::wstring(api->ModDirectory(mod)) + L"\\catalog.bin", error)) {
        api->Log(mod, SDGO_LOG_ERROR, "%s", error.c_str()); return 0;
    }
    if (!Bind(g_npcManager, "catalog.npc_manager") || !Bind(g_unitManager, "catalog.unit_manager")) return 0;
    g_prepared.resize(g_catalog.records.size());
    if (!api->HookCode(mod, "catalog.find_ai_npc", reinterpret_cast<void*>(&LookupNpc), reinterpret_cast<void**>(&g_findNpc)) ||
        !api->HookCode(mod, "catalog.find_unit", reinterpret_cast<void*>(&LookupUnit), reinterpret_cast<void**>(&g_findUnit)) ||
        !api->HookCode(mod, "battle.build_ai_pilot", reinterpret_cast<void*>(&Build), reinterpret_cast<void**>(&g_buildPilot))) return 0;
    g_active = true;
    api->Log(mod, SDGO_LOG_INFO, "ready: %u owner-provided AI candidates, process-local data only", static_cast<unsigned>(g_catalog.records.size()));
    return 1;
}
