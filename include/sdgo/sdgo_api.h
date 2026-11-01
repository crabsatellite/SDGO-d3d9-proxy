/*
 * SDGO mod framework - public ABI shared by the core and every mod.
 *
 * Loading chain:
 *   GOnline.exe      --static import--> d3d9.dll  (loader shim, next to the exe)
 *   BattleServer.exe --static import--> winmm.dll (loader shim, next to the exe)
 *     --early CRT hook--> sdgo/core.dll  (this API)
 *       --mod.ini manifests--> sdgo/mods/<id>/<id>.dll
 * Reference: docs/framework.md.
 *
 * Rules every mod follows:
 *   - Never patch raw game addresses. Ask the core for a named symbol from the
 *     host's symbol file (sdgo/symbols/<host_key>.ini). The core only hands
 *     out addresses whose bytes matched the recorded signature, so a client
 *     update or a conflicting patch disables the mod instead of crashing.
 *   - One owner per hook site. A second hook on the same site is refused.
 *   - Mods live for the whole process. There is no unload entry point.
 *
 * ABI stability: fields are only ever appended to SdgoApi. A mod that needs
 * a field added after version 1 must check api->size first.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define SDGO_API_VERSION 1

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SdgoMod SdgoMod; /* opaque, owned by the core */

enum SdgoLogLevel {
    SDGO_LOG_DEBUG = 0,
    SDGO_LOG_INFO = 1,
    SDGO_LOG_WARN = 2,
    SDGO_LOG_ERROR = 3
};

typedef struct SdgoApi {
    uint32_t size;    /* sizeof(SdgoApi) as compiled into the running core */
    uint32_t version; /* SDGO_API_VERSION of the running core */

    const char* host_key; /* "<exe stem>-<TimeDateStamp>-<SizeOfImage>", e.g. GOnline-54BE1568-00C97000 */
    void* host_base;      /* load address of the host executable */

    void (*Log)(SdgoMod* mod, int level, const char* fmt, ...);

    /* Address of a verified symbol, or NULL when it is missing or its bytes did not match. */
    void* (*Symbol)(SdgoMod* mod, const char* name);

    /* Inline detour (MinHook) on a verified kind=code symbol. Returns nonzero on success.
     * Hooks made during SdgoModLoad take effect together right after the last mod has loaded
     * (before the host's own code runs); `original` can be called at once. HookSystemExport
     * hooks take effect at once. */
    int (*HookCode)(SdgoMod* mod, const char* name, void* detour, void** original);

    /* Swap the pointer stored in a verified kind=slot symbol (vtable entry, function pointer). */
    int (*HookSlot)(SdgoMod* mod, const char* name, void* detour, void** original);

    /* Replace one entry of the host executable's import table. A named export
     * request also matches an ordinal import using the dependency's exact
     * export ordinal (not equality of potentially aliased function addresses). */
    int (*HookImport)(SdgoMod* mod, const char* dll, const char* function, void* detour, void** original);

    /* Values from the [settings] section of the mod's mod.ini. */
    int (*SettingInt)(SdgoMod* mod, const char* key, int def);
    uint32_t (*SettingString)(SdgoMod* mod, const char* key, const char* def, char* out, uint32_t out_size);

    /* Absolute directory of the mod (contains mod.ini and the mod's own files). */
    const wchar_t* (*ModDirectory)(SdgoMod* mod);

    /* Inline detour on a named export of an already loaded system DLL. dll is a
     * basename (e.g. L"user32.dll"); the core resolves it in the system directory.
     * When MinHook can relocate the entry, the trampoline retains its prior call
     * chain (including an existing external detour). Same ownership
     * and failed-load rollback as HookCode. Install only during SdgoModLoad.
     * Optional API 1 extension: check size before reading this field. */
    int (*HookSystemExport)(SdgoMod* mod, const wchar_t* dll, const char* function,
                            void* detour, void** original);
    /* Inline detour at a verified kind=site instruction boundary. There is no
     * function-call ABI at a site: the detour must preserve all live registers,
     * flags, stack and floating-point/SIMD state, then jump to the trampoline.
     * Same queued activation, ownership and rollback as HookCode. Optional API
     * 1 extension: check size before reading this field. */
    int (*HookSite)(SdgoMod* mod, const char* name, void* detour, void** original);
} SdgoApi;

/* Every mod DLL exports this. Return nonzero when the mod is active. */
typedef int(__cdecl* SdgoModLoadFn)(const SdgoApi* api, SdgoMod* mod);

#ifdef __cplusplus
}
#define SDGO_MOD_ENTRY extern "C" __declspec(dllexport) int __cdecl SdgoModLoad(const SdgoApi* api, SdgoMod* mod)
#else
#define SDGO_MOD_ENTRY __declspec(dllexport) int __cdecl SdgoModLoad(const SdgoApi* api, SdgoMod* mod)
#endif

/* Boot contract between the loader shim and core.dll. */
typedef struct SdgoBoot {
    uint32_t size;
    const char* loader; /* which shim started the core, e.g. "d3d9" */
    void* host_module;  /* HMODULE of the host executable */
} SdgoBoot;

typedef int(__cdecl* SdgoCoreInitFn)(const SdgoBoot* boot);
