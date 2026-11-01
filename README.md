# SDGO mod framework

Core fixes and optional extensions for SD Gundam Online clients and private servers.
All mods share one framework for build verification, loading, configuration and updates.
Unofficial fan project; no game files are included. [简体中文](README.zh-CN.md)

**Start with core fixes, then choose your extensions.** Beta is a release channel, independent of category.

[Install and configure](docs/setup.md) · [Latest release](https://github.com/crabsatellite/SDGO-d3d9-proxy/releases/latest) · [Developer API](docs/framework.md)

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
| New-account starter profile | Start with 5,000 GP, zero cash and Corporal rank; cap automatic first-profile unit gifts at one; keep existing progress | `new_account_profile` | World server | [Guide](docs/new-account-profile.md) |
<!-- /mod-category:fixes -->

The rank repair loads through the framework in WorldServer. It routes matching battle calls to
Mod-owned procedures while preserving the original procedures. Players need no client update.

**New-account defaults are enabled in Base and Full packages built from current master; only the owner installs them on WorldServer.**
New players start with **5,000 GP, zero cash and Corporal 1**. Automatic unit gifts from
the first-profile call are capped at one instead of granting an entire catalog.
Existing ownership and progress are preserved. Normal starter selection, later purchases,
capsule draws and rewards continue under the native rules.

Owner settings live in `sdgo/mods/new_account_profile/config.ini`. `InitialUnitLimit=1`
caps those automatic gifts; use `0` to remove them. To disable the Mod, set
`new_account_profile=0` under `[mods]` in `sdgo/sdgo.ini` and restart World. Future
creations return to native rules; existing accounts keep their data.
See the [new-account installation, configuration and acceptance guide](docs/new-account-profile.md).
This new Mod is on master. If your downloaded release lacks
`server/sdgo/mods/new_account_profile/`, [build a package from current source](docs/setup.md#building-from-source).

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
| Mission AI wingmen | Add AI teammates with `!ai` in a mission title | `coop_ai`, `mission_ai` | Battle servers + clients | [Use](docs/coop-wingmen.md) |
| PvP AI (Beta) | AI opponents and teammates with scoring, respawn, decks and spectating | `pvp_ai`, `match_ai` | Battle servers + all participants | [Use](docs/pvp-ai.md) |
| Battle contribution rewards (Beta) | Native contribution settlement, durable MB and owner-controlled multipliers | `battle_rewards` | Battle servers + owner worker | [Use](docs/battle-rewards.md) |
| Per-round random loadouts (Beta) | Use `!rand` to randomize each human's owned units every battle; deck modes total exactly ten points. Composes with `!ai`. | `room_random` | Battle server | [Use](docs/room-random.md) |
<!-- /mod-category:gameplay -->

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
<!-- /mod-category:network -->

## Reward rules and verification

[Battle contribution rewards (Beta)](docs/battle-rewards.md) are wired to native
settlement and the owner wallet worker. The [rule guide](docs/battle-reward-design.md)
contains the approved values and offline calculator. Real account combat, HUD and
reconnect acceptance remain manual; installation keeps existing unrelated state.

## Developer tools

For development and troubleshooting; disabled by default.

<!-- mod-category:development -->
| Tool | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| Packet logger | Capture outgoing packets for protocol troubleshooting | `net_probe` | Clients | [Use](docs/net-probe.md) |
<!-- /mod-category:development -->

## Install and use

1. Follow the [installation guide](docs/setup.md#install) for the hosts required by your chosen feature. New-account defaults and rank repair install only on World; players need no client component.
2. Start with your usual launcher and enable extensions using their guides.
3. Use the installer to update while keeping your configuration. Categories,
   whole features and individual mods have separate switches.

`CONTENTS.md` and `sdgo/catalog.json` describe the features selected for the package.
[Installation, removal and troubleshooting](docs/setup.md) · [Build and add extensions](docs/framework.md#adding-a-feature)

MIT: [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
