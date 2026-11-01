# Install and configure

[中文](setup.zh-CN.md) · [Feature categories](../README.md)

Choose core fixes first, then add the gameplay or hosting features you need.
Install each feature on the hosts its guide lists. Multi-host features need matching
parts; server-only features such as new-account defaults need no player client Mod.

## Supported game builds

The framework only touches code it can verify. Each exe build has a *host key*,
`<exe name>-<TimeDateStamp>-<SizeOfImage>`, and a matching file in `sdgo\symbols\`:

| exe | host key | known distributions |
|---|---|---|
| GOnline.exe | `GOnline-54BE1568-00C97000` | SDGO Ultimate Repack v1.2 (and the client it is based on) |
| BattleServer.exe | `BattleServer-548A891D-0046C000` | the common private server pack (BattleServer1..N folders) |
| AgentServer.exe | `AgentServer-548A86BC-000D7000` | Native Agent relay hosts |
| WorldServer.exe | `WorldServer-548A86E3-0010C000` | Native World account initialization and settlement host |

The install scripts print the host key of your exe. With another build the framework still
loads, logs that no symbol file matches and keeps every mod disabled, so the game is
unaffected. See [Other game builds](#other-game-builds).

## Install

Requirements: Windows with Windows PowerShell 5.1 (built in). No VC++ redistributable is
needed. Download `SDGO-ModFramework-<version>.zip` from the
[latest release](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest), extract it,
then open PowerShell in the extracted folder.

### Client (features with a client component)

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

### Agent servers

Use the same server installer with the Agent role for networking tools and the relay log fix:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server\AgentServer1" -Role Agent
```

The default role is Battle. Owner/player profile preparation is covered by the
[FRP distribution guide](frp-distribution.md).

### World servers

WorldServer core fixes use the same installer with `-Role World` (stop that
WorldServer first):

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server\WorldServer" -Role World
```

Base and Full packages built from current master include the default-enabled [new-account policy](new-account-profile.md)
and [rank repair](rank-progression-fix.md). Neither needs a client component. Install
on each independent World instance, restart World, and check `loaded` plus the
new-account `new-account defaults active` line in `sdgo/logs/sdgo.log`.
If a downloaded release does not yet contain `server/sdgo/mods/new_account_profile/`,
[build a package from current source](#building-from-source).

New-account defaults are 5,000 GP, zero cash and Corporal 1, with at most one automatic
unit gift from first profile creation. Copy the bundled `config.example.ini` to
`sdgo/mods/new_account_profile/config.ini` to customize them, then restart World.
`InitialUnitLimit=0` removes only those automatic gifts; normal starter selection and
later unit acquisition remain available. Existing pilots and hangars keep their data;
use a fresh account for [manual acceptance](new-account-profile.md#verification).

To disable only the new-account policy, set `new_account_profile=0` under `[mods]`
in World's `sdgo/sdgo.ini` and restart. Use `-Role World -Uninstall` only when removing
the entire World framework.

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

Each feature's guide names its configuration file. Mods with a separate `config.ini`
read its `[settings]` first; without that file they read `[settings]` in `mod.ini`.
The new-account policy uses `sdgo/mods/new_account_profile/config.ini`. The updater
preserves declared owner configuration files.

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

A different `GOnline.exe`, `BattleServer.exe`, `AgentServer.exe` or `WorldServer.exe` needs its own `sdgo\symbols\<host key>.ini`
with addresses and signatures found for that build; see
[docs/framework.md](framework.md#symbol-files). Until then the framework loads and does
nothing.

## Building from source

Visual Studio 2022 (C++ tools) and CMake 3.20 or newer and Python 3.10+ (build tools only); both targets are 32-bit.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\package.ps1     # dist\SDGO-ModFramework-<version>.zip
```

The install scripts also install straight from a build:
`tools\install_client.ps1 -GameDir ... -Source build\stage\Release\client`.


## Category and feature switches

In each relevant host's `sdgo/sdgo.ini`:

```ini
[categories]
gameplay=0
[features]
pvp-ai=0
[mods]
net_probe=0
```

A zero disables that category, complete feature, or individual mod. A category or
feature switch cannot turn a mod on when its own manifest is disabled. Existing
configurations without these sections keep their previous behaviour. The
categories are `fixes`, `gameplay`, `network`, and `development`. Diagnostics are
off by default. Apply a multi-host feature's switches on each relevant client/server.

## Package contents and updates

The framework is shared by all features: client `d3d9.dll`, server `winmm.dll`,
`sdgo/core.dll`, symbol files and `sdgo/sdgo.ini`. Mod directories remain at
`sdgo/mods/<id>/`; categories do not change their loading paths.

`CONTENTS.md` and `sdgo/catalog.json` list the features actually selected for the
package. A feature can include several client/server mods. Updates keep your
mod switches, settings, declared user config files and other installed mods.
Use `-OverwriteConfig` only when you intend to restore bundled defaults.
