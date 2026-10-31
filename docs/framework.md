# Framework reference

For people who build the framework or write mods. Installing is covered in the
[README](../README.md); using each feature in its guide, linked from the README's
[Features](../README.md#features) table.

* [How the framework gets into the process](#how-the-framework-gets-into-the-process)
* [Folder layout and configuration](#folder-layout-and-configuration)
* [Symbol files](#symbol-files)
* [Mod manifest (mod.ini)](#mod-manifest-modini)
* [Mod API](#mod-api)
* [Writing a mod](#writing-a-mod)
* [Adding a feature](#adding-a-feature)
* [Building, testing, packaging](#building-testing-packaging)
* [Source tree](#source-tree)

## How the framework gets into the process

```
GOnline.exe      ── static import ──> d3d9.dll   (loader/d3d9)
BattleServer.exe ── static import ──> winmm.dll  (loader/winmm)
        │  DllMain: redirect the exe's kernel32!GetSystemTimeAsFileTime import
        │  first CRT call (__security_init_cookie): restore it, then boot      (loader/common)
        ▼
  sdgo/core.dll  SdgoCoreInit(SdgoBoot*)                                      (core/)
        │  host key -> sdgo/symbols/<host key>.ini -> verify every entry
        │  sdgo/mods/*/mod.ini -> eligible mods sorted by order -> SdgoModLoad
        ▼
  sdgo/mods/<id>/<id>.dll
```

* Both host executables import a DLL from their own folder at start-up (`d3d9.dll`, and
  `winmm.dll` for `timeGetTime`), so a DLL of that name next to the exe is loaded before
  any of the program's own code runs. Neither name is a Windows KnownDLL.
* The loader does two things. It forwards its exports to the real DLL (system32, or the DLL
  named by `[loader] chain_d3d9` / `chain_winmm` in `sdgo.ini`). And it starts the core.
  Loading other DLLs inside `DllMain` is unsafe, so `DllMain` only patches one import of the
  exe (`GetSystemTimeAsFileTime`, the MSVC CRT's first call). That first call runs on the
  main thread before `main`/`WinMain`; it restores the import and loads `sdgo\core.dll`. If
  the import is missing, the first forwarded call (`Direct3DCreate9` / `timeGetTime`) boots
  the core instead.
* Paths are always derived from the exe's location (`GetModuleFileName(NULL)`), never from the
  working directory, so launchers and shortcuts do not matter.
* The core computes the host key `<exe stem>-<TimeDateStamp>-<SizeOfImage>` from the
  running image, loads the matching symbol file, verifies it against the code in memory
  (after the loader, so other in-process patches are seen), sets up MinHook and loads the
  mods. Everything is logged to `sdgo\logs\sdgo.log`.
* Mods live for the whole process; there is no unload. If `SdgoModLoad` returns 0 or
  throws, the core removes every hook that mod installed (rollback).

## Folder layout and configuration

```
<exe folder>\
  d3d9.dll | winmm.dll          loader
  sdgo\
    core.dll
    sdgo.ini                    global settings
    symbols\<host key>.ini      one per supported exe build (files for other hosts are ignored)
    mods\<id>\mod.ini           manifest + [settings]
    mods\<id>\<id>.dll          (+ any files the mod reads, located with ModDirectory)
    mods\<id>\config.ini        optional user settings, supported by the network mods
    logs\sdgo.log, sdgo.prev.log
```

`sdgo.ini`:

```ini
[core]
log_level=1            ; 0 debug, 1 info, 2 warn, 3 error
[loader]
chain_d3d9=            ; forward to this DLL instead of system32 (relative to the exe folder)
chain_winmm=
[mods]
<id>=0                 ; disable a mod
```

## Symbol files

Mods never patch raw addresses. They ask the core for named symbols, and the core only hands
out addresses whose bytes match what was recorded for that exact build. A different build
or a conflicting patch therefore disables the mods that depend on the changed code, instead
of crashing the process.

`symbols/<host key>.ini`:

```ini
[host]
image=GOnline.exe
timestamp=0x54BE1568
size_of_image=0x00C97000

; comments above an entry say what it is and how it was found
[CMissionMode.Init]
kind=code
rva=0x006D5830
expect=55 8B EC 6A FF 68 07 75 E0 00 64 A1 00 00 00 00
```

| key | meaning |
|---|---|
| `kind` | `code` (function start; `HookCode` and plain calls), `site` (an instruction inside a function; only verified, for checks), `slot` (a pointer in memory such as a vtable entry; `HookSlot`), `data` (an address of a global; not verified) |
| `rva` | address minus the preferred image base (0x00400000 for both hosts) |
| `expect` | at least 5 bytes expected at `rva`, hex, `??` for wildcards; required for `code` and `site` |
| `target_rva` | `slot` only: the rva the pointer must currently point to |

Rules:

* `BattleServer.exe` is relocatable (ASLR). Its `expect` bytes must not contain absolute
  addresses (immediates the loader relocates, such as SEH handler pushes or `mov eax,[global]`):
  pin only the position-independent prefix. `GOnline.exe` has no relocations.
* Other code in the process may patch the host at run time (the battle server's
  `SDBattlePlus.dll` detours at least one call site). The core verifies against memory, so
  such a patch rejects the symbol; pick a different hook point (see `battle.start_notify`).
* `tools/verify_symbols.py <exe>` checks a symbol file offline against the exe file.

Porting to another build: find each symbol in the new exe (the comments name the function,
its signature and how it is referenced), create `symbols/<new host key>.ini`, run
`verify_symbols.py`, and add the new host key to the `hosts=` line of each mod you ported.

## Mod manifest (mod.ini)

```ini
[mod]
id=my_mod                 ; folder name by default
name=My mod
version=1.0.0
api=1                     ; SDGO_API_VERSION the mod was built for
dll=my_mod.dll            ; default <folder>.dll
order=100                 ; load order, lower first
enabled=1                 ; 0 skips the mod (sdgo.ini [mods] my_mod=0 does the same)
hosts=GOnline-54BE1568-00C97000          ; comma list; the mod loads only into these
requires=CMissionMode.Init,lua.pcall     ; symbols that must verify, or the mod is skipped

[settings]
; free-form; read with SettingInt / SettingString
MySetting=1
```

A mod is loaded when it is enabled, its `api` is supported, the running host key is listed
in `hosts`, and every symbol in `requires` verified. The reason for skipping a mod is
logged.

## Mod API

`include/sdgo/sdgo_api.h` (C ABI, stable; fields are only ever appended):

```c
SDGO_MOD_ENTRY      // extern "C" __declspec(dllexport) int __cdecl SdgoModLoad(const SdgoApi* api, SdgoMod* mod)
{
    // return nonzero when active; 0 rolls back every hook this mod installed
}
```

| member | purpose |
|---|---|
| `size`, `version` | size of the struct and API version of the running core (check `size` before using fields added later) |
| `host_key`, `host_base` | running host key and the exe's load address |
| `Log(mod, level, fmt, ...)` | printf-style line in `sdgo.log`, prefixed with the mod id |
| `Symbol(mod, name)` | address of a verified symbol, or NULL |
| `HookCode(mod, name, detour, &original)` | MinHook inline detour on a `kind=code` symbol |
| `HookSlot(mod, name, detour, &original)` | swap the pointer stored in a `kind=slot` symbol |
| `HookImport(mod, dll, function, detour, &original)` | replace an entry of the exe's import table |
| `SettingInt` / `SettingString(mod, key, default, ...)` | values from the mod's `[settings]` |
| `ModDirectory(mod)` | absolute folder of the mod |
| `HookSystemExport(mod, dll, function, detour, &original)` | optional API 1 extension: MinHook detour on a named export in an already loaded system DLL; `dll` is a wide basename, resolved in the system directory |

The network mods use `ModDirectory` to read `config.ini` without changing this ABI.
When present, its `[settings]` section is authoritative; otherwise they use the
legacy manifest settings. Other mods keep their existing configuration behavior.
The incremental FRP installer always updates core and loader to the bundled latest
version and verifies their hashes. It refuses an older kit instead of downgrading.
Other mods, global switches and settings remain. Network symbols are merged without
replacing unrelated definitions; conflicts are rejected. Core 1.1.3 supports exact
ordinal imports in HookImport for the Agent bind. See [distribution](frp-distribution.zh-CN.md).
Build, regression and installer commands are in [FRP verification](frp-testing.md).

Before reading `HookSystemExport`, check
`api->size >= offsetof(SdgoApi, HookSystemExport) + sizeof(api->HookSystemExport)`
and then that the pointer is non-null. Older API 1 mods keep their existing ABI.
This API does not load DLLs or resolve application-local DLLs. It shares the
ownership and rollback mechanism with `HookCode`. When MinHook can relocate an
existing external detour, `original` calls that prior chain; the framework does
not remove it. The system DLL must stay loaded for the hook's process lifetime.

Rules: one owner per hook site (a second hook on the same site fails); install hooks in
`SdgoModLoad`, so a failed load can roll them back; detours run on whatever thread the host
calls them on, so guard shared state; keep detours exception-safe (the bundled mods wrap
game calls in `__try`/`__except`).

`HookCode` hooks made while the mods load are switched on together right after the last mod
has loaded (`core: N code hooks switched on`), before the host's own code runs; `original`
works as soon as the call returns. `HookSystemExport` hooks take effect at once: other threads
(input methods, other injected DLLs) may run system code at any time. Switching them on one by one froze
every thread once per hook, about 70 ms each, and the game must reach `WinMain` within 3 s of
being started by `GUpdate.exe` (it opens the `9YOU SDGO` event there, which `GUpdate.exe`
closes after 3 s; without it the game exits silently). Keep `SdgoModLoad` short for the same
reason.

## Writing a mod

1. Add the symbols you need to the host's symbol file and verify them.
2. Create `mods/<id>/` with `<id>.cpp` and `mod.ini` (`hosts`, `requires`).
3. Register it in `CMakeLists.txt`:

   ```cmake
   sdgo_add_mod(my_mod DESCRIPTION "what it does" SOURCES mods/my_mod/my_mod.cpp)
   # battle server mods: sdgo_add_mod(my_mod SERVER DESCRIPTION ... SOURCES ...)
   # extra files copied next to the DLL: FILES script.lua
   ```

4. Build; the mod is staged into `build/stage/Release/client/sdgo/mods/<id>/` (or `server/`).

Minimal mod, a detour that logs each call:

```cpp
#include <windows.h>
#include "sdgo/sdgo_api.h"

namespace {
    const SdgoApi* g_api;
    SdgoMod* g_mod;
    using UpdateFn = void(__thiscall*)(void* self);
    UpdateFn g_original;

    void __fastcall Update(void* self, void* /*edx*/)
    {
        g_original(self);
        g_api->Log(g_mod, SDGO_LOG_DEBUG, "update %p", self);
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    return api->HookSlot(mod, "CMissionMode.vtbl.Update", reinterpret_cast<void*>(&Update),
                         reinterpret_cast<void**>(&g_original));
}
```

Real examples: `ime_fix` (one import hook), `mission_ai` (code + vtable hooks, calls into the
game, runs Lua in the game's Lua 5.0 state), `coop_ai` (server side, calls game functions
under the game's own locks). Their internals are in the "How it works" section of
[coop-wingmen.md](coop-wingmen.md), [ime-fix.md](ime-fix.md), and
[findwindow-fix.md](findwindow-fix.md) (`HookSystemExport` on a user32 export, scoped by calling module).

## Adding a feature

A feature is what players see: one or more mods that work together, with one guide. The
build, the installers and the package handle any number of mods, so a new feature touches
only its own files plus the two READMEs.

1. **Symbols.** Add what the feature needs to `symbols/<host key>.ini` of each exe it runs in,
   with a comment above every entry, and check it with `tools/verify_symbols.py`
   (see [Symbol files](#symbol-files)).
2. **Mods.** One mod per program: `mods/<id>/<id>.cpp` and `mods/<id>/mod.ini`. A feature that
   needs both the client and the battle server has two mods, as the wingmen have `mission_ai`
   (client) and `coop_ai` (server).
   * `mod.ini`: a header comment saying what the mod does and which feature it belongs to,
     `hosts`, `requires`, and every setting with a comment above it.
   * An update installs the new `mod.ini` and writes back the player's `[mod] enabled` and
     every `[settings]` value that differs from it (`-OverwriteConfig` installs it as it is).
     The manifest, the comments and new keys reach every install; a value in the player's
     file stays, even an untouched old default, so a changed default reaches only new
     installs. Use a new key when existing installs must get the new value.
   * Give every setting the same default in code (`SettingInt(mod, key, default)`) as in
     `mod.ini`, so a missing key behaves the same.
   * A feature not meant for everyone starts with `enabled=0`.
3. **Build.** Register each mod in `CMakeLists.txt` with `sdgo_add_mod` (`SERVER` for the
   battle server, `FILES` for files the mod reads). It is staged into
   `build/stage/<config>/client` or `server`, and from there goes into the package and
   through the installers. List it in the commented `[mods]` block of `config/sdgo.ini`.
4. **Tests.** Checks that run without the game live next to the mod (`mods/<id>/test/`),
   like `mods/mission_ai/test/sim_mission.lua`.
5. **Guide.** Write `docs/<feature>.md` with the outline below; add `docs/<feature>.zh-CN.md`
   when the feature is meant for players.
6. **Catalog.** Add a row to the Features table in `README.md` and `README.zh-CN.md`: name,
   what it does and how to trigger it in one or two sentences, mods, where to install
   (`clients`, or `battle server + clients`), guide link. Add its mods to the package listing
   ("What is in the package") of both READMEs.
7. **Version.** Bump `VERSION` and each changed mod's `version=` before packaging.

Guide outline (the existing guides follow it; [coop-wingmen.md](coop-wingmen.md) is the
complete example):

```markdown
# <Feature name>

What the player gets, in one paragraph.

| mod | host | part |
|---|---|---|
| `<id>` | client / battle server | what this mod does for the feature |

## Use
How to turn it on or trigger it, and what happens.

## Settings
One table per mod: key, default, meaning. Say when a change takes effect.

## Logs and troubleshooting
Example log lines, then a table: symptom or log line -> cause.

## Extending
Only for data-driven features: how users add their own content. Name the section after
what is added (the wingmen guide has "Adding more bot units").

## How it works
Hook points, symbols, the game functions it calls, its tests.
```

## Building, testing, packaging

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
```

* All DLLs link the CRT statically and import only system DLLs.
* `build/stage/Release/client/` and `.../server/` mirror the install targets.
* `tests/loader_test.ps1`: runs the client loader in a test exe and checks forwarding
  (plain, chained, self-chain refused) and that it starts the core.
* `tests/findwindow_fix_test.ps1`: runs the real core and `findwindow_fix` against a simulated
  SDPlus hook, and checks `HookSystemExport` and its rollback.
* `tests/install_test.ps1`: updates a throw-away install with both installers and checks that
  the new `mod.ini` arrives with the player's settings written back (no build needed).
* `mods/mission_ai/test/sim_mission.lua`: drives `mission_ai.lua` against a mock of the game's
  script API under real Lua 5.0 (`tools/lua50_host.c`, built against Lua 5.0.3 sources).
* `tools/verify_symbols.py <exe>`: offline check of a symbol file.
* `tools/package.ps1`: zips `dist/SDGO-ModFramework-<VERSION>.zip`.
* `tools/install_client.ps1` / `install_server.ps1 -Source build/stage/Release/<tree>`:
  install straight from a build.

## Source tree

| path | contents |
|---|---|
| `loader/common` | early boot, chain loading (shared by both loaders) |
| `loader/d3d9`, `loader/winmm` | the two loader DLLs and their export lists |
| `core/` | host key, symbol files, hooks (MinHook), mod loading, log |
| `include/sdgo/sdgo_api.h` | the mod API |
| `symbols/` | one verified symbol file per supported exe build |
| `mods/` | the mods (one folder each) |
| `config/sdgo.ini` | default global settings |
| `docs/` | this reference and one guide per feature (`<feature>.md`, `<feature>.zh-CN.md`) |
| `cmake/version.rc.in` | version resource of every DLL |
| `tests/`, `tools/` | tests, install/package scripts, symbol checker |
| `third_party/minhook` | MinHook 1.3.4, unmodified (BSD 2-Clause) |
