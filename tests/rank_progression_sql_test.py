"""Run the actual native battle procedures in a new synthetic SQL database.

Reads source definitions, table schemas and the non-player level configuration.
No source player rows are copied or changed. Unrelated procedure dependencies
are fail-fast stubs; only Haro bookkeeping is an explicit no-op. The one native
cross-database skill-table reference is rebound to the scratch database.
"""
from __future__ import annotations

import argparse
import datetime
import json
from pathlib import Path
import re
import sys
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import rank_progression_fix as fix


def ident(name):
    return "[" + name.replace("]", "]]") + "]"


def params(connection, name):
    return [tuple(r) for r in connection.cursor().execute(
        "SELECT p.name,t.name,p.max_length,p.precision,p.scale,p.is_output "
        "FROM sys.parameters p JOIN sys.types t ON p.user_type_id=t.user_type_id "
        "WHERE p.object_id=OBJECT_ID(?) AND p.parameter_id>0 ORDER BY p.parameter_id", "dbo."+name)]


def data_type(row):
    _, kind, length, precision, scale, _ = row
    if kind in ("varchar", "char", "varbinary", "binary"):
        return kind + "(" + ("max" if length == -1 else str(length)) + ")"
    if kind in ("nvarchar", "nchar"):
        return kind + "(" + ("max" if length == -1 else str(length//2)) + ")"
    if kind in ("numeric", "decimal"):
        return f"{kind}({precision},{scale})"
    if kind not in ("int", "bigint", "smallint", "tinyint", "real", "float", "bit", "datetime", "smalldatetime"):
        raise ValueError("Unsupported fixture parameter type: " + kind)
    return kind


def insert_synthetic(connection, table, overrides):
    columns = connection.cursor().execute(
        "SELECT c.name,t.name,c.is_computed,c.is_identity FROM sys.columns c JOIN sys.types t "
        "ON c.user_type_id=t.user_type_id WHERE c.object_id=OBJECT_ID(?) ORDER BY c.column_id",
        "dbo."+table).fetchall()
    names, values = [], []
    identity = any(row[3] for row in columns)
    for name, kind, computed, _ in columns:
        if computed or kind in ("timestamp", "rowversion"):
            continue
        if name in overrides:
            value = overrides[name]
        elif kind in ("char", "varchar", "nvarchar", "nchar", "text", "ntext"):
            value = "0"
        elif kind in ("datetime", "smalldatetime", "datetime2", "date"):
            value = datetime.datetime(2020, 1, 1)
        elif kind in ("binary", "varbinary", "image"):
            value = b"\0"
        elif kind == "uniqueidentifier":
            value = str(uuid.UUID(int=0))
        else:
            value = 0
        names.append(name)
        values.append(value)
    sql = "INSERT dbo." + ident(table) + "(" + ",".join(map(ident, names)) + ") VALUES (" + ",".join("?" for _ in names) + ")"
    if identity:
        connection.cursor().execute("SET IDENTITY_INSERT dbo."+ident(table)+" ON")
    try:
        connection.cursor().execute(sql, *values)
    finally:
        if identity:
            connection.cursor().execute("SET IDENTITY_INSERT dbo."+ident(table)+" OFF")


def execute_battle(connection, name, signature, account, delta):
    args, values, declares, outputs = [], [], [], []
    for index, row in enumerate(signature):
        param = row[0]
        if not re.fullmatch(r"@\w+", param):
            raise ValueError("Unsupported native parameter name")
        if row[-1]:
            local = f"@fixture_out_{index}"
            declares.append("DECLARE " + local + " " + data_type(row) + ";")
            args.append(param + "=" + local + " OUTPUT")
            outputs.append((param, local))
        else:
            value = {"@v_AccountNum": account, "@v_ExpVal": delta, "@v_UnitLevel": 1,
                     "@v_UserBattleShipNum": 1, "@v_BattleCnt": 1, "@v_WinCnt": 1}.get(param, 0)
            args.append(param+"=?")
            values.append(value)
    select = "SELECT @fixture_rc AS fixture_return" + "".join(
        "," + value + " AS " + ident(name) for name, value in outputs)
    sql = "SET NOCOUNT ON;" + "".join(declares) + "DECLARE @fixture_rc int; EXEC @fixture_rc=dbo." + ident(name) + " " + ",".join(args) + ";" + select
    cur = connection.cursor().execute(sql, *values)
    result = None
    while True:
        if cur.description:
            keys = [x[0] for x in cur.description]
            rows = cur.fetchall()
            if "fixture_return" in keys:
                result = dict(zip(keys, rows[0]))
        if not cur.nextset():
            break
    if result is None or result["fixture_return"] != 0:
        raise AssertionError("Native battle procedure did not succeed: " + str(result))
    if any(v not in (0, None) for k,v in result.items() if "retval" in k.lower()):
        raise AssertionError("Native battle output reported an error: " + str(result))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ini", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--keep-database", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    source = fix.connect_ini(args.server_ini)
    original_modules = fix.read_modules(source)
    source_database = source.cursor().execute("SELECT DB_NAME()").fetchone()[0]
    signatures, dep_signatures, tables = {}, {}, {"tbl_UserBattleShips"}
    for name in fix.TARGETS:
        if fix.plan(name, original_modules[name]["definition"]).state != "affected":
            raise ValueError("Regression fixture requires the affected native procedure versions")
        signatures[name] = params(source, name)
        deps = source.cursor().execute(
            "SELECT referenced_entity_name,referenced_database_name,referenced_server_name "
            "FROM sys.sql_expression_dependencies WHERE referencing_id=OBJECT_ID(?)", "dbo."+name).fetchall()
        for dep, database, server in deps:
            if server or (database and (database != source_database or dep != "tbl_ShipSkillData")):
                raise ValueError("Native procedure has an unsupported external dependency")
            if dep.startswith("tbl_"):
                tables.add(dep)
            elif dep not in dep_signatures:
                dep_signatures[dep] = params(source, dep)
    levels = [dict(zip([v[0] for v in cur.description], row)) for cur in [source.cursor().execute(
        "SELECT levelID,LevelStep,levelExp,levelExpMax FROM dbo.tbl_Levelname ORDER BY levelExp")]
              for row in cur.fetchall()]
    source.rollback()
    source.autocommit = True
    database = "SDGO_RankFixTest_" + uuid.uuid4().hex[:12]
    owner = uuid.uuid4().hex
    source.cursor().execute("CREATE DATABASE " + ident(database))
    tested = None
    passed = False
    receipt = {"database": database, "native_procedures": list(fix.TARGETS),
               "source_player_rows_copied": 0, "unrelated_haro_bookkeeping": "fixture no-op"}
    try:
        source.cursor().execute("USE " + ident(database))
        source.cursor().execute("EXEC sys.sp_addextendedproperty @name=N'sdgo_rank_test_owner', @value=?", owner)
        for table in sorted(tables):
            # Copy schemas, never source user rows. If SQL Server retains an
            # identity property, insert_synthetic handles IDENTITY_INSERT.
            source.cursor().execute("SELECT TOP (0) src.* INTO dbo." + ident(table) + " FROM "
                + ident(source_database) + ".dbo." + ident(table)
                + " src CROSS JOIN (SELECT 1 AS fixture) identity_blocker")
        source.cursor().execute("INSERT dbo.tbl_Levelname SELECT * FROM " + ident(source_database) + ".dbo.tbl_Levelname")
        for name, signature in dep_signatures.items():
            fields = [r[0]+" "+data_type(r)+(" OUTPUT" if r[-1] else "") for r in signature]
            if name == "sp_UseHaro":
                body = "".join("SET "+r[0]+"=0;" for r in signature if r[-1]) + " RETURN 0;"
            else:
                body = "RAISERROR('Unexpected non-rank fixture dependency',16,1); RETURN 999;"
            source.cursor().execute("CREATE PROCEDURE dbo."+ident(name)+" "+",".join(fields)+" AS "+body)
        for name, module in original_modules.items():
            text = module["definition"]
            text = re.sub(r"(?:\[GOnlineGame\]|GOnlineGame)\s*\.\s*(?:\[dbo\]|dbo)\s*\.\s*(?:\[tbl_ShipSkillData\]|tbl_ShipSkillData)",
                          "dbo.tbl_ShipSkillData", text, flags=re.I)
            # The fixture is a new database; retain native CREATE definitions.
            text = re.sub(r"\bALTER(?=\s+(?:PROC|PROCEDURE)\b)", "CREATE", text, count=1, flags=re.I)
            source.cursor().execute("SET ANSI_NULLS "+("ON" if module["ansi_nulls"] else "OFF"))
            source.cursor().execute("SET QUOTED_IDENTIFIER "+("ON" if module["quoted_identifier"] else "OFF"))
            source.cursor().execute(text)
        insert_synthetic(source, "tbl_Users", {"AccountNumber": 900000001, "PilotName": "rank-fixture", "UserLevelNumber":15,
            "UserLevelStep":1, "UserLvlNm":"rank15-1", "UserRating":1000, "UserExp":30090})
        insert_synthetic(source, "tbl_UserBattleShips", {"AccountNumber":900000001,"UserBattleShipNumber":1})
        # Fixture creation/inserts used autocommit. Test operations use their own
        # connection and transactions so the source session never changes users.
        tested = fix.connect_ini(args.server_ini)
        tested.cursor().execute("USE "+ident(database))
        tested.commit()
        native_before = fix.read_modules(tested)
        reproductions = []
        for name in fix.TARGETS:
            tested.cursor().execute("UPDATE dbo.tbl_Users SET UserLevelNumber=15,UserLevelStep=1,UserExp=30090,UserLvlNm='rank15-1' WHERE AccountNumber=900000001")
            execute_battle(tested,name,signatures[name],900000001,20)
            row = tested.cursor().execute("SELECT UserLevelNumber,UserLevelStep,UserExp FROM dbo.tbl_Users WHERE AccountNumber=900000001").fetchone()
            assert tuple(row)==(5,2,30110), (name,tuple(row))
            reproductions.append({"procedure":name,"before":[15,1],"old_result":list(row)})
            tested.rollback()
        receipt["native_bug_reproductions"] = reproductions
        applied = fix.apply(tested, args.output/"backup.json")
        assert applied["changed"]==2
        assert fix.apply(tested,args.output/"not-created.json")["changed"]==0
        assert not (args.output/"not-created.json").exists()
        installed = fix.read_modules(tested)
        tested.rollback()
        checks = []
        # Every configured EXP boundary, plus stored-rank ahead-of-XP cases,
        # same-major sublevels, major changes resetting sublevel, and rank caps.
        cases = [(15,1,int(l["levelExp"])) for l in levels]
        cases += [(5,1,30100),(5,5,48600),(7,5,103400),(8,5,180000),
                  (15,1,1000),(15,4,30100),(15,5,3500000),(16,1,8000000),(20,1,30000)]
        for name in fix.TARGETS:
            for major,step,exp in cases:
                l = next(l for l in levels if l["levelExp"]<=exp<l["levelExpMax"])
                candidate=(int(l["levelID"]),int(l["LevelStep"]))
                expected=(major,step)
                if major<16 and (major,step)!=(15,5) and candidate>expected:
                    expected=candidate
                tested.cursor().execute("UPDATE dbo.tbl_Users SET UserLevelNumber=?,UserLevelStep=?,UserExp=?,UserLvlNm='fixture',UserBattle=0,UserWin=0 WHERE AccountNumber=900000001",major,step,max(exp-1,0))
                execute_battle(tested,name,signatures[name],900000001,exp-max(exp-1,0))
                row=tested.cursor().execute("SELECT UserLevelNumber,UserLevelStep,UserExp,UserBattle,UserWin FROM dbo.tbl_Users WHERE AccountNumber=900000001").fetchone()
                assert tuple(row)==(*expected,exp,1,1),(name,major,step,exp,tuple(row),expected)
                checks.append({"procedure":name,"stored":[major,step],"exp":exp,"result":list(row)})
                tested.rollback()
        receipt["native_boundary_cases_passed"] = len(checks)
        (args.output/"boundary-cases.json").write_text(json.dumps(checks,indent=2),encoding="utf-8")
        # A subsequent manual edit is never overwritten by rollback.
        edited=installed[fix.TARGETS[0]]["definition"]+"\n-- unrelated later change\n"
        fix.alter(tested,fix.TARGETS[0],edited,installed[fix.TARGETS[0]])
        tested.commit()
        try:
            fix.rollback(tested,args.output/"backup.json")
        except ValueError as exc:
            assert "subsequently edited" in str(exc)
        else:
            raise AssertionError("Rollback overwrote a later edit")
        fix.alter(tested,fix.TARGETS[0],installed[fix.TARGETS[0]]["definition"],installed[fix.TARGETS[0]])
        tested.commit()
        assert fix.rollback(tested,args.output/"backup.json")["restored"]==2
        assert fix.rollback(tested,args.output/"backup.json")["restored"]==0
        current=fix.read_modules(tested)
        assert all(fix.same_module(current[n],fix.module_hash(native_before[n]["definition"],n),native_before[n],n) for n in fix.TARGETS)
        tested.rollback()
        # Reject the second ALTER to verify all-or-nothing installation.
        source.cursor().execute("CREATE TRIGGER fixture_reject_grid ON DATABASE FOR ALTER_PROCEDURE AS "
            "IF EVENTDATA().value('(/EVENT_INSTANCE/ObjectName)[1]','nvarchar(128)')=N'sp_SetGridBattleExp_V2' "
            "BEGIN ROLLBACK; RAISERROR('Fixture rejects second ALTER',16,1); END")
        try:
            fix.apply(tested,args.output/"rejected-install-backup.json")
        except Exception:
            pass
        else:
            raise AssertionError("Fixture DDL trigger did not reject the second ALTER")
        current=fix.read_modules(tested)
        assert all(fix.same_module(current[n],fix.module_hash(native_before[n]["definition"],n),native_before[n],n) for n in fix.TARGETS)
        tested.rollback()
        source.cursor().execute("DROP TRIGGER fixture_reject_grid ON DATABASE")
        receipt.update({"idempotent_apply":True,"idempotent_rollback":True,"rollback_preserves_later_edit":True,
                        "atomic_failure_rollback":True})
        passed=True
    finally:
        if tested is not None:
            tested.close()
        if passed and not args.keep_database:
            assert re.fullmatch(r"SDGO_RankFixTest_[0-9a-f]{12}",database)
            actual=source.cursor().execute("SELECT value FROM sys.extended_properties WHERE class=0 AND name=N'sdgo_rank_test_owner'").fetchone()
            assert actual and actual[0]==owner,"Scratch database ownership differs"
            source.cursor().execute("USE "+ident(source_database))
            source.cursor().execute("DROP DATABASE "+ident(database))
            receipt["scratch_database_removed"]=True
        source.close()
        verify=fix.connect_ini(args.server_ini)
        try:
            receipt["source_procedures_unchanged"] = fix.read_modules(verify)==original_modules
            assert receipt["source_procedures_unchanged"]
        finally:
            verify.close()
        receipt["passed"]=passed
        (args.output/"result.json").write_text(json.dumps(receipt,indent=2),encoding="utf-8")
    if not passed:
        raise SystemExit("SQL regression failed")
    print(json.dumps(receipt))


if __name__ == "__main__":
    main()
