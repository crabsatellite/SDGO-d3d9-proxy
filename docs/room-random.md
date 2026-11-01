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
commands. The 0.1.6 client companion applies complete records before that queue,
then refreshes them at the start boundary. It also records native human-factory
observations for manual acceptance. Failed starts and teardown restore originals.

Close the client, select it in Mod Manager and import the supplied
`SDGO-RoomRandomClient-0.1.6-beta.zip`, then use the normal launcher. No new settings
are needed. Distribute this overlay to every participant. It contains no game files
or server credentials, and its symbol fragment merges with other installed Mods.

The optional command `build/Release/room_random_test.exe D:/Server/BattleServer.exe D:/Client/GOnline.exe`
executes the actual server model-selection and client cache-copy instructions
in private fixture memory, without game entry points, imports or networking.
Full mission/PvP models, weapons, respawn and reward persistence remain manual acceptance.

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
