#include <windows.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "sdgo/ai_catalog_format.h"
#include "sdgo/sdgo_api.h"

namespace {
using namespace sdgo::catalog;
// The native creators inline an RB-tree lookup. A grid.find detour alone cannot
// supply their records. Publish native-allocated entries before data loading ends.
struct NativeValue { uint32_t type; uint8_t* record; };
struct NativePair { uint32_t id; NativeValue* value; };
struct InsertResult { void* node; uint8_t inserted; uint8_t padding[3]; };
static_assert(sizeof(NativeValue) == 8 && sizeof(NativePair) == 8 && sizeof(InsertResult) == 8);
// Native Find returns bool in AL; the upper EAX bits are unspecified on a miss.
using GridFind = bool(__thiscall*)(void*, const uint8_t**, uint32_t, int, char);
using GridData = void*(__cdecl*)();
using LoadNpc = void(__thiscall*)(void*, const wchar_t*, uint32_t);
using Allocate = void*(__cdecl*)(size_t);
using Release = void(__cdecl*)(void*);
using MakeNode = void*(__thiscall*)(void*, const NativePair*);
using InsertNode = void(__thiscall*)(void*, InsertResult*, void*, char);
using RegisterRecord = void(__thiscall*)(void*, const void*, uint32_t, uint32_t);
using MapFind = void(__thiscall*)(void*, void**, const uint32_t*);
using CreateNpc = uint8_t*(__thiscall*)(void*, uint32_t, float, float, float, int, float, float, float);
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
Catalog g_catalog;
GridFind g_find = nullptr;
GridData g_data = nullptr;
LoadNpc g_loadNpc = nullptr;
Allocate g_allocate = nullptr;
Release g_release = nullptr;
MakeNode g_makeNode = nullptr;
InsertNode g_insertNode = nullptr;
RegisterRecord g_registerRecord = nullptr;
MapFind g_mapFind = nullptr;
CreateNpc g_baseCreate = nullptr, g_missionCreate = nullptr, g_remoteCreate = nullptr;

struct Allocation {
    uint8_t* record = nullptr;
    NativeValue* value = nullptr;
    void* node = nullptr;
    ~Allocation() {
        if (node) g_release(node);
        if (value) g_release(value);
        if (record) g_release(record);
    }
    void Published() { node = nullptr; value = nullptr; record = nullptr; }
};

bool HasAnyKey(void* grid, uint32_t id) {
    void* node = nullptr;
    g_mapFind(grid, &node, &id);
    return node && node != *reinterpret_cast<void**>(static_cast<uint8_t*>(grid) + 4);
}

int InstallRecords(void* grid) {
    std::vector<const Record*> pending;
    int existing = 0;
    // Complete preflight before touching the native tree. Preserve foreign ids,
    // including entries of another grid type, instead of replacing their data.
    for (const auto& row : g_catalog.records) {
        const uint8_t* source = nullptr;
        if (!g_find(grid, &source, row.unitId, 2, 0) || !source ||
            Hash64(source, kClientUnitBytes) != row.clientUnitHash) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "unit %lu missing or changed; native catalog registration aborted",
                       static_cast<unsigned long>(row.unitId));
            return -1;
        }
        if (HasAnyKey(grid, row.npcId)) {
            const uint8_t* record = nullptr;
            if (!g_find(grid, &record, row.npcId, 41, 0) || !record ||
                memcmp(record, row.clientNpc.data(), kClientNpcBytes)) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "NPC %lu conflicts with native data; registration aborted",
                           static_cast<unsigned long>(row.npcId));
                return -1;
            }
            ++existing;
        } else pending.push_back(&row);
    }
    // Both wrappers and records use the game's allocator, so the game's normal
    // table cleanup owns and frees them. DLL-CRT/vector storage must never enter it.
    std::vector<std::unique_ptr<Allocation>> prepared;
    prepared.reserve(pending.size());
    for (const auto* row : pending) {
        prepared.push_back(std::make_unique<Allocation>());
        auto& item = *prepared.back();
        item.record = static_cast<uint8_t*>(g_allocate(kClientNpcBytes));
        item.value = static_cast<NativeValue*>(g_allocate(sizeof(NativeValue)));
        if (!item.record || !item.value) return -1;
        memcpy(item.record, row->clientNpc.data(), kClientNpcBytes);
        *item.value = NativeValue{41, item.record};
        NativePair pair{row->npcId, item.value};
        item.node = g_makeNode(grid, &pair);
        if (!item.node) return -1;
    }
    int added = 0;
    for (size_t i = 0; i < prepared.size(); ++i) {
        auto& item = *prepared[i];
        auto* raw = item.record;
        InsertResult result{};
        auto* node = item.node;
        item.node = nullptr; // native insert consumes it, even for a duplicate key
        g_insertNode(grid, &result, node, 0);
        if (!result.inserted) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "NPC %lu could not be registered", static_cast<unsigned long>(pending[i]->npcId));
            return -1;
        }
        item.Published();
        g_registerRecord(grid, raw, static_cast<uint32_t>(kClientNpcBytes), 41);
        const uint8_t* found = nullptr;
        if (!g_find(grid, &found, pending[i]->npcId, 41, 0) || found != raw) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "NPC %lu native registration readback failed", static_cast<unsigned long>(pending[i]->npcId));
            return -1;
        }
        ++added;
    }
    g_api->Log(g_mod, SDGO_LOG_INFO, "native NPC catalog registered: %d added, %d already present; inline creators can resolve %u records",
               added, existing, static_cast<unsigned>(g_catalog.records.size()));
    return added;
}
int GuardedInstall(void* grid, DWORD* code) {
    __try { return InstallRecords(grid); }
    __except(EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return -1; }
}
void __fastcall LoadTable(void* self, void*, const wchar_t* file, uint32_t type) {
    g_loadNpc(self, file, type);
    const wchar_t* name = file ? file : L"";
    for (const wchar_t* p = name; *p; ++p) if (*p == L'\\' || *p == L'/') name = p + 1;
    // Last NPC table of the supported build; native units are already loaded,
    // while derived lists/security iteration are initialized after this returns.
    if (type == 41 && _wcsicmp(name, L"0222.gdt") == 0) {
        DWORD code = 0;
        if (GuardedInstall(self, &code) < 0)
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native catalog registration failed (%08lX); missing catalog creators will be refused", code);
    }
}
bool CanCreate(uint32_t id) {
    const auto index = g_catalog.Find(id);
    if (index == g_catalog.records.size()) return true;
    const uint8_t* npc = nullptr;
    if (g_find(g_data(), &npc, id, 41, 0) && npc &&
        !memcmp(npc, g_catalog.records[index].clientNpc.data(), kClientNpcBytes)) return true;
    g_api->Log(g_mod, SDGO_LOG_ERROR, "NPC %lu has no native catalog record; creation refused safely", static_cast<unsigned long>(id));
    return false;
}
uint8_t* __fastcall BaseCreate(void* self, void*, uint32_t id, float x, float y, float z, int team, float dx, float dy, float dz) {
    return CanCreate(id) ? g_baseCreate(self, id, x, y, z, team, dx, dy, dz) : nullptr;
}
uint8_t* __fastcall MissionCreate(void* self, void*, uint32_t id, float x, float y, float z, int team, float dx, float dy, float dz) {
    return CanCreate(id) ? g_missionCreate(self, id, x, y, z, team, dx, dy, dz) : nullptr;
}
uint8_t* __fastcall RemoteCreate(void* self, void*, uint32_t id, float x, float y, float z, int team, float dx, float dy, float dz) {
    return CanCreate(id) ? g_remoteCreate(self, id, x, y, z, team, dx, dy, dz) : nullptr;
}
template<class T> bool Bind(T& target, const char* name) {
    target = reinterpret_cast<T>(g_api->Symbol(g_mod, name)); return target != nullptr;
}
}

extern "C" __declspec(dllexport) int __cdecl SdgoAiCatalogMasteredWeaponTypeV1(uint32_t unitId) {
    for(const auto& row:g_catalog.records) if(row.unitId==unitId) {
        // Packed native UnitData: UnitNumber + four byte attributes precede
        // MasteredWeaponType. Same schema as backend/ai_catalog.py UNIT_FIELDS.
        int role=row.nativeUnitPrefix[8];return role<=2?role:-1;
    }
    return -1;
}

SDGO_MOD_ENTRY {
    g_api = api; g_mod = mod;
    std::string error;
    if (!g_catalog.Load(std::wstring(api->ModDirectory(mod)) + L"\\catalog.bin", error)) {
        api->Log(mod, SDGO_LOG_ERROR, "%s", error.c_str()); return 0;
    }
    if (!Bind(g_find, "grid.find") || !Bind(g_data, "grid.data") || !Bind(g_allocate, "crt.operator_new") ||
        !Bind(g_release, "crt.operator_delete") || !Bind(g_makeNode, "grid.make_node") ||
        !Bind(g_insertNode, "grid.insert_node") || !Bind(g_registerRecord, "grid.register_record") ||
        !Bind(g_mapFind, "grid.map_find")) return 0;
    if (!api->HookCode(mod, "grid.load_npc_table", reinterpret_cast<void*>(&LoadTable), reinterpret_cast<void**>(&g_loadNpc)) ||
        !api->HookCode(mod, "mode.base_create_npc", reinterpret_cast<void*>(&BaseCreate), reinterpret_cast<void**>(&g_baseCreate)) ||
        !api->HookCode(mod, "mode.create_npc", reinterpret_cast<void*>(&MissionCreate), reinterpret_cast<void**>(&g_missionCreate)) ||
        !api->HookCode(mod, "mode.create_remote_npc", reinterpret_cast<void*>(&RemoteCreate), reinterpret_cast<void**>(&g_remoteCreate))) return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: %u owner-provided NPC records; native registration deferred to client data loading", static_cast<unsigned>(g_catalog.records.size()));
    return 1;
}
