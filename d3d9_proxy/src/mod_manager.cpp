#include "mod_manager.h"
#include "hook.h"
#include "config.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <cstring>

// ============================================================
// Static API function implementations
// (These are called by mod DLLs via the SdgoModAPI struct)
// ============================================================

// Global pointer to the active ModManager (only one instance)
static ModManager* g_pModManager = nullptr;

// Fullscreen quad helper (same as PostProcessor::DrawQuad)
struct ModScreenVertex {
    float x, y, z, w;
    float u, v;
};

static void API_DrawFullscreenQuad(IDirect3DDevice9* pDevice, float w, float h)
{
    ModScreenVertex quad[4] = {
        { -0.5f,    -0.5f,    0.5f, 1.0f, 0.0f, 0.0f },
        { w - 0.5f, -0.5f,    0.5f, 1.0f, 1.0f, 0.0f },
        { -0.5f,    h - 0.5f, 0.5f, 1.0f, 0.0f, 1.0f },
        { w - 0.5f, h - 0.5f, 0.5f, 1.0f, 1.0f, 1.0f },
    };
    pDevice->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ModScreenVertex));
}

static bool API_RegisterPostPass(const char* name, PostPassFn fn, int priority, void* ud)
{
    if (!g_pModManager || !name || !fn) return false;
    // Access via friend-like pattern (ModManager exposes internals to API)
    // We'll use a callback stored in the manager
    return true;  // Implemented below via ModManager methods
}

// ---- Actual API callbacks (using lambdas stored as function pointers) ----

// We need a way for the static API functions to reach the ModManager.
// Using the global g_pModManager pointer.

namespace ModAPI_Impl
{
    static bool RegisterPostPass(const char* name, PostPassFn fn, int priority, void* ud);
    static void UnregisterPostPass(const char* name);
    static bool RegisterDrawHook(const char* name, DrawHookFn fn, void* ud);
    static void UnregisterDrawHook(const char* name);
    static bool RegisterFrameHook(const char* name, FrameHookFn fn, void* ud);
    static void UnregisterFrameHook(const char* name);

    static bool HookIATFunc(const char* module, const char* function,
                            void* detour, void** original);
    static bool HookInlineFunc(void* target, void* detour, void** original,
                               int stolenBytes);
    static void UnhookFunc(void** original);

    static IDirect3DDevice9* GetDevice();
    static IDirect3DTexture9* GetDepthTexture();
    static void GetProjParams(float* nearZ, float* farZ);

    static IDirect3DPixelShader9* CompilePixelShader(const char* hlsl,
                                                      const char* entryPoint);

    static float GetConfigFloat(const char* section, const char* key, float def);
    static int GetConfigInt(const char* section, const char* key, int def);
    static bool GetConfigBool(const char* section, const char* key, bool def);

    static void Log(const char* fmt, ...);

    static void* GetModuleHandleFunc(const char* moduleName);
    static void* GetExportAddress(const char* moduleName, const char* funcName);
    static IDirect3DTexture9* GetPreTransparentTexture();
}

// ============================================================
// API Implementation
// ============================================================

bool ModAPI_Impl::RegisterPostPass(const char* name, PostPassFn fn, int priority, void* ud)
{
    if (!g_pModManager) return false;
    g_pModManager->AddPostPass(name, fn, priority, ud);
    Log("Registered post-pass: %s (priority %d)", name, priority);
    return true;
}

void ModAPI_Impl::UnregisterPostPass(const char* name)
{
    if (!g_pModManager || !name) return;
    g_pModManager->RemovePostPass(name);
}

bool ModAPI_Impl::RegisterDrawHook(const char* name, DrawHookFn fn, void* ud)
{
    if (!g_pModManager) return false;
    g_pModManager->AddDrawHook(name, fn, ud);
    Log("Registered draw hook: %s", name);
    return true;
}

void ModAPI_Impl::UnregisterDrawHook(const char* name)
{
    if (!g_pModManager || !name) return;
    g_pModManager->RemoveDrawHook(name);
}

bool ModAPI_Impl::RegisterFrameHook(const char* name, FrameHookFn fn, void* ud)
{
    if (!g_pModManager) return false;
    g_pModManager->AddFrameHook(name, fn, ud);
    Log("Registered frame hook: %s", name);
    return true;
}

void ModAPI_Impl::UnregisterFrameHook(const char* name)
{
    if (!g_pModManager || !name) return;
    g_pModManager->RemoveFrameHook(name);
}

bool ModAPI_Impl::HookIATFunc(const char* module, const char* function,
                              void* detour, void** original)
{
    HMODULE hGame = ::GetModuleHandleA(nullptr);  // GOnline.exe
    return Hook::HookIAT(hGame, module, function, detour, original);
}

bool ModAPI_Impl::HookInlineFunc(void* target, void* detour, void** original,
                                 int stolenBytes)
{
    return Hook::HookInline(target, detour, original, stolenBytes);
}

void ModAPI_Impl::UnhookFunc(void** original)
{
    Hook::UnhookInline(original);
}

IDirect3DDevice9* ModAPI_Impl::GetDevice()
{
    return g_pModManager ? g_pModManager->GetDevice() : nullptr;
}

IDirect3DTexture9* ModAPI_Impl::GetDepthTexture()
{
    return g_pModManager ? g_pModManager->GetDepth() : nullptr;
}

void ModAPI_Impl::GetProjParams(float* nearZ, float* farZ)
{
    if (!g_pModManager) return;
    if (nearZ) *nearZ = g_pModManager->GetNearZ();
    if (farZ)  *farZ  = g_pModManager->GetFarZ();
}

IDirect3DPixelShader9* ModAPI_Impl::CompilePixelShader(const char* hlsl,
                                                        const char* entryPoint)
{
    if (!g_pModManager || !g_pModManager->GetDevice() || !hlsl || !entryPoint)
        return nullptr;

    ID3DBlob* pCode = nullptr;
    ID3DBlob* pErrors = nullptr;

    HRESULT hr = D3DCompile(hlsl, strlen(hlsl), nullptr, nullptr, nullptr,
        entryPoint, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pCode, &pErrors);

    if (FAILED(hr)) {
        if (pErrors) {
            Log("Shader compile error: %s", (const char*)pErrors->GetBufferPointer());
            pErrors->Release();
        }
        return nullptr;
    }

    IDirect3DPixelShader9* pShader = nullptr;
    g_pModManager->GetDevice()->CreatePixelShader(
        (const DWORD*)pCode->GetBufferPointer(), &pShader);
    pCode->Release();
    if (pErrors) pErrors->Release();
    return pShader;
}

float ModAPI_Impl::GetConfigFloat(const char* section, const char* key, float def)
{
    if (!g_pModManager) return def;
    char buf[32], defStr[32];
    sprintf_s(defStr, "%.6f", def);
    GetPrivateProfileStringA(section, key, defStr, buf, sizeof(buf), g_pModManager->GetIniPath());
    return (float)atof(buf);
}

int ModAPI_Impl::GetConfigInt(const char* section, const char* key, int def)
{
    if (!g_pModManager) return def;
    return GetPrivateProfileIntA(section, key, def, g_pModManager->GetIniPath());
}

bool ModAPI_Impl::GetConfigBool(const char* section, const char* key, bool def)
{
    if (!g_pModManager) return def;
    char buf[16];
    GetPrivateProfileStringA(section, key, def ? "1" : "0", buf, sizeof(buf), g_pModManager->GetIniPath());
    return buf[0] == '1' || buf[0] == 't' || buf[0] == 'T';
}

void ModAPI_Impl::Log(const char* fmt, ...)
{
    if (!g_Config.enableLog) return;
    FILE* f = fopen("d3d9_proxy.log", "a");
    if (!f) return;
    fprintf(f, "[Mod] ");
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

void* ModAPI_Impl::GetModuleHandleFunc(const char* moduleName)
{
    return (void*)::GetModuleHandleA(moduleName);
}

void* ModAPI_Impl::GetExportAddress(const char* moduleName, const char* funcName)
{
    HMODULE hMod = ::GetModuleHandleA(moduleName);
    if (!hMod) return nullptr;
    return (void*)::GetProcAddress(hMod, funcName);
}

IDirect3DTexture9* ModAPI_Impl::GetPreTransparentTexture()
{
    if (!g_pModManager) return nullptr;
    return g_pModManager->GetPreTransTexture();
}

// ============================================================
// ModManager Implementation
// ============================================================

void ModManager::AddPostPass(const char* name, PostPassFn fn, int priority, void* ud)
{
    m_postPasses.push_back({name, fn, priority, ud});
    std::sort(m_postPasses.begin(), m_postPasses.end(),
              [](const PostPassEntry& a, const PostPassEntry& b) { return a.priority < b.priority; });
}

void ModManager::RemovePostPass(const char* name)
{
    m_postPasses.erase(std::remove_if(m_postPasses.begin(), m_postPasses.end(),
        [name](const PostPassEntry& e) { return e.name == name; }), m_postPasses.end());
}

void ModManager::AddDrawHook(const char* name, DrawHookFn fn, void* ud)
{
    m_drawHooks.push_back({name, fn, ud});
}

void ModManager::RemoveDrawHook(const char* name)
{
    m_drawHooks.erase(std::remove_if(m_drawHooks.begin(), m_drawHooks.end(),
        [name](const DrawHookEntry& e) { return e.name == name; }), m_drawHooks.end());
}

void ModManager::AddFrameHook(const char* name, FrameHookFn fn, void* ud)
{
    m_frameHooks.push_back({name, fn, ud});
}

void ModManager::RemoveFrameHook(const char* name)
{
    m_frameHooks.erase(std::remove_if(m_frameHooks.begin(), m_frameHooks.end(),
        [name](const FrameHookEntry& e) { return e.name == name; }), m_frameHooks.end());
}

void ModManager::BuildAPI()
{
    m_api.RegisterPostPass    = ModAPI_Impl::RegisterPostPass;
    m_api.UnregisterPostPass  = ModAPI_Impl::UnregisterPostPass;
    m_api.RegisterDrawHook    = ModAPI_Impl::RegisterDrawHook;
    m_api.UnregisterDrawHook  = ModAPI_Impl::UnregisterDrawHook;
    m_api.RegisterFrameHook   = ModAPI_Impl::RegisterFrameHook;
    m_api.UnregisterFrameHook = ModAPI_Impl::UnregisterFrameHook;
    m_api.HookIAT             = ModAPI_Impl::HookIATFunc;
    m_api.HookInline          = ModAPI_Impl::HookInlineFunc;
    m_api.Unhook              = ModAPI_Impl::UnhookFunc;
    m_api.GetDevice           = ModAPI_Impl::GetDevice;
    m_api.GetDepthTexture     = ModAPI_Impl::GetDepthTexture;
    m_api.GetProjParams       = ModAPI_Impl::GetProjParams;
    m_api.CompilePixelShader  = ModAPI_Impl::CompilePixelShader;
    m_api.GetConfigFloat      = ModAPI_Impl::GetConfigFloat;
    m_api.GetConfigInt        = ModAPI_Impl::GetConfigInt;
    m_api.GetConfigBool       = ModAPI_Impl::GetConfigBool;
    m_api.Log                 = ModAPI_Impl::Log;
    m_api.GetModuleHandle     = ModAPI_Impl::GetModuleHandleFunc;
    m_api.GetExportAddress    = ModAPI_Impl::GetExportAddress;
    m_api.DrawFullscreenQuad  = API_DrawFullscreenQuad;
    m_api.GetPreTransparentTexture = ModAPI_Impl::GetPreTransparentTexture;
}

bool ModManager::Init(IDirect3DDevice9* pDevice, const char* modsDir)
{
    if (m_initialized) return true;

    m_pDevice = pDevice;
    m_modsDir = modsDir;
    g_pModManager = this;

    // Build full path to d3d9.ini (next to the proxy DLL)
    {
        char dllPath[MAX_PATH];
        GetModuleFileNameA(nullptr, dllPath, MAX_PATH);  // GOnline.exe path
        char* slash = strrchr(dllPath, '\\');
        if (slash) *(slash + 1) = '\0';
        m_iniPath = std::string(dllPath) + "d3d9.ini";
    }

    BuildAPI();
    LoadMods(modsDir);

    m_initialized = true;
    return true;
}

void ModManager::LoadMods(const char* modsDir)
{
    // Scan mods/ directory for .dll files
    std::string searchPath = std::string(modsDir) + "\\*.dll";
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        if (g_Config.enableLog) {
            ModAPI_Impl::Log("No mods directory or no mods found: %s", modsDir);
        }
        return;
    }

    do {
        std::string dllPath = std::string(modsDir) + "\\" + fd.cFileName;

        HMODULE hMod = LoadLibraryA(dllPath.c_str());
        if (!hMod) {
            ModAPI_Impl::Log("Failed to load mod: %s (error %lu)", fd.cFileName, GetLastError());
            continue;
        }

        // Check for required exports
        auto pLoad    = (ModLoadFn)GetProcAddress(hMod, "ModLoad");
        auto pName    = (ModNameFn)GetProcAddress(hMod, "ModName");
        auto pUnload  = (ModUnloadFn)GetProcAddress(hMod, "ModUnload");

        if (!pLoad || !pName) {
            ModAPI_Impl::Log("Skipping %s: missing ModLoad or ModName export", fd.cFileName);
            FreeLibrary(hMod);
            continue;
        }

        const char* modName = pName();

        // Call ModLoad with our API
        if (!pLoad(&m_api)) {
            ModAPI_Impl::Log("Mod %s (%s) failed to initialize", modName, fd.cFileName);
            FreeLibrary(hMod);
            continue;
        }

        auto pVersion = (ModVersionFn)GetProcAddress(hMod, "ModVersion");
        const char* version = pVersion ? pVersion() : "?.?";

        LoadedMod mod;
        mod.hModule = hMod;
        mod.name = modName;
        mod.path = dllPath;
        mod.unloadFn = pUnload;
        mod.deviceLostFn  = (ModDeviceLostFn)GetProcAddress(hMod, "ModDeviceLost");
        mod.deviceResetFn = (ModDeviceResetFn)GetProcAddress(hMod, "ModDeviceReset");
        m_mods.push_back(mod);

        ModAPI_Impl::Log("Loaded mod: %s v%s (%s)", modName, version, fd.cFileName);

    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

    if (g_Config.enableLog) {
        ModAPI_Impl::Log("ModManager: %zu mods loaded, %zu post-passes, %zu draw hooks",
                         m_mods.size(), m_postPasses.size(), m_drawHooks.size());
    }
}

void ModManager::Shutdown()
{
    // Unload mods in reverse order
    for (auto it = m_mods.rbegin(); it != m_mods.rend(); ++it) {
        if (it->unloadFn)
            it->unloadFn();
        FreeLibrary(it->hModule);
    }
    m_mods.clear();
    m_postPasses.clear();
    m_drawHooks.clear();
    m_frameHooks.clear();

    Hook::Cleanup();
    g_pModManager = nullptr;
    m_initialized = false;
}

void ModManager::OnEndScene(IDirect3DDevice9* pDevice)
{
    // Call frame hooks
    for (auto& hook : m_frameHooks)
        hook.fn(pDevice, hook.userData);
}

void ModManager::OnReset()
{
    for (auto& mod : m_mods) {
        if (mod.deviceLostFn)
            mod.deviceLostFn();
    }
}

void ModManager::OnResetComplete(IDirect3DDevice9* pDevice)
{
    m_pDevice = pDevice;
    for (auto& mod : m_mods) {
        if (mod.deviceResetFn)
            mod.deviceResetFn(pDevice);
    }
}

bool ModManager::OnPreDraw(IDirect3DDevice9* pDevice,
                           D3DPRIMITIVETYPE primType, UINT primCount)
{
    for (auto& hook : m_drawHooks) {
        if (!hook.fn(pDevice, primType, primCount, hook.userData))
            return false;  // Mod wants to skip this draw call
    }
    return true;
}

bool ModManager::HasPostPasses() const
{
    return !m_postPasses.empty();
}

void ModManager::RunPostPasses(IDirect3DDevice9* pDevice,
                               IDirect3DTexture9* pScene, IDirect3DSurface9* pSceneSurf,
                               IDirect3DTexture9* pTemp, IDirect3DSurface9* pTempSurf,
                               IDirect3DTexture9* pDepth,
                               float width, float height, float nearZ, float farZ)
{
    if (m_postPasses.empty()) return;

    // Ping-pong between scene and temp textures
    IDirect3DTexture9* src = pScene;
    IDirect3DSurface9* srcSurf = pSceneSurf;
    IDirect3DTexture9* dst = pTemp;
    IDirect3DSurface9* dstSurf = pTempSurf;

    for (size_t i = 0; i < m_postPasses.size(); i++) {
        auto& pass = m_postPasses[i];

        pDevice->SetRenderTarget(0, dstSurf);
        pDevice->SetTexture(0, src);

        bool wrote = pass.fn(pDevice, src, dstSurf, pDepth,
                            width, height, nearZ, farZ, pass.userData);

        if (wrote) {
            // Swap src/dst for next pass
            std::swap(src, dst);
            std::swap(srcSurf, dstSurf);
        }
    }

    // If the result ended up in the temp texture, copy back to scene
    if (src == pTemp) {
        pDevice->StretchRect(pTempSurf, nullptr, pSceneSurf, nullptr, D3DTEXF_NONE);
    }
}
