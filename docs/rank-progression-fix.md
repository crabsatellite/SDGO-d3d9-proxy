# Battle rank progression Mod

[简体中文](rank-progression-fix.zh-CN.md)

`rank_progression_fix` is a default-enabled core fix for
`WorldServer-548A86E3-0010C000`. Install it through the normal Mod framework.
Players need no client update, Python, ODBC driver or extra connection configuration.

## Install

Stop the target WorldServer, then use the Base or Full package:

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO\Server\WorldServer" -Role World
```

Start WorldServer normally. The Mod payload is
`sdgo/mods/rank_progression_fix/{mod.ini,rank_progression_fix.dll}`. It shares the
existing `winmm.dll` loader, `sdgo/core.dll`, symbols and global configuration.
There is no separate maintenance tool to run.

The Mod reads only the database name from WorldServer's existing `[DB Game]`
configuration and borrows its leased OLE DB session. It never reads or exports
the connection password and opens no separate game-database connection.
The existing database identity needs permission to read the two native procedure
definitions and create the Mod-owned schema, state table and procedures.
Each participating WorldServer must load the Mod; sharing a database alone does
not redirect calls made by an unmodified process.

## Behavior and scope

The native `dbo.sp_SetBattleExp_V4` and `dbo.sp_SetGridBattleExp_V2` can lower a
pilot's major rank when the EXP-derived rank has a higher sublevel. For example,
rank **15-1** with 30,090 EXP plus 20 EXP becomes **5-2**. The Mod derives private
copies from the installed definitions, replacing only the procedure identity and
the faulty rank comparison with:

```sql
IF(@UserLevelNumber < @newlevel OR
   (@UserLevelNumber = @newlevel AND @UserLevelStep < @NewLevelStep))
```

On a matching battle settlement, it prepares both copies in the
`sdgo_rank_progression_fix` schema under one transaction and application lock.
Preparation checks ownership, supported comparison, hashes and SQL options.
Unsupported definitions, foreign schemas and external edits are refused.
The native `dbo` definitions remain unchanged. No native SQL dump is bundled.

Only the two exact native command strings are redirected. Native parameter
binding, output processing, EXP awards, rewards and counters stay in place.
The descriptor's command pointer is restored after the call. Normal promotions,
the native 15-5/ranked-tier limits and login-time leaderboard recalculation are
preserved. Past lost ranks are not reconstructed.

`sdgo/logs/sdgo.log` distinguishes installed hooks from an active policy.
`rank policy active` appears when the corresponding command is redirected.
If preparation fails, an error is logged and the native command is retained;
on an affected native database that call still has the original rank bug.

## Disable or remove

Set this in `sdgo/sdgo.ini`, then restart the target WorldServer:

```ini
[mods]
rank_progression_fix=0
```

The standard `[features] rank-progression-fix=0` or `[categories] fixes=0` switch
also disables it. Inactive private SQL objects may stay in the database; original
calls do not use them. Disabling does not undo SQL changes made by an older
standalone repair or another administrator.

## Developer verification

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The C++ fixture covers SQL transformation, framework entry, both hooks, exact
matching, fallback and descriptor/output preservation. Packaging tests check
Base/Full catalogs and the packaged World installer.

The optional integration fixture needs the existing server INI and a private
recovery backup containing the original two native definitions:

```powershell
powershell -ExecutionPolicy Bypass -File tests\rank_progression_db_test.ps1 -ServerIni "D:\SDGO\Server\WorldServer\WorldServer.ini" -OriginalBackup "D:\Maintenance\original-rank-procedures.json" -BuildDir build
```

It creates an ownership-marked scratch database, copies schemas and level
configuration, and uses synthetic pilots/decks without copying source player rows.
It executes full native procedures through real SQLOLEDB and production policy code.
With the tested 65-interval configuration, 152 native calls cover the regression,
enabled/disabled SQL paths, interval starts, normal promotions and rank caps.
Tests also check idempotence, ownership/edit refusal, atomic rollback if the
second procedure creation fails, and unchanged source definitions. Only Haro
bookkeeping is a no-op; unexpected dependencies fail.
Four additional bound RPC calls exercise the exact OLE DB escape syntax with
native parameters and return codes, including policy preparation on a session
whose command already owns parameter bindings. Existing transactions are left alone.

Temporary credential files are deleted on success or failure. Successful fixtures
drop only their ownership-marked database; failed database names are recorded in
local `result.json`. These checks do not constitute live game/login acceptance.
