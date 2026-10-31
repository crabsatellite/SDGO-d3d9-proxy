# PvP AI players

Use the existing PvP room title to add AI opponents and teammates. They run the
game's native AI battle bot scripts and participate in its kill, respawn, deck,
HUD scoring and dead-view flows. 中文：[pvp-ai.zh-CN.md](pvp-ai.zh-CN.md)

This is a source feature added after the released 1.1.2 package. Use a build
containing both parts below; the existing 1.1.2 release does not include them.

| Mod | Install on | Responsibility |
|---|---|---|
| `pvp_ai` | Every battle server handling these rooms | Room-title commands, AI pilots, teams, decks, scores, respawn and cleanup |
| `match_ai` | Every participating client | AI units and native scripts, remote copies, player lookup, HUD and spectator support |

Use matching versions on all participants. Follow the standard
[client and battle server installation](../README.md#install). Configuration is
relative to the game folder; it needs no developer-machine paths or FRP service.

## Use

Set the room title, ready up and start normally. AI pilots join at battle start;
empty slots in the waiting room are not already-spawned AI. The native title
holds 20 bytes; the ASCII examples below fit.

| Title | Effect |
|---|---|
| `!ai` | Fill each team to `FillTeamSize` (default 3), including humans and existing AI |
| `!ai3` | Add three enemies |
| `!ai e2 a1 lv2` | Two enemies, one ally, level 2 unit pool |
| `!ai e3 a2 lv4` | Three enemies, two allies, level 4 pool |
| `!ai0` or `!ai e0 a0` | No AI; keep the native player/team start checks |
| No `!ai` | Do not enable the feature |

Commands are case-insensitive. `eN` is the team opposite the room host; `aN` is
the host's team. Requests are bounded by six slots per team and twelve overall;
human slots are not overwritten. `!ai lvN` with no count still fills both teams.

`lv1`–`lv5` select NPC/unit pools, not smarter decision-making. This feature does
not include the separate navigation, stuck recovery or aggression enhancement.
For custom NPC pools, both sides need matching NPC and unit data; see
[adding more bot units](coop-wingmen.md#adding-more-bot-units).

## Modes, scoring and spectators

The client integrates five native PvP mode classes. Menu translations vary;
Grid and Tag correspond to room modes 6 and 7 in the code.

| Mode | Integration |
|---|---|
| Normal PvP | AI join either team and participate in combat and scoring |
| Death match | AI use the configured player-grade life costs, native respawn rules and HUD accounting |
| Grid (6) | Distinct AI deck units, at most four and ten points; units rotate on respawn; gauge includes AI |
| Tag (7) | The same deck limits; units/lives are consumed and the next eligible unit is selected by native rules |
| Leader match | Team size and scoring include AI; leader handling follows the native mode |

Grid uses a common full-bar value on both server teams and the HUD: the native
value for the larger team when their sizes differ. Tag uses deck points for HUD
and kill accounting. Missing NPCs or failed deck additions cannot compact later
units into different server indices.

The native dead view can select AI players; its tracking camera looks toward
their combat target. Respawn remains subject to the current mode's rules. At a
normal battle end, added AI leave the room slots and are added again from the
title at the next battle start.

The host simulates the AI; other clients display remote copies. A lone human's
AI battle skips the peer NAT check that has no other human to answer it. Rooms
with multiple humans retain the game's networking. This feature does not
configure forwarding or relays.

## Settings

Server: `[settings]` in `sdgo/mods/pvp_ai/mod.ini`; restart the affected battle
servers after changing it. Values cannot have inline comments.

| Key | Default | Meaning |
|---|---|---|
| `DefaultLevel` | 2 | Pool when no `lvN` is specified, 1–5; invalid values fall back to 2 |
| `FillTeamSize` | 3 | Total participants per team for a bare `!ai`, 1–6; invalid values use 6 |
| `LifeCostGrade1`…`4` | 250/334/455/556 | Death match life costs for C/B/A/S grades; 0 preserves the NPC cost |
| `Level1`…`Level5` | See file | NPC data ids; only entries in the server's AI-NPC list are used; overflowing ids are ignored |
| `LogRoom` | 1 | Log mode, team counts and room title at battle start |
| `LogBattle` | 0 | Diagnostic score, death and respawn logs |

Client: `sdgo/mods/match_ai/mod.ini`; `LogErrors` and `LogDiagnostics` are off
by default. Remove `!ai` from the title to stop adding AI at the next start.

## Logs and troubleshooting

Logs are beside the host exe in `sdgo/logs/sdgo.log`; simultaneous instances can
use `sdgo.2.log` through `sdgo.4.log`.

| Symptom | Check |
|---|---|
| No AI | Both mods loaded, title contains `!ai`, a valid NPC pool and free team slots |
| `no NPC of LevelN is in the server's AI-NPC list` | Match the configured ids to server NPC data |
| Server adds pilots but client has no units | `match_ai` loading and complete client NPC/unit data |
| `no matching deck`, missing NPC or failed append | Match both sides' unit data; the client retains only the successfully built consecutive prefix, so incomplete decks are not fully supported |
| Incorrect scoring, respawn or team display | Retain mode, full room title, versions, both sides' logs and a continuous recording |

Remove account information and server connection configuration before sharing
diagnostic material.

## Validation and implementation

The 2026-11-01 review build passed nine offline suites in both Release and Debug,
including 313 native checks and nine Lua scenarios per configuration. The two
supported hosts passed 101 static symbol checks. See the
[offline test guide](ai-offline-tests.md).

These fixtures invoke production mod code with synthetic rooms, packets, HUDs
and callbacks. They do not establish actual rendered behaviour, every native
hook, multiplayer synchronization or host migration. No new gameplay recording
was produced for this merge; the [recording checklist](pvp-ai-video.zh-CN.md)
tracks manual demonstration and acceptance separately.

The server adds AI pilots after human setup, recounts teams/scores, and handles
respawn and cleanup at room ticks/end. The client spawns AI in five mode classes
and supplies Lua bindings, object/player-slot mapping, decks and spectator
support. Hooks use the framework's named, byte-verified symbols; game asset
archives are not rewritten.
