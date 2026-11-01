#include <windows.h>
#include <intrin.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/operator_model.h"

namespace {
using namespace sdgo::operators;
using LoadFn = uint8_t(__thiscall*)(void*, const wchar_t*, int);
using DataFn = void*(__cdecl*)();
// Find returns AL, not a normalized 32-bit EAX value (native ret 0x10).
using FindFn = uint8_t(__thiscall*)(void*, void**, int, int, char);
using NewFn = void*(__cdecl*)(size_t);
using DeleteFn = void(__cdecl*)(void*);
struct Box { uint32_t type; void* data; };
struct Pair { uint32_t id; Box* box; };
struct InsertResult { void* node; uint8_t inserted; uint8_t padding[3]; };
using MakeFn = void*(__thiscall*)(void*, const Pair*);
using InsertFn = void(__thiscall*)(void*, InsertResult*, void*, char);
struct Entry { uint32_t id, category, source, avatar; std::wstring name, sprite; bool fixedName = false; };
using ConvertFn = int(WINAPI*)(UINT,DWORD,LPCCH,int,LPWSTR,int);
ConvertFn g_convert = nullptr;
uintptr_t g_nameDecoder = 0;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
LoadFn g_originalLoad = nullptr;
DataFn g_data = nullptr;
FindFn g_find = nullptr;
NewFn g_new = nullptr;
DeleteFn g_delete = nullptr;
MakeFn g_make = nullptr;
InsertFn g_insert = nullptr;
std::vector<Entry> g_entries;

bool FixedToken(const char* text, uint32_t* id)
{
    __try {
        if (!text || std::memcmp(text,"@SDGO:",6)) return false;
        uint32_t value=0;
        for (size_t i=6;i<11;++i) {
            if (text[i]<'0' || text[i]>'9') return false;
            value=value*10+static_cast<uint32_t>(text[i]-'0');
        }
        if (text[11]) return false;
        *id=value;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int WINAPI ConvertName(UINT page,DWORD flags,LPCCH text,int count,LPWSTR output,int capacity)
{
    const uintptr_t caller=reinterpret_cast<uintptr_t>(_ReturnAddress());
    uint32_t id=0;
    // Only the verified host's 108-byte CP874 decoder may resolve these tokens.
    // Hooking the game IAT preserves shop_labels' ownership of that decoder.
    if (caller>=g_nameDecoder && caller<g_nameDecoder+108 && page==874 && !flags && count==-1 &&
        FixedToken(text,&id)) {
        for (const Entry& entry:g_entries) {
            if (!entry.fixedName || entry.id!=id) continue;
            const int needed=static_cast<int>(entry.name.size()+1);
            if (!capacity) return needed;
            if (capacity<0 || !output) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
            if (capacity<needed) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return 0; }
            std::memcpy(output,entry.name.c_str(),static_cast<size_t>(needed)*sizeof(wchar_t));
            return needed;
        }
    }
    return g_convert(page,flags,text,count,output,capacity);
}

bool ReadRecord(uint32_t id, int type, uint8_t* out, size_t size)
{
    __try {
        void* row = nullptr;
        if (!g_find(g_data(), &row, static_cast<int>(id), type, 0) || !row) return false;
        std::memcpy(out, row, size);
        return ReadId(out) == id;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool WriteExisting(uint32_t id, int type, const uint8_t* value, size_t size)
{
    __try {
        void* row = nullptr;
        if (!g_find(g_data(), &row, static_cast<int>(id), type, 0) || !row || ReadId(static_cast<uint8_t*>(row)) != id) return false;
        std::memcpy(row, value, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool RegisterRecord(uint32_t id, uint32_t type, const uint8_t* bytes, size_t size)
{
    // The game owns successful insertions. Its native ClearAllData releases the
    // payload, box and node with this same CRT; no cross-CRT allocations escape.
    void* payload = nullptr;
    Box* box = nullptr;
    __try {
        void* exists = nullptr;
        if (g_find(g_data(), &exists, static_cast<int>(id), static_cast<int>(type), 0))
            return exists && std::memcmp(exists, bytes, size) == 0;
        payload = g_new(size);
        box = static_cast<Box*>(g_new(sizeof(Box)));
        std::memcpy(payload, bytes, size);
        box->type = type;
        box->data = payload;
        const Pair pair = {id, box};
        void* node = g_make(g_data(), &pair);
        InsertResult result = {};
        g_insert(g_data(), &result, node, 0);
        if (result.inserted) return true;
        g_delete(payload);
        g_delete(box);
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Native allocation failure must not let an ABI exception cross into
        // the loader hook. Insertions are bounded by the validated catalog.
        return false;
    }
}

void Apply()
{
    std::map<uint32_t, std::array<uint8_t, kOperatorSize>> source;
    for (const Entry& e : g_entries) {
        for (uint32_t id : {e.source, e.avatar}) {
            if (source.count(id)) continue;
            auto& row = source[id];
            if (!ReadRecord(id, 0x2C, row.data(), row.size())) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "operator source %lu unavailable", static_cast<unsigned long>(id));
                return;
            }
        }
    }
    std::array<uint8_t, kCategorySize> categoryTemplate = {};
    if (!ReadRecord(1210001, 0x22, categoryTemplate.data(), categoryTemplate.size())) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "native operator item template unavailable");
        return;
    }
    for (const Entry& e : g_entries) {
        auto row = source.at(e.source);
        auto category = categoryTemplate;
        if (!PatchOperator(row.data(), row.size(), e.id, e.name, source.at(e.avatar).data(), e.sprite)) continue;
        std::array<uint8_t, kCategorySize> registered = {};
        const bool exists = ReadRecord(e.category, 0x22, registered.data(), registered.size());
        uint32_t ref = 0;
        if (exists) std::memcpy(&ref, registered.data() + 105, 4);
        if (!exists && !PatchCategory(category.data(), category.size(), e.category, e.id, e.name)) continue;
        if ((exists && ref != e.id) || (!exists && !RegisterRecord(e.category, 0x22, category.data(), category.size()))) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "operator item %lu conflicts with an existing record", static_cast<unsigned long>(e.category));
            continue;
        }
        std::array<uint8_t, kOperatorSize> existing = {};
        const bool success = ReadRecord(e.id, 0x2C, existing.data(), existing.size())
            ? WriteExisting(e.id, 0x2C, row.data(), row.size())
            : RegisterRecord(e.id, 0x2C, row.data(), row.size());
        g_api->Log(g_mod, success ? SDGO_LOG_INFO : SDGO_LOG_ERROR, "operator %lu catalog %s",
                   static_cast<unsigned long>(e.id), success ? "applied" : "failed");
    }
}

uint8_t __fastcall Load(void* self, void*, const wchar_t* filename, int type)
{
    const uint8_t result = g_originalLoad(self, filename, type);
    if (type == 0x2C && result) Apply();
    return result;
}

bool ReadConfig(const wchar_t* dir)
{
    const std::wstring file = std::wstring(dir) + L"\\display.ini";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return true;
    if (GetPrivateProfileIntW(L"catalog", L"Version", 0, file.c_str()) != 1) return false;
    std::vector<wchar_t> sections(65536);
    const DWORD count = GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), file.c_str());
    if (count >= sections.size() - 2) return false;
    std::map<uint32_t, bool> seen;
    for (const wchar_t* section = sections.data(); *section; section += wcslen(section) + 1) {
        if (wcsncmp(section, L"operator:", 9)) continue;
        wchar_t* end = nullptr;
        const unsigned long id = wcstoul(section + 9, &end, 10);
        if (!end || *end || id < 60001 || id > 60999 || seen.count(id) || g_entries.size() >= 256) return false;
        seen[static_cast<uint32_t>(id)] = true;
        Entry e = {};
        e.id = static_cast<uint32_t>(id);
        e.category = GetPrivateProfileIntW(section, L"Category", 0, file.c_str());
        e.source = GetPrivateProfileIntW(section, L"Template", 0, file.c_str());
        e.avatar = GetPrivateProfileIntW(section, L"AvatarProfile", 0, file.c_str());
        const bool defaultId=e.id==60003 || e.id==60013 || e.id==60018 || e.id==60025 || e.id==60041;
        // Older Manager exports omit this optional field. Their default entries
        // still resolve the server's fixed-name tokens after a client DLL update.
        e.fixedName = GetPrivateProfileIntW(section,L"FixedName",defaultId?1:0,file.c_str())!=0;
        if (e.fixedName && e.id!=60003 && e.id!=60013 && e.id!=60018 && e.id!=60025 && e.id!=60041) return false;
        wchar_t name[100] = {}, sprite[100] = {}, nameHex[200] = {};
        DWORD n = GetPrivateProfileStringW(section, L"Name", L"", name, 100, file.c_str());
        const DWORD hexCount = GetPrivateProfileStringW(section, L"NameHex", L"", nameHex, 200, file.c_str());
        if (hexCount) {
            if (hexCount > 192 || hexCount % 4) return false;
            n = hexCount / 4;
            for (DWORD i = 0; i < n; ++i) {
                unsigned value = 0;
                for (DWORD byte = 0; byte < 2; ++byte) {
                    unsigned decoded = 0;
                    for (DWORD digit = 0; digit < 2; ++digit) {
                        const wchar_t c = nameHex[i*4+byte*2+digit];
                        const int v = c >= L'0' && c <= L'9' ? c-L'0' :
                                      c >= L'a' && c <= L'f' ? c-L'a'+10 :
                                      c >= L'A' && c <= L'F' ? c-L'A'+10 : -1;
                        if (v < 0) return false;
                        decoded = decoded*16 + static_cast<unsigned>(v);
                    }
                    value |= decoded << (byte*8);
                }
                if (!value || value < 32) return false;
                name[i] = static_cast<wchar_t>(value);
            }
            name[n] = 0;
        }
        const DWORD s = GetPrivateProfileStringW(section, L"AvatarSprite", L"", sprite, 100, file.c_str());
        if (!n || n > 48 || s >= 50 || e.category < 1200000 || e.category >= 1300000 ||
            e.source < 60001 || e.source > 60999 || e.avatar < 60001 || e.avatar > 60999) return false;
        e.name = name;
        e.sprite = sprite;
        if (!e.sprite.empty()) {
            const std::wstring expected = L"sdgo\\mods\\operator_catalog\\assets\\" + std::to_wstring(e.id) + L".spr";
            if (e.sprite != expected) return false;
        }
        g_entries.push_back(std::move(e));
    }
    return true;
}
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    if (!api || api->version != SDGO_API_VERSION || !api->SettingInt(mod, "Enabled", 1)) return 0;
    g_data = reinterpret_cast<DataFn>(api->Symbol(mod, "operator.grid_data"));
    g_find = reinterpret_cast<FindFn>(api->Symbol(mod, "operator.grid_find"));
    g_make = reinterpret_cast<MakeFn>(api->Symbol(mod, "operator.grid_make_node"));
    g_insert = reinterpret_cast<InsertFn>(api->Symbol(mod, "operator.grid_insert"));
    g_new = reinterpret_cast<NewFn>(api->Symbol(mod, "operator.native_new"));
    g_delete = reinterpret_cast<DeleteFn>(api->Symbol(mod, "operator.native_delete"));
    if (!g_data || !g_find || !g_make || !g_insert || !g_new || !g_delete || !ReadConfig(api->ModDirectory(mod))) return 0;
    if (g_entries.empty()) { api->Log(mod, SDGO_LOG_INFO, "no operator overrides configured"); return 1; }
    for (const Entry& entry:g_entries) {
        if (!entry.fixedName) continue;
        g_nameDecoder=reinterpret_cast<uintptr_t>(api->Symbol(mod,"operator.native_name_decode"));
        if (!g_nameDecoder) g_nameDecoder=reinterpret_cast<uintptr_t>(api->Symbol(mod,"text.ansi_to_wide"));
        if (!g_nameDecoder || !api->HookImport ||
            !api->HookImport(mod,"kernel32.dll","MultiByteToWideChar",reinterpret_cast<void*>(&ConvertName),
                             reinterpret_cast<void**>(&g_convert))) return 0;
        break;
    }
    return api->HookCode(mod, "operator.load_data", reinterpret_cast<void*>(&Load), reinterpret_cast<void**>(&g_originalLoad));
}
