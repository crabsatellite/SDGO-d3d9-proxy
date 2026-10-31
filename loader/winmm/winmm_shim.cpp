// winmm.dll loader shim for the battle server.
//
// BattleServer.exe imports a single function from WINMM.dll (timeGetTime), so Windows
// loads this file from the server directory before the server's own code runs. Like
// the client's d3d9.dll shim it has two jobs:
//   1. Forward timeGetTime to the real system32\winmm.dll, or to another winmm wrapper
//      named in sdgo.ini [loader] chain_winmm.
//   2. Start sdgo\core.dll as early as possible (loader/common/early_boot).
// If the early-boot import is missing, the first timeGetTime call boots instead.

#include <windows.h>

#include "../common/early_boot.h"

namespace
{
    using TimeGetTimeFn = DWORD(WINAPI*)();
    TimeGetTimeFn g_timeGetTime = nullptr;
    INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT;

    BOOL CALLBACK LoadRealCallback(PINIT_ONCE, PVOID, PVOID*)
    {
        if (HMODULE real = LoadForwardTarget(L"chain_winmm", L"winmm.dll"))
            g_timeGetTime = reinterpret_cast<TimeGetTimeFn>(GetProcAddress(real, "timeGetTime"));
        if (!g_timeGetTime)
            EarlyBootTrace(L"the winmm.dll forwarded to has no timeGetTime");
        return TRUE;
    }
}

extern "C" DWORD WINAPI Shim_timeGetTime()
{
    EarlyBoot();
    InitOnceExecuteOnce(&g_realOnce, LoadRealCallback, nullptr, nullptr);
    return g_timeGetTime ? g_timeGetTime() : GetTickCount();
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        EarlyBootInstall("winmm");
    }
    // Process-lifetime module: nothing is torn down under the loader lock.
    return TRUE;
}
