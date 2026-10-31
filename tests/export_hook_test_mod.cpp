// Loaded only by findwindow_fix_test.ps1. Exercises the public HookSystemExport API,
// then declines to load so the core must restore the pre-existing external hook.
#include <windows.h>
#include "sdgo/sdgo_api.h"

namespace
{
    using FindFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
    FindFn g_original;
    HWND WINAPI Detour(LPCWSTR, LPCWSTR) { return reinterpret_cast<HWND>(123); }
}

SDGO_MOD_ENTRY
{
    void* original = nullptr;
    if (api->size < sizeof(SdgoApi) || !api->HookSystemExport)
        return 0;
#define REQUIRE(condition) do { if (!(condition)) { \
    api->Log(mod, SDGO_LOG_ERROR, "TEST FAILURE: %s", #condition); return 0; } } while (0)
    REQUIRE(!api->HookSystemExport(mod, L"user32.dll", "sdgo_missing_export", reinterpret_cast<void*>(&Detour), &original));
    REQUIRE(!api->HookSystemExport(mod, L"sdgo_missing_system_dll.dll", "FindWindowW", reinterpret_cast<void*>(&Detour), &original));
    REQUIRE(!api->HookSystemExport(mod, L"..\\user32.dll", "FindWindowW", reinterpret_cast<void*>(&Detour), &original));
    REQUIRE(api->HookSystemExport(mod, L"user32.dll", "FindWindowW", reinterpret_cast<void*>(&Detour),
                                   reinterpret_cast<void**>(&g_original)));
    // The trampoline reaches the simulated SDPlus routine (it crashes on NULL strings).
    REQUIRE(g_original(L"_GONLINE_", L"Gundam Online") == nullptr);
    REQUIRE(g_original(L"x", L"y") == reinterpret_cast<HWND>(1));
    REQUIRE(FindWindowW(nullptr, nullptr) == reinterpret_cast<HWND>(123));
    // A second hook must be refused while the site is owned.
    REQUIRE(!api->HookSystemExport(mod, L"user32.dll", "FindWindowW", reinterpret_cast<void*>(&Detour), &original));
    api->Log(mod, SDGO_LOG_INFO, "export API checks passed; deliberately decline for rollback");
    return 0;
}
