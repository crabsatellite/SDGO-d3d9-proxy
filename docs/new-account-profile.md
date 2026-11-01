# New-account starter profile

`new_account_profile` is an optional server fix, enabled by default in the fixes
package. It applies a modest starting balance and a consistent starting rank to
new accounts and first game-profile creation. Players need no client Mod.

Defaults are **5,000 GP, 0 cash, Corporal (rank 5, step 1)**. Starting experience is
read from the server's `tbl_Levelname` row rather than hard-coded; in the inspected
pack rank 5/1 starts at 25,700 experience and meets the Expert channel minimum 5.
Ordinary rank/channel restrictions and later earnings remain in charge.

## Install and configure

Stop the World server you intend to update, then use the latest framework package:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File install_server.ps1 -ServerRoot D:\SDGO\WorldServer -Role World
```

Install on each independent World instance that handles new-player creation.
The World server owns this initialization; it is not necessary to install this
Mod in every Battle channel, Agent, Store server or player client.

Owner settings live at `sdgo/mods/new_account_profile/config.ini`. Copy the bundled
`config.example.ini` there if you want a separate settings file:

```ini
[settings]
InitialGP=5000
InitialCash=0
RankNumber=5
RankStep=1
```

Without `config.ini`, the legacy `[settings]` in `mod.ini` is used. Restart the
World server after edits. `RankNumber` is 0..20, `RankStep` is 1..5, and the selected
rank/step must have exactly one non-negative experience row in `tbl_Levelname`.
Currency settings must be non-negative 32-bit integers.

The Mod reads only `[DB StoreBilling] DBName` from the host's own INI. It borrows
the already authenticated OLE DB session and its local transactions; it does not
read, copy or export database passwords. The normal pack's Common, Game and
Billing catalogs must be accessible on that native SQL instance. Its database
identity needs reads/writes on the affected account, profile, wallet and rank
reference tables, as well as transaction support.

## Scope and recovery

The inspected registration procedure gives both cash and GP the value 9,999,999.
The initial profile procedure inserts rank 15/1 while returning a different rank.
The Mod does not change those stored-procedure definitions. Instead it wraps the
verified World registration/profile calls in a transaction, applies the configured
values, and publishes a consistent profile result only after commit. Native starter
items, skills, operators and unrelated fields are retained.

- A newly auto-registered account gets the configured wallet before its native
  registration transaction is committed. The original first-login retry result is retained.
- A profile that does not yet exist gets the configured rank, matching starting
  experience and wallet before first play. This also covers accounts registered
  elsewhere or registered earlier but never given a game profile.
- Existing game profiles are not initialized again. Their currency, XP, rank and
  later rewards are not reset. A concurrent initialization is rechecked under locks.
- Failure rolls back incomplete account/profile creation and its grants. No partial
  high-rank/large-wallet profile is published as a success.
- If the native database session is unavailable, the intercepted call fails with a
  database error rather than running creation with the original generous defaults.
  Confirm the activation line in `sdgo/logs/sdgo.log` after installation; unsupported
  builds, a disabled category/feature, or rejected settings leave the Mod inactive.

Disable only this Mod in `sdgo/sdgo.ini`:

```ini
[mods]
new_account_profile=0
```

Or stop the World server and move `sdgo/mods/new_account_profile/` out of `mods/`.
After restart, future registrations/creations use the original server rules again.
Existing accounts keep their current data; uninstall does not inject the old huge
balances into them or roll back earned progress. Keep the common loader, core and
other Mods. `install_server.ps1 -Uninstall` removes the entire framework.

Supported native World build: `WorldServer-548A86E3-0010C000`. Other builds are skipped
until their hooks/layouts are verified. No original game executable is patched on disk.

## Verification

- `new_account_policy_test`: new/existing profiles, rechecks, failure and exception
  rollback, preservation of grants/earnings and bypass behavior.
- `tests/run_new_account_sql_test.py`: actual core + actual Mod + OLE DB sessions
  with synthetic native callbacks in uniquely owned test databases in a supplied
  SQL Docker container. It verifies owner `config.ini` overrides, commit/rollback, bound usernames and disabled
  defaults, then removes only its owned databases/login. No game runs.
- Native hook ABI and layouts are checked against the exact PE and retained Ghidra
  evidence; `tools/verify_symbols.py <WorldServer.exe>` verifies all five symbols.

Real account login, client presentation and gameplay remain manual acceptance.
SQL/ABI fixtures are not evidence that a live game session has been exercised.

For manual acceptance on a test World instance:

1. Install, start World and confirm `new-account defaults active` in its log.
2. Register a fresh test account (the native auto-registration may require a login
   retry), create a pilot, and check 5,000 GP, 0 cash and Corporal 1. Try the Expert channel.
3. Earn some GP/experience, log out and back in, and confirm that progress persists.
   An existing pilot must keep its previous progress too.
4. To check restoration, disable this Mod on the test instance, restart and create
   another fresh account. It should use native defaults. Re-enable and restart to
   apply the configured defaults to future creations again.

To repeat the isolated SQL check after building Win32 Release:

```powershell
python tests/run_new_account_sql_test.py --container <isolated-sql-container> --credential-file <local-private.json> --out build/starter-sql-check
```

Use an existing disposable SQL Server Docker container and a local JSON file with
its `sa_password`. The output directory must be new. The harness creates uniquely
marked fixture databases and removes them afterwards; it does not need production
database names, modify an existing database, restart the container or run the game.
