# Capsule interfaces for other Mods

The capsule feature lives in master. Reuse its semantic catalog and compilation
pipeline when another owner tool needs to add capsule machines. Do not maintain
an independent writer for the same client tables or install another hook at the
same native file entry point. The Python interfaces below are owner-side tools;
there is no capsule-specific state added to the core C ABI.

## Catalog and edits

`tools/capsule_catalog.py` owns schema version 1, validation, acquisition analysis,
initialization, mutations and semantic export. Add `tools` to the owner tool's
module path and use:

```python
from capsule_catalog import read_json, validate, mutate, digest, export_bundle

catalog = read_json(catalog_path)
validate(catalog)
updated = mutate(catalog, "fixed_machines", "update", machine_id,
                 {"price": owner_price})
revision = digest(updated)
export_bundle(updated, semantic_output)
```

`mutate` returns a validated copy. Supported actions are `create`, `update` and
`delete`; delete retires the machine and reserves its ID. Restore through a
validated update. Persist with the UI's revision lock or the CLI's backup path.
The native visible fixed-machine list is reversed relative to catalog order;
use the compiler's `placements.json` for player-facing locations.

Preserve source identity, native machine metadata and
`custom_candidate_policy.native_candidate_unit_ids`. That whitelist is bound to
the imported source, not a way to authorize arbitrary added S/SS units. The UI
rebinds imported configuration to current source facts before accepting it.
New machine IDs must avoid native, existing and retired IDs. Slot weights are
positive integers totaling 1000; `draw_policy` distinguishes independent weights
from a retained native sequence. A label or price edit alone does not convert a
native pool to independent selection.

`digest(catalog)` hashes canonical sorted-key UTF-8 JSON. It is the revision used
by client and server exports. Keep derived assets and private connection settings
outside the semantic catalog. Do not assign a revision manually or combine
outputs from different catalogs.

## Compile, preview and apply

| Module | Contract |
|---|---|
| `capsule_client_compile.compile_catalog(catalog, client, lookup, output)` | Compile native overlay and write runtime hashes, placements and asset manifest |
| `capsule_server_runtime` | Build install/verify/rollback SQL from the semantic server export |
| `capsule_ui.CatalogStore` | CRUD, import, template and unit queries; expected-revision checks on writes; compile/apply callbacks |
| `capsule_deployment.DeploymentTarget` | Build a revision-aligned distributable package, back up and apply configured targets |
| `package_capsule_editor.py` | Package public tools/configuration without local credentials or test-environment scripts |

```python
from pathlib import Path
from capsule_deployment import DeploymentTarget

target = DeploymentTarget(
    client_target=Path(owner_game),
    server_ini=Path(owner_server_ini),  # None means client-only application
    framework_stage=Path(current_framework_client_stage),
    symbol_fragment=Path(capsule_symbol_fragment),
)
receipt = target.apply(updated, compiled_output, semantic_output)
```

Use a current framework stage so another Mod's newer loader/core/symbols are
preserved. The installer merges symbol sections, preserves other Mods and rejects
conflicting non-owned symbols. It checks both the target executable and mapped
capsule DLL before replacement. Never stop a game by name to bypass these checks.
On SQL Server 2014, the adapter uses the ODBC transaction as the outer transaction;
after commit it requires `@@TRANCOUNT = 0`. Verify persistence with a new connection.

The HTTP UI binds to loopback. Queries include `/api/bootstrap`, `/api/machine`,
`/api/units` and `/api/catalog/download`. Changes and `/api/export`, `/api/compile`,
`/api/apply` carry `expected_revision`; stale edits are rejected. Reuse `CatalogStore`
when embedding another UI rather than calling private installation helpers.

## Resource and native boundaries

The runtime accepts only `0033`, `0101`, `0154` and `0155` capsule GDT overlays,
declared SPR/TXR resources and the exact extra `uirs/1161.uir` overlay. The checksum
bridge is scoped to the four declared GDT streams; UI resources do not widen it.
All declared bytes must match their manifest hashes before hooks are enabled.

Generated SPR/TXR IDs currently use 60000–65535. Other Mods must avoid that range
or coordinate a shared allocation before combining output. A compiler should
fail rather than wrap or overwrite IDs. Prefer verified native two-state cards;
render reconstructed heads into the native frame and picture window. Do not
treat a local generated sprite lookup as proof of authentic native artwork.

The price UI patch accepts only a known source layout/hash, clears eight default
UTF-16 first code units and changes no geometry, fonts or colors. Unknown or
partially patched inputs are rejected. Changes to supported client versions,
resource IDs, the JSON schema or the wire/runtime format require coordinated
compiler/runtime/installer changes and corresponding version checks.

Keep tests focused on public behavior: CRUD and retirement, source-bound policy,
revision conflicts, compile round trips, overlay scope, SQL persistence and
installation rollback. Actual account charging, delivery and gameplay remain
manual acceptance steps.
