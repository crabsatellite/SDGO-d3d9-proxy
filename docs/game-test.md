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

The example tests normal, death match and leader match in order.
Each mode covers room creation, real battle startup, AI state assertions and
normal completion and return. Select a single mode when diagnosing a failure.
These three modes have completed real-client lifecycle tests. Grid (`6`) and
Tag (`7`) are accepted by the runner but still need native VS channel selection
before their positive paths can be verified. A server rejection fails the case.
Results are under `receipts/report.json` and `receipts/junit.xml` in the run directory.

Both components register with Mod Manager as development Mods. The Python
runner is a developer test command for a bounded job; it is not a player launcher
or a persistent service. Keep account profiles and run receipts out of distributed
packages and source control.
