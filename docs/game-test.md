# Native GameTest

An optional development Mod, disabled by default. Test cases run inside the
real client and use its normal business requests against a real server.
The external runner prepares a separate client, starts it and collects results.
It does not operate the desktop.

A scenario sends a request, waits for the real reply and checks the resulting
room, battle and AI state. Login, server data, room creation and battle loading
use the normal game flow. Tests must not fabricate profiles, rooms or replies.

Test accounts and registration retry policy live in private local configuration.
Missing steps, failed assertions, native errors and timeouts fail the run.
Results are written as JSON and JUnit XML. Installing this Mod alone does not
start tests or change ordinary play.

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

Both components register with Mod Manager as development Mods. The Python
runner is a developer test command for a bounded job; it is not a player launcher
or a persistent service. Keep account profiles and run receipts out of distributed
packages and source control.

For optional developer footage, add `--sfx-only` to `prepare`, build the
`game_test_audio` target, then use `python tools/game_test_record.py --run-dir
build/gametest/pvp --output E:\videos\SDGO-Mod-GameTest`. The copied client's BGM
is muted and effects remain enabled. Recording is bounded to 25 seconds per
passed battle, using only the owned game window and its process audio. Failed,
silent or black clips are deleted. Retained clips still need visual review for
combat activity and framing before editing; a passed AI assertion alone does
not guarantee a useful shot.
