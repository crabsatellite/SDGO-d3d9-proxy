# Mods

Settings live in each mod's `mod.ini` (`[settings]`); values cannot carry inline comments.
Addresses below are virtual addresses at the preferred base 0x00400000; the symbol names
refer to `symbols/<host key>.ini`.

* [coop_ai + mission_ai: co-op mission wingmen](#coop_ai--mission_ai-co-op-mission-wingmen)
* [ime_fix](#ime_fix)
* [net_probe](#net_probe)

## coop_ai + mission_ai: co-op mission wingmen

Two halves of one feature: `coop_ai` on the battle server adds AI pilots to the room,
`mission_ai` on every client spawns them. Usage, the room title commands and how to add
units are in the [README](../README.md#co-op-mission-wingmen).

### How it works

The game already has AI participants: in AI battle (room mode 11, `0x004B79D0`) the battle
server adds one AI pilot per human, in AI battleship (mode 12, `0x004B88E0`) also AI pilots
on the human team. Each is a full pilot record with an id that has bit 31 set, built from an
NPC of the server's AI-NPC list. Because the server knows them, their hits, kills and
deaths go through the normal battle code. The wingmen reuse exactly that.

**Server (`coop_ai`).**

* Co-op missions are room modes 5, 8 and 10: the modes the battle-start dispatcher
  (`0x004B9BF0`) exempts from team checks and the client runs as `CMissionMode`. For them the
  dispatcher sets up the humans (`0x004B6EB0`), then announces the start
  (`battle.start_notify`, `0x004B61E0`). `coop_ai` detours that announcement and, for a
  mission room, adds the wingmen right before it. (`SDBattlePlus.dll` detours the call site
  at run time but still calls the announcement.)
* The title is not in the battle room. It is read from the lobby room: a human pilot id is
  looked up in the lobby proc's map (`battle.lobby_proc` + 0x14, under its lock at + 0x38),
  and the room record's title is at + 0x53 (length) / + 0x54.
* For each wingman: pick a unit from the level pool that is in the AI-NPC list
  (`battle.ai_npc_list`, filled from `tbl_NpcData` ids 3604001–3607000 when the server
  starts), build the pilot with `battle.build_ai_pilot`, write the slot table, then run the
  per-slot setup of the AI modes with team 1 (`battle.pilot_reset`, `pilot_state_*`,
  `battle.setup_ai_pilot`). The announcement then includes the new slots.

**Client (`mission_ai`).**

* The client puts the room's AI participants into `room+0x3C` (records of 0x344 bytes), but
  `CMissionMode::Init` only spawns the humans. `mission_ai` detours it and, afterwards, spawns
  each AI record with the steps of `CAINormalGameMode::Init`'s AI loop (`0x004049B3`):
  start point (`um.find_start_point`), `mode.create_npc` on the room master (a CNPC that
  runs the Lua AI named by its NPC data, the AI battle bots' `VSAI_*` scripts) or
  `mode.create_remote_npc` elsewhere, `um.set_player_info`, the NPC name, `um.add_ai_unit`,
  and the server id at `unit+0x2B68`; then `um.calculate_team`.
* The player count stays at the humans: mission scripts loop over `GetPlayer(1..n)`, which
  only knows humans.
* AI battle mode and its relatives derive from `CMissionMode` and spawn their AI themselves;
  `mission_ai` only acts on a plain `CMissionMode` (checked by vtable).
* `mission_ai.lua` runs in the game's Lua 5.0 state each frame of a mission. Mission enemy
  scripts pick targets through the global `SearchLockPlayer`, which only considers humans;
  the script wraps it so an enemy may switch to a wingman closer than its target.
  `SDGO_Wingmen()` returns the wingmen's object ids. Edits to the script or `mod.ini` apply
  within 2 seconds, even mid-mission.

### coop_ai settings (battle server)

| key | default | meaning |
|---|---|---|
| `RequireRoomCommand` | 1 | 1: only rooms whose title has `!ai`; 0: every co-op mission |
| `DefaultCount` | 0 | wingmen for a bare `!ai`; 0 fills the free slots |
| `SquadSize` | 4 | player slots of a co-op room; wingmen never exceed SquadSize minus humans |
| `DefaultLevel` | 2 | level when the title has no `lvN` |
| `LogRoom` | 1 | log the lobby room and title at every mission start |
| `Level1`..`Level5` | see file | NPC data ids per level; ids not in the AI-NPC list are skipped |

Changes need a battle server restart.

### mission_ai settings (client)

| key | default | meaning |
|---|---|---|
| `EnemiesTargetWingmen` | 1 | 1: an enemy may switch to a wingman closer than its target |
| `EnemyAggroRange` | 2500 | farthest wingman an enemy switches to |
| `LogCombat` | 0 | 1: log every wingman's HP and target every 5 s |

### Logs

Battle server (`sdgo\logs\sdgo.log` in the battle server folder):

```
coop_ai: mission start: lobby room 1 (user 6228) title '!ai3 lv4', 1 human(s), 0 AI; wingmen requested
coop_ai: slot 1: AI pilot 80000002 (NPC 3604014) joined on team 1
coop_ai: 3 wingmen added (level 4)
```

Client:

```
mission_ai: AI participant 80000002 joined as object 1014 (NPC 3604014, team 1, slot 1, AI runs here)
mission_ai: mission 1: 3 wingmen
```

## ime_fix

One call site reads `GCS_RESULTSTR` from `ImmGetCompositionStringW` and then treats the
returned byte count as a character count. `ime_fix` hooks the import and, for that call site
only (checked by return address, symbol `ime.result_call`), returns the length in characters.

| key | default | meaning |
|---|---|---|
| `Trace` | 0 | 1: log every committed IME string length |

## net_probe

Development aid: logs outgoing TCP packets (`net.send_packet`) and UDP `sendto` calls,
the first `MaxPerOpcode` packets of every opcode with a hex dump of `DumpBytes` bytes.
Off by default (`enabled=0` in its `mod.ini`).

| key | default | meaning |
|---|---|---|
| `MaxPerOpcode` | 60 | packets logged per opcode |
| `DumpBytes` | 64 | bytes dumped per packet |
