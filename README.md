# SDGO mod framework

A mod loader for the SD Gundam Online game client (`GOnline.exe`) and battle server
(`BattleServer.exe`), plus the mods built on it. The headline mod adds **AI wingmen to co-op
missions**: AI pilots that join your team the way the game's own AI battle mode adds bots,
fight with the game's own bot AI, score real kills and can be shot down.

Unofficial fan project. It contains no game files; SD Gundam Online and its assets belong
to their owners. 简体中文说明：[README.zh-CN.md](README.zh-CN.md)

* [What is in the package](#what-is-in-the-package)
* [Supported game builds](#supported-game-builds)
* [Install](#install) · [Uninstall](#uninstall)
* [Co-op mission wingmen](#co-op-mission-wingmen)
* [Adding more bot units](#adding-more-bot-units)
* [Configuration](#configuration) · [Troubleshooting](#troubleshooting)
* [Building from source](#building-from-source) · [Writing mods](docs/framework.md)

## What is in the package

```
SDGO-ModFramework-<version>/
  client/             goes next to GOnline.exe       d3d9.dll + sdgo\
  server/             goes next to BattleServer.exe  winmm.dll + sdgo\
  install_client.ps1  installs client\ into a game folder
  install_server.ps1  installs server\ into battle server folders
  README.md, README.zh-CN.md, docs\, LICENSE, THIRD_PARTY_NOTICES.md, licenses\
```

| part | where | what it does |
|---|---|---|
| `d3d9.dll` | game folder | Loader: GOnline.exe loads it at start; it starts `sdgo\core.dll` and forwards Direct3D to the real `d3d9.dll`. |
| `winmm.dll` | battle server folder | Same loader for BattleServer.exe (forwards `timeGetTime`). |
| `sdgo\core.dll` | both | Framework core: checks the exe build, verifies every code address it hands out, owns all hooks, loads mods. |
| `sdgo\symbols\*.ini` | both | Verified addresses and byte signatures, one file per supported exe build. |
| `sdgo\mods\<id>\` | both | One folder per mod: `<id>.dll`, `mod.ini` (manifest + settings), extra files. |

Mods in this release:

| mod | host | what it does |
|---|---|---|
| `coop_ai` | battle server | Adds AI wingmen to co-op missions whose room title asks for them (`!ai3 lv4`). |
| `mission_ai` | client | Spawns those wingmen in the mission and lets mission enemies target them. |
| `ime_fix` | client | Fixes IME (Chinese/Japanese/Korean input) commits at one call site that reads a byte count as a character count. |
| `net_probe` | client | Diagnostic packet logger for mod development; off by default. |

## Supported game builds

The framework only touches code it can verify. Each exe build has a *host key*,
`<exe name>-<TimeDateStamp>-<SizeOfImage>`, and a matching file in `sdgo\symbols\`:

| exe | host key | known distributions |
|---|---|---|
| GOnline.exe | `GOnline-54BE1568-00C97000` | SDGO Ultimate Repack v1.2 (and the client it is based on) |
| BattleServer.exe | `BattleServer-548A891D-0046C000` | the common private server pack (BattleServer1..N folders) |

The install scripts print the host key of your exe. With another build the framework still
loads, logs that no symbol file matches and keeps every mod disabled, so the game is
unaffected. See [Other game builds](#other-game-builds).

## Install

Requirements: Windows with Windows PowerShell 5.1 (built in). No VC++ redistributable is
needed. Download and extract the release zip, then open PowerShell in the extracted folder.

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
* Re-running the script updates an existing install and keeps your edited `.ini` files
  (`-OverwriteConfig` replaces them with the defaults).

Manual install: copy the contents of `client\` into the game folder (next to GOnline.exe).

Check: after the game has started, `sdgo\logs\sdgo.log` begins with
`core: SDGO core started by d3d9 loader ...`, `symbols: N of N verified` and one
`loaded <mod>` line per mod.

### Battle server (whoever hosts the server)

Stop the battle servers, then:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server"
```

`-ServerRoot` is either one battle server folder (with `BattleServer.exe`) or the folder
that holds several (`BattleServer1`, `BattleServer2`, ...). `-Only BattleServer3` limits it to
named folders. Start the battle servers again afterwards. How they are started (a server
manager, batch files, a service) does not matter. Manual install: copy the contents of
`server\` into each battle server folder.

The wingmen need both halves: `coop_ai` on the battle servers and `mission_ai` on the
clients. Players without the client part do not see the wingmen.

## Uninstall

```powershell
powershell -ExecutionPolicy Bypass -File .\install_client.ps1 -GameDir "D:\Games\SDGO" -Uninstall
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server" -Uninstall
```

This removes the loader DLL and `sdgo\` (to the Recycle Bin) and puts a chained
`d3d9.dll`/`winmm.dll` back under its original name. To turn a single mod off instead, set
`<mod id>=0` under `[mods]` in `sdgo\sdgo.ini`.

## Co-op mission wingmen

Put a command in the co-op mission room title (the title holds 20 bytes):

| title | effect |
|---|---|
| `!ai` | fill the free player slots with wingmen |
| `!ai2` | two wingmen (still capped by the free slots) |
| `!ai3 lv4` | three wingmen from the level 4 pool |
| no `!ai` | no wingmen (unless `RequireRoomCommand=0`) |

* Wingmen are added when the mission starts. They are AI pilots on your team, added by the
  battle server the same way the AI battle modes add theirs, so the game treats them as
  real participants: their kills count, enemies' hits on them count, and they are destroyed
  like any unit.
* They fight with the game's own AI battle bot AI (the `VSAI_*` scripts of the AI battle
  bots), on the room host's client.
* The squad never exceeds `SquadSize` (4) players: wingmen = SquadSize minus humans, or
  fewer if `!aiN` asks for fewer.
* Levels pick from the AI battle bot units (`tbl_NpcData` 3604001–3604030). Default pools:
  `lv1` Dynames, Nadleeh, Exia, Sword Calamity, Launcher/Sword Strike, Deathscythe Hell,
  Blitz, Berga-Giros, Palace Athene; `lv2` Brave, Blu Duel, Aile Strike, Vigna Ghina,
  Gundam (MC), Hyaku Shiki; `lv3` mobile armors (Val-Walo, Big-Zam, Psycho Gundam,
  Apsalus II); `lv4`/`lv5` Strike Freedom, Zeta (awakened), Crossbone X3, Sandrock,
  Infinite Justice, Seravee, Master Gundam, Deathscythe Hell (EW), Heavy Arms (EW), Neue Ziel.
  Without `lvN` the room uses `DefaultLevel` (2).
* Mission enemies normally only target human players; the client mod lets an enemy switch
  to a wingman that is closer than its current target (`EnemiesTargetWingmen`).

Settings: `sdgo\mods\coop_ai\mod.ini` on the battle server, `sdgo\mods\mission_ai\mod.ini` on
the client. Details: [docs/mods.md](docs/mods.md).

## Adding more bot units

The level pools can only use NPCs that the battle server knows as AI battle bots. A battle
server builds that list when it starts: every row of `GOnlineGame.dbo.tbl_NpcData` whose
`NpcDataID` is between **3604001 and 3607000** becomes an AI bot (the stock database has 30,
3604001–3604030). The mods never write to the database; a pool id that is not in the list
is skipped, and the battle server log says
`no NPC of LevelN is in the server's AI-NPC list` if a whole pool is missing.

A new bot unit needs three things that match each other:

1. **A server database row** in `tbl_NpcData` with a new `NpcDataID` in the range.
   `UseUnitID` is the unit it flies; `UseUnitHP`, `NpcPoint` and `NpcUnitLevel` set its
   strength. The simplest start is a copy of an existing bot:

   ```sql
   INSERT INTO GOnlineGame.dbo.tbl_NpcData
     (NpcDataID, NpcName, UseUnitID, UseUnitHP, NpcPoint, ObjectHpUp, NpcType, AddUnitEXP,
      AddPoint, BossFlag, NpcUnitLevel, ItemBuffNumber, CreateUnitHp, DestroyUnitHp)
   SELECT 3604031, 'MY NEW BOT', UseUnitID, 16000, NpcPoint, ObjectHpUp, NpcType, AddUnitEXP,
          AddPoint, BossFlag, NpcUnitLevel, ItemBuffNumber, CreateUnitHp, DestroyUnitHp
   FROM GOnlineGame.dbo.tbl_NpcData WHERE NpcDataID = 3604001;
   ```

   The unit (`UseUnitID`) must exist in the server's unit data. The stock bots use the
   dedicated AI units 3607001–3607030; a different, non-AI unit has not been tested.

2. **A client NPC record** with the same id. Every player's client needs it, or it cannot
   spawn the unit. NPC records live in the client's grid data, table `0198` (inside
   `GPackData`, encrypted). Rows are 598 bytes:

   | offset | field |
   |---|---|
   | `0x00` | NPC data id (u32), e.g. 3604031 |
   | `0x05` | display name (UTF-16) |
   | `0x69` | unit id (u32), e.g. 3607001 |
   | `0x6D` | AI script name (UTF-16): one of the AI battle bot scripts, `VSAI_Gunfighter_01`–`10`, `VSAI_Infighter_01`–`07`, `VSAI_Tanker_01`–`05`, `VSAI_Stoker_01`–`08` |
   | `0xE5` | HP (u32) |

   The unit must exist in the client's unit tables as well (the AI units are in tables
   `0199` and `0245`). This repository has no grid data editor; any tool that decrypts and
   rewrites the client's `.gdt` tables can do it.

3. **A pool entry**: add the id to a `LevelN=` line in the battle server's
   `sdgo\mods\coop_ai\mod.ini`, then restart the battle servers (they load the NPC table and
   the mod settings at start).

Check: the battle server log (`sdgo\logs\sdgo.log` in its folder) shows
`slot N: AI pilot ... (NPC 3604031) joined on team 1`, and the room host's client log shows
`AI participant ... joined as object ... (NPC 3604031 ...)`.

## Configuration

`sdgo\sdgo.ini` (one per exe folder):

| section / key | meaning |
|---|---|
| `[core] log_level` | 0 debug, 1 info (default), 2 warn, 3 error |
| `[loader] chain_d3d9` / `chain_winmm` | another DLL to forward to instead of the system copy, relative to the exe folder (set by the installers) |
| `[mods] <id>=0` | turn a mod off |

Each mod's settings are in the `[settings]` section of its `mod.ini`; every key is
documented there and in [docs/mods.md](docs/mods.md).

## Troubleshooting

All diagnostics go to `sdgo\logs\sdgo.log` next to the exe (the previous run is
`sdgo.prev.log`).

| log line | meaning |
|---|---|
| no `sdgo.log` at all | the loader did not run: the DLL is not next to the exe, or another program replaced it |
| `no symbol file for host ...` | unsupported exe build; every mod stays off and the game is unaffected |
| `symbol X rejected: byte +N is .., expected ..` | the code at that address differs (another patch or a different build); mods that need X stay off |
| `required symbol X ...` / `0 of N mods active` | a mod could not start because a symbol it needs failed |
| server: `mission start: ... title '...'` | the battle server read the room title; `wingmen not requested` means no `!ai` |

### Other game builds

A different `GOnline.exe` or `BattleServer.exe` needs its own `sdgo\symbols\<host key>.ini`
with addresses and signatures found for that build; see
[docs/framework.md](docs/framework.md#symbol-files). Until then the framework loads and does
nothing.

## Building from source

Visual Studio 2022 (C++ tools) and CMake 3.20 or newer; both targets are 32-bit.

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\package.ps1     # dist\SDGO-ModFramework-<version>.zip
```

The install scripts also install straight from a build:
`tools\install_client.ps1 -GameDir ... -Source build\stage\Release\client`.
Architecture, the mod API, symbol files, tests and how to write a mod:
[docs/framework.md](docs/framework.md).

## License

MIT, see [LICENSE](LICENSE). Third-party code: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
(MinHook, BSD 2-Clause).
