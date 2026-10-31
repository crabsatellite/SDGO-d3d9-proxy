# Battle rank regression repair

[简体中文](rank-progression-fix.zh-CN.md)

The native `sp_SetBattleExp_V4` and `sp_SetGridBattleExp_V2` procedures can replace
a pilot's stored rank with a lower major rank when the EXP-derived rank has a
higher sublevel. This is especially visible on servers that grant a higher
starting rank without matching EXP.

For example, a pilot stored at **15-1** with 30,090 EXP receives 20 EXP. The
configured EXP interval selects **5-2**. Both native procedures use:

```sql
IF(@UserLevelNumber < @newlevel OR @UserLevelStep < @NewLevelStep)
```

The second comparison is true, so the pilot is saved as 5-2. The repair compares
the rank pair in order:

```sql
IF(@UserLevelNumber < @newlevel OR
   (@UserLevelNumber = @newlevel AND @UserLevelStep < @NewLevelStep))
```

This preserves same-major sublevel promotions and promotions to a higher major
rank with a lower sublevel. The native 15-5/ranked-tier restrictions, EXP awards,
rewards, procedure signatures and other battle logic are retained. Login-time
leaderboard rank recalculation is a separate system and is not changed.

## Server installation

This is a database procedure repair; players do not need a client DLL or new
client configuration. The installer reads the server owner's existing
`[DB Game]` INI settings. It has no machine-specific defaults and never exports
connection credentials. It needs Python, `pyodbc`, an installed SQL Server ODBC
driver, and a database identity permitted to alter the two procedures.

Audit is the default and performs no procedure or player-data writes:

```powershell
python tools/rank_progression_fix.py audit --server-ini "D:\SDGO\StoreServer\StoreServer.ini"
```

Apply during a server maintenance window, since an `ALTER PROCEDURE` needs SQL
schema locks. The tool does not restart processes or alter player rows:

```powershell
python tools/rank_progression_fix.py apply --server-ini "D:\SDGO\StoreServer\StoreServer.ini"
```

Before the first SQL change, the installer writes an exclusive UTF-8 backup under
`sdgo/backups/rank_progression_fix` beside the supplied INI. Use `--backup` to
choose another recovery file. Keep this host-only recovery copy private: it
contains the original native SQL definitions. No native procedure dump is
included in this repository.

Both procedures are checked before either is changed. Comments and SQL literals
are excluded from comparison matching. Unsupported/ambiguous definitions are
refused. The repair uses one owned transaction and a database application lock,
preserves `ANSI_NULLS`/`QUOTED_IDENTIFIER`, alters the existing objects in place,
and checks exact definition fingerprints before commit. Reapplying an installed
repair makes no changes and creates no extra backup.

Restore a repair using its exact recovery file:

```powershell
python tools/rank_progression_fix.py rollback --server-ini "D:\SDGO\StoreServer\StoreServer.ini" --backup "D:\rank-fix-backup.json"
```

Rollback checks the original and installed fingerprints. It refuses to overwrite
a procedure edited after installation. Repeated rollback is harmless. A driver
or connection failure can leave commit status uncertain; run `audit` and inspect
the retained recovery copy before retrying.

The installer prevents this future rank regression. It does not infer or rewrite
previously lost ranks, grant EXP, or change starting-rank policy.

## Verification

Pure offline tests:

```powershell
python -B -m unittest discover -s tests -p test_rank_progression_fix.py -v
```

The SQL integration test reads the host's native procedure definitions, table
schemas and non-player level configuration, then creates a nonce-named scratch
database. It does not copy source player rows. It runs the full native battle
procedures against synthetic pilots/decks. Unrelated dependencies fail if called;
only Haro bookkeeping is a fixture no-op. The native skill-table cross-database
reference is rebound to the scratch table.

```powershell
python -B tests/rank_progression_sql_test.py --server-ini "D:\SDGO\StoreServer\StoreServer.ini" --output build/rank-progression-test/run
```

The tested native SQL configuration reproduced 15-1 → 5-2 in both procedures
before repair. After repair, **148 cases** passed: all 65 EXP interval starts for
each procedure plus stored-rank-ahead-of-EXP, normal sublevel/major promotions,
and the native ranked-tier caps. Tests check persisted rank, EXP and battle/win
counters. They also verify repeat apply/rollback, refusal to overwrite later
edits, and complete rollback when a fixture DDL trigger rejects the second ALTER.
The source procedure definitions are checked unchanged at the end.

Successful tests remove only their ownership-marked scratch database. Failed
fixtures are retained for diagnosis; `result.json` records their names. Use
`--keep-database` to retain a successful fixture too. No game login or human
gameplay acceptance is claimed by these SQL tests.
