# Per-round random owned loadouts (Beta)

Add the standalone token `!rand` or `!random` to a room title. Every human is
assigned a new loadout from their own eligible inventory each round. Single-unit
modes retain that unit through respawns. Native deck modes 6/7 get distinct owned serials,
at most four slots and exactly ten native points. When alternatives exist, the next
round excludes the previous model or model set. Insufficient inventories cancel
the whole start; they never borrow another account's units or silently use eight points.

This feature needs `room_random` on BattleServer and `room_random_client` on every
participating client. It uses native constructors and messages and does not update
`GoUserUnitNum`, inventory or saved decks. AI mods retain their own pools.
The core's shared room-command service scopes arguments to each command. Both
`!rand !ai3 lv4` and `!ai3 lv4 !rand` work; keep AI options immediately after
`!ai`, before the next `!command`. Native short titles retain their 20-byte limit.
For longer descriptive names, enable [extended room titles](room-titles.md) on clients,
Agents and Battle servers and update `room_random` plus the AI Mods you use. For example:
`Friendly practice for newcomers !ai e2 a1 lv4 !rand` (128 Unicode scalars / 512 UTF-8 bytes maximum).
The whole title is read, including commands after byte 20; randomization itself remains server-side.
Title transport does not install or enable the AI or random-loadout features.
See [combined commands](room-commands.md).

`!ai !rand` is valid. The server sends native `0x0861` full-unit and, for deck modes,
`0x098B` full-ship notifications before `0x06C3` battle start. Version 0.1.5 alone
was insufficient in real gameplay: the 3D room handles those notices as queued
commands. Client 0.1.8 updated the visible 3D cache, but live testing still created
the old model from a separate pre-start snapshot. Client 0.1.9 also supplies the
shared native human converter with a private complete source matched to this
round's start entry and owner. It preserves unit UIDs, skills, appearance and full
decks without overwriting the frozen snapshot. Failed starts and teardown restore originals.
The mission retest confirmed a changed controlled unit, matching the server and
native factory logs. Client 0.1.10 removes that temporary factory observer; the
functional dispatch and human-construction hooks remain. Full PvP, multiplayer,
respawn and reward-persistence acceptance is still pending.

Server 0.1.7 also retires stale bookkeeping when a disconnect/abort skipped the
resource-release callback. It requires the native setup's cleared human pilot
records before accepting the next round, preserves newly populated selections
and rejects overwriting genuinely live loadouts. This prevents a stale round
from causing `ERR_BATTLE_START_FAIL` on re-entry.

Close the client, select it in Mod Manager and import the supplied
current `SDGO-RoomRandomClient-*-beta.zip`, then use the normal launcher. No new settings
are needed. Distribute this overlay to every participant. It contains no game files
or server credentials, and its symbol fragment merges with other installed Mods.

The 3D path does not use the legacy profile room pointer. It resolves the current
3D manager and calls the native room/slot accessors; mission and ordinary room
layouts differ. The runtime fixture covers a null legacy pointer and a separate
mission-slot layout. Missing live context remains a logged rejection.
The final converter resolves its supplied model directly, including after a
scene transition. A missing notice, changed owner/room/start entry or incomplete
deck leaves the native source unchanged.

The optional command `build/Release/room_random_test.exe D:/Server/BattleServer.exe D:/Client/GOnline.exe`
executes the actual server model-selection and client cache-copy instructions
in private fixture memory, without game entry points, imports or networking.
Beyond the confirmed mission model change, other modes, weapon/skill fidelity,
respawn, multiplayer and reward persistence remain manual acceptance.
The real core/DLL regression also covers frozen mission/PvP sources and complete
deck conversion. Supply a local client to execute the authenticated native source
selection and model/attribute projection spans as well:

```powershell
powershell -File tests/room_random_client_runtime_test.ps1 -GameExe "D:\Client\GOnline.exe"
```

The incremental installer updates core and already installed `pvp_ai` / `coop_ai`
DLLs together, preserving their manifests and settings. It does not install missing
AI mods. Updated mods also share the same SDK parser when run under an older core.

See [the Chinese guide](room-random.zh-CN.md) for host configuration, installation,
the exact supported executable, source evidence, limits and manual acceptance.
Automatic checks are not live multiplayer proof.
The feature is catalogued as `gameplay` / `room-random` / `beta` / `room-title`.
Category installers can select `-Features room-random`; the category, feature and
per-mod switches retain their normal precedence. Keep both components enabled;
AI composition retains the AI mods' existing client requirements.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_room_random.ps1 `
  -ServerRoot "D:\Server\BattleServer1" -DatabaseIni "D:\Server\WorldServer\WorldServer.ini"
```

Install into a stopped instance with the mod framework already installed. The
incremental package includes no game files, account data or database credentials.
