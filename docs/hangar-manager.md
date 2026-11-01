# Hangar manager (Beta)

Server-persisted sorting and native duplicate exchange.
[简体中文](hangar-manager.zh-CN.md)

Version 0.4.0 makes GB preview immediate from the native inventory summary.
It no longer requests every full unit record or waits for one network dismantle
reply per unit. After two confirmations it transfers the exact selected IDs,
types and slots, then submits one server transaction. The server rechecks owner,
slot, active/locked/deck/rental/training protection, KeepCopies and reward rates;
calls native dismantling and credits GB atomically. Incomplete selections do
nothing; stale or failed batches roll back. Repeated messages do not repeat
deletion or rewards. Update the server adapter through Manager as well as the
client; log in again after submission. Experience-capsule exchange retains its
separate native per-unit path.

Sorting also ensures the registered capacity covers the resulting inventory in
six-slot blocks, within the native 1200-slot limit. An imported inventory larger
than `24 + ExpandSlotCount` can crash the native login slot writer.

Version 0.3.0 requires the owner to install the server sort adapter once through
Manager's hangar server panel, using the native INI with DB Game and DB Member.
Players submit one request through their existing game connection and log in
again. No client SQL credentials, extra port or resident Manager are needed.
The installer preserves normal native mail and can restore the original procedure.
It reads back SQL module hashes without sorting any player's inventory.

Duplicates now use UnitNumber/type, counting protected copies toward KeepCopies.
Full records remain exact-instance freshness guards. Untrained paint variants
count as the same type; lock copies you intend to retain before previewing.
Buttons leave the native page selector clear and long status text stays within
the right-hand panel. The following 0.2.x notes describe historical behavior.

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

Version 0.2.4 keeps the native unit grid visible during sorting and returns to it
after exchange. Sorting no longer opens the empty duplicate preview. The same
sort entry becomes a stop button while sorting; status appears below the grid.
Preview controls follow the actual right-hand grid origin in the full-screen
native dialog. Refresh follows the original list, icon and page-visibility order.
Zero changes means eligible units already match the selected order. Other menu
or room inventory views may retain their cache; log in again to synchronize them.

## Install and open

This gameplay extension is included in Full framework packages and omitted from
Base packages. Install the client portion with the [normal installer](setup.md#install).
Only `GOnline-54BE1568-00C97000` is supported. Sorting requires the server SQL
adapter; players need no account configuration or Python runtime.

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

Sorting transactionally changes UserUnitSlotNumber on the server, following the
existing script's grade, subgrade, unit type, acquisition date and stable-ID order.
Every other inventory column, active stable UID, lock, deck, GB and drop box is
preserved. Duplicate preview fetches fresh details, counts copies by type and
keeps at least two in total by default. Locked, active, deck-assigned, rental and
trained copies cannot be exchanged, but count toward that minimum. No exchange
runs until both confirmations.

Choose native GB or native item rewards. The server controls the actual reward
through `tbl_UnitGradeInfo`; the bundled `units.tsv` only supplies preview/filtering
data. Items use the normal dismantling reward/claim flow, not a direct inventory
write. This server version generally uses filled experience capsules. Unknown units
are excluded from exchange; server sorting retains unknown types after known grades.

Every change revalidates the stable unit ID, slot and state, sends one request and
waits for the native inventory to match the expected result. A changed layout,
disconnect or timeout stops the batch without retry. Stop cancels future requests;
already confirmed operations are not rolled back.

## Refresh and verification limits

The native mail transport has no proven sort-completion acknowledgement. The
client says submitted, never completed on send alone; server SQL records committed
requests independently. Relog after sorting; further Mod operations stay blocked
until the cache/session resets. Native exchange replies still drive per-item
confirmation and grid redraw.

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
| `KeepCopies` | 2 | Minimum copies per unit type, including protected copies, from 1 to 20 |
| `SortAfterExchange` | 1 | Submit server sorting after exchange; relog required |
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
