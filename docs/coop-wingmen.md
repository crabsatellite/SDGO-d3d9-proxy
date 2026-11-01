# Co-op mission wingmen

AI pilots join your team in co-op missions the way the game's AI battle modes add their
bots. They fly with the game's own AI battle bot AI, score real kills and can be shot down.
中文：[coop-wingmen.zh-CN.md](coop-wingmen.zh-CN.md)

![Mission AI wingmen](coop-wingmen-demo.webp)

| mod | host | part |
|---|---|---|
| `coop_ai` | battle server | adds the AI pilots to the room when the mission starts |
| `mission_ai` | client | spawns them in the mission, runs their AI, lets enemies target them |

Both halves are needed: `coop_ai` on the battle servers and `mission_ai` on the clients.
Players without the client part do not see the wingmen.

* [Use](#use)
* [Settings](#settings)
* [Adding more bot units](#adding-more-bot-units)
* [Logs and troubleshooting](#logs-and-troubleshooting)
* [How it works](#how-it-works)

## Use

Put a command in the co-op mission room title:

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

## Longer names and combined commands

With the [extended-title client, Agent and Battle Mods](room-titles.md) enabled and an updated
`coop_ai`, commands are read from the full title, up to 128 Unicode scalars / 512 UTF-8 bytes.
For example, `Weekend mission with friends !ai3 lv4 !rand` requests level-4 wingmen and,
when `room_random` is installed and enabled, randomizes the humans each round.
Existing free-slot limits and `mission_ai` client requirements still apply.
Keep `lv4` inside the `!ai` argument span, before `!rand`.
Without the title extension, the native limit remains 20 bytes.
See [combined commands](room-commands.md) for both mission and PvP examples.

## Settings

Values go in the `[settings]` section of each `mod.ini` and cannot carry inline comments.

`coop_ai`, battle server, `sdgo\mods\coop_ai\mod.ini` (changes need a battle server restart):

| key | default | meaning |
|---|---|---|
| `RequireRoomCommand` | 1 | 1: only rooms whose title has `!ai`; 0: every co-op mission |
| `DefaultCount` | 0 | wingmen for a bare `!ai`; 0 fills the free slots |
| `SquadSize` | 4 | player slots of a co-op room; wingmen never exceed SquadSize minus humans |
| `DefaultLevel` | 2 | level when the title has no `lvN` |
| `LogRoom` | 1 | log the lobby room and title at every mission start |
| `Level1`..`Level5` | see file | NPC data ids per level; ids not in the AI-NPC list are skipped |

`mission_ai`, client, `sdgo\mods\mission_ai\mod.ini` (applies within 2 seconds, even
mid-mission):

| key | default | meaning |
|---|---|---|
| `EnemiesTargetWingmen` | 1 | 1: an enemy may switch to a wingman closer than its target |
| `EnemyAggroRange` | 2500 | farthest wingman an enemy switches to |
| `LogCombat` | 0 | 1: log every wingman's HP and target every 5 s |

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

Check: the battle server log shows `slot N: AI pilot ... (NPC 3604031) joined on team 1`, and
the room host's client log shows `AI participant ... joined as object ... (NPC 3604031 ...)`.

## Logs and troubleshooting

Battle server (`sdgo\logs\sdgo.log` in the battle server folder):

```
coop_ai: mission start: lobby room 1 (user 6228) title '!ai3 lv4', 1 human(s), 0 AI; wingmen requested
coop_ai: slot 1: AI pilot 80000002 (NPC 3604014) joined on team 1
coop_ai: 3 wingmen added (level 4)
```

Client (`sdgo\logs\sdgo.log` in the game folder):

```
mission_ai: AI participant 80000002 joined as object 1014 (NPC 3604014, team 1, slot 1, AI runs here)
mission_ai: mission 1: 3 wingmen
```

| symptom / log line | meaning |
|---|---|
| no `coop_ai: mission start` line (with `LogRoom=1`) | `coop_ai` is not loaded on that battle server (check the top of its log), or the room is not a co-op mission |
| `wingmen not requested` | the room title has no `!ai` and `RequireRoomCommand=1` |
| `no NPC of LevelN is in the server's AI-NPC list` | that battle server's database has none of the pool's ids in 3604001–3607000; see [Adding more bot units](#adding-more-bot-units) |
| server adds wingmen, a client log has no `AI participant` line | that client does not have `mission_ai` |
| client: `AI participant ...: creating NPC N failed` | that client has no NPC record N (client table `0198`) |

## How it works

Addresses are virtual addresses at the preferred base 0x00400000; the symbol names refer to
`symbols/<host key>.ini`.

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
* Tests: `mods/mission_ai/test/sim_mission.lua` drives `mission_ai.lua` against a mock of the
  game's script API (see [framework.md](framework.md#building-testing-packaging)).
