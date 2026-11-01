"""Opt-in SQL transaction tests in one uniquely owned disposable database.

The native World payment/refund boundary is simulated here. Native C++/OLE DB
binding is tested separately; no game or real account is used.
"""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import sys
import threading
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.collection_capsule.pool import build_pool
from owner.collection_capsule.sql import installation, disable
from owner.collection_capsule.deployment import register_unit_family

spec = importlib.util.spec_from_file_location("starter_bridge", ROOT / "tests/run_new_account_sql_test.py")
bridge_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge_module)


def fixture_unit(uid, target=0, ready=1):
    return dict(UnitNumber=uid, UnitNumber_Transform=target, SallyReady=ready,
                UnitGradeNumber=1, UnitGradeNumber2=2, UnitGraph=1000000 + uid,
                UnitModelName=f"Fixture {uid}")


def run(container: str, credential_file: Path, output: Path, native_build: Path | None = None,
        config: str = "Release", native_only: bool = False):
    import pyodbc
    output.mkdir(parents=True, exist_ok=False)
    private = json.loads(credential_file.read_text(encoding="utf-8-sig"))
    tag, token = uuid.uuid4().hex[:10], uuid.uuid4().hex
    database, login = "CollectionTest_" + tag, "CollectionLogin_" + tag
    test_password = "Test_" + secrets.token_hex(14) + "!7"
    env = dict(os.environ, SQLCMDPASSWORD=private["sa_password"], COLLECTION_PASSWORD=test_password)

    def admin(sql):
        result = subprocess.run(["docker", "exec", "-i", "-e", "SQLCMDPASSWORD", "-e", "COLLECTION_PASSWORD",
            container, "/opt/mssql-tools18/bin/sqlcmd", "-S", "localhost", "-U", "sa", "-C", "-b", "-h", "-1", "-W"],
            input=sql, text=True, capture_output=True, env=env)
        if result.returncode:
            raise RuntimeError("Isolated SQL fixture failed: " + result.stdout + result.stderr)
        return result.stdout.strip()

    bridge = None
    created = False
    cases = []
    try:
        admin(f"CREATE DATABASE [{database}];\nGO\nUSE [{database}];CREATE TABLE dbo.fixture_owner(token varchar(32));INSERT dbo.fixture_owner VALUES('{token}');\nGO\n")
        created = True
        admin(f"USE master;CREATE LOGIN [{login}] WITH PASSWORD='$(COLLECTION_PASSWORD)',CHECK_POLICY=OFF;GRANT VIEW SERVER STATE TO [{login}];GRANT VIEW SERVER PERFORMANCE STATE TO [{login}];\nGO\nUSE [{database}];CREATE USER [{login}] FOR LOGIN [{login}];ALTER ROLE db_owner ADD MEMBER [{login}];\nGO\n")
        bridge_module.Bridge.request_queue_size = 128
        bridge = bridge_module.Bridge(("127.0.0.1", 0), bridge_module.Handler)
        bridge.container = container
        threading.Thread(target=bridge.serve_forever, daemon=True).start()
        port = bridge.server_address[1]

        def connect():
            value = (f"DRIVER={{SQL Server}};SERVER=127.0.0.1,{port};DATABASE={database};"
                     f"UID={login};PWD={test_password};")
            return pyodbc.connect(value, autocommit=True, timeout=30)

        def execute(sql):
            with contextlib.closing(connect()) as connection:
                connection.cursor().execute(sql)

        def scalar(sql):
            with contextlib.closing(connect()) as connection:
                return connection.cursor().execute(sql).fetchone()[0]

        def check(ok, label):
            if not ok:
                raise AssertionError(label)
            cases.append(label)
            print("PASS " + label, flush=True)

        execute("""
CREATE TABLE dbo.tbl_Users(AccountNumber int PRIMARY KEY,ExpandSlotCount int DEFAULT 0);
CREATE TABLE dbo.tbl_Units(UnitNumber int PRIMARY KEY,UnitNumber_Transform int,SallyReady int,
 UnitGradeNumber int,UnitGradeNumber2 int,UnitGraph int,UnitModelName nvarchar(100));
CREATE TABLE dbo.tbl_UserUnits(UserUnitNumber bigint IDENTITY(2147483648,1) PRIMARY KEY,
 AccountNumber int,UserUnitSlotNumber int,UnitNumber int,UnitLevelNumber tinyint DEFAULT 1);
CREATE INDEX IX_OwnerUnits ON dbo.tbl_UserUnits(AccountNumber,UnitNumber);
CREATE TABLE dbo.tbl_CapsulePicked(AccountID int,MachineID int,UnitGradeNumber tinyint,UnitID int,PickedDate datetime DEFAULT GETDATE());
CREATE TABLE dbo.tbl_MachinePrice(MachineID int PRIMARY KEY,Price int,Disable int,Type int);
CREATE TABLE dbo.tbl_MachineList(MachineID int PRIMARY KEY);
CREATE TABLE dbo.fixture_wallet(AccountNumber int PRIMARY KEY,GP int NOT NULL CHECK(GP>=0));
CREATE TABLE dbo.fixture_flags(FailCreate bit NOT NULL);
INSERT dbo.fixture_flags VALUES(0);
INSERT dbo.tbl_Users VALUES(1,0);
INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber,UnitNumber) VALUES(1,1,102);
INSERT dbo.tbl_CapsulePicked(AccountID,MachineID,UnitGradeNumber,UnitID) VALUES(1,101,1,100);
""")
        execute("""CREATE PROCEDURE dbo.sp_CreateUserUnit
 @account int,@shop varchar(6),@item int,@category int,@date datetime,@shopUnit int,
 @unit int,@level tinyint,@timeLimit tinyint,@buyType int,
 @slot int OUTPUT,@instance bigint OUTPUT,@serial bigint OUTPUT,@maxLevel tinyint OUTPUT
AS
BEGIN
 SET NOCOUNT ON;
 IF EXISTS(SELECT 1 FROM dbo.fixture_flags WHERE FailCreate=1) RETURN 3;
 IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Units WHERE UnitNumber=@unit) RETURN 2;
 SELECT @slot=ISNULL(MAX(UserUnitSlotNumber),0)+1 FROM dbo.tbl_UserUnits WHERE AccountNumber=@account;
 INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber,UnitNumber,UnitLevelNumber) VALUES(@account,@slot,@unit,@level);
 SET @instance=SCOPE_IDENTITY();SET @serial=100000000000+@instance;SET @maxLevel=2;
 RETURN 0;
END""")
        rows = [fixture_unit(100, 101), fixture_unit(101, 100, 0), fixture_unit(102),
                fixture_unit(103, 104), fixture_unit(104, ready=1)]
        execute("INSERT dbo.tbl_Units VALUES " + ",".join(
            f"({r['UnitNumber']},{r['UnitNumber_Transform']},{r['SallyReady']},1,2,{r['UnitGraph']},N'{r['UnitModelName']}')"
            for r in rows))
        install = installation(build_pool(rows), rows, 990001)
        (output / "install.sql").write_text(install, encoding="utf-8")
        execute(install)
        check(scalar("SELECT Complete FROM sdgo_collection_capsule.Coverage WHERE AccountNumber=1") == 0,
              "legacy history is imported without pretending it is exhaustive")
        check(scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.Seen WHERE AccountNumber=1") == 2,
              "existing inventory and native draw history are seeded")
        execute(install)
        check(scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.Seen WHERE AccountNumber=1") == 2,
              "idempotent reinstall preserves collection history")

        def account(number):
            execute(f"INSERT dbo.tbl_Users VALUES({number},0);INSERT dbo.fixture_wallet VALUES({number},1000000);")

        def draw(number, *, abort_success=False):
            # This models the already verified World prepay/refund boundary.
            with contextlib.closing(connect()) as wallet:
                if wallet.cursor().execute("UPDATE dbo.fixture_wallet SET GP=GP-100000 WHERE AccountNumber=? AND GP>=100000", number).rowcount != 1:
                    return 1001, None
                code, prize = 3002, None
                try:
                    with contextlib.closing(connect()) as game:
                        cur = game.cursor()
                        try:
                            cur.execute("BEGIN TRANSACTION;")
                            cur.execute(f"DECLARE @r int;EXEC @r=sdgo_collection_capsule.Draw {number},990001;SELECT @r AS FixtureReturn;")
                            while True:
                                if cur.description:
                                    names = [column[0] for column in cur.description]
                                    data = cur.fetchall()
                                    if names == ["FixtureReturn"]:
                                        code = data[0][0]
                                    elif names == ["UserUnitNum", "UnitID", "SlotIndex", "SerialNum", "nCustomLvMax"]:
                                        if len(data) != 1:
                                            raise AssertionError("Unexpected native result row count")
                                        prize = tuple(data[0])
                                    else:
                                        raise AssertionError("Unexpected native result schema")
                                if not cur.nextset():
                                    break
                            if code == 0 and prize and not abort_success:
                                cur.execute("COMMIT TRANSACTION;")
                            else:
                                cur.execute("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;")
                                code, prize = (3002 if abort_success else code), None
                        except Exception:
                            with contextlib.suppress(Exception):
                                cur.execute("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;")
                            raise
                except Exception:
                    wallet.cursor().execute("UPDATE dbo.fixture_wallet SET GP=GP+100000 WHERE AccountNumber=?", number)
                    raise
                if code:
                    wallet.cursor().execute("UPDATE dbo.fixture_wallet SET GP=GP+100000 WHERE AccountNumber=?", number)
                return code, prize

        if native_build:
            execute("UPDATE sdgo_collection_capsule.State SET Enabled=1;UPDATE dbo.tbl_MachinePrice SET Disable=0 WHERE MachineID=990001;")
            target = output / "native_core"
            mod = target / "sdgo/mods/collection_capsule"
            mod.mkdir(parents=True)
            (target / "sdgo/symbols").mkdir()
            for name, destination in (("collection_capsule_sql_test.exe", target),
                                      ("core.dll", target / "sdgo"),
                                      ("collection_capsule.dll", mod)):
                shutil.copy2(native_build / config / name, destination / name)
            manifest = (ROOT / "mods/collection_capsule/mod.ini").read_text(encoding="utf-8-sig")
            (mod / "mod.ini").write_text(re.sub(r"(?m)^hosts=.*$", "hosts=*", manifest), encoding="ascii")
            (target / "sdgo/sdgo.ini").write_text("[core]\nlog_level=0\n[mods]\ncollection_capsule=1\n", encoding="ascii")
            process_env = dict(os.environ, CommonProgramFiles=r"C:\Program Files (x86)\Common Files",
                SDGO_COLLECTION_GAME=f"Provider=SQLOLEDB;Data Source=127.0.0.1,{port};Initial Catalog={database};User ID={login};Password={test_password};")
            result = subprocess.run([str(target / "collection_capsule_sql_test.exe")], capture_output=True,
                                    text=True, errors="replace", env=process_env, timeout=120)
            (target / "stdout.txt").write_text(result.stdout + result.stderr, encoding="utf-8")
            if result.returncode:
                raise RuntimeError("Native core/OLE DB fixture failed: " + result.stdout + result.stderr)
            check("ALL COLLECTION CAPSULE SQL CHECKS PASSED" in result.stdout,
                  "actual core/Mod/OLE DB native ABI, separate sessions and malformed-row rollback")

        if not native_only:
            execute("INSERT dbo.fixture_wallet VALUES(1,1000000)")
            check(draw(1)[0] != 0 and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=1") == 1000000,
                  "incomplete legacy history rejects with native-style full refund")
            account(2)
            result = draw(2)
            check(result[0] == 0 and result[1][0] > 2147483647 and result[1][3] > 2147483647,
                  "successful draw returns the exact native 64-bit result fields")
            first = result[1][1]
            execute("DELETE dbo.tbl_UserUnits WHERE AccountNumber=2")
            check(scalar(f"SELECT COUNT(*) FROM sdgo_collection_capsule.Seen WHERE AccountNumber=2 AND UnitID={first}") == 1,
                  "selling a committed unit does not erase acquisition history")
            remaining = [draw(2), draw(2)]
            check(all(r[0] == 0 for r in remaining) and len({first, *(r[1][1] for r in remaining)}) == 3,
                  "draws do not repeat a previously sold or currently owned base unit")
            check(draw(2)[0] != 0 and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=2") == 700000,
                  "exhausted pool refunds the entire 100000 GP charge")

            account(3)
            execute("UPDATE dbo.fixture_flags SET FailCreate=1")
            check(draw(3)[0] == 3 and scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.Seen WHERE AccountNumber=3") == 0,
                  "native creation failure leaves no unit or collection mark")
            execute("UPDATE dbo.fixture_flags SET FailCreate=0")
            check(draw(3, abort_success=True)[0] != 0
                  and scalar("SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE AccountNumber=3") == 0
                  and scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.Seen WHERE AccountNumber=3") == 0,
                  "outer caller abort rolls back delivery, history and audit before refund")

            account(4)
            with contextlib.closing(connect()) as c:
                cur = c.cursor()
                cur.execute("BEGIN TRANSACTION;INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber,UnitNumber) VALUES(4,1,100),(4,2,102),(4,3,103);DELETE dbo.tbl_UserUnits WHERE AccountNumber=4 AND UnitNumber<>100;COMMIT;")
            check(scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.Seen WHERE AccountNumber=4") == 1,
                  "uncommitted bulk gifts removed by starter policy are not marked as received")

            account(5)
            with ThreadPoolExecutor(max_workers=8) as workers:
                results = list(workers.map(lambda _: draw(5), range(8)))
            successful = [r[1][1] for r in results if r[0] == 0]
            check(len(successful) == len(set(successful)) == 3
                  and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=5") == 700000,
                  "concurrent draws serialize per account without duplicates or lost refunds")

            account(6)
            execute("INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber,UnitNumber) VALUES(6,1,101)")
            results = [draw(6), draw(6)]
            check(all(r[0] == 0 and r[1][1] != 100 for r in results),
                  "ownership of a transformed form counts toward its base family")

            # No pool rebuild: a newly imported standalone unit is discovered live.
            execute("INSERT dbo.tbl_Units VALUES(110,0,1,2,2,1000110,N'Imported base')")
            check(draw(2)[1][1] == 110,
                  "newly imported standalone units enter the live pool without an ID allowlist")
            execute("INSERT dbo.tbl_Units VALUES(111,112,1,2,2,1000111,N'Imported transform base'),(112,111,0,2,2,1000112,N'Imported alternate')")
            check(draw(2)[1][1] == 111,
                  "new unique-root transform families enter dynamically without a pool rebuild")
            execute("INSERT dbo.tbl_Units VALUES(120,121,1,3,2,1000120,N'Imported alternate'),(121,120,1,3,2,1000121,N'Imported base')")
            before = scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=2")
            check(draw(2)[0] == 3008 and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=2") == before,
                  "missing imported reciprocal-root metadata is reported, not silently omitted or charged")
            declaration = {"protocol": "sdgo.unit-family", "protocol_version": 1, "installable": True,
                           "package_id": "fixture.imported-family", "family": {"root_form": "base", "forms": [
                               {"id": 121, "key": "base", "transform_target": 120},
                               {"id": 120, "key": "alternate", "transform_target": 121}]}}
            with contextlib.closing(connect()) as importer:
                importer.cursor().execute("BEGIN TRANSACTION;")
                register_unit_family(importer, declaration, in_transaction=True)
                importer.cursor().execute("COMMIT TRANSACTION;")
            # Replaying an older feature package must not erase later unit imports.
            execute(install)
            check(draw(2)[1][1] == 121,
                  "imported reciprocal family registers its declared root and survives feature reinstall")
            with contextlib.closing(connect()) as importer:
                cur = importer.cursor()
                cur.execute("BEGIN TRANSACTION;INSERT dbo.tbl_Units VALUES(130,131,1,3,2,1000130,N'Bad import'),(131,130,1,3,2,1000131,N'Bad alternate')")
                bad = json.loads(json.dumps(declaration))
                bad["package_id"] = "fixture.bad-family"
                bad["family"]["root_form"] = None
                bad["family"]["forms"] = [{"id": 130, "key": "base", "transform_target": 131},
                                               {"id": 131, "key": "alternate", "transform_target": 130}]
                rejected = False
                try:
                    register_unit_family(importer, bad, in_transaction=True)
                except ValueError:
                    rejected = True
                cur.execute("ROLLBACK TRANSACTION;")
            check(rejected and scalar("SELECT COUNT(*) FROM dbo.tbl_Units WHERE UnitNumber IN(130,131)") == 0,
                  "unit-import transaction rejects missing root declarations without a half-installed family")
            execute(disable(990001))
            check(draw(3)[0] != 0 and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=3") == 1000000,
                  "disabling this machine preserves history and fully refunds rejected draws")

        receipt = dict(passed=cases, database=database, no_original_database_changes=True,
                       no_game_execution=True, native_payment_boundary_simulated=not native_only, native_ole_binding_exercised=bool(native_build))
    finally:
        if bridge:
            bridge.shutdown()
            bridge.server_close()
        if created:
            marker = admin(f"SET NOCOUNT ON;SELECT token FROM [{database}].dbo.fixture_owner;\nGO\n")
            if marker.strip() != token or not database.startswith("CollectionTest_") or not database.endswith(tag):
                raise RuntimeError("Fixture ownership changed; refusing database cleanup")
            admin(f"USE master;ALTER DATABASE [{database}] SET SINGLE_USER WITH ROLLBACK IMMEDIATE;DROP DATABASE [{database}];IF EXISTS(SELECT 1 FROM sys.sql_logins WHERE name='{login}') DROP LOGIN [{login}];\nGO\n")
    receipt["cleanup_completed"] = True
    (output / "result.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--container", required=True)
    parser.add_argument("--credential-file", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--native-build", type=Path)
    parser.add_argument("--config", default="Release")
    parser.add_argument("--native-only", action="store_true")
    args = parser.parse_args()
    run(args.container, args.credential_file, args.out.resolve(), args.native_build, args.config, args.native_only)
