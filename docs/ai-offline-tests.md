# Offline AI review and tests

The `pvp-ai` branch has executable tests for the logic added by `pvp_ai`,
`match_ai`, `coop_ai`, and `mission_ai`. They do not start a game, connect to a
server, use an account, or install the staged DLLs into a game directory.

## Run

Requirements: Visual Studio 2022 C++ tools and CMake, configured for Win32.
The Lua simulation additionally uses the extracted official
[Lua 5.0.3 source archive](https://www.lua.org/ftp/lua-5.0.3.tar.gz). The test VM
opens only the base and string libraries used by the existing mission script.
It is never linked into a shipped mod or staged in a package.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_ai_tests.ps1 -LuaSource C:/path/to/lua-5.0.3
powershell -NoProfile -ExecutionPolicy Bypass -File tests/run_ai_tests.ps1 -Config Debug -LuaSource C:/path/to/lua-5.0.3
```

Alternatively, put the Lua source at `build/ai_review/deps/lua-5.0.3` and omit
`-LuaSource`. `-NativeOnly` explicitly skips the Lua simulation. A normal CMake
build can also use `-DBUILD_TESTING=OFF` to omit the new test targets.

The script builds into `build/ai_review` by default. `-BuildDir` selects another
build tree. It leaves the existing `build/Release` and installed clients alone.
After building, individual scenarios can be selected with CTest, for example:

```powershell
ctest --test-dir build/ai_review -C Release --output-on-failure -R match_ai_offline
```

## Coverage

| Test | What it checks |
| --- | --- |
| `pvp_ai_offline` | Room commands and bounded numeric input, team/host/colour selection, explicit zero-bot start checks, native setup failure, player life costs, unique deck units and budget, team capacity, grid/death/tag scores, respawn gating and end-of-battle cleanup |
| `match_ai_offline` | Master/remote spawning, slot limits and identity reuse, NPC-to-player lookup, initial HUD scores and gauge callbacks, packet deck replacement and invalid pointers, declared deck length, missing NPC/failed append, respawn ordering, life costs, temporary UDP slot masking, solo NAT scope, spectator linked-list integrity, camera geometry and exception restoration |
| `coop_ai_offline` | Room commands, invalid configuration, NPC list traversal, squad capacity, explicit counts and mode isolation |
| `mission_ai_offline` | INI-to-Lua escaping, local/remote mission spawning, observer behaviour, slot bounds and derived-mode isolation |
| `mission_ai_lua_offline` | Nearest/live/in-range wingmen, preserving human targets, enable switch, hot reload, missing units, stale missions and clock wrap, status-log throttling |
| `loader_offline` | Real loader forwarding and self-chain fallback in a synthetic executable |
| `findwindow_fix_offline` | Real core/export-hook rollback in fixtures on a private desktop |
| `installer_offline` | Synthetic client/server installs, update configuration preservation and explicit overwrite |
| `mod_staging_offline` | Actual incremental build repairs stale staged Lua/manifest files; the test restores the prior staged bytes afterwards |

Each native AI test includes the production `.cpp` directly in its own test
executable. Game function pointers are replaced by fixture callbacks and the
room, packet, unit, HUD and linked-list storage is owned by the test. There is
no duplicate implementation of the algorithms under test. Checks remain active
in Release builds. The Debug build additionally uses MSVC runtime checks.

## Review fixes

The regression cases were first run against the original implementation and
failed before the corresponding fixes:

- Room counts saturated only after signed overflow; NPC identifiers could wrap
  to a different valid identifier. Counts now saturate at the supported slot
  limit and overflowing identifiers are skipped.
- `!ai0` bypassed the native player/team checks despite requesting no AI.
- An invalid co-op `DefaultLevel` could index outside the level pools. Invalid
  counts and squad sizes now fall back to the documented defaults.
- Client deck building ignored the packet's declared unit count. Missing NPCs
  were skipped, compacting later entries into different indices from the server;
  a failed native append still advanced the metadata. Only a successfully built
  consecutive prefix is retained.
- Server and HUD grid totals disagreed for asymmetric teams. For example, a 1v3
  room used server totals 80/60 but the client HUD's single total was 60. Both
  sides now use the same shared rule from `sdgo/ai_scoring.h`: the native total
  for the larger team, applied to both sides. Balanced rooms keep their values.
- Tag-match score initialization could retain a zero divisor when no usable
  deck/NPC points existed, or overflow when multiplying malformed points. The
  divisor is positive and the displayed score is capped before multiplication.
- Partial or impossible record/deck ranges were silently truncated, and mission
  spawning could exceed the unit manager's 12 player slots.
- An exception in native UDP preparation left AI pilot IDs temporarily removed.
  Restoration now runs in `__finally`, also checked with an injected exception.
- Camera angle normalization could propagate NaNs or loop indefinitely on large
  floats. It now uses bounded arithmetic and handles non-finite inputs.
- Lua mission liveness/report deadlines did not handle `GetTickCount` wrap.
  Both now compare elapsed unsigned-clock time.

## Evidence and limits

Review run: 2026-11-01, base `5838080`, original checkout on `pvp-ai`.
Builds and receipts are under `build/ai_review`. Baseline logs retain the failing
regressions; `Release-tests.log` and `Debug-tests.log` hold the final CTest runs.

The symbol verifier can separately check on-disk host signatures, without
executing either host:

```powershell
python tools/verify_symbols.py C:/path/to/GOnline.exe
python tools/verify_symbols.py C:/path/to/BattleServer.exe
```

Offline tests verify control flow and data contracts under the documented native
layouts. They do not prove those layouts against every game build, execute the
inline assembly detours inside the game, render the actual HUD, test real
multiplayer synchronization, or establish improved navigation/combat AI.
Startup, real battles, visual acceptance, host migration, teardown and remote
clients remain manual game checks. No gameplay acceptance is claimed by a green
CTest result.
