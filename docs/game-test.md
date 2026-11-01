# Native GameTest

An optional development Mod, disabled by default. Test cases run inside the
real client and use its normal business requests against a real server.
The external runner prepares a separate client, starts it and collects results.
It does not operate the desktop.

A scenario sends a request, waits for the real reply and checks the resulting
room, battle and AI state. Login, server data, room creation and battle loading
use the normal game flow. Tests must not fabricate profiles, rooms or replies.

Test accounts and registration retry policy live in private local configuration.
Missing steps, failed assertions, unhandled native errors and timeouts fail the run.
Results are written as JSON and JUnit XML. Installing this Mod alone does not
start tests or change ordinary play.

Tests default to a separate background desktop with BGM, effects and voice
muted in the copied client. Desktop/window ownership is checked separately and
missing evidence fails the run. A dedicated graphics thread creates the real
D3D9 device through one invisible, nonactivating proxy; actual game pages stay
on the private desktop. Fullscreen is rejected. The native Miles WaveOut
driver's zero master gain is read back independently. The copied logic-test
profile disables native postprocessing, sets all five native effect levels to
zero and checks the effective runtime flags. These checks do not claim visual or
postprocessing acceptance. Local direct tests bind only loopback and do
not change firewall profiles or notifications. Nonlocal quiet runs require
permission to prepare their scoped application rule; setup failure never opens
a UAC prompt or falls back to an interactive client.

Runs reuse `%LOCALAPPDATA%\SDGO-GameTest\account.ini`, or the file selected with
`--account-profile`. Preparing another run never generates a new account.
Only explicit `--create-test-account` creates the first persistent profile;
existing profiles are never overwritten.

New-account setup covers nickname creation, leaving the beginner trial, the
first unit draw and required in-game confirmations before battle scenarios.

Developers can register tick-driven cases through
[`game_test.h`](../include/sdgo/game_test.h). Callbacks run on the native main
thread and can wait across ticks for actual service events.

Build first, then use an existing normal client with PvP AI installed as the
template. The target server needs `pvp_ai`. The runner starts a separate client
and does not restart existing servers.

```powershell
python tools/game_test.py prepare --client-template E:\Games\SDGO --stage build/stage/Release --run-dir build/gametest/pvp --mode-sequence 1,3,9
python tools/game_test.py run --run-dir build/gametest/pvp --timeout 3600
```

Add `--create-test-account` to `prepare` only when creating the first account
profile. Later runs reuse that profile. Use a fresh `run-dir` for each receipt;
the same account cannot run concurrently. The default endpoint is
`127.0.0.1:5001`; change it with `--target-host` and `--login-port`. The original
client selects its server and channel through the normal native flow.
The default `--network-policy direct` disables inherited routing only in the
test copy and verifies that process's actual TCP peer before login. Use
`preserve` explicitly when testing an owner's FRP routes.

The example tests normal, death match and leader match in order.
Each mode covers room creation, real battle startup, AI state assertions and
normal completion and return. Select a single mode when diagnosing a failure.
Grid (`6`) and Tag (`7`) use the native VS channel business flow and require a
complete battleship deck on the account. Missing decks and server rejections
fail the case.
Results are under `receipts/report.json` and `receipts/junit.xml` in the run directory.

Account preparation can use `--bootstrap-only`, `--battleship-only`,
`--gift-requests-only` or `--deck-only`; these do not claim PvP acceptance.
Ship acquisition uses the native tutorial. Gifts use an existing sender's normal
mail business and require the server's ordinary gift service plus independent
processing evidence. Deck setup reads four genuinely owned unit UIDs from
`[deck] unit_uid0` through `unit_uid3` in the private account profile and waits
for the original Save and GoOut replies. Log in again with that same account for
battle tests; no units or gameplay database records are supplemented.

Use `--pages-only` to test the authenticated unit-management page, inventory
readback, both exchange previews and cancellation. It compares unit UIDs, slots
and the active unit before and after; it never confirms an exchange with candidates.
Page cases do not count as battle acceptance.
The suite requires the hangar and operator/AI catalog Mods plus the server's
1200-slot capacity Mod. It reads every configured operator and NPC from the
native grid and compares the configured names, category references, NPC bytes
and source unit hashes. These catalog checks do not claim shop interaction or
mission completion.

For a passive server check, run `tools/game_test_server_report.py --run-dir
build/gametest/pvp --events <server-observer-events.jsonl> --host-pilot <pilot-id>`.
Repeat `--events` for each Battle host used by the matrix. The report requires a
matching run, room title, pilot, teams and complete four-event native lifecycle
for every requested mode. It writes `receipts/server-report.json`; a single
server scenario summary does not prove a whole client matrix.

Both components register with Mod Manager as development Mods. The Python
runner is a developer test command for a bounded job; it is not a player launcher
or a persistent service. Keep account profiles and run receipts out of distributed
packages and source control.

For explicitly visible developer footage, add `--visible --sfx-only` to `prepare`, build the
`game_test_audio` target, then use `python tools/game_test_record.py --run-dir
build/gametest/pvp --output E:\videos\SDGO-Mod-GameTest`. The copied client's BGM
is muted and effects remain enabled. Recording is bounded to 25 seconds per
passed battle, using only the owned game window and its process audio. Failed,
silent or black clips are deleted. Retained clips still need visual review for
combat activity and framing before editing; a passed AI assertion alone does
not guarantee a useful shot.
