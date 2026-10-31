// Installs Lua decorators before native CNPC::Think. Native movement, weapons,
// damage and networking remain the executor. Never reads another process.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <unordered_map>
#include "sdgo/sdgo_api.h"

namespace {
struct lua_State;
using LuaFn = int(__cdecl*)(lua_State*);
using Reader = const char*(__cdecl*)(lua_State*, void*, size_t*);
struct Lua {
    lua_State*(__cdecl* state)(lua_State*);
    int(__cdecl* gettop)(lua_State*);
    void(__cdecl* settop)(lua_State*, int);
    const char*(__cdecl* tostring)(lua_State*, int);
    void(__cdecl* pushnumber)(lua_State*, double);
    void(__cdecl* pushstring)(lua_State*, const char*);
    void(__cdecl* pushcclosure)(lua_State*, LuaFn, int);
    void(__cdecl* gettable)(lua_State*, int);
    void(__cdecl* settable)(lua_State*, int);
    int(__cdecl* pcall)(lua_State*, int, int, int);
    int(__cdecl* load)(lua_State*, Reader, void*, const char*);
} g_lua = {};
using ThinkFn = void(__thiscall*)(uint8_t*);
using InitFn = void(__thiscall*)(uint8_t*, void*);
ThinkFn g_think = nullptr;
InitFn g_init = nullptr;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
constexpr int kGlobals = -10001;
uint8_t* g_current = nullptr;
std::unordered_map<uint8_t*, uint64_t> g_generations;
uint64_t g_serial = 0;
lua_State* g_loaded = nullptr;
std::wstring g_script, g_config;
ULONGLONG g_checkAt = 0;
FILETIME g_scriptTime = {}, g_configTime = {};
bool g_loading = false;
bool g_preparationFault = false;

template<typename T> bool Bind(T& out, const char* name) {
    out = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return out != nullptr;
}
FILETIME Stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) ? d.ftLastWriteTime : FILETIME{};
}
bool Same(FILETIME a, FILETIME b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}
bool Read(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 512 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD n = 0;
        ok = ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &n, nullptr) && n == out.size();
    }
    CloseHandle(f);
    return ok;
}
struct Chunk { const char* data; size_t size; };
const char* __cdecl ReadChunk(lua_State*, void* user, size_t* n) {
    auto& c = *static_cast<Chunk*>(user);
    *n = c.size; c.size = 0;
    return *n ? c.data : nullptr;
}
bool Run(lua_State* L, const std::string& code) {
    int top = g_lua.gettop(L);
    Chunk c = {code.data(), code.size()};
    int err = g_lua.load(L, ReadChunk, &c, "=battle_ai");
    if (!err) err = g_lua.pcall(L, 0, 0, 0);
    if (err) g_api->Log(g_mod, SDGO_LOG_ERROR, "Lua reload rejected; previous policy retained: %s",
                        g_lua.tostring(L, -1) ? g_lua.tostring(L, -1) : "unknown error");
    g_lua.settop(L, top);
    return err == 0;
}
void Register(lua_State* L, const char* name, LuaFn fn) {
    g_lua.pushstring(L, name); g_lua.pushcclosure(L, fn, 0); g_lua.settable(L, kGlobals);
}
int __cdecl Now(lua_State* L) { g_lua.pushnumber(L, static_cast<double>(GetTickCount64())); return 1; }
int __cdecl Log(lua_State* L) {
    const char* s = g_lua.tostring(L, 1);
    if (s) g_api->Log(g_mod, SDGO_LOG_INFO, "%s", s);
    return 0;
}
// Layouts belong only to the manifest's verified GOnline build. Remote copies
// (+2B65) must never run an additional decision loop. Lua also checks GetNPCID.
bool Eligible() {
    __try {
        return g_current && g_current[0x2B65] == 0 && g_current[0x326C] != 0 &&
               *reinterpret_cast<uint32_t*>(g_current + 0x3238) >= 3604001 &&
               *reinterpret_cast<uint32_t*>(g_current + 0x3238) <= 3607000;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int __cdecl Authority(lua_State* L) { g_lua.pushnumber(L, Eligible() ? 1 : 0); return 1; }
int __cdecl Generation(lua_State* L) {
    auto it = g_generations.find(g_current);
    g_lua.pushnumber(L, it == g_generations.end() ? 0 : static_cast<double>(it->second));
    return 1;
}
struct Option { const wchar_t* name; int def, low, high; };
const Option kOptions[] = {
    {L"Enabled",1,0,1}, {L"DecisionMs",150,50,1000}, {L"ReactionMs",300,0,3000},
    {L"StuckMs",1400,300,10000}, {L"ProgressDistance",30,1,300}, {L"ArrivalDistance",100,10,500},
    {L"RecoveryDistance",350,50,1500}, {L"RecoveryMs",650,150,2000}, {L"RecoveryCooldownMs",1000,250,10000},
    {L"RangedMinDistance",450,0,2000}, {L"TacticCooldownMs",900,300,10000}, {L"LogDecisions",0,0,1}
};
std::string ConfigChunk() {
    std::string code = "local config = {\n";
    for (const auto& o : kOptions) {
        wchar_t buf[64] = {}, *end = nullptr;
        GetPrivateProfileStringW(L"settings", o.name, L"", buf, 64, g_config.c_str());
        long value = *buf ? wcstol(buf, &end, 10) : o.def;
        while (end && (*end == L' ' || *end == L'\t')) ++end;
        if ((end && *end) || value < o.low || value > o.high) {
            value = o.def;
            g_api->Log(g_mod, SDGO_LOG_WARN, "invalid config.ini option %ls; using %d", o.name, o.def);
        }
        std::string key;
        for (const wchar_t* p = o.name; *p; ++p) key.push_back(static_cast<char>(*p));
        code += key + "=" + std::to_string(value) + ",\n";
    }
    return code + "}\n";
}
void Prepare() {
    lua_State* L = g_lua.state(nullptr);
    if (!L) return;
    ULONGLONG now = GetTickCount64();
    if (L == g_loaded && now < g_checkAt) return;
    g_checkAt = now + 2000;
    FILETIME st = Stamp(g_script), ct = Stamp(g_config);
    if (L == g_loaded && Same(st,g_scriptTime) && Same(ct,g_configTime)) {
        Run(L, "if SDGO_BattleAI_Refresh then SDGO_BattleAI_Refresh() end");
        return;
    }
    // Remember a failed attempt too: no compile/error spam each frame. An edit retries.
    g_loaded = L; g_scriptTime = st; g_configTime = ct;
    std::string script;
    if (!Read(g_script, script)) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "cannot read battle_ai.lua; native AI retained"); return;
    }
    Register(L,"SDGO_BattleAI_Now",Now);
    Register(L,"SDGO_BattleAI_Log",Log);
    Register(L,"SDGO_BattleAI_Authority",Authority);
    Register(L,"SDGO_BattleAI_Generation",Generation);
    if (Run(L, ConfigChunk() + script))
        g_api->Log(g_mod, SDGO_LOG_INFO, "policy loaded (config.ini); native VSAI decorators ready");
}
void GuardedPrepare() {
    __try { Prepare(); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        g_preparationFault = true;
        g_api->Log(g_mod, SDGO_LOG_ERROR, "policy preparation failed; native Think retained");
    }
}
void __fastcall Think(uint8_t* unit, void*) {
    if (!g_loading && !g_preparationFault) { g_loading = true; GuardedPrepare(); g_loading = false; }
    uint8_t* prior = g_current; g_current = unit;
    // Init can precede loading in fixtures or a late loader; give that unit an identity.
    if (g_generations.find(unit) == g_generations.end()) {
        if (g_generations.size() >= 1024) g_generations.clear();
        g_generations[unit] = ++g_serial;
    }
    g_think(unit);
    g_current = prior;
}
void __fastcall Init(uint8_t* unit, void*, void* data) {
    if (g_generations.size() >= 1024) g_generations.clear();
    g_generations[unit] = ++g_serial;
    g_init(unit, data);
}
}

SDGO_MOD_ENTRY {
    g_api = api; g_mod = mod;
    if (!Bind(g_lua.state,"lua_tinker.state") || !Bind(g_lua.gettop,"lua.gettop") ||
        !Bind(g_lua.settop,"lua.settop") || !Bind(g_lua.tostring,"lua.tostring") ||
        !Bind(g_lua.pushnumber,"lua.pushnumber") || !Bind(g_lua.pushstring,"lua.pushstring") ||
        !Bind(g_lua.pushcclosure,"lua.pushcclosure") || !Bind(g_lua.gettable,"lua.gettable") ||
        !Bind(g_lua.settable,"lua.settable") || !Bind(g_lua.pcall,"lua.pcall") || !Bind(g_lua.load,"lua.load")) return 0;
    std::wstring dir = api->ModDirectory(mod);
    g_script = dir + L"\\battle_ai.lua"; g_config = dir + L"\\config.ini";
    std::string check;
    if (!Read(g_script,check)) { api->Log(mod,SDGO_LOG_ERROR,"missing battle_ai.lua"); return 0; }
    if (!api->HookCode(mod,"npc.think",reinterpret_cast<void*>(&Think),reinterpret_cast<void**>(&g_think)) ||
        !api->HookCode(mod,"npc.init_ai",reinterpret_cast<void*>(&Init),reinterpret_cast<void**>(&g_init))) return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: local battle bots only; separate optional config.ini");
    return 1;
}
