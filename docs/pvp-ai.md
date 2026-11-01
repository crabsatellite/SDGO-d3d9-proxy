# PvP AI players (Beta)

Put `!ai` in a PvP room title to add AI opponents and teammates, using the existing
combat, scoring, respawn and spectator features. 中文：[pvp-ai.zh-CN.md](pvp-ai.zh-CN.md)

This feature is in **Beta**.

![AI teammates and opponents in battle](pvp-ai-demo.webp)

Also supports death match, Grid, leader and Tag modes.

## Install

Use a Beta build containing both parts, with matching versions on every participant:

| Mod | Install on |
|---|---|
| `pvp_ai` | Battle servers |
| `match_ai` | Every participating client |

Follow the [installation guide](setup.md#install), then use your usual launcher.

## Use

Set the room title, ready up and start normally. AI join at battle start.

| Title | Effect |
|---|---|
| `!ai` | Fill both teams to three participants, including humans and existing AI |
| `!ai3` | Add three enemies |
| `!ai e2 a1 lv2` | Two enemies and one ally from the level 2 unit pool |
| `!ai e3 a2 lv4` | Three enemies and two allies from the level 4 pool |
| `!ai0` or no `!ai` | Do not add AI |

`eN` means the team opposite the host; `aN` means the host's team. Commands are
case-insensitive, with at most six participants per team and twelve overall.
`lv1`–`lv5` select unit pools. With [extended room titles](room-titles.md) enabled on clients,
Agents and Battle servers, an updated `pvp_ai` reads commands anywhere in the confirmed full title
(up to 128 Unicode scalars / 512 UTF-8 bytes). For example:
`Friendly practice for newcomers !ai e2 a1 lv4 !rand`.
With `room_random` also installed and enabled, humans randomize each round while AI retain their own pools.
Keep `e2 a1 lv4` after `!ai`, before `!rand`; all participants still need `match_ai`.
Without the title extension, the native 20-byte limit remains.
See [combined commands](room-commands.md).

Each battle samples the complete eligible pool again, including deck entries within
the mode's points/count limits. The owner supplies the unit pool; selection covers
its eligible candidates while enforcing unique deck entries and the mode limits.

The host runs the AI and other clients display synchronized copies. Grid/Tag
modes can use AI decks. The existing dead view can follow AI players. Respawn,
life consumption and victory conditions follow the selected mode.

`match_ai 0.1.2` repairs the original search's participant indices and team-1 branch:
bots on either team can target opposing humans and AI while excluding teammates.
This compatibility fix also applies to Easy or clients without enhanced Battle AI.
See [Battle AI](battle-ai.md) for the separate Medium/Hard controller and version checks.

## Settings

Server: `[settings]` in `sdgo/mods/pvp_ai/mod.ini`; restart the affected battle
servers after changes.

| Setting | Default | Meaning |
|---|---|---|
| `DefaultLevel` | 2 | Pool when no `lvN` is specified, 1–5 |
| `FillTeamSize` | 3 | Total participants per team for `!ai`, 1–6 |
| `LifeCostGrade1`…`4` | 250/334/455/556 | Death match life costs for C/B/A/S grades; 0 preserves the original |
| `Level1`…`Level5` | See file | NPC data ids in each unit pool |
| `LogRoom` | 1 | Log room information at battle start |
| `LogBattle` | 0 | Score, death and respawn logs for troubleshooting |

Custom pools need matching NPC/unit data on both sides; see
[adding more bot units](coop-wingmen.md#adding-more-bot-units). Values cannot have
inline comments. Client `LogErrors` and `LogDiagnostics` are off by default.

Logs are in `sdgo/logs/` beside the client or battle server. If AI do not appear,
check that both mods loaded, the room command, available unit data and free team slots.
