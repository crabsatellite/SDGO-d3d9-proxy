# SDGO mod framework

Core fixes and optional extensions for SD Gundam Online clients and private servers.
Unofficial fan project; no game files are included. [简体中文](README.zh-CN.md)

[Installation](docs/setup.md) · [SDGO Mod Manager](docs/mod-manager.md) · [Latest release](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)

## Install and use

1. Follow the [setup guide](docs/setup.md) to install the components required by your chosen features.
2. Use **SDGO Mod Manager** for installation, settings and owner tools. Players keep their normal game launcher.
3. Open each feature's guide below for configuration, gameplay, distribution and verification details.

Start with core fixes and add extensions as needed. Beta and Experimental are release channels, independent of category.
The package's `CONTENTS.md` and `sdgo/catalog.json` list the features it includes.

## Core fixes

<!-- mod-category:fixes -->

| Feature | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| IME input fix | Correct committed text handling | `ime_fix` | Clients | [Use](docs/ime-fix.md) |
| Activity capsule currency | Match the displayed coin to native billing | `capsule_currency_fix` | Clients | [Use](docs/capsule-currency-fix.md) |
| Input method freeze fix | Restore compatible window lookup | `findwindow_fix` | Clients | [Use](docs/findwindow-fix.md) |
| Startup crash fix | Skip loading screens with missing units | `startup_crash_fix` | Clients | [Use](docs/startup-crash-fix.md) |
| Agent relay log fix | Correct relay logging | `agent_relay_log_fix` | Agent servers | [Use](docs/agent-relay-log-fix.md) |
| Battle rank regression fix | Prevent rank regression | `rank_progression_fix` | World server | [Use](docs/rank-progression-fix.md) |
| New-account starter profile | Configure starting currency, rank and units | `new_account_profile` | World server | [Use](docs/new-account-profile.md) |
| Shop rental and Unicode labels | Clarify rental terms and item captions | `shop_labels` | Clients | [Use](docs/shop-labels.md) |

<!-- /mod-category:fixes -->

## Gameplay extensions

<!-- mod-category:gameplay -->

| Feature | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| Mission AI wingmen | Add AI teammates to missions | `coop_ai`, `mission_ai` | Battle servers + clients | [Use](docs/coop-wingmen.md) |
| PvP AI (Beta) | Add AI opponents and teammates | `pvp_ai`, `match_ai` | Battle servers + all participants | [Use](docs/pvp-ai.md) |
| AI mech catalog (Beta, separate addon) | Expand AI unit pools with paired catalog data | `ai_catalog` | Battle servers + all participants | [Use](docs/ai-catalog.md) |
| Battle contribution rewards (Beta) | Configure contribution rewards and multipliers | `battle_rewards` | Battle servers + Manager | [Use](docs/battle-rewards.md) |
| Collection capsule (Beta) | Draw previously unacquired unit forms | `collection_capsule` | World server + client resources | [Use](docs/collection-capsule.md) |
| Per-round random loadouts (Beta) | Randomize owned human loadouts each battle | `room_random` | Battle servers | [Use](docs/room-random.md) |
| Extended room titles (Beta) | Support longer titles and combined commands | `room_titles`, `room_titles_agent`, `room_titles_server` | Clients + Agent + Battle | [Use](docs/room-titles.md) |
| Battle AI difficulty (Beta) | Choose Easy, Medium or Hard combat AI | `battle_ai`, `battle_ai_server` | Battle servers + all participants | [Use](docs/battle-ai.md) |
| Hangar manager (Beta) | Sort units and preview duplicate exchanges | `hangar_manager` | Clients | [Use](docs/hangar-manager.md) |
| Operator supplies and manager (Beta) | Configure operator portraits and rewards | `operator_catalog` | Clients + owner SQL adapter | [Use](docs/operator-assistant.md) |
| Capsule catalog editor (Beta) | Configure machines, pools and portraits | `capsule_catalog` | Clients + owner SQL adapter | [Use](docs/capsule-catalog.md) |
| Daily double rewards (Beta) | Configure a daily double-reward window | `daily_rewards` | Battle servers | [Use](docs/daily-rewards.md) |

<!-- /mod-category:gameplay -->

<a id="long-room-title-commands"></a>

AI and room-title command combinations: [grammar and examples](docs/room-commands.md).

## Networking and hosting

<!-- mod-category:network -->

| Feature | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| FRP connectivity (Beta) | Configure endpoints and UDP port mapping | `net_route`, `agent_udp_pool` | Clients + Agent servers | [Use](docs/frp-distribution.md) |
| Shop catalog manager (Beta) | Manage native shop offers through Mod Manager | `shop_catalog` | World server + Manager | [Use](docs/shop-catalog.md) |

<!-- /mod-category:network -->

## Developer tools

Diagnostics are disabled by default. Automatic fixes appear in the Manager archive; internal fields and diagnostics are in its developer view.

<!-- mod-category:development -->

| Tool | Purpose | Mods | Install on | Guide |
|---|---|---|---|---|
| Packet logger | Inspect outgoing packets | `net_probe` | Clients | [Use](docs/net-probe.md) |
| Capsule price probe (Experimental) | Inspect capsule price refresh | `capsule_price_probe` | Clients | [Use](docs/capsule-price-probe.md) |
| Native GameTest (Experimental) | Run assertions in isolated native game instances | `game_test_client`, `game_test_server` | Clients + Battle servers | [Use](docs/game-test.md) |

<!-- /mod-category:development -->

## Project documentation

[Build and developer API](docs/framework.md) · [Contribution and AI disclosure rules](docs/contributing.zh-CN.md) · [Setup and troubleshooting](docs/setup.md)

MIT: [LICENSE](LICENSE). Third-party notices: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
