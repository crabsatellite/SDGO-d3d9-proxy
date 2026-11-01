# Per-round random owned loadouts (Beta)

Add the standalone token `!rand` or `!random` to a room title. Every human is
assigned a new loadout from their own eligible inventory each round. Single-unit
modes retain that unit through respawns. Native deck modes 6/7 get distinct owned serials,
at most four slots and exactly ten native points. When alternatives exist, the next
round excludes the previous model or model set. Insufficient inventories cancel
the whole start; they never borrow another account's units or silently use eight points.

This is a server-only mod using native constructors and start packets. It does not
update `GoUserUnitNum`, inventory or saved decks. No process is restarted by the
installer. AI mods retain their own pools and use different hook sites.
The core's shared room-command service scopes arguments to each command. Both
`!rand !ai3 lv4` and `!ai3 lv4 !rand` work; keep AI options immediately after
`!ai`, before the next `!command`. The title still holds at most 20 bytes.

The incremental installer updates core and already installed `pvp_ai` / `coop_ai`
DLLs together, preserving their manifests and settings. It does not install missing
AI mods. Updated mods also share the same SDK parser when run under an older core.

See [the Chinese guide](room-random.zh-CN.md) for host configuration, installation,
the exact supported executable, source evidence, limits and manual acceptance.
Automatic checks are not live multiplayer proof.
The feature is catalogued as `gameplay` / `room-random` / `beta` / `room-title`.
Category installers can select `-Features room-random`; the category, feature and
per-mod switches retain their normal precedence. It needs only the server mod;
AI composition retains the AI mods' existing client requirements.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_room_random.ps1 `
  -ServerRoot "D:\Server\BattleServer1" -DatabaseIni "D:\Server\WorldServer\WorldServer.ini"
```

Install into a stopped instance with the mod framework already installed. The
incremental package includes no game files, account data or database credentials.
