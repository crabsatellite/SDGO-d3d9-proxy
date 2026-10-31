// findwindow_fix - normal FindWindowW for code other than the game.
//
// The repack's SDPlus.dll patches user32!FindWindowW with a routine that never looks
// for a window. It returns NULL only for class "_GONLINE_" with title "Gundam Online",
// the game's single-instance check, so several clients can run. Every other query
// gets -1 or +1, and a NULL or atom class name is dereferenced and crashes. Input
// methods and other DLLs in the game process that look up a window therefore hang on
// a bogus handle or crash.
//
// Calls from GOnline.exe (its only call is that check) and from SDPlus itself keep
// SDPlus's routine. Every other caller gets the normal Win32 lookup.

#include <windows.h>
#include <intrin.h>

#include <cstddef>
#include <cstdint>

#include "sdgo/sdgo_api.h"

#pragma intrinsic(_ReturnAddress)

namespace
{
    using FindWindowWFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);

    struct ModuleRange {
        uintptr_t begin = 0;
        uintptr_t end = 0;
        bool Contains(uintptr_t address) const { return address >= begin && address < end; }
    };

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    FindWindowWFn g_original = nullptr;
    ModuleRange g_host;
    ModuleRange g_sdplus;
    bool g_trace = false;
    volatile LONG g_reported = 0;

    ModuleRange RangeOf(const void* module)
    {
        auto base = static_cast<const BYTE*>(module);
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        auto begin = reinterpret_cast<uintptr_t>(base);
        return {begin, begin + nt->OptionalHeader.SizeOfImage};
    }

    HWND WINAPI FixedFindWindowW(LPCWSTR className, LPCWSTR title)
    {
        auto caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (g_host.Contains(caller) || g_sdplus.Contains(caller))
            return g_original(className, title);

        // FindWindowExW(NULL, NULL, ...) would also return message-only windows;
        // FindWindowW searches top-level windows only, so name the desktop as parent.
        // FindWindowW leaves the last error alone; keep that guarantee.
        DWORD lastError = GetLastError();
        HWND window = FindWindowExW(GetDesktopWindow(), nullptr, className, title);
        if (g_trace || (window && InterlockedCompareExchange(&g_reported, 1, 0) == 0))
            g_api->Log(g_mod, SDGO_LOG_INFO, "window lookup for an external caller: %p", window);
        SetLastError(lastError);
        return window;
    }
}

SDGO_MOD_ENTRY
{
    if (api->size < offsetof(SdgoApi, HookSystemExport) + sizeof(api->HookSystemExport) || !api->HookSystemExport) {
        api->Log(mod, SDGO_LOG_ERROR, "core lacks HookSystemExport; update sdgo/core.dll together with this mod");
        return 0;
    }
    // GOnline.exe imports SDPlus.dll, so it is loaded and has patched FindWindowW by now.
    HMODULE sdplus = GetModuleHandleW(L"SDPlus.dll");
    if (!sdplus) {
        api->Log(mod, SDGO_LOG_INFO, "SDPlus.dll is not loaded; nothing to fix");
        return 0;
    }
    g_api = api;
    g_mod = mod;
    g_host = RangeOf(api->host_base);
    g_sdplus = RangeOf(sdplus);
    g_trace = api->SettingInt(mod, "Trace", 0) != 0;
    if (!api->HookSystemExport(mod, L"user32.dll", "FindWindowW", reinterpret_cast<void*>(&FixedFindWindowW),
                               reinterpret_cast<void**>(&g_original)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: normal FindWindowW for every caller except GOnline.exe and SDPlus.dll");
    return 1;
}
