# Collection capsule (Beta)

Collection capsule is an independently installable and disableable gameplay Mod.
It adds one machine to the ordinary capsule list:

- default machine ID `990001`;
- a fixed price of **100,000 GP** per draw, with no other accepted currency;
- uniform selection among the account's currently eligible unit families;
- a permanent, independently ownable base/initial form that the account has never acquired;
- sold, consumed, or exchanged units remain acquired and cannot be drawn again;
- no owned unit is consumed, and non-ownable post-transform forms are not prizes.

The stock World path prepays the registered GP price. The Mod selects and delivers
the unit and never charges a second time. Exhaustion, incomplete history, inventory
delivery failure, database failure, or result-validation failure returns through
the native failure path, which should refund the complete prepayment and leave a
net charge of zero.

## Runtime and distribution boundary

The server installs the `collection_capsule` World Mod, SQL schema, and machine
price. The client uses the shared `capsule_catalog` for machine registration and
resources. The owner must take `server-package\` and `player\` from the same
`build-kit` output so both sides use matching machine data, resources, and framework.

The player package contains no server INI, database connection, account, collection
history, or FRP credential. The owner distributes only `player\`; players close
their client and run its installer.

This feature requires the current framework/core. Retaining an old core instead
of installing the matched current build is an update failure, not a supported
compatibility mode.

## Mod Manager owner tutorial

Use **SDGO Mod Manager → Collection capsule (Beta)** for everyday configuration.
Select the target WorldServer INI, matching client, and full framework or owner
toolkit directory. An optional cover must be a 182×108 PNG.

1. Generate matched resources. Only public configuration is exported; existing
   shared client machines are preserved.
2. Close the selected client and check installation conditions. Stop the selected
   WorldServer, then install its server package and the matching local client.
   Manager does not control game processes.
3. Restart that WorldServer, export the player package and distribute it to friends.
4. Import trusted complete acquisition history for legacy accounts. Current hangar
   contents alone are insufficient. Register reciprocal imported-family metadata
   before building when needed.

After separately updating the ordinary capsule catalog, regenerate and install
the composed client resources here before exporting a new player package.
Enable or disable the gameplay Mod on its World feature card and restart that
World; acquisition history is retained. Price is fixed at 100,000 GP. The internal
machine ID is developer information and must match SQL and client resources.
All paths remain in the owner's local workspace. There is no additional UI,
server or game launcher.

## Developer and automation commands

Run these commands from the source tree or portable owner-kit root that contains
`tools\collection_capsule.py`. All paths below are replaceable `D:\...` examples.

### 1. Prepare the current framework

When building from source, perform a complete Release build so the loader, core,
Mod, symbols, data, and shared `capsule_catalog` enter one matching stage:

```powershell
cmake --build build --config Release
```

The portable owner kit stores its framework under `ROOT\framework\`. By default,
`build-kit` selects:

1. `ROOT\framework\`;
2. `ROOT\build\stage\Release\` when the first directory is absent.

Use an explicit stage only when selecting another verified build:

```powershell
--framework-stage D:\SDGO\FrameworkStage
```

That directory must contain the matched `client\`, `server\`, and
`support\capsule-registry\capsule_catalog.dll`.

### 2. Export a public configuration snapshot

Export from the target server:

```powershell
python tools/collection_capsule.py export `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --out D:\SDGO\CollectionCapsuleWork\public-snapshot
```

The output directory must not already exist. This command exports only the public
configuration needed to build the pool:

- `dbo.tbl_Units` unit definitions;
- `dbo.tbl_MachinePrice` machine definitions;
- published imported-family metadata;
- row counts, file hashes, and an export-time receipt.

It exports no database credential, account, inventory, player acquisition history,
or other private row. The snapshot can be moved to a separate build computer; the
server INI itself must not be distributed.

### 3. Build the matched server and player packages

```powershell
python tools/collection_capsule.py build-kit `
  --snapshot D:\SDGO\CollectionCapsuleWork\public-snapshot `
  --client D:\SDGO\Client `
  --out D:\SDGO\CollectionCapsuleWork\collection-capsule-kit
```

The output directory must also be absent. The builder reads the real client,
validates native initial-form metadata, and uses one model to generate the server
pool and ordinary client machine.

If the owner's client already has a shared machine registry, the tool automatically
detects `D:\SDGO\Client\sdgo\mods\capsule_catalog\runtime.ini` and uses its directory
as the current registry. It preserves every other ordinary machine, custom machine,
and generated resource before adding or updating this machine. Select a registry
elsewhere with:

```powershell
--current-registry D:\SDGO\SharedCapsuleRegistry
```

Declarations for newly imported multi-form unit families can be repeated:

```powershell
--unit-family D:\SDGO\UnitPackages\family-a.json `
--unit-family D:\SDGO\UnitPackages\family-b.json
```

Standalone, unambiguous base units need no declaration. `--unit-family` supplies
inventory-root metadata; it is not a frozen prize allowlist.

The default placeholder cover is `mods\collection_capsule\assets\cover.png`.
A replacement must be an exact **182×108 PNG**:

```powershell
python tools/collection_capsule.py build-kit `
  --snapshot D:\SDGO\CollectionCapsuleWork\public-snapshot `
  --client D:\SDGO\Client `
  --cover D:\SDGO\Artwork\collection-capsule.png `
  --out D:\SDGO\CollectionCapsuleWork\collection-capsule-kit
```

The output contains:

- `server-package\`: owner-only World Mod, framework, SQL, and model;
- `player\`: the no-Python package distributed to players;
- `client-registry\`: the complete generated shared registry for audit or a later explicit update;
- `build-receipt.json`: matched server, registry, and player revisions and file receipts.

Distribute only `player\`. Do not distribute `server-package\`, the server INI,
or history-import files.

### 4. Check and install the server package

Run the read-only check first:

```powershell
python tools/collection_capsule.py install-server `
  --package D:\SDGO\CollectionCapsuleWork\collection-capsule-kit\server-package `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --check-only
```

The current `--check-only` validates the package manifest and hashes, the matched
server framework, the selected World executable, the existing owner machine ID,
symbol-merge compatibility, and the database connection, then reports the target
database. It does not copy files or execute SQL.

After the check passes, stop only the WorldServer selected by that INI. Other Agent,
Battle, or separate server instances do not need to stop. Remove `--check-only`
to install:

```powershell
python tools/collection_capsule.py install-server `
  --package D:\SDGO\CollectionCapsuleWork\collection-capsule-kit\server-package `
  --server-ini D:\SDGO\Server\World\WorldServer.ini
```

The installer first creates a recovery backup limited to this server payload, then
opens an outer database transaction. SQL installation and readback of the machine
ID, 100,000 GP price, enabled state, and pool hash occur inside that transaction.
The standard installer then updates this World's framework and Mod files, and the
database commits only after the file update succeeds. A caught failure rolls back
the database and restores only the controlled files. If another process changed a
controlled file concurrently, recovery retains that newer content and reports the
backup directory instead of overwriting external work.

This is a compensating installation workflow, not a distributed transaction that
can survive forced process termination, power loss, or disk failure atomically.
After such an interruption, keep the selected WorldServer stopped, inspect the
reported `sdgo-install-backups\collection-...` directory, database state, and file
hashes, then rerun the check and installation.

Start the selected WorldServer only after the command succeeds.

## Player tutorial

Each player receives the `player\` directory:

1. Close the client being updated.
2. Double-click `Install-Client.cmd`.
3. Select the game directory containing `GOnline.exe`.
4. Start normally and use Collection capsule in the ordinary capsule list.

The installer validates the client build, every package hash, and the required
symbols, then writes an installation receipt. It preserves the existing `sdgo.ini`,
unrelated Mods, unrelated symbol sections, and other shared machines. If a
third-party `d3d9.dll` exists, installation continues only when one explicit
supported loader chain can be established. A partial failure is restored from
the installation journal.

To remove the client feature, close the client, double-click
`Uninstall-Client.cmd` from the same player package, and select the same game
directory. The uninstaller uses its receipt to restore or remove only the files
controlled under `sdgo\mods\capsule_catalog\`. It preserves the current latest
framework/core, global settings, symbols, and unrelated Mods. If another tool
changed a controlled file after installation, uninstall refuses to overwrite
that newer content.

The ordinary machine screen still contains six preview tiles. They are presentation
anchors, not a prize whitelist. The server result may be any eligible, previously
unacquired base unit and need not appear among those six previews.

## Complete history for existing accounts

Installation treats existing registrations as incomplete history (`Coverage=0`). It seeds `Seen` with
facts from current inventory, native capsule records, and any recognized native
deletion/recovery records, but it does not label those partial facts as complete
from account creation. Such accounts cannot use Collection capsule until a trusted
source proves complete coverage. A failed attempt should receive native compensation
and have a net charge of zero.

The installer reads only the account-ID high water mark through the host INI's
`[DB Member]` connection; it does not export account rows. Updates retain this
original baseline. An old registration creating or recreating a profile is still
an old account. New registrations after installation are tracked from their first
profile and unit-acquisition transactions.

The Game identity needs the feature's schema/module/trigger and read/write
permissions. Unit writers also need transaction-ID access: `VIEW SERVER STATE`
on SQL Server 2014, or `VIEW SERVER PERFORMANCE STATE` on SQL Server 2022+.
See [Microsoft's permission reference](https://learn.microsoft.com/sql/relational-databases/system-dynamic-management-views/sys-dm-tran-current-transaction-transact-sql).
The tool checks capabilities without granting existing identities broader rights.
The Member identity must be able to read the aggregate of `tbl_Account.f_idx`.

Only an upstream system that can attest to every acquisition since account creation
may produce an old-account history file:

```powershell
python tools/collection_capsule.py import-history `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --history D:\SDGO\CollectionCapsuleWork\complete-history.json
```

Each file represents one account and must contain:

| Field | Required value or type |
|---|---|
| `schema_version` | integer `1` |
| `coverage` | string `from-account-creation` |
| `complete` | boolean `true` |
| `source` | non-empty exporting-system or receipt identity |
| `account` | the target account's positive SQL `INT` |
| `units` | every UnitID acquired since account creation, as an integer list |

The importer rejects missing completeness/source attestations, invalid accounts,
invalid lists, and any payload that omits a unit already recorded in `Seen`. It
adds history and records the payload hash; it never clears or resets collection
history.

**Current inventory alone cannot prove complete history.** A sold, deleted,
consumed, or exchanged unit may no longer be present. Do not reset `Seen` from
the current inventory or treat unavailable old events as events that never happened.

## Future imported unit families

Each draw derives eligible inventory roots from the live `tbl_Units` catalog.
Registration resolves only the initial-form identity of an ambiguous multi-form
family. It does not freeze a prize-ID list and does not require a DLL rebuild for
each new unit.

For existing native units, the builder resolves reciprocal forms from enabled
entries in the client's `0220.gdt`, checked against all six unit tables and the
server transform graph. All 28 ambiguous families in the current data have one
initial entry. Neither these families nor the catalog count are hardcoded; names
containing `After`, ID order, and historical inventory are not root evidence.
New unambiguous units enter automatically. Imported reciprocal forms declare
their initial form in the content package. Missing or conflicting declarations
reject the draw through the refund path instead of silently removing the family
while continuing to charge.

For multi-form content already installed in the live database, publish its
declaration with:

```powershell
python tools/collection_capsule.py register-family `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --unit-family D:\SDGO\UnitPackages\family-a.json
```

An `sdgo.unit-family` v1 declaration must contain:

- `installable: true` and a stable `package_id`;
- one explicit `family.root_form`;
- every form in `family.forms`;
- an integer `id`, unique `key`, and live-catalog-matching `transform_target` for each form.

A content importer should publish that metadata inside the same active transaction
that installs the unit rows:

```python
from owner.collection_capsule.deployment import register_unit_family

register_unit_family(db, declaration, in_transaction=True)
```

The unit rows and root metadata then commit or roll back together. The standalone
`register-family` command manages its own transaction and is appropriate when the
content is already installed. The next `export` writes published family metadata
into the public snapshot, and later `build-kit` runs reuse it automatically.

## Update, disable, and restore

For a catalog, cover, Mod, or framework update:

1. `export` the target database into a new empty snapshot directory;
2. run `build-kit` with a new empty output directory;
3. stop and update the selected World;
4. redistribute the new `player\` from the same build to every player.

If the build client does not have the current shared registry installed, pass the
previous build's `client-registry\` as `--current-registry` so unrelated machines
remain in the new package.

Disable the server machine while preserving collection history:

```powershell
python tools/collection_capsule.py disable `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --machine-id 990001
```

Restart the selected WorldServer after disabling, and have players run the matched
`Uninstall-Client.cmd`. Disabling the server machine or removing client resources
does not erase `Coverage`, `Seen`, or draw records. A later reinstall can continue
from the same history.

## Verification status and manual acceptance

Automated evidence has three distinct scopes:

- Python/offline tests cover form resolution, the dynamic pool, preservation of
  the shared registry, the player package, and receipt-based install/uninstall;
- isolated SQL tests cover existing-account coverage, complete-history import,
  concurrent non-repetition, dynamic imported families, transaction rollback,
  and a failure-compensation model;
- a native fixture has exercised the actual `core.dll`, `collection_capsule.dll`,
  and OLE DB sessions for the ABI, separate sessions, publish-after-commit, and
  malformed-result rollback.

The Python World-payment fixture simulates wallet prepayment and refund. Static
native-path evidence separately supports the stock prepay/nonzero-return refund
behavior. Neither is a real WorldServer + GOnline gameplay draw. The feature remains
Beta; real UI, animation, wallet movement, and restart persistence have not yet
been marked as gameplay-verified.

Basic source-tree checks:

```powershell
cmake --build build --config Release
python -m unittest discover -s tests -p "test_collection*.py" -v
ctest --test-dir build -C Release -R "^collection_capsule_offline$" --output-on-failure
```

Manual acceptance should confirm:

1. the ordinary list shows Collection capsule at the fixed 100,000 GP price;
2. a successful draw grants an independently ownable base form never acquired by that account;
3. selling it and drawing again cannot repeat the same unit family;
4. success produces exactly one net 100,000 GP charge;
5. exhaustion, incomplete history, delivery failure, and backend failure leave a net charge of zero;
6. unit, history, and machine state survive client and WorldServer restarts;
7. player uninstall leaves unrelated Mods, shared machines, and the current framework/core working.
