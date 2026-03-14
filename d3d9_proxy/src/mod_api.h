#pragma once
//
// SDGO Mod API - Pluggable mod interface for engine and rendering mods.
//
// Each mod is a separate DLL in the mods/ directory.
// Drop a mod DLL to enable it, remove to disable.
//
// Mod DLL exports:
//   ModLoad(SdgoModAPI*)    - called on load, receives API pointer
//   ModUnload()             - called on shutdown
//   ModName()               - returns mod display name
//   ModVersion()            - returns version string
//
// Usage from mod DLL:
//   #include "mod_api.h"
//   SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api) { ... }
//

#include <d3d9.h>
#include <cstdint>

#ifdef SDGO_MOD_BUILDING
#define SDGO_MOD_EXPORT extern "C" __declspec(dllexport)
#else
#define SDGO_MOD_EXPORT extern "C"
#endif

// ============================================================
// Post-process pass callback
// ============================================================

// Called during the post-processing pipeline.
// src = input texture (scene or previous pass output)
// dst = output surface (next pass input or backbuffer)
// Return true if you wrote to dst, false to skip.
typedef bool (*PostPassFn)(
    IDirect3DDevice9* pDevice,
    IDirect3DTexture9* pSrc,
    IDirect3DSurface9* pDst,
    IDirect3DTexture9* pDepth,   // INTZ depth texture (may be null)
    float width, float height,
    float nearZ, float farZ,
    void* userData
);

// ============================================================
// Draw hook callback
// ============================================================

// Called before each DrawIndexedPrimitive / DrawPrimitive.
// Return true to let the draw proceed, false to skip it.
typedef bool (*DrawHookFn)(
    IDirect3DDevice9* pDevice,
    D3DPRIMITIVETYPE PrimitiveType,
    UINT PrimitiveCount,
    void* userData
);

// ============================================================
// Frame hook callback
// ============================================================

typedef void (*FrameHookFn)(IDirect3DDevice9* pDevice, void* userData);

// ============================================================
// Mod API - provided to mods by the framework
// ============================================================

struct SdgoModAPI
{
    // --- Rendering hooks ---

    // Register a post-processing pass. Lower priority = runs first.
    // name: unique identifier for this pass.
    bool (*RegisterPostPass)(const char* name, PostPassFn fn, int priority, void* userData);
    void (*UnregisterPostPass)(const char* name);

    // Register a pre-draw hook (called before each Draw* call).
    bool (*RegisterDrawHook)(const char* name, DrawHookFn fn, void* userData);
    void (*UnregisterDrawHook)(const char* name);

    // Register a per-frame hook (called at EndScene before post-processing).
    bool (*RegisterFrameHook)(const char* name, FrameHookFn fn, void* userData);
    void (*UnregisterFrameHook)(const char* name);

    // --- Engine hooking ---

    // Hook an imported function (IAT hook). Safe for DLL exports.
    // module: target module name (e.g., "zoa_graphics.dll")
    // function: exported function name
    // detour: your replacement function
    // original: receives pointer to original function
    bool (*HookIAT)(const char* module, const char* function,
                    void* detour, void** original);

    // Hook a function at a specific address (inline hook, x86 only).
    // target: address of the function to hook
    // detour: your replacement function
    // original: receives pointer to trampoline (calls original)
    // stolenBytes: minimum bytes to steal (>= 5, must be instruction-aligned)
    bool (*HookInline)(void* target, void* detour, void** original,
                       int stolenBytes);

    // Remove a hook by original pointer.
    void (*Unhook)(void** original);

    // --- Device access ---

    IDirect3DDevice9*  (*GetDevice)();
    IDirect3DTexture9* (*GetDepthTexture)();
    void (*GetProjParams)(float* nearZ, float* farZ);

    // --- Shader compilation ---

    // Compile a ps_3_0 pixel shader from HLSL source.
    IDirect3DPixelShader9* (*CompilePixelShader)(const char* hlsl,
                                                  const char* entryPoint);

    // --- Configuration ---

    // Read from the mod's own INI file (mods/<modname>.ini).
    float (*GetConfigFloat)(const char* section, const char* key, float def);
    int   (*GetConfigInt)(const char* section, const char* key, int def);
    bool  (*GetConfigBool)(const char* section, const char* key, bool def);

    // --- Logging ---

    void (*Log)(const char* fmt, ...);

    // --- Module access ---

    // Get a loaded module handle (e.g., "GOnline.exe", "zoa_graphics.dll").
    void* (*GetModuleHandle)(const char* moduleName);

    // Get an exported function address from a module.
    void* (*GetExportAddress)(const char* moduleName, const char* funcName);

    // --- Fullscreen quad helper ---

    // Draw a fullscreen quad with the current shader and textures.
    void (*DrawFullscreenQuad)(IDirect3DDevice9* pDevice, float w, float h);

    // --- Pre-transparent scene (for fog/particle masking) ---

    // Returns the backbuffer captured just before transparent objects rendered.
    // Use in SSAO to detect fog/particle pixels (compare with final scene).
    // Returns nullptr if not available this frame.
    IDirect3DTexture9* (*GetPreTransparentTexture)();
};

// ============================================================
// Mod DLL entry points (exported by each mod)
// ============================================================

// Called when the mod is loaded. Return true on success.
typedef bool (*ModLoadFn)(SdgoModAPI* api);

// Called when the mod is unloaded.
typedef void (*ModUnloadFn)();

// Called before device Reset — release all D3DPOOL_DEFAULT resources.
typedef void (*ModDeviceLostFn)();

// Called after device Reset succeeds — recreate resources.
typedef void (*ModDeviceResetFn)(IDirect3DDevice9* pDevice);

// Returns the mod's display name.
typedef const char* (*ModNameFn)();

// Returns the mod's version string.
typedef const char* (*ModVersionFn)();
