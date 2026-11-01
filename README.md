# SDGO mod framework

Core fixes and optional extensions for SD Gundam Online clients and private servers.
All mods share one framework for build verification, loading, configuration and updates.
Unofficial fan project; no game files are included. [简体中文](README.zh-CN.md)

**One entry: [SDGO Mod Manager](docs/mod-manager.md).** Daily management includes clients,
servers, shop/operators/capsules, rewards and networking. Automatic fixes live in the
archive; developer details expand separately. Each feature can copy an AI debugging
or GitHub issue handoff. [Repair PRs must disclose the actual AI models used](docs/contributing.zh-CN.md).

**Start with core fixes, then choose your extensions.** Beta is a release channel, independent of category.

[Install and configure](docs/setup.md) · [Latest release](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest) · [Developer API](docs/framework.md)

## Core fixes

Client compatibility fixes, server settlement repairs and new-account defaults.
Each fix can be disabled separately.

<!-- mod-category:fixes -->
| Feature | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| IME input fix | Correct the length of committed IME text | `ime_fix` | Clients | [Use](docs/ime-fix.md) |
| Input method freeze fix | Restore window lookup needed by input methods alongside existing plugins | `findwindow_fix` | Clients | [Use](docs/findwindow-fix.md) |
| Startup crash fix | Avoid loading screens referencing missing units | `startup_crash_fix` | Clients | [Use](docs/startup-crash-fix.md) |
| Agent relay log fix | Correct native relay logging | `agent_relay_log_fix` | Agent servers | [Use](docs/agent-relay-log-fix.md) |
| Battle rank regression fix | Prevent a higher sublevel from lowering the major rank, such as 15-1 → 5-2 | `rank_progression_fix` | World server | [Use](docs/rank-progression-fix.md) |
| New-account starter profile | Configure starting currency, rank and unit grants while preserving existing progress | `new_account_profile` | World server | [Use](docs/new-account-profile.md) |
| Shop rental and Unicode labels | Explicit rental/permanent labels in hover, buy and gift options, with guarded Unicode captions; also distributed separately | `shop_labels` | clients | [Guide](docs/shop-labels.md) |
<!-- /mod-category:fixes -->

The rank repair loads through the framework in WorldServer. It routes matching battle calls to
Mod-owned procedures while preserving the original procedures. Players need no client update.

<details>
<summary>Input method example</summary>

![Using an input method in the lobby](docs/findwindow-fix-ime.png)

</details>

## Gameplay extensions

Optional features for a more complete solo or multiplayer game. NPCs, unit pools
and random-unit gameplay belong in this category.

<!-- mod-category:gameplay -->
| Feature | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| Mission AI wingmen | Add AI teammates with `!ai` in a mission title; [long-title combinations](#long-room-title-commands) | `coop_ai`, `mission_ai` | Battle servers + clients | [Use](docs/coop-wingmen.md) |
| PvP AI (Beta) | AI opponents and teammates with scoring, respawn, decks and spectating; [long-title combinations](#long-room-title-commands) | `pvp_ai`, `match_ai` | Battle servers + all participants | [Use](docs/pvp-ai.md) |
| AI mech catalog (Beta, separate addon) | Expand LV1–LV5 AI pools using paired owner-generated data; restart with it disabled to restore stock pools; [long-title combinations](#long-room-title-commands) | `ai_catalog` | Battle servers + all participants | [Use](docs/ai-catalog.md) |
| Battle contribution rewards (Beta) | Native contribution settlement, durable MB and owner-controlled multipliers | `battle_rewards` | Battle servers + owner worker | [Use](docs/battle-rewards.md) |
| Collection capsule (Beta) | Activity machine: 100,000 GP for a never-acquired initial form, weighted by rank; configure and distribute through Mod Manager | `collection_capsule` | World server + client machine resources | [Use](docs/collection-capsule.md) |
| Per-round random loadouts (Beta) | Use `!rand` to randomize each human's owned units every battle; deck modes total exactly ten points. Composes with `!ai`; [long-title combinations](#long-room-title-commands) | `room_random` | Battle server | [Use](docs/room-random.md) |
| Extended room titles (Beta) | Full mission and PvP titles, including rules after byte 20 | `room_titles`, `room_titles_agent`, `room_titles_server` | Clients + Agent + Battle | [Use](docs/room-titles.md) |
| Hangar manager (Beta) | Sort movable units and preview duplicate exchanges for native GB or item rewards; confirm before exchanging | `hangar_manager` | Clients | [Use](docs/hangar-manager.md) |
| Operator supplies and manager (Beta) | Configure portraits and reward rules; five permanent free support officers with in-place migration | `operator_catalog` | Clients + owner assistant/SQL adapter | [Use](docs/operator-assistant.md) |
| Daily double rewards (Beta) | Double normal G points, pilot EXP and unit EXP daily at 18:00–22:00 in configurable server-local time | `daily_rewards` | battle servers | [Guide](docs/daily-rewards.md) |
<!-- /mod-category:gameplay -->

On the in-game **unit management** page, use **一键整理** (sort) or **重复兑换** (duplicate exchange)
beside the native lock/delete buttons. Reward selection, paged previews, both confirmations
and stopping stay inside that page. Preferences remain in the unified Mod Manager.
Identical untrained copies default to two kept. Exchange excludes trained units;
locked, active, battleship-assigned, rental and unknown-state units are protected.
Successful native operations
update the visible hangar without relogging; an unconfirmed result stops the batch
and requires relogging. Real-game acceptance remains manual. See the
[hangar guide](docs/hangar-manager.md) for installation, preview rules and reward handling.

Daily double rewards need only a BattleServer update; players keep their current clients.
See the [setup and manual acceptance guide](docs/daily-rewards.md#manual-acceptance).

Open the operator host UI with the included `tools/Start-Assistant.ps1` to configure
names, portraits and drop probabilities/amounts. Five permanent free support officers
cover blueprints, experience, cosmetics, upgrade kits and capsule coins. Existing
holdings migrate in place, preserving equipped references and personal nicknames.
The owner applies server rules and distributes a matching client update; players
keep their normal launcher. The module registers products for the storefront to
price and list. In-game display, claiming and settlement still need manual acceptance.
See the [configuration and migration guide](docs/operator-assistant.md).

The AI catalog is a separate, default-off addon; Base/Full framework packages do
not install it. The owner generates `catalog.bin` from their database catalog and
matching client tables, installs both halves, and exports a player overlay.
Players run its included setup once, then use their normal launcher. Room titles
such as `!ai e2 a1 lv2` use the expanded random pool; there is no extra selection
screen. No SQL rows or game archives are rewritten. See the
[owner installation and player distribution guide](docs/ai-catalog.md).

<a id="long-room-title-commands"></a>

### Longer room names and combined commands

Mission wingmen, PvP AI and random loadouts can use [extended room titles](docs/room-titles.md)
for descriptive names with several commands. With the matching three parts enabled,
limits are **128 Unicode scalars / 512 UTF-8 bytes**, including commands after byte 20:

| Use | Complete room title example |
|---|---|
| Mission: request three level-4 wingmen and randomize humans | `Weekend mission with friends !ai3 lv4 !rand` |
| PvP: two enemies, one ally and random human loadouts | `Friendly practice for newcomers !ai e2 a1 lv4 !rand` |

Existing player-slot limits still apply. Keep `lv4`, `e2` and `a1` after `!ai`, before the next `!command`.
Enable `room_titles` on clients, `room_titles_agent` on Agents and `room_titles_server` on Battle servers,
and update the consuming `coop_ai` / `pvp_ai` / `room_random` DLLs. Players use their normal launcher.
The three title Mods default to off; AI and random-loadout features still need their own installation and settings.
Without the title extension, use native 20-byte short titles.
[Full command grammar and examples](docs/room-commands.md).


<details>
<summary>Short AI gameplay examples</summary>

Mission wingmen:

![Mission AI wingmen](docs/coop-wingmen-demo.webp)

PvP AI (Beta); also supports death match, Grid, leader and Tag:

![AI teammates and opponents in battle](docs/pvp-ai-demo.webp)

</details>

## Networking and hosting

Connections, relays, port mapping and owner deployment tools have their own
category and owner-supplied configuration. They are managed separately from
gameplay. The package's feature directory lists the tools actually included.

<!-- mod-category:network -->
| Feature | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| FRP connectivity (Beta) | Owner-configured endpoint mapping and reusable UDP ports | `net_route`, `agent_udp_pool` | Clients + Agent servers | [Use](docs/frp-distribution.md) |
| Shop catalog manager (Beta) | Windows owner UI for product CRUD, pricing, listing and portable configuration; uses native shop billing | `shop_catalog` | World server + owner window | [Guide](docs/shop-catalog.md) |
<!-- /mod-category:network -->
Double-click `SdgoShopManager.exe` to manage offers in a Windows owner window. Save edits as drafts,
then use “Apply to shop”; players keep using the native shop. [Manual checks (中文)](docs/shop-testing.zh-CN.md).
The label patch and catalog manager also have separate packages; exports contain no database credentials.


## Reward rules and verification

[Battle contribution rewards (Beta)](docs/battle-rewards.md) are wired to native
settlement and the owner wallet worker. Owners change the GB/MB base multiplier
at `/rewards`; players keep their normal client and launcher. Kills, effective
damage, assists, mission minions and Boss contribution affect payouts, and PvP
MVP earns additional MB. AI matches use the same PvP tier. Each extra qualifying
human multiplies rewards by another `1.1`, together with double time: base ten,
double time and four humans give `10 × 2 × 1.1³ = 26.62`.

Use the full package's `rewards/` toolkit to install every participating
BattleServer with one shared owner workspace, then run
`tools/Start-Battle-Rewards.ps1` for the wallet worker and assistant. It can mount
the installed operator assistant on the same port while preserving its modules;
closing the browser does not stop the worker. Stop each target BattleServer for
installation and restart it afterward; later multiplier changes are hot-loaded.
See [installation, startup and recovery](docs/battle-rewards.md) and the
[full reward rules](docs/battle-reward-design.md). Real account combat, HUD and
reconnect acceptance remain manual; installation keeps existing unrelated state.

## Developer tools

For development and troubleshooting; disabled by default.

<!-- mod-category:development -->
| Tool | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| Packet logger | Capture outgoing packets for protocol troubleshooting | `net_probe` | Clients | [Use](docs/net-probe.md) |
| Native GameTest (Experimental) | Run assertions inside isolated native game instances | `game_test_client`, `game_test_server` | Clients + Battle servers | [Use](docs/game-test.md) |
<!-- /mod-category:development -->

## Install and use

1. Follow the [installation guide](docs/setup.md#install) for the hosts required by your chosen feature. New-account defaults and rank repair install only on World; players need no client component.
2. Start with your usual launcher and enable extensions using their guides.
3. Use the installer to update while keeping your configuration. Categories,
   whole features and individual mods have separate switches.

`CONTENTS.md` and `sdgo/catalog.json` describe the features selected for the package.
[Installation, removal and troubleshooting](docs/setup.md) · [Build and add extensions](docs/framework.md#adding-a-feature)

MIT: [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
