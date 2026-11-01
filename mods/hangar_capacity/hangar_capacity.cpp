#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"
#include "capacity_policy.h"
#include "oledb_session.h"

namespace {
namespace capacity = sdgo::hangar_capacity;
#pragma pack(push, 1)
struct CountArgs {
    std::int32_t account;
    std::int16_t rawExtra;
    std::int32_t procedureResult;
};
#pragma pack(pop)
static_assert(sizeof(CountArgs) == 10 && offsetof(CountArgs, rawExtra) == 4 &&
              offsetof(CountArgs, procedureResult) == 6, "native capacity ABI changed");
using GetCountFn = int(__thiscall*)(void*, void*, CountArgs*);
GetCountFn g_original = nullptr;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
std::wstring g_database;
int g_lockTimeout = 1000;
unsigned g_queryTimeout = 3;
std::atomic<unsigned> g_errors{0};
std::atomic<bool> g_announced{false};

struct LastError {
    DWORD value = GetLastError();
    ~LastError() { SetLastError(value); }
    void BeforeNative() const { SetLastError(value); }
    void AfterNative() { value = GetLastError(); }
};
bool ReadAccount(const CountArgs* input, std::int32_t* account) {
    __try { if (!input) return false; std::memcpy(account, input, sizeof(*account)); return *account > 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool ReadExtra(const CountArgs* input, std::int16_t* extra) {
    __try { if (!input) return false; std::memcpy(extra, reinterpret_cast<const BYTE*>(input) + 4, sizeof(*extra)); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool EnsureNativeCapacity(void* connection, std::int32_t account) {
    const auto sql = capacity::MakeEnsureSql(g_database, account, g_lockTimeout);
    if (!sql) return false;
    capacity::PolicySession session(g_queryTimeout);
    return session.Open(connection) && session.Ensure(*sql);
}
using EnsureFn = bool (*)(void*, std::int32_t);
EnsureFn g_ensure = &EnsureNativeCapacity; // fixture injection; never exported
void Failed() noexcept {
    try {
        if (g_errors.fetch_add(1) < 5 && g_api && g_api->Log)
            g_api->Log(g_mod, SDGO_LOG_ERROR, "hangar capacity policy unavailable; native request reports a database failure (check schema, rights and session state)");
    } catch (...) {}
}
int __fastcall GetCount(void* self, void*, void* connection, CountArgs* args) {
    LastError error;
    std::int32_t account = 0;
    if (!ReadAccount(args, &account) || !connection) { Failed(); return 1; }
    bool ensured = false;
    try { ensured = g_ensure(connection, account); }
    catch (...) { ensured = false; }
    if (!ensured) { Failed(); return 1; }
    error.BeforeNative();
    const int result = g_original(self, connection, args);
    error.AfterNative();
    if (result != 0) return result;
    std::int16_t extra = 0;
    if (!ReadExtra(args, &extra) || extra < capacity::kTargetExtra) { Failed(); return 1; }
    try {
        if (!g_announced.exchange(true) && g_api && g_api->Log)
            g_api->Log(g_mod, SDGO_LOG_INFO, "capacity policy verified: at least 1200 total slots, original account data and getter reply retained");
    } catch (...) {}
    return result;
}
bool ReadNumber(const sdgo::ModSettings& config, const char* key, const char* fallback,
                unsigned low, unsigned high, unsigned& result) {
    char text[24]{};
    const auto size = config.String(key, fallback, text, sizeof(text));
    if (!size || size >= sizeof(text) - 1) return false;
    unsigned value = 0;
    for (unsigned i = 0; i < size; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const auto digit = static_cast<unsigned>(text[i] - '0');
        if (value > high / 10 || (value == high / 10 && digit > high % 10)) return false;
        value = value * 10 + digit;
    }
    if (value < low) return false;
    result = value; return true;
}
} // namespace

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, ModDirectory) + sizeof(api->ModDirectory) ||
        !api->Symbol || !api->HookCode || !api->Log || !api->ModDirectory || !api->SettingString) return 0;
    g_api = api; g_mod = mod;
    try {
        sdgo::ModSettings config(api, mod);
        unsigned lock = 0, query = 0;
        if (!ReadNumber(config, "LockTimeoutMs", "1000", 0, 5000, lock) ||
            !ReadNumber(config, "QueryTimeoutSeconds", "3", 1, 10, query) ||
            lock >= query * 1000) return 0;
        g_lockTimeout = static_cast<int>(lock); g_queryTimeout = query;
        wchar_t path[32768]{}, database[130]{};
        const DWORD pathLength = GetModuleFileNameW(nullptr, path, 32768);
        if (!pathLength || pathLength >= 32768) return 0;
        std::wstring ini(path);
        const auto dot = ini.find_last_of(L'.');
        if (dot == std::wstring::npos) return 0;
        ini.replace(dot, std::wstring::npos, L".ini");
        const auto count = GetPrivateProfileStringW(L"DB Game", L"DBName", L"", database, 130, ini.c_str());
        if (!count || count > 128) {
            api->Log(mod, SDGO_LOG_ERROR, "need [DB Game] DBName in the host INI; no database credentials are copied");
            return 0;
        }
        g_database.assign(database, count);
        if (!capacity::MakeEnsureSql(g_database, 1, g_lockTimeout)) return 0;
        if (!api->Symbol(mod, "hangar.capacity.connection_command") ||
            !api->Symbol(mod, "hangar.capacity.args_layout") ||
            !api->Symbol(mod, "hangar.capacity.args_bindings") ||
            !api->Symbol(mod, "hangar.capacity.request_args")) return 0;
        if (!api->HookCode(mod, "hangar.capacity.get_count", reinterpret_cast<void*>(&GetCount),
                reinterpret_cast<void**>(&g_original))) return 0;
        api->Log(mod, SDGO_LOG_INFO, "automatic 1200-slot policy registered; applies before each native inventory capacity read");
        return 1;
    } catch (...) { return 0; }
}
