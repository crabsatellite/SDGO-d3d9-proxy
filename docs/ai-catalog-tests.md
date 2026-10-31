# AI catalog validation

Validation date: 2026-11-01. Feature branch: `feature/ai-unit-catalog`, based on
`pvp-ai`. Builds and owned runtime fixtures are under `build/catalog`; owner data
and generated catalogs remain outside Git. No running game/client/server was
updated or restarted.

## Results

| Check | Result | Evidence scope |
| --- | --- | --- |
| Python catalog/install/export/package tests | 31 passed | Actual generator/installer code, synthetic catalogs and arbitrary paths |
| Release CTest suite | 10 passed | Existing co-op/PvP/client/Lua tests, loader, hooks, installers, staging and new catalog integration |
| Debug CTest suite | All 10 passed across the suite and one targeted rerun | Initial `pvp_ai_offline` invocation timed out; direct retry passed 90 checks and CTest retry passed in 0.04 s. The timeout was not reproduced. |
| Real core + catalog DLL | 20 scenarios per configuration, Release and Debug | Off/on/off/on, collisions, changed source, deferred source, corrupt/missing file, competing hook owner and partial-load rollback |
| Actual owner client records | 819 units, 3281 checks, Release and Debug | Production client resolver and owner-extracted GDT data, without starting the game |
| Actual BattleServer builder code | 819 units, 4117 checks, Release and Debug | Caller-supplied image mapped without its entry point, with owned maps/records/buffers |
| Host symbols | 27 BattleServer symbols verified, 0 rejected | On-disk supported executable; includes the new NPC/unit lookup symbols |
| Standalone package | 21 whitelisted files, CRC/SHA256 readback passed | No game executable, owner catalog, cache, database or runtime logs |
| Player overlay | 5 whitelisted files, CRC/SHA256 readback passed | Only client addon, matching catalog, checksums and instructions |

The owner dataset had 1071 player-unit records, of which 819 were sortie-ready
with positive HP. The other 252 remain excluded. Default LV counts were
177 / 239 / 237 / 107 / 59. Both sides received the same 616731-byte catalog.

## State consistency

The core/DLL scenarios verify that disabled or rejected addons leave original
function bytes and records intact. A competing hook owner remains active when
the catalog's partially installed hooks roll back. Unknown lookups preserve
native outputs and flags. Colliding NPC IDs keep their original records.

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
build/catalog/Release/ai_catalog_client_data_test.exe C:/owner/client/sdgo/mods/ai_catalog C:/owner/client-unit-fixture.bin
```

The client test fixture is `SDGOUN1` plus NUL, a little-endian u32 count, then
that many original 4976-byte source unit records, in any unique-ID order. The
records can be read with the generator's `read_client` and selected with the
generated `catalog.json` entries. Keep this game data private to the owner.

## Limits

The native test exercises the real builder and lookup instructions against
owned table fixtures. It does not run the server's startup, SQL download,
network, room lifecycle or battle-result processing. The client-data test does
not create a rendered unit or execute its Lua AI. Native script choice is a
heuristic with owner overrides.

Actual rendering, weapon/skill behavior, transformations, death/respawn, battle
results and human multiplayer require manual acceptance with matching catalog
and mod versions on all participants. These results do not certify every unit's
gameplay or improve the AI's navigation/combat policy.
