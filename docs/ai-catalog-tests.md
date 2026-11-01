# AI catalog validation

Validation date: 2026-11-01. Feature branch: `feature/ai-unit-catalog`, based on
`pvp-ai`. Builds and owned runtime fixtures are under `build/catalog`; owner data
and generated catalogs remain outside Git. The owner subsequently installed the
addon in the original AI environment. Client 0.1.1 has been installed and enabled;
this repair did not restart servers or change SQL/game archives. Gameplay
acceptance is pending the user's next test.

## Results

| Check | Result | Evidence scope |
| --- | --- | --- |
| Python catalog/install/export/package tests | 32 passed | Includes exported Windows player setup: symbol merge, preserved unrelated content, idempotence and conflict refusal |
| Release CTest suite after master integration | 33 passed | Current framework, co-op/PvP, shared room commands, rewards, category gates, packaging/installers, staging and catalog integration |
| Debug catalog integration | 20 scenarios passed | Fresh processes with real core and updated DLLs; other Debug suites were run during the initial 0.1.0 validation |
| Real core + catalog DLL | 24 Release scenarios after integration; earlier 20 Debug scenarios | Off/on/off/on, feature/category disables, collisions, changed/deferred source, corrupt/missing file, competing hook owner and rollback |
| Actual native client map + owner records | 819 units, 18055 checks in each of Release and Debug | Shipped addon DLL, native Find and inline tree lookup, insertion, integrity registration, idempotence, unchanged original units and complete native destructor cleanup |
| Actual BattleServer builder code | 819 units, 4117 checks, Release and Debug | Caller-supplied image mapped without its entry point, with owned maps/records/buffers |
| Host symbols | 84 client symbols verified, 0 rejected; earlier 27 server symbols verified | Supported on-disk images; new client loader/map/allocator/checksum symbols included |
| Standalone package | 22 whitelisted files, CRC/SHA256 readback passed | No game executable, owner catalog, cache, database or runtime logs |
| Player overlay | 7 whitelisted files, CRC/SHA256 readback passed | Client addon, catalog, scoped symbol fragment, one-time setup, checksums and instructions |

The owner dataset had 1071 player-unit records, of which 819 were sortie-ready
with positive HP. The other 252 remain excluded. Default LV counts were
177 / 239 / 237 / 107 / 59. Both sides received the same 616731-byte catalog.

## State consistency

The core/DLL scenarios verify that disabled or rejected addons leave original
function bytes and records intact. A competing hook owner remains active when
the catalog's partially installed hooks roll back. Unknown lookups preserve
native outputs and flags. Colliding NPC IDs keep their original records.
The client now preflights the whole roster before insertion; a foreign ID of any
grid type or a changed/missing source prevents publication. Missing or different
catalog records are refused by all three creator guards. A competing remote
creator hook causes rollback of the earlier loader/base/mission hooks. Repeated
table loading does not duplicate map or integrity entries. Disabled fresh
processes add no runtime records.

The server builder uses private unit data only for the catalog participant.
Native exceptions restore its thread-local context. A nested non-catalog build
temporarily uses original data, then restores the outer catalog context. The
test compares all original records byte-for-byte afterwards.

The installer tests snapshot every original file, then perform install, repeated
install, disable, repeated disable, enable, update and uninstall. Original file
bytes and unrelated settings are restored. An update preserves the disabled
state. Changed bridge DLLs block uninstall restoration, while disabling remains
available. Newer bridge versions cannot be downgraded. Injected copy failure
rolls back; interrupted-operation recovery refuses to overwrite a foreign edit.

Allocation-ledger tests retain both active and retired NPC IDs across updates.
Generation checks IDs, client records, direct transform references, declared
weapons, native template compatibility and bounded binary layouts. Semantic
record validation still runs after a valid payload checksum. Generated strings
must be terminated within their fixed NPC fields.

## Reproduce

```powershell
python -X utf8 -m unittest discover -s tests -p test_ai_catalog.py -v
cmake -S . -B build/catalog -G "Visual Studio 17 2022" -A Win32 -DBUILD_TESTING=ON -DSDGO_LUA50_SOURCE=C:/path/to/lua-5.0.3
cmake --build build/catalog --config Release --parallel 4
ctest --test-dir build/catalog -C Release --output-on-failure
cmake --build build/catalog --config Debug --parallel 4
ctest --test-dir build/catalog -C Debug --output-on-failure
```

The official Lua 5.0.3 sources are test-only and are never included in the
addon. If they are omitted, the Lua simulation is explicitly excluded by CMake.

Optional owner-data checks use artifacts the caller supplies, not repository
fixtures containing game assets:

```powershell
build/catalog/Release/ai_catalog_native_test.exe C:/path/to/BattleServer.exe C:/owner/catalog.bin C:/new/isolated-output
cmake --build build/catalog --config Release --target ai_catalog_client_map_test
build/catalog/Release/ai_catalog_client_map_test.exe C:/path/to/GOnline.exe build/catalog/catalog_client/Release/ai_catalog.dll C:/owner/client/sdgo/mods/ai_catalog C:/owner/client-unit-fixture.bin
```

The client test fixture is `SDGOUN1` plus NUL, a little-endian u32 count, then
that many original 4976-byte source unit records, in any unique-ID order. The
records can be read with the generator's `read_client` and selected with the
generated `catalog.json` entries. Keep this game data private to the owner.

## Real-test failure and 0.1.1 correction

The initial client-data test exercised only the old `grid.find` detour. It did
not cover native creators' inline tree lookup. The owner's real battle selected
three expanded NPCs on the server, but the client logged `C0000005` for each
spawn. Consequently the original test result was insufficient evidence for
client creation.

0.1.1 replaces that detour with native runtime registration after `0222.gdt`
loads. The new owner test executes the supplied GOnline map, Find, integrity and
destructor instructions in a test-owned address range; it does not run the game
entry point. It also exposed that native Find returns a bool in AL, with the
upper EAX bits unspecified on failure; the production binding now uses bool.
The obsolete resolver-only test has been removed.

Master integration preserves the current 1.1.4 public ABI, shared room command
parser, pool provider and categorized package selection. Both addon manifests
declare `category=gameplay` and `feature=ai-catalog`; the independent addon is
still excluded from Base/Full packages. The installer refuses enable operations
when a mod/feature/category override disables it. The owner client log has also
confirmed registration of all 819 records during ordinary startup; this is data
loading evidence, not rendered battle acceptance.

Deployment readback confirmed client 0.1.1 enabled, unchanged paired catalog
SHA256 `f95f01cd46eeb6dee27bbc596a3d3c1673e043f60cce98f8d509331d3e73dc27`,
and 31 unrelated files unchanged. Another tool had updated the shared symbol
file. The original catalog install had left that file byte-identical, so its
receipt baseline was advanced to the current external file before appending
only missing catalog definitions; uninstall preserves those external changes.
The local receipt and backups are under
`build/deployment/original-ai/client-fix-0.1.1/` and stay outside Git.

## Limits

The native test exercises the real builder and lookup instructions against
owned table fixtures. It does not run the server's startup, SQL download,
network, room lifecycle or battle-result processing. The native client test does
not create a rendered unit or execute its Lua AI; its creator callbacks are
test substitutes while its map/checksum/destructor code is real. Native script choice is a
heuristic with owner overrides.

Actual rendering, weapon/skill behavior, transformations, death/respawn, battle
results and human multiplayer require manual acceptance with matching catalog
and mod versions on all participants. These results do not certify every unit's
gameplay or improve the AI's navigation/combat policy.
