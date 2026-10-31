# SDGO mod framework

A mod loader for the SD Gundam Online game client (`GOnline.exe`), Agent server
(`AgentServer.exe`) and battle server (`BattleServer.exe`). Its core checks the exe build,
verifies every code address before using it and loads mods. Features are built as mods on
top of it, on the client, the battle server, or both.

Unofficial fan project. It contains no game files; SD Gundam Online and its assets belong
to their owners. 简体中文说明：[README.zh-CN.md](README.zh-CN.md)

* [Features](#features)
* [What is in the package](#what-is-in-the-package)
* [Supported game builds](#supported-game-builds)
* [Install](#install) · [Uninstall](#uninstall)
* [Configuration](#configuration) · [Troubleshooting](#troubleshooting)
* [Building from source](#building-from-source) · [Adding a feature](#adding-a-feature)

## Features

FRP networking is in **Beta**. For incremental installation and owner/player configuration, see the
[distribution guide](docs/frp-distribution.zh-CN.md). The bundled framework is updated;
other mods and global switches are preserved. Players use their existing launcher;
no special networking launch script is required. Human multiplayer acceptance remains manual.

Each feature is one or more mods with its own guide (use, settings, logs, how it works).

| feature | what it does | mods | install on | guide |
|---|---|---|---|---|
| Co-op mission wingmen | AI pilots join your team in co-op missions, fly with the game's AI battle bot AI, score real kills and can be shot down. Put `!ai` in the room title. | `coop_ai`, `mission_ai` | battle server + clients | [English](docs/coop-wingmen.md) · [中文](docs/coop-wingmen.zh-CN.md) |
| IME input fix | Fixes text typed with a Chinese/Japanese/Korean IME at one place that reads a byte count as a character count. | `ime_fix` | clients | [English](docs/ime-fix.md) · [中文](docs/ime-fix.zh-CN.md) |
| General input method fix | Stops input methods from freezing or crashing the game, whichever input method it is. The repack's `SDPlus.dll` breaks window lookup (`FindWindowW`) for every DLL in the game process, so any input method that looks up a window hangs or crashes when it is switched on. The fix restores normal lookup for everything except the game itself. | `findwindow_fix` | clients | [English](docs/findwindow-fix.md) · [中文](docs/findwindow-fix.zh-CN.md) |
| Random crash at game start | Stops the game from crashing now and then right after it starts, on the loading screen (`0x008FE610`): the screen picks a loading set at random and crashes when its unit is missing from the client's data; such a set is now replaced by the nearest set whose unit is known. | `startup_crash_fix` | clients | [English](docs/startup-crash-fix.md) · [中文](docs/startup-crash-fix.zh-CN.md) |
| Packet logger | Logs outgoing packets for mod development. Off by default. | `net_probe` | clients | [docs/net-probe.md](docs/net-probe.md) |
| Endpoint routing (Beta) | Configurable TCP/UDP destinations and UDP reply addresses for arbitrary tunnel ports. Battle relay acceptance is separate. Off by default. | `net_route` | clients | [English](docs/net-route.md) · [中文](docs/net-route.zh-CN.md) |
| Native UDP port pool (Beta) | Bounds per-user Agent UDP sockets to reusable tunnelled ports. Off by default. | `agent_udp_pool` | Agent servers | [Sakura setup](docs/sakura-frp-setup.zh-CN.md) |
| Native relay diagnostic fix | Corrects a verified integer-IP printf format that can crash the original Agent after UDP relay forwarding. Enabled by default. | `agent_relay_log_fix` | Agent servers | [Sakura setup](docs/sakura-frp-setup.zh-CN.md) |

A feature that lists the battle server works only when the battle servers have the server
part and the players have the client part. Players without the client part simply do not
get the feature; the game is otherwise unaffected.

### In game

Co-op mission wingmen: a mission room with `!ai` in its title. AI wingmen fly with the
player and fight the mission's enemies.

![Co-op mission with AI wingmen fighting alongside the player](docs/coop-wingmen-demo.webp)

General input method fix: the game lobby with an input method switched on (its toolbar is
boxed in red; iFlytek in this screenshot). Without `findwindow_fix`, any input method that
looks up a window freezes or crashes the game as soon as it is switched on.

![Game lobby with an input method switched on](docs/findwindow-fix-ime.png)

## What is in the package

```
SDGO-ModFramework-<version>/
  client/                 goes next to GOnline.exe
    d3d9.dll
    sdgo\core.dll, sdgo\sdgo.ini, sdgo\symbols\
    sdgo\mods\            findwindow_fix, ime_fix, mission_ai, net_probe (off), startup_crash_fix, net_route (off)
  server/                 goes next to BattleServer.exe or AgentServer.exe
    winmm.dll
    sdgo\core.dll, sdgo\sdgo.ini, sdgo\symbols\
    sdgo\mods\            coop_ai, agent_udp_pool (off), agent_relay_log_fix
  frp/                    complete incremental FRP kit, owner tools and tutorial
  install_client.ps1      installs client\ into a game folder
  install_server.ps1      installs server\ into battle server folders
  README.md, README.zh-CN.md, docs\, LICENSE, THIRD_PARTY_NOTICES.md, licenses\
```

The mods are the features in [Features](#features); `net_probe` is off until it is switched
on in its `mod.ini`.

| part | where | what it does |
|---|---|---|
| `d3d9.dll` | game folder | Loader: GOnline.exe loads it at start; it starts `sdgo\core.dll` and forwards Direct3D to the real `d3d9.dll`. |
| `winmm.dll` | battle server folder | Same loader for BattleServer.exe (forwards `timeGetTime`). |
| `sdgo\core.dll` | both | Framework core: checks the exe build, verifies every code address it hands out, owns all hooks, loads mods. |
| `sdgo\sdgo.ini` | both | Framework settings: log level, a chained loader, single mods turned off (see [Configuration](#configuration)). |
| `sdgo\symbols\*.ini` | both | Verified addresses and byte signatures, one file per supported exe build. |
| `sdgo\mods\<id>\` | both | One folder per mod: `<id>.dll`, `mod.ini` (manifest + settings), extra files. |

The installers copy whatever the package contains, so new features need no installer
changes.

## Supported game builds

The framework only touches code it can verify. Each exe build has a *host key*,
`<exe name>-<TimeDateStamp>-<SizeOfImage>`, and a matching file in `sdgo\symbols\`:

| exe | host key | known distributions |
|---|---|---|
| GOnline.exe | `GOnline-54BE1568-00C97000` | SDGO Ultimate Repack v1.2 (and the client it is based on) |
| BattleServer.exe | `BattleServer-548A891D-0046C000` | the common private server pack (BattleServer1..N folders) |
| AgentServer.exe | `AgentServer-548A86BC-000D7000` | the corresponding private server pack (AgentServer1..3 folders) |

The install scripts print the host key of your exe. With another build the framework still
loads, logs that no symbol file matches and keeps every mod disabled, so the game is
unaffected. See [Other game builds](#other-game-builds).

## Install

Requirements: Windows with Windows PowerShell 5.1 (built in). No VC++ redistributable is
needed. Download `SDGO-ModFramework-<version>.zip` from the
[latest release](https://github.com/crabsatellite/SDGO-d3d9-proxy/releases/latest), extract it,
then open PowerShell in the extracted folder.

### Client (every player)

```powershell
powershell -ExecutionPolicy Bypass -File .\install_client.ps1 -GameDir "D:\Games\SDGO"
```

`-GameDir` is the folder that contains `GOnline.exe`. Close the game first.

* How the game is started does not matter (any launcher, a shortcut, GOnline.exe directly):
  Windows loads `d3d9.dll` from GOnline.exe's own folder, and the framework finds all of its
  files relative to GOnline.exe.
* Other mods are left alone. Plugins loaded through other DLLs (for example `dinput8.dll` +
  `SDPlus.dll` in the Ultimate Repack) keep working. If the folder already has a `d3d9.dll`
  from another mod (a graphics wrapper, say), the installer renames it to `d3d9_chain.dll`
  and chains it, so both run.
* Re-running the script updates an existing install. It keeps `sdgo\sdgo.ini`, and installs
  each mod's new `mod.ini` with your own values written back (`[mod] enabled` and every
  `[settings]` value that differs from the new file); the output lists them.
  `-OverwriteConfig` installs both as they are in the package.

Manual install: copy the contents of `client\` into the game folder (next to GOnline.exe).
This replaces `sdgo.ini` and every `mod.ini` with the package's.

Check: after the game has started, `sdgo\logs\sdgo.log` begins with
`core: SDGO core started by d3d9 loader ...`, `symbols: N of N verified` and one
`loaded <mod>` line per mod.

### Battle server (whoever hosts the server)

For the Beta native UDP pool, install into **AgentServer** folders with
`install_server.ps1 -ServerRoot ... -Role Agent`; see the [Sakura setup guide](docs/sakura-frp-setup.zh-CN.md).
The default role remains Battle for existing AI features.

Only needed for features that list the battle server. Stop the battle servers, then:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server"
```

`-ServerRoot` is either one battle server folder (with `BattleServer.exe`) or the folder
that holds several (`BattleServer1`, `BattleServer2`, ...). `-Only BattleServer3` limits it to
named folders. Start the battle servers again afterwards. How they are started (a server
manager, batch files, a service) does not matter. An existing `winmm.dll` is chained, and an
update keeps the settings, the same way as on the client. Manual install: copy the contents
of `server\` into each battle server folder.

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File .\install_client.ps1 -GameDir "D:\Games\SDGO" -Uninstall
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server" -Uninstall
```

This removes the loader DLL and `sdgo\` (to the Recycle Bin) and puts a chained
`d3d9.dll`/`winmm.dll` back under its original name. To turn a single mod off instead, set
`<mod id>=0` under `[mods]` in `sdgo\sdgo.ini`.

## Configuration

`sdgo\sdgo.ini` (one per exe folder):

| section / key | meaning |
|---|---|
| `[core] log_level` | 0 debug, 1 info (default), 2 warn, 3 error |
| `[loader] chain_d3d9` / `chain_winmm` | another DLL to forward to instead of the system copy, relative to the exe folder (set by the installers) |
| `[mods] <id>=0` | turn a mod off |

Each mod's settings are in the `[settings]` section of its `mod.ini`; every key is
documented there and in the feature's guide.

## Troubleshooting

All diagnostics go to `sdgo\logs\sdgo.log` next to the exe (the previous run is
`sdgo.prev.log`). Framework messages:

| log line | meaning |
|---|---|
| no `sdgo.log` at all | the loader did not run: the DLL is not next to the exe, or another program replaced it |
| `no symbol file for host ...` | unsupported exe build; every mod stays off and the game is unaffected |
| `symbol X rejected: byte +N is .., expected ..` | the code at that address differs (another patch or a different build); mods that need X stay off |
| `required symbol X ...` / `0 of N mods active` | a mod could not start because a symbol it needs failed |

Messages of a particular feature are listed in its guide.

### Other game builds

A different `GOnline.exe` or `BattleServer.exe` needs its own `sdgo\symbols\<host key>.ini`
with addresses and signatures found for that build; see
[docs/framework.md](docs/framework.md#symbol-files). Until then the framework loads and does
nothing.

## Building from source

Visual Studio 2022 (C++ tools), CMake 3.20 or newer and Python 3.10+ for packaging;
all host targets are 32-bit. Regression commands: [FRP verification](docs/frp-testing.md).

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\package.ps1     # dist\SDGO-ModFramework-<version>.zip
```

The install scripts also install straight from a build:
`tools\install_client.ps1 -GameDir ... -Source build\stage\Release\client`.

## Adding a feature

A new feature is one or more mods under `mods/`, registered in `CMakeLists.txt`, plus a guide
in `docs/`, a row in the [Features](#features) table of both READMEs and its mods in their
[package listing](#what-is-in-the-package). The build stages it
into `client\` or `server\`, and the installers and the package pick it up without changes.
Step by step, with the mod API, symbol files and the guide outline:
[docs/framework.md](docs/framework.md#adding-a-feature).

## License

MIT, see [LICENSE](LICENSE). Third-party code: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
(MinHook, BSD 2-Clause).
