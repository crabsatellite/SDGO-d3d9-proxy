#include <windows.h>
#include <process.h>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/operator_model.h"

namespace {
using namespace sdgo::operators;
struct Box { uint32_t type; void* data; };
struct Pair { uint32_t id; Box* box; };
struct InsertResult { void* node; uint8_t inserted; uint8_t pad[3]; };
struct Row { uint8_t* data; size_t size; Box* box; void* node; bool owned; };
using Key = std::pair<int, int>;

std::map<Key, Row> g_rows;
std::array<uint8_t, kOperatorSize> g_a, g_b, g_unrelated;
std::array<uint8_t, kCategorySize> g_filteredTemplate, g_template, g_existingA, g_existingB;
std::array<uint8_t, kCategorySize> g_filteredA, g_filteredB, g_filteredC, g_conflict;
std::wstring g_directory;
std::vector<std::string> g_logs;
SdgoApi g_apiFixture = {};
void* g_hook = nullptr;
int g_originalCalls = 0;
int g_allocationCalls = 0;
bool g_legacy = false;

void RequireImpl(bool ok, int line)
{
    if (ok) return;
    std::fprintf(stderr, "operator_catalog_test failure at line %d\n", line);
    ExitProcess(90);
}
#define Require(value) RequireImpl((value), __LINE__)
using ConvertFn = int(WINAPI*)(UINT, DWORD, LPCCH, int, LPWSTR, int);
ConvertFn g_import = &MultiByteToWideChar;

__declspec(noinline) int NativeDecode(UINT page, const char* text, wchar_t* output, int capacity)
{
    volatile int count = g_import(page, 0, text, -1, output, capacity);
    return count;
}

int Import(SdgoMod*, const char* dll, const char* function, void* replacement, void** original)
{
    Require(!_stricmp(dll, "kernel32.dll") && !strcmp(function, "MultiByteToWideChar"));
    *original = reinterpret_cast<void*>(g_import);
    g_import = reinterpret_cast<ConvertFn>(replacement);
    return 1;
}

void* __cdecl Grid() { return reinterpret_cast<void*>(0x12345678); }

uint8_t __fastcall Find(void* self, void*, void** out, int id, int type, char)
{
    Require(self == Grid());
    const auto found = g_rows.find({id, type});
    if (found == g_rows.end()) return 0;
    *out = found->second.data;
    return 1;
}

void* __cdecl Allocate(size_t size)
{
    ++g_allocationCalls;
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

void __cdecl Free(void* value) { Require(HeapFree(GetProcessHeap(), 0, value) != 0); }

void* __fastcall Make(void* self, void*, const Pair* pair)
{
    Require(self == Grid());
    auto* node = static_cast<uint8_t*>(Allocate(24));
    std::memcpy(node + 12, pair, 8);
    return node;
}

void __fastcall Insert(void* self, void*, InsertResult* result, void* node, char duplicate)
{
    Require(self == Grid() && duplicate == 0);
    Pair pair = {};
    std::memcpy(&pair, static_cast<uint8_t*>(node) + 12, 8);
    const Key key = {static_cast<int>(pair.id), static_cast<int>(pair.box->type)};
    if (g_rows.count(key)) {
        Free(node);
        result->inserted = 0;
        return;
    }
    const size_t size = pair.box->type == 0x2C ? kOperatorSize : kCategorySize;
    g_rows.emplace(key, Row{static_cast<uint8_t*>(pair.box->data), size, pair.box, node, true});
    result->node = node;
    result->inserted = 1;
}

uint8_t __fastcall Original(void* self, void*, const wchar_t*, int)
{
    Require(self == Grid());
    ++g_originalCalls;
    return 1;
}

void* Symbol(SdgoMod*, const char* name)
{
    if (!strcmp(name, "operator.native_name_decode"))
        return g_legacy ? nullptr : reinterpret_cast<void*>(&NativeDecode);
    if (!strcmp(name, "text.ansi_to_wide")) return reinterpret_cast<void*>(&NativeDecode);
    if (!strcmp(name, "operator.grid_data")) return reinterpret_cast<void*>(&Grid);
    if (!strcmp(name, "operator.grid_find")) return reinterpret_cast<void*>(&Find);
    if (!strcmp(name, "operator.grid_make_node")) return reinterpret_cast<void*>(&Make);
    if (!strcmp(name, "operator.grid_insert")) return reinterpret_cast<void*>(&Insert);
    if (!strcmp(name, "operator.native_new")) return reinterpret_cast<void*>(&Allocate);
    if (!strcmp(name, "operator.native_delete")) return reinterpret_cast<void*>(&Free);
    return nullptr;
}

int Hook(SdgoMod*, const char* name, void* detour, void** original)
{
    Require(!strcmp(name, "operator.load_data"));
    g_hook = detour;
    *original = reinterpret_cast<void*>(&Original);
    return 1;
}

void Log(SdgoMod*, int, const char* format, ...)
{
    char text[512] = {};
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    g_logs.emplace_back(text);
}

bool HasLog(const char* fragment)
{
    for (const std::string& text : g_logs)
        if (text.find(fragment) != std::string::npos) return true;
    return false;
}

int Setting(SdgoMod*, const char*, int def) { return def; }
const wchar_t* Directory(SdgoMod*) { return g_directory.c_str(); }

void Put(const std::wstring& file, const wchar_t* section, const wchar_t* key, const wchar_t* value)
{
    Require(WritePrivateProfileStringW(section, key, value, file.c_str()) != 0);
}

void NativeRow(std::array<uint8_t, kOperatorSize>& row, uint32_t id, const wchar_t* marker)
{
    row.fill(0x3A);
    WriteId(row.data(), id);
    row[4] = 0;
    Require(Text(row.data() + 5, 100, marker));
    for (size_t i = 0; i < 7; ++i)
        Require(Text(row.data() + kAvatarOffsets[i], kAvatarCapacities[i],
                     std::wstring(marker) + std::to_wstring(i)));
}

void CategoryRow(std::array<uint8_t, kCategorySize>& row, uint32_t id, uint8_t security,
                 uint32_t ref, uint8_t time, uint32_t option, uint8_t seed)
{
    for (size_t i = 0; i < row.size(); ++i)
        row[i] = static_cast<uint8_t>(seed + static_cast<uint8_t>(i * 37));
    WriteId(row.data(), id);
    row[4] = security;
    WriteId(row.data() + 105, ref);
    row[109] = time;
    std::memcpy(row.data() + 110, &option, sizeof(option));
}

// Mirrors the native type-0x22 decoder: secured rows never enter the runtime map.
void DecodeCategory(std::array<uint8_t, kCategorySize>& row)
{
    if (row[4] != 0) return;
    const uint32_t id = ReadId(row.data());
    g_rows[{static_cast<int>(id), 0x22}] = {row.data(), row.size(), nullptr, nullptr, false};
}

void RuntimeCategory(uint32_t key, std::array<uint8_t, kCategorySize>& row)
{
    g_rows[{static_cast<int>(key), 0x22}] = {row.data(), row.size(), nullptr, nullptr, false};
}

void OperatorConfig(const std::wstring& file, uint32_t id, uint32_t category,
                    const wchar_t* name, const wchar_t* sprite = nullptr)
{
    const std::wstring section = L"operator:" + std::to_wstring(id);
    Put(file, section.c_str(), L"Category", std::to_wstring(category).c_str());
    Put(file, section.c_str(), L"Template", L"60003");
    Put(file, section.c_str(), L"AvatarProfile", L"60013");
    Put(file, section.c_str(), L"Name", name);
    if (sprite) Put(file, section.c_str(), L"AvatarSprite", sprite);
}

void WriteConfig(int mode)
{
    const std::wstring file = g_directory + L"\\display.ini";
    DeleteFileW(file.c_str());
    Put(file, L"catalog", L"Version", L"1");
    OperatorConfig(file, 60003, 1210002, L"display");
    Put(file, L"operator:60003", L"Template", mode == 2 ? L"69999" : L"60003");
    Put(file, L"operator:60003", L"FixedName", g_legacy ? nullptr : L"1");
    // UTF16LE hex supports quotes and non-ASCII names without INI quote stripping.
    Put(file, L"operator:60003", L"NameHex", mode == 1 ? L"GG00" : L"0C54F3892200");
    OperatorConfig(file, 60013, 1210012, L"default support");
    OperatorConfig(file, 60018, 1250002, L"default support");
    OperatorConfig(file, 60025, 1220001, L"default support");
    OperatorConfig(file, 60041, 1220008, L"default support");
    OperatorConfig(file, 60047, 1219047, L"conflicting operator");
    OperatorConfig(file, 60048, 1219048, L"new operator",
                   L"sdgo\\mods\\operator_catalog\\assets\\60048.spr");
}

void BaseRows()
{
    NativeRow(g_a, 60003, L"avatarA");
    NativeRow(g_b, 60013, L"avatarB");
    NativeRow(g_unrelated, 60001, L"unrelated");
    g_rows[{60003, 0x2C}] = {g_a.data(), g_a.size(), nullptr, nullptr, false};
    g_rows[{60013, 0x2C}] = {g_b.data(), g_b.size(), nullptr, nullptr, false};
    g_rows[{60001, 0x2C}] = {g_unrelated.data(), g_unrelated.size(), nullptr, nullptr, false};
}

void NormalCategories()
{
    CategoryRow(g_filteredTemplate, 1210001, 1, 60001, 1, 0, 0x11);
    DecodeCategory(g_filteredTemplate);
    Require(!g_rows.count({1210001, 0x22}));

    CategoryRow(g_template, 1210017, 0, 60021, 1, 0, 0x27);
    DecodeCategory(g_template);
    CategoryRow(g_existingA, 1210012, 0, 60013, 1, 0, 0x31);
    DecodeCategory(g_existingA);
    CategoryRow(g_existingB, 1220008, 0, 60041, 0, 0, 0x42);
    DecodeCategory(g_existingB);

    CategoryRow(g_filteredA, 1210002, 1, 60003, 1, 0, 0x53);
    CategoryRow(g_filteredB, 1250002, 1, 60018, 1, 0, 0x64);
    CategoryRow(g_filteredC, 1220001, 1, 60025, 1, 0, 0x75);
    DecodeCategory(g_filteredA);
    DecodeCategory(g_filteredB);
    DecodeCategory(g_filteredC);
    Require(!g_rows.count({1210002, 0x22}) && !g_rows.count({1250002, 0x22}) &&
            !g_rows.count({1220001, 0x22}));

    CategoryRow(g_conflict, 1219047, 0, 60013, 0, 0, 0x86);
    DecodeCategory(g_conflict);
}

void InvalidCandidate(int mode)
{
    CategoryRow(g_filteredTemplate, 1210001, 1, 60001, 1, 0, 0x11);
    DecodeCategory(g_filteredTemplate);
    if (mode == 4) return;
    const uint32_t rowId = mode == 9 ? 1210999 : 1210017;
    const uint8_t security = mode == 5 ? 1 : 0;
    const uint32_t ref = mode == 6 ? 61000 : 60021;
    const uint8_t time = mode == 7 ? 3 : 1;
    const uint32_t option = mode == 8 ? 1 : 0;
    CategoryRow(g_template, rowId, security, ref, time, option, 0x27);
    // Security and ID mismatch are injected directly to exercise defensive
    // validation even though the stock decoder would not produce these rows.
    RuntimeCategory(1210017, g_template);
}

using HookFn = uint8_t(__fastcall*)(void*, void*, const wchar_t*, int);

HMODULE LoadMod(const wchar_t* path, int mode)
{
    HMODULE module = LoadLibraryW(path);
    Require(module != nullptr);
    auto entry = reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module, "SdgoModLoad"));
    Require(entry != nullptr);
    g_apiFixture = {};
    g_apiFixture.size = sizeof(g_apiFixture);
    g_apiFixture.version = 1;
    g_apiFixture.Log = &Log;
    g_apiFixture.Symbol = &Symbol;
    g_apiFixture.HookCode = &Hook;
    g_apiFixture.SettingInt = &Setting;
    g_apiFixture.ModDirectory = &Directory;
    g_apiFixture.HookImport = &Import;
    const int loaded = entry(&g_apiFixture, reinterpret_cast<SdgoMod*>(1));
    if (mode == 1 || mode == 2) {
        Require(loaded == 0 && !g_hook);
    } else {
        Require(loaded != 0 && g_hook);
    }
    return module;
}

bool CategoryOwnedField(size_t offset)
{
    return offset < 4 || (offset >= 5 && offset < 114);
}

void CheckCreatedCategory(uint32_t category, uint32_t op, const std::wstring& expectedName)
{
    const Row& created = g_rows.at({static_cast<int>(category), 0x22});
    Require(created.owned && ReadId(created.data) == category && created.data[4] == g_template[4]);
    uint32_t ref = 0, option = 1;
    std::memcpy(&ref, created.data + 105, sizeof(ref));
    std::memcpy(&option, created.data + 110, sizeof(option));
    Require(ref == op && created.data[109] == 0 && option == 0);
    wchar_t name[50] = {};
    std::memcpy(name, created.data + 5, sizeof(name));
    Require(std::wstring(name) == expectedName);
    for (size_t i = 0; i < kCategorySize; ++i)
        if (!CategoryOwnedField(i)) Require(created.data[i] == g_template[i]);
}

std::map<Key, std::vector<uint8_t>> SnapshotRows()
{
    std::map<Key, std::vector<uint8_t>> result;
    for (const auto& pair : g_rows)
        result[pair.first] = std::vector<uint8_t>(pair.second.data, pair.second.data + pair.second.size);
    return result;
}

void Cleanup(HMODULE module)
{
    for (auto& pair : g_rows) {
        if (!pair.second.owned) continue;
        Free(pair.second.data);
        Free(pair.second.box);
        Free(pair.second.node);
    }
    FreeLibrary(module);
}

void CheckNameHook()
{
    wchar_t rendered[64] = {};
    Require(NativeDecode(874, "@SDGO:60003", rendered, 64) > 0 &&
            !wcscmp(rendered, L"\x540c\x89f3\""));
    Require(NativeDecode(874, "@SDGO:60003", nullptr, 0) == 4);
    Require(NativeDecode(874, "@SDGO:60003", rendered, 2) == 0 &&
            GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    Require(NativeDecode(874, "personal-name", rendered, 64) > 0 &&
            !wcscmp(rendered, L"personal-name"));
    Require(NativeDecode(874, "@SDGO:60048", rendered, 64) > 0 &&
            !wcscmp(rendered, L"@SDGO:60048"));
    Require(NativeDecode(65001, "@SDGO:60003", rendered, 64) > 0 &&
            !wcscmp(rendered, L"@SDGO:60003"));
    Require(g_import(874, 0, "@SDGO:60003", -1, rendered, 64) > 0 &&
            !wcscmp(rendered, L"@SDGO:60003"));
}

void RunNormal(const wchar_t* dll, int mode)
{
    NormalCategories();
    const auto before = g_a;
    const auto otherBefore = g_unrelated;
    const auto templateBefore = g_template;
    const auto existingABefore = g_existingA;
    const auto existingBBefore = g_existingB;
    const auto conflictBefore = g_conflict;
    HMODULE module = LoadMod(dll, mode);
    CheckNameHook();
    auto invoke = reinterpret_cast<HookFn>(g_hook);
    Require(invoke(Grid(), nullptr, L"other.gdt", 2) == 1 && g_a == before &&
            g_originalCalls == 1);
    Require(invoke(Grid(), nullptr, L"gdrs\\0255.gdt", 0x2C) == 1 &&
            g_originalCalls == 2);

    Require(g_unrelated == otherBefore && g_template == templateBefore);
    Require(g_existingA == existingABefore && g_existingB == existingBBefore);
    Require(g_conflict == conflictBefore);
    Require(!g_rows.count({60047, 0x2C}));
    Require(HasLog("operator item 1219047 conflicts"));

    const std::array<std::pair<uint32_t, uint32_t>, 3> filteredDefaults = {{
        {1210002, 60003}, {1250002, 60018}, {1220001, 60025}
    }};
    for (const auto& item : filteredDefaults)
        CheckCreatedCategory(item.first, item.second,
                             item.second == 60003 ? L"\x540c\x89f3\"" : L"default support");
    CheckCreatedCategory(1219048, 60048, L"new operator");

    Require(g_a[1200] == before[1200] && g_a[kMissionPartsOffset] == 1);
    for (uint32_t id : {60013u, 60018u, 60025u, 60041u})
        Require(g_rows.at({static_cast<int>(id), 0x2C}).data[kMissionPartsOffset] == 1);
    for (size_t i = 0; i < 7; ++i)
        Require(!memcmp(g_a.data() + kAvatarOffsets[i], g_b.data() + kAvatarOffsets[i],
                        kAvatarCapacities[i] * 2));
    wchar_t name[100] = {};
    std::memcpy(name, g_a.data() + 5, 200);
    Require(name[0] == 0x540C && name[1] == 0x89F3 && name[2] == L'"');

    const Row& custom = g_rows.at({60048, 0x2C});
    Require(ReadId(custom.data) == 60048 && custom.data[1200] == before[1200]);
    Require(custom.data[kMissionPartsOffset] == before[kMissionPartsOffset]);
    for (size_t i = 0; i < 7; ++i) {
        wchar_t path[54] = {};
        std::memcpy(path, custom.data + kAvatarOffsets[i], kAvatarCapacities[i] * 2);
        Require(std::wstring(path) ==
                L"sdgo\\mods\\operator_catalog\\assets\\60048_" + std::to_wstring(i) + L".spr");
    }

    const auto firstApply = SnapshotRows();
    const int allocations = g_allocationCalls;
    Require(invoke(Grid(), nullptr, L"gdrs\\0255.gdt", 0x2C) == 1 &&
            g_originalCalls == 3);
    Require(SnapshotRows() == firstApply && g_allocationCalls == allocations);
    Cleanup(module);
}

void RunInvalidCandidate(const wchar_t* dll, int mode)
{
    InvalidCandidate(mode);
    const auto beforeA = g_a;
    const auto beforeB = g_b;
    const auto beforeOther = g_unrelated;
    const auto beforeRows = SnapshotRows();
    HMODULE module = LoadMod(dll, mode);
    auto invoke = reinterpret_cast<HookFn>(g_hook);
    Require(invoke(Grid(), nullptr, L"other.gdt", 2) == 1 && g_originalCalls == 1);
    Require(invoke(Grid(), nullptr, L"gdrs\\0255.gdt", 0x2C) == 1 &&
            g_originalCalls == 2);
    Require(g_a == beforeA && g_b == beforeB && g_unrelated == beforeOther);
    Require(SnapshotRows() == beforeRows && g_allocationCalls == 0);
    Require(!g_rows.count({60048, 0x2C}) && !g_rows.count({1219048, 0x22}));
    Require(HasLog("native operator item template unavailable"));
    Cleanup(module);
}

void RunChildCases(wchar_t** argv)
{
    for (int mode = 4; mode <= 9; ++mode) {
        const std::wstring value = std::to_wstring(mode);
        const wchar_t* child[] = {argv[0], argv[1], argv[2], value.c_str(), nullptr};
        Require(_wspawnv(_P_WAIT, argv[0], child) == 0);
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    Require(argc == 4);
    const int mode = _wtoi(argv[3]);
    Require(mode >= 0 && mode <= 9);
    g_legacy = mode == 3;
    Require(CreateDirectoryW(argv[2], nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS);
    g_directory = std::wstring(argv[2]) + L"\\runtime-" + std::to_wstring(mode);
    Require(CreateDirectoryW(g_directory.c_str(), nullptr) != 0 ||
            GetLastError() == ERROR_ALREADY_EXISTS);
    WriteConfig(mode);
    BaseRows();
    if (mode == 1 || mode == 2) {
        const auto before = g_a;
        HMODULE module = LoadMod(argv[1], mode);
        Require(g_a == before);
        FreeLibrary(module);
        return 0;
    }
    if (mode <= 3) RunNormal(argv[1], mode);
    else RunInvalidCandidate(argv[1], mode);
    if (mode == 0) RunChildCases(argv);
    return 0;
}
