#include "d3d9_proxy.h"
#include "config.h"
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

// Real d3d9.dll handle and function pointer
static HMODULE g_hRealD3D9 = nullptr;
typedef IDirect3D9* (WINAPI* PFN_Direct3DCreate9)(UINT SDKVersion);
static PFN_Direct3DCreate9 g_pfnDirect3DCreate9 = nullptr;

ProxyConfig g_Config;

static void LoadRealD3D9()
{
    if (g_hRealD3D9)
        return;

    // Load the real d3d9.dll from system32
    char systemDir[MAX_PATH];
    GetSystemDirectoryA(systemDir, MAX_PATH);
    strcat_s(systemDir, "\\d3d9.dll");

    g_hRealD3D9 = LoadLibraryA(systemDir);
    if (!g_hRealD3D9) {
        MessageBoxA(nullptr, "Failed to load system d3d9.dll", "D3D9 Proxy", MB_ICONERROR);
        return;
    }

    g_pfnDirect3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_hRealD3D9, "Direct3DCreate9");
}

// Exported function - replaces the real Direct3DCreate9
extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT SDKVersion)
{
    LoadRealD3D9();

    if (!g_pfnDirect3DCreate9)
        return nullptr;

    IDirect3D9* pReal = g_pfnDirect3DCreate9(SDKVersion);
    if (!pReal)
        return nullptr;

    // Wrap the real IDirect3D9 with our proxy
    return new Direct3D9Proxy(pReal);
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpvReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);

        // Load config from INI file next to the DLL
        {
            char dllPath[MAX_PATH];
            GetModuleFileNameA(hinstDLL, dllPath, MAX_PATH);
            // Replace .dll with .ini
            char* dot = strrchr(dllPath, '.');
            if (dot) strcpy_s(dot, 5, ".ini");
            g_Config.Load(dllPath);
        }

        if (g_Config.enableLog) {
            FILE* f = fopen("d3d9_proxy.log", "w");
            if (f) {
                fprintf(f, "D3D9 Proxy loaded\n");
                fclose(f);
            }
        }
        break;

    case DLL_PROCESS_DETACH:
        if (g_hRealD3D9) {
            FreeLibrary(g_hRealD3D9);
            g_hRealD3D9 = nullptr;
        }
        break;
    }
    return TRUE;
}
