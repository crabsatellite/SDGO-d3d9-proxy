// d3d9.dll loader shim.
//
// GOnline.exe imports Direct3DCreate9 from d3d9.dll, so Windows loads this
// file from the game directory before the game's own code runs. The shim has
// exactly two jobs:
//   1. Forward the d3d9 exports to the real system32\d3d9.dll, or to another d3d9
//      wrapper named in sdgo.ini [loader] chain_d3d9.
//   2. Start sdgo\core.dll as early as possible (loader/common/early_boot).
// If the early-boot import is missing, the first Direct3DCreate9 call boots instead.

#include <windows.h>
#include <d3d9.h>

#include "../common/early_boot.h"

namespace
{
    INIT_ONCE g_realOnce = INIT_ONCE_STATIC_INIT;
    HMODULE g_real = nullptr;

    BOOL CALLBACK LoadRealCallback(PINIT_ONCE, PVOID, PVOID*)
    {
        g_real = LoadForwardTarget(L"chain_d3d9", L"d3d9.dll");
        if (!g_real)
            EarlyBootTrace(L"no d3d9.dll to forward to could be loaded");
        return TRUE;
    }

    template <typename Fn>
    Fn Real(const char* name)
    {
        InitOnceExecuteOnce(&g_realOnce, LoadRealCallback, nullptr, nullptr);
        return g_real ? reinterpret_cast<Fn>(GetProcAddress(g_real, name)) : nullptr;
    }
}

extern "C" IDirect3D9* WINAPI Shim_Direct3DCreate9(UINT sdkVersion)
{
    EarlyBoot();
    auto fn = Real<IDirect3D9*(WINAPI*)(UINT)>("Direct3DCreate9");
    return fn ? fn(sdkVersion) : nullptr;
}

extern "C" HRESULT WINAPI Shim_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out)
{
    EarlyBoot();
    auto fn = Real<HRESULT(WINAPI*)(UINT, IDirect3D9Ex**)>("Direct3DCreate9Ex");
    return fn ? fn(sdkVersion, out) : D3DERR_NOTAVAILABLE;
}

extern "C" int WINAPI Shim_D3DPERF_BeginEvent(D3DCOLOR color, LPCWSTR name)
{
    auto fn = Real<int(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_BeginEvent");
    return fn ? fn(color, name) : -1;
}

extern "C" int WINAPI Shim_D3DPERF_EndEvent()
{
    auto fn = Real<int(WINAPI*)()>("D3DPERF_EndEvent");
    return fn ? fn() : -1;
}

extern "C" DWORD WINAPI Shim_D3DPERF_GetStatus()
{
    auto fn = Real<DWORD(WINAPI*)()>("D3DPERF_GetStatus");
    return fn ? fn() : 0;
}

extern "C" BOOL WINAPI Shim_D3DPERF_QueryRepeatFrame()
{
    auto fn = Real<BOOL(WINAPI*)()>("D3DPERF_QueryRepeatFrame");
    return fn ? fn() : FALSE;
}

extern "C" void WINAPI Shim_D3DPERF_SetMarker(D3DCOLOR color, LPCWSTR name)
{
    if (auto fn = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetMarker"))
        fn(color, name);
}

extern "C" void WINAPI Shim_D3DPERF_SetOptions(DWORD options)
{
    if (auto fn = Real<void(WINAPI*)(DWORD)>("D3DPERF_SetOptions"))
        fn(options);
}

extern "C" void WINAPI Shim_D3DPERF_SetRegion(D3DCOLOR color, LPCWSTR name)
{
    if (auto fn = Real<void(WINAPI*)(D3DCOLOR, LPCWSTR)>("D3DPERF_SetRegion"))
        fn(color, name);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        EarlyBootInstall("d3d9");
    }
    // Process-lifetime module: nothing is torn down under the loader lock.
    return TRUE;
}
