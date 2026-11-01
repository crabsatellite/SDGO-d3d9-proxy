# AI catalog addon

`ai_catalog` provides an optional owner-generated roster for the existing co-op
and PvP AI mods. Install its server DLL in each battle server and its client DLL
in every participating client. Both sides use the same generated `catalog.bin`.
Players keep their normal launcher. [中文与完整命令](ai-catalog.zh-CN.md).

The addon is disabled by default. It requires `coop_ai` + `mission_ai` for missions
or `pvp_ai` + `match_ai` for PvP. It does not change the native mode 11/12 roster.

## Owner workflow

Python 3.10+ is required only for owner tools. Generate from your own public
catalog JSON exports and the complete extracted GDT tables of the matching client:

```powershell
python backend/ai_catalog.py --db-export D:/OwnerData/GOnlineGame --client-gdrs D:/OwnerData/gdrs --generate --out my-roster
python backend/install_ai_catalog.py install --role server --target D:/Server/BattleServer1 --source . --catalog my-roster/server/sdgo/mods/ai_catalog/catalog.bin
python backend/install_ai_catalog.py install --role client --target D:/Games/SDGO --source . --catalog my-roster/client/sdgo/mods/ai_catalog/catalog.bin
python backend/export_ai_catalog.py --source . --catalog my-roster/client/sdgo/mods/ai_catalog/catalog.bin --out players.zip
```

Install the framework and AI gameplay mods first. Close each selected host before
installing or changing its state. `--source` is the standalone addon package root,
or the matching `build/catalog/stage/Release` tree. No shared core or loader is
replaced. Existing server AI DLLs receive the small optional provider bridge;
their manifests, settings and enabled states are preserved. Missing AI mods are
not installed by the addon.

The player export contains the enabled client addon, generated catalog, required
symbol fragment, one-time PowerShell setup, checksums and instructions. After
extracting into the stopped game's folder, run
`powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-AiCatalog.ps1`
once. Setup adds missing symbols without replacing other mods' definitions,
refuses conflicts and preserves the original symbol file. Players then use their
normal launcher; no Python/ODBC, owner backend, accounts or database settings
are included.

Keep `catalog.json` as the owner's allocation ledger. Pass it with
`--previous-catalog my-roster/catalog.json` on updates and choose a new output
directory. Retired IDs stay reserved. `--rules` supports per-unit `exclude`,
`levels`, and native NPC `templates`. Defaults group sortie-ready C/B/A/S/SS-SR
units into LV1/2/3/4/5; these are roster groups, not measured AI difficulty.
Without `--generate`, the tool remains a read-only audit/planner.

An ODBC catalog source is optional: use `--connection-env SDGO_SQL_CONNECTION`
instead of `--db-export`. Only the owner needs `pyodbc` and an SQL Server driver.
The connection string stays local; the reader executes fixed catalog SELECTs.

## Toggle and remove

Set `[mod] enabled=0/1` in `sdgo/mods/ai_catalog/mod.ini`, or use:

```powershell
python backend/install_ai_catalog.py disable --target D:/Games/SDGO
python backend/install_ai_catalog.py enable --target D:/Games/SDGO
python backend/install_ai_catalog.py uninstall --target D:/Games/SDGO
```

Apply to the server and every participating client, then restart them. There is
no live unloading. `[mods] ai_catalog=0` in `sdgo.ini` is also respected. With the
addon off, the AI mods use their original `LevelN` settings and native filtering.

The installer records originals and restores them on uninstall. It refuses to
overwrite another updater's subsequent changes; disabling remains available.
`recover --target ...` handles an interrupted installation when its scoped files
still match the recorded old/new values. It never modifies unrelated settings.

## Implementation and limits

No database rows or encrypted game archives are changed.
The server provides additional NPC records and private unit data only during an
expanded AI build, restoring thread-local context even on native exceptions.
After the last native NPC table loads, the client registers generated NPCs in
the process's native map and integrity registry. This also serves native creators
that bypass `grid.find` with an inline map lookup. Records, wrappers and nodes
use the game's allocator and are released by its normal table destructor.
Original unit bytes and ordinary lookups remain unchanged. All source hashes
and collisions are checked before publication; conflicts abort registration
without replacing native records. Creator guards refuse missing or different
catalog data. Restarting with the addon off loads only original archive records.
Generated owner data is not part of the generic addon distribution.

The addon expands the existing random LV pools; it does not add a mech selection
screen. For example, `!ai e2 a1 lv2` uses the expanded LV2 pool for two enemies
and one ally.

Supported builds: `GOnline-54BE1568-00C97000` and
`BattleServer-548A891D-0046C000`. Native AI script templates are selected
heuristically with owner overrides. Rendering, weapon behavior, transformations,
respawn gameplay and real multiplayer still require manual acceptance with the
paired client/server setup. Do not mix clients without the addon into expanded
AI rooms.

Build/package: `cmake --build build/catalog --config Release`, then
`python tools/package_ai_catalog.py`. Tests: Python `test_ai_catalog.py`, CTest's
`ai_catalog_integration`, and the optional caller-supplied-image tests
`ai_catalog_native_test` (server) and `ai_catalog_client_map_test` (client).
These tests do not execute the game's entry point or connect to a game server.
Automated checks are not gameplay evidence.
