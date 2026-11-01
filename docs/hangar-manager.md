# Hangar manager (Beta)

Client-side sorting and duplicate exchange using the game's authenticated native
requests. [简体中文](hangar-manager.zh-CN.md)

Version 0.2.1 fixes non-clickable buttons and random colored rectangles on the
native page. Action buttons now use the adjacent native button's normal, hover
and pressed skins, font and text color. Import the repair overlay in Manager and
restart the target client to load it; no server restart is needed.

Version 0.2.2 publishes the action buttons only after all controls, skins and
captions initialize successfully. A partial failure leaves the Mod controls hidden
and preserves the original lock/delete event handler. Initialization is automatic
and uses the client's native assets; players supply no skin files or settings.
Distribute the new DLL with its symbol fragment. An already running old DLL still
requires the normal close, update and restart sequence.

Version 0.2.3 fixes clicks rejected by the old battle-pointer guard. The app's
state number identifies menu services; lobby objects have no game-mode field at
offset `+0C`. Page attachment and received clicks are logged separately from
successful Mod loading.

## Install and open

This gameplay extension is included in Full framework packages and omitted from
Base packages. Install the client portion with the [normal installer](setup.md#install).
Only the supported `GOnline-54BE1568-00C97000` client is supported; no server Mod,
account configuration or player Python runtime is required.

The dedicated hangar addon preserves existing core files and unrelated Mods.
Close the target client, extract that addon, then run:

```powershell
powershell -ExecutionPolicy Bypass -File install_hangar.ps1 -GameDir "D:\Games\SDGO"
```

On the in-game **unit management** page, click **一键整理** (sort) or **重复兑换**
(duplicate exchange) beside the native lock/delete buttons. Reward selection,
paged previews, both confirmations and stopping remain inside the same game page.
Closing it invalidates the preview. There is no desktop popup, global toolbar or
F8 shortcut, and the unified Manager does not need to stay open.
Disable it using `[features] hangar-manager=0` or `[mods] hangar_manager=0` in
`sdgo/sdgo.ini`; the gameplay category switch also applies.

## Sorting, preview and confirmation

Sorting swaps movable units through the native server operation. Locked, active,
battleship-assigned, rental, special and unknown-state units stay in place.
Trained units can be sorted but are excluded from exchange.
Duplicate preview fetches fresh full details before grouping identical untrained
copies. Different paint, skills or customization remain separate groups. The default
keeps two copies of each identical state. No exchange runs until you confirm it.

Choose native GB or native item rewards. The server controls the actual reward
through `tbl_UnitGradeInfo`; the bundled `units.tsv` only supplies preview/filtering
data. Items use the normal dismantling reward/claim flow, not a direct inventory
write. This server version generally uses filled experience capsules. Unknown units
are excluded from exchange and kept in place during sorting.

Every change revalidates the stable unit ID, slot and state, sends one request and
waits for the native inventory to match the expected result. A changed layout,
disconnect or timeout stops the batch without retry. Stop cancels future requests;
already confirmed operations are not rolled back.

## Refresh and verification limits

Successful native replies update the game's hangar data, then the Mod redraws it,
allowing normal operations without relogging. Completion and returning to the grid
only redraw the visible hangar; they do not fetch the whole inventory from the server. Login inventory
pagination cannot safely be reused as a complete cache reset. If a result is
uncertain, further operations stay blocked until the cache/session resets; relog.

Offline planner, native-adapter, native-page and installer fixtures pass. Page tests
cover host-owned child controls, layout bounds, pagination, confirmation, restoring
original visibility and invalidating stale pages. They use synthetic
inventories and do not prove live-game refresh or reward delivery. Manual acceptance
should begin with sorting, then known expendable duplicates for both reward choices,
followed by relogging to check persistence. No real account is modified by the tests.

## Settings and owner rules

Edit everyday preferences in the unified Mod Manager. The underlying
`sdgo/mods/hangar_manager/mod.ini` `[settings]` supports:

| Key | Default | Meaning |
|---|---|---|
| `KeepCopies` | 2 | Identical untrained copies to keep, from 1 to 20 |
| `SortAfterExchange` | 1 | Sort movable units after exchange |
| `RequestIntervalMs` | 150 | Minimum native request interval |
| `ReplyTimeoutMs` | 10000 | Maximum wait for inventory synchronization |
| `MaxSlots` | 1200 | Verified native movable slot limit |
| `RulesFile` | units.tsv | Preview table, relative to the Mod folder |

Installers preserve existing settings and `units.tsv`. The dedicated installer can
explicitly refresh rules with `-RefreshRules`; the full installer uses
`-OverwriteConfig` to replace preserved configuration. A server owner can regenerate
the non-user rules locally using an installed ODBC driver and `pyodbc`:

```powershell
python tools/export_hangar_rules.py --server-ini "D:\Server\StoreServer.ini" --output units.tsv
```

The standalone addon puts `export_hangar_rules.py` at its root. Database credentials
stay in the owner's existing local INI; only unit definitions and reward metadata
are exported. See the [Chinese guide](hangar-manager.zh-CN.md) for native packet
details and build/test commands.
