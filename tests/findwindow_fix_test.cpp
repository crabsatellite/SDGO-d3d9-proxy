// Integration test of the real core and findwindow_fix. Before the core loads, an
// independent MinHook instance replaces FindWindowW with a copy of SDPlus's routine
// (string compare, no NULL check), as the repack does. All windows live on a private
// desktop. This exe plays GOnline.exe; SDPlus.dll and generic_input_method.dll are
// caller fixtures (tests/findwindow_caller_fixture.cpp).
#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cwchar>
#include <string>

#include "sdgo/sdgo_api.h"

namespace
{
    constexpr wchar_t kGameClass[] = L"_GONLINE_";
    constexpr wchar_t kGameTitle[] = L"Gundam Online";
    constexpr wchar_t kClass[] = L"C31F6480-11D4-44AB-B4C6-282DDB42F3AA";
    constexpr wchar_t kTitle[] = L"4B59FF7B-1D1C-4224-ACDB-EC6CC404562F";
    constexpr DWORD kIncomingError = 0x1234;
    LONG g_sdplusCalls = 0;
    DWORD g_sdplusIncoming = 0;
    LPCWSTR g_sdplusClass = nullptr;
    LPCWSTR g_sdplusTitle = nullptr;
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++g_failures;
    }

    HWND SdplusResult(LPCWSTR className, LPCWSTR title)
    {
        int c = wcscmp(className, kGameClass);
        if (!c) c = wcscmp(title, kGameTitle);
        return c ? reinterpret_cast<HWND>(static_cast<INT_PTR>(c < 0 ? -1 : 1)) : nullptr;
    }

    // SDPlus.dll+0x3020: never calls Windows, dereferences both strings unchecked.
    HWND WINAPI SdplusFindWindowW(LPCWSTR className, LPCWSTR title)
    {
        g_sdplusIncoming = GetLastError();
        g_sdplusClass = className;
        g_sdplusTitle = title;
        InterlockedIncrement(&g_sdplusCalls);
        return SdplusResult(className, title);
    }

    bool IsBogus(HWND window)
    {
        INT_PTR value = reinterpret_cast<INT_PTR>(window);
        return value == -1 || value == 1;
    }

    void FakeLog(SdgoMod*, int, const char*, ...) {}

    void CheckOldCore(const std::wstring& modPath)
    {
        HMODULE mod = LoadLibraryW(modPath.c_str());
        auto load = mod ? reinterpret_cast<SdgoModLoadFn>(GetProcAddress(mod, "SdgoModLoad")) : nullptr;
        Check(load != nullptr, "mod export exists");
        if (load) {
            SdgoApi api = {};
            api.size = static_cast<uint32_t>(offsetof(SdgoApi, HookSystemExport));
            api.version = 1;
            api.Log = FakeLog;
            Check(load(&api, nullptr) == 0, "mod declines an old core without reading the absent field");
            api.size = sizeof(api);
            Check(load(&api, nullptr) == 0, "mod declines a null HookSystemExport");
        }
        if (mod) FreeLibrary(mod);
    }

    HWND CreateTopLevel(LPCWSTR className, LPCWSTR title)
    {
        return CreateWindowExW(0, className, title, WS_OVERLAPPED, 0, 0, 10, 10, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    }
}

int wmain(int argc, wchar_t** argv)
{
    bool disabled = argc > 1 && !wcscmp(argv[1], L"disabled");
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    *wcsrchr(path, L'\\') = L'\0';
    std::wstring root = path;
    CheckOldCore(root + L"\\sdgo\\mods\\findwindow_fix\\findwindow_fix.dll");

    wchar_t desktopName[80];
    swprintf_s(desktopName, L"sdgo_findwindow_test_%lu", GetCurrentProcessId());
    HDESK originalDesktop = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(desktopName, nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) {
        std::printf("FAIL private desktop: %lu\n", GetLastError());
        return 2;
    }

    void* target = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "FindWindowW"));
    void* unusedOriginal = nullptr;
    if (MH_Initialize() != MH_OK || MH_CreateHook(target, reinterpret_cast<void*>(&SdplusFindWindowW),
                                                &unusedOriginal) != MH_OK || MH_EnableHook(target) != MH_OK) {
        std::puts("FAIL cannot install the simulated SDPlus hook");
        return 2;
    }
    Check(FindWindowW(kGameClass, kGameTitle) == nullptr, "simulated SDPlus: single-instance query gives NULL");
    Check(IsBogus(FindWindowW(kClass, kTitle)), "simulated SDPlus: any other query gives a bogus handle");

    using FindFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
    // GOnline.exe imports SDPlus.dll, so it is loaded before the core.
    HMODULE sdplus = LoadLibraryW((root + L"\\SDPlus.dll").c_str());
    auto sdplusFind = sdplus ? reinterpret_cast<FindFn>(GetProcAddress(sdplus, "_Lookup@8")) : nullptr;
    if (!sdplusFind) { std::puts("FAIL SDPlus fixture missing"); return 2; }

    HMODULE core = LoadLibraryW((root + L"\\sdgo\\core.dll").c_str());
    auto init = core ? reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core, "SdgoCoreInit")) : nullptr;
    if (!init) { std::puts("FAIL core export missing"); return 2; }
    SdgoBoot boot = {sizeof(SdgoBoot), "findwindow_fix_test", GetModuleHandleW(nullptr)};
    Check(init(&boot) != 0, "real core initializes with the test manifests");

    // Input methods load after the core (on activation).
    HMODULE input = LoadLibraryW((root + L"\\generic_input_method.dll").c_str());
    auto inputFind = input ? reinterpret_cast<FindFn>(GetProcAddress(input, "_Lookup@8")) : nullptr;
    auto inputDynamicFind = input ? reinterpret_cast<FindFn>(GetProcAddress(input, "_DynamicLookup@8")) : nullptr;
    if (!inputFind || !inputDynamicFind) { std::puts("FAIL input fixture missing"); return 2; }

    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kGameClass;
    RegisterClassW(&wc);
    wc.lpszClassName = kClass;
    ATOM atom = RegisterClassW(&wc);
    HWND game = CreateTopLevel(kGameClass, kGameTitle);
    Check(game && atom, "game window and server class created");

    Check(FindWindowW(kGameClass, kGameTitle) == nullptr,
          "game exe: single-instance query still NULL with a game window present (several clients)");
    Check(IsBogus(sdplusFind(kClass, kTitle)), "SDPlus's own calls keep its routine");
    if (disabled) {
        Check(IsBogus(inputFind(kClass, kTitle)), "disabled: an input DLL still gets SDPlus's bogus handle");
    } else {
        LONG calls = g_sdplusCalls;
        Check(inputFind(kGameClass, kGameTitle) == game, "input DLL finds the real game window");
        SetLastError(kIncomingError);
        Check(inputFind(kClass, kTitle) == nullptr, "missing server gives NULL");
        Check(GetLastError() == kIncomingError, "lookup leaves the last error alone");

        HWND messageOnly = CreateWindowExW(0, kClass, kTitle, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                           wc.hInstance, nullptr);
        Check(messageOnly && FindWindowExW(HWND_MESSAGE, nullptr, kClass, kTitle) == messageOnly,
              "message-only window with the server's class and title exists");
        Check(inputFind(kClass, kTitle) == nullptr, "lookup ignores message-only windows");

        HWND server = CreateTopLevel(kClass, kTitle);
        Check(server != nullptr, "server window created after the fix loaded");
        Check(inputFind(kClass, kTitle) == server, "imported FindWindowW finds the server");
        Check(inputDynamicFind(kClass, kTitle) == server, "GetProcAddress FindWindowW finds the server");
        Check(inputFind(kClass, nullptr) == server, "class-only lookup");
        Check(inputFind(L"c31f6480-11d4-44ab-b4c6-282ddb42f3aa", L"4b59ff7b-1d1c-4224-acdb-ec6cc404562f") == server,
              "lookup is case-insensitive");
        Check(inputFind(MAKEINTRESOURCEW(atom), kTitle) == server, "class atom (SDPlus would crash)");
        Check(inputFind(nullptr, kTitle) == server, "NULL class, title only (SDPlus would crash)");
        Check(inputFind(L"unrelated", L"other") == nullptr, "unrelated query gives NULL");
        Check(g_sdplusCalls == calls, "no external lookup reached SDPlus's routine");

        // String arguments only: the game never passes NULL, and SDPlus would crash on it.
        const struct { LPCWSTR cls; LPCWSTR title; } queries[] = {
            {kGameClass, kGameTitle}, {kClass, kTitle}, {kClass, L"other"}, {L"unrelated", L"other"}
        };
        for (const auto& query : queries) {
            calls = g_sdplusCalls;
            SetLastError(kIncomingError);
            HWND result = reinterpret_cast<FindFn>(target)(query.cls, query.title);
            Check(result == SdplusResult(query.cls, query.title) && g_sdplusCalls == calls + 1 &&
                      g_sdplusIncoming == kIncomingError && g_sdplusClass == query.cls && g_sdplusTitle == query.title,
                  "game exe query goes to SDPlus once, with its arguments and last error");
            SetLastError(kIncomingError);
            HWND expected = FindWindowExW(GetDesktopWindow(), nullptr, query.cls, query.title);
            SetLastError(kIncomingError);
            HWND external = inputDynamicFind(query.cls, query.title);
            Check(external == expected && GetLastError() == kIncomingError && g_sdplusCalls == calls + 1,
                  "same query from an input DLL gets the normal lookup");
        }
        DestroyWindow(server);
        Check(inputFind(kClass, kTitle) == nullptr, "server closed: NULL again");
        DestroyWindow(messageOnly);
    }
    DestroyWindow(game);
    UnregisterClassW(kClass, wc.hInstance);
    UnregisterClassW(kGameClass, wc.hInstance);
    // Core and mods live for the whole process; the simulated SDPlus hook stays in
    // place because the core's trampoline still chains to it.
    SetThreadDesktop(originalDesktop);
    CloseDesktop(desktop);
    std::puts(g_failures ? "FINDWINDOW CHECKS FAILED" : "ALL FINDWINDOW CHECKS PASSED");
    return g_failures ? 1 : 0;
}
