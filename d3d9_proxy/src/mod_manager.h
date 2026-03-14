#pragma once
#include "mod_api.h"
#include <vector>
#include <string>

// ============================================================
// ModManager - Discovers, loads, and manages mod DLLs.
//
// On init:
//   1. Scans mods/ directory for .dll files
//   2. Loads each DLL and calls ModLoad(api)
//   3. Provides API functions for hooks, rendering, config
//
// On shutdown:
//   1. Calls ModUnload() on each mod
//   2. Frees all mod DLLs
//   3. Cleans up hooks
// ============================================================

class PostProcessor;

class ModManager
{
public:
    bool Init(IDirect3DDevice9* pDevice, const char* modsDir);
    void Shutdown();

    // Called by device proxy at key points
    void OnEndScene(IDirect3DDevice9* pDevice);
    void OnReset();
    void OnResetComplete(IDirect3DDevice9* pDevice);

    // Called by device proxy before draw calls
    // Returns false if any draw hook wants to skip the draw.
    bool OnPreDraw(IDirect3DDevice9* pDevice, D3DPRIMITIVETYPE primType,
                   UINT primCount);

    // Run registered post-processing passes (called by PostProcessor)
    void RunPostPasses(IDirect3DDevice9* pDevice,
                       IDirect3DTexture9* pScene, IDirect3DSurface9* pSceneSurf,
                       IDirect3DTexture9* pTemp, IDirect3DSurface9* pTempSurf,
                       IDirect3DTexture9* pDepth,
                       float width, float height, float nearZ, float farZ);

    bool HasPostPasses() const;

    void SetDepthTexture(IDirect3DTexture9* pDepth) { m_pDepth = pDepth; }
    void SetProjParams(float nearZ, float farZ) { m_nearZ = nearZ; m_farZ = farZ; }
    // API accessors (used by ModAPI_Impl in mod_manager.cpp)
    void AddPostPass(const char* name, PostPassFn fn, int priority, void* ud);
    void RemovePostPass(const char* name);
    void AddDrawHook(const char* name, DrawHookFn fn, void* ud);
    void RemoveDrawHook(const char* name);
    void AddFrameHook(const char* name, FrameHookFn fn, void* ud);
    void RemoveFrameHook(const char* name);
    IDirect3DDevice9* GetDevice() const { return m_pDevice; }
    const char* GetIniPath() const { return m_iniPath.c_str(); }
    IDirect3DTexture9* GetDepth() const { return m_pDepth; }
    float GetNearZ() const { return m_nearZ; }
    float GetFarZ() const { return m_farZ; }

private:
    void BuildAPI();
    void LoadMods(const char* modsDir);

    // Registered hooks from mods
    struct PostPassEntry {
        std::string name;
        PostPassFn fn;
        int priority;
        void* userData;
    };

    struct DrawHookEntry {
        std::string name;
        DrawHookFn fn;
        void* userData;
    };

    struct FrameHookEntry {
        std::string name;
        FrameHookFn fn;
        void* userData;
    };

    struct LoadedMod {
        HMODULE hModule;
        std::string name;
        std::string path;
        ModUnloadFn unloadFn;
        ModDeviceLostFn deviceLostFn;
        ModDeviceResetFn deviceResetFn;
    };

    std::vector<PostPassEntry> m_postPasses;
    std::vector<DrawHookEntry> m_drawHooks;
    std::vector<FrameHookEntry> m_frameHooks;
    std::vector<LoadedMod> m_mods;

    SdgoModAPI m_api = {};
    IDirect3DDevice9* m_pDevice = nullptr;
    IDirect3DTexture9* m_pDepth = nullptr;
    float m_nearZ = 0.1f;
    float m_farZ = 1000.0f;
    std::string m_modsDir;
    std::string m_iniPath;
    bool m_initialized = false;
};
