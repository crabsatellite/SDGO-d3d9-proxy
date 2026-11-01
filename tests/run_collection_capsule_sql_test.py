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
from owner.collection_capsule.sql import installation, disable, catalog_upgrade
from owner.collection_capsule.deployment import register_unit_family

spec = importlib.util.spec_from_file_location("starter_bridge", ROOT / "tests/run_new_account_sql_test.py")
bridge_module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge_module)


def fixture_unit(uid, target=0, ready=1, rank=1):
    return dict(UnitNumber=uid, UnitNumber_Transform=target, SallyReady=ready,
                UnitGradeNumber=rank, UnitGradeNumber2=2, UnitGraph=1000000 + uid,
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
        temp_collation = admin("SELECT CONVERT(nvarchar(128),DATABASEPROPERTYEX('tempdb','Collation'));\nGO\n")
        fixture_collation = 'Latin1_General_CI_AS' if temp_collation.strip() == 'Thai_CI_AS' else 'Thai_CI_AS'
        admin(f"CREATE DATABASE [{database}] COLLATE {fixture_collation};\nGO\nUSE [{database}];CREATE TABLE dbo.fixture_owner(token varchar(32));INSERT dbo.fixture_owner VALUES('{token}');\nGO\n")
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

        def execute_fails(sql):
            try:
                execute(sql)
            except pyodbc.Error:
                return True
            return False

        check(scalar("SELECT CONVERT(nvarchar(128),DATABASEPROPERTYEX(DB_NAME(),'Collation'))")
              != scalar("SELECT CONVERT(nvarchar(128),DATABASEPROPERTYEX('tempdb','Collation'))"),
              "fixture database and tempdb deliberately use different collations")

        execute("""
CREATE TABLE dbo.tbl_Users(AccountNumber int PRIMARY KEY,ExpandSlotCount int DEFAULT 0);
CREATE TABLE dbo.tbl_Units(UnitNumber int PRIMARY KEY,UnitNumber_Transform int,SallyReady int,
 UnitGradeNumber int,UnitGradeNumber2 int,UnitGraph int,UnitModelName nvarchar(100));
CREATE TABLE dbo.tbl_UserUnits(UserUnitNumber bigint IDENTITY(2147483648,1) PRIMARY KEY,
 AccountNumber int,UserUnitSlotNumber int,UnitNumber int,UnitLevelNumber tinyint DEFAULT 1,TimeLimit tinyint NOT NULL DEFAULT 0,RunOut tinyint NOT NULL DEFAULT 0);
CREATE INDEX IX_OwnerUnits ON dbo.tbl_UserUnits(AccountNumber,UnitNumber);
CREATE TABLE dbo.tbl_CapsulePicked(AccountID int,MachineID int,UnitGradeNumber tinyint,UnitID int,PickedDate datetime DEFAULT GETDATE());
CREATE TABLE dbo.tbl_MachinePrice(MachineID int PRIMARY KEY,Price int,Disable int,Type int);
CREATE TABLE dbo.tbl_MachineList(MachineID int PRIMARY KEY);
CREATE TABLE dbo.tbl_ItemMachinePrice(MachineID int PRIMARY KEY,Price int,Disable int,Type int);
CREATE TABLE dbo.tbl_ItemMachineList(MachineID int PRIMARY KEY,Idx int,CategoryID int,Amount int,used tinyint,ItemName varchar(50));
CREATE TABLE dbo.tbl_ItemCategory(CatalogueID int PRIMARY KEY,Name nvarchar(100),RefIndex int,
 TimeLimit tinyint,Option1 int,Option2 int);
CREATE TABLE dbo.tbl_UserDropItemBox(UserDropItemIdx bigint IDENTITY(2147483648,1) PRIMARY KEY,
 AccountNumber int,ItemCategory int,Amount int,DropConditionID int,Applied bit NOT NULL DEFAULT 0,
 DropDate datetime NOT NULL DEFAULT GETDATE());
CREATE INDEX IX_OwnerDropItems ON dbo.tbl_UserDropItemBox(AccountNumber,ItemCategory);
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
            f"({r['UnitNumber']},{r['UnitNumber_Transform']},{r['SallyReady']},{r['UnitGradeNumber']},2,{r['UnitGraph']},N'{r['UnitModelName']}')"
            for r in rows))
        execute("""INSERT dbo.tbl_ItemCategory(CatalogueID,Name,RefIndex,TimeLimit,Option1,Option2) VALUES
(1100100,N'Fixture 100',100,0,1,2),(1100102,N'Fixture 102',102,0,1,2),
(1100103,N'Fixture 103',103,0,1,2);""")
        install = installation(build_pool(rows), rows, 990001,
                               unit_items=[])
        (output / "install.sql").write_text(install, encoding="utf-8")
        execute(install)
        check(scalar("SELECT COUNT(*) FROM sys.tables WHERE schema_id=SCHEMA_ID('sdgo_collection_capsule') AND name IN ('Coverage','Seen','DrawLog')") == 0,
              "obsolete complete-history proof and audit tables are absent")
        check(scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=1") == 1,
              "only current permanent inventory seeds the initial catalog")
        execute("""UPDATE sdgo_collection_capsule.State SET SchemaVersion=2;
CREATE TABLE sdgo_collection_capsule.Seen(AccountNumber int,UnitID int);
INSERT sdgo_collection_capsule.Seen VALUES(1,100);
CREATE TABLE sdgo_collection_capsule.Coverage(AccountNumber int,Complete bit);
INSERT sdgo_collection_capsule.Coverage VALUES(1,0);
CREATE TABLE sdgo_collection_capsule.DrawLog(AccountNumber int,UnitID int);
ALTER TABLE sdgo_collection_capsule.State ADD LegacyAccountMax int NOT NULL DEFAULT 2,RegistrationBaseline bit NOT NULL DEFAULT 1;
""")
        execute(catalog_upgrade())
        check(scalar("SELECT SchemaVersion FROM sdgo_collection_capsule.State") == 3
              and scalar("SELECT COUNT(*) FROM sys.tables WHERE schema_id=SCHEMA_ID('sdgo_collection_capsule') AND name IN ('Coverage','Seen','DrawLog')") == 0
              and scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=1 AND UnitID=100") == 0
              and scalar("SELECT COUNT(*) FROM dbo.tbl_CapsulePicked WHERE AccountID=1 AND UnitID=100") == 1,
              "owned schema-2 upgrade removes extra history without deleting native records or importing old ledger facts")
        execute(catalog_upgrade())
        check(scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=1") == 1,
              "catalog-only hot upgrade is idempotent")

        execute("""UPDATE sdgo_collection_capsule.State SET SchemaVersion=1;
DELETE dbo.tbl_ItemMachinePrice WHERE MachineID=990001;
INSERT dbo.tbl_MachinePrice VALUES(990001,99999,0,0);""")
        check(execute_fails(install)
              and scalar("SELECT SchemaVersion FROM sdgo_collection_capsule.State") == 1
              and scalar("SELECT Price FROM dbo.tbl_MachinePrice WHERE MachineID=990001") == 99999
              and scalar("SELECT COUNT(*) FROM dbo.tbl_ItemMachinePrice WHERE MachineID=990001") == 0,
              "schema-1 migration rejects an externally edited ordinary price without partial changes")
        execute("UPDATE dbo.tbl_MachinePrice SET Price=100000 WHERE MachineID=990001")
        execute(install)
        check(scalar("SELECT SchemaVersion FROM sdgo_collection_capsule.State") == 3
              and scalar("SELECT COUNT(*) FROM dbo.tbl_MachinePrice WHERE MachineID=990001") == 0
              and scalar("SELECT Price FROM dbo.tbl_ItemMachinePrice WHERE MachineID=990001") == 100000,
              "owned schema-1 ordinary machine migrates atomically to the activity machine")
        execute("UPDATE dbo.tbl_ItemMachinePrice SET Price=99999 WHERE MachineID=990001")
        check(execute_fails(install)
              and scalar("SELECT SchemaVersion FROM sdgo_collection_capsule.State") == 3
              and scalar("SELECT Price FROM dbo.tbl_ItemMachinePrice WHERE MachineID=990001") == 99999,
              "schema-2 reinstall rejects an externally edited activity price")
        execute("UPDATE dbo.tbl_ItemMachinePrice SET Price=100000 WHERE MachineID=990001")
        execute(install)
        check(scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=1") == 1,
              "idempotent activity reinstall preserves catalog unlocks")
        policy = re.sub(r"\s+", "", (ROOT / "owner/collection_capsule/sql/draw.sql").read_text(encoding="utf-8"))
        check("(VALUES(1,35),(2,30),(3,20),(4,15))" in policy
              and "WHEREEXISTS(SELECT1FROM@choicescWHEREc.UnitRank=w.UnitRank)" in policy
              and "SELECT@total=SUM(Weight)FROM@grades" in policy,
              "rank plan is C35/B30/A20/S15 and renormalizes over nonempty ranks")
        execute("INSERT dbo.tbl_Users VALUES(2,0)")
        check(scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=2") == 0,
              "existing and new profiles need no complete-history import")

        def account(number):
            execute(f"INSERT dbo.tbl_Users VALUES({number},0);INSERT dbo.fixture_wallet VALUES({number},1000000);")

        def publish_category(unit, identity=None):
            identity = identity or 1100000 + unit
            execute("INSERT dbo.tbl_ItemCategory(CatalogueID,Name,RefIndex,TimeLimit,Option1,Option2) "
                    f"VALUES({identity},N'Imported {unit}',{unit},0,1,2)")
            return identity

        def unit_for(prize):
            return scalar(f"SELECT RefIndex FROM dbo.tbl_ItemCategory WHERE CatalogueID={int(prize[1])}")

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
                                    elif names == ["DropBoxSerial", "ItemCategory", "Amount"]:
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

        def claim(number, prize):
            drop_box, category, amount = (int(value) for value in prize)
            if amount != 1:
                raise AssertionError("Unexpected fixture reward amount")
            with contextlib.closing(connect()) as game:
                cur = game.cursor()
                try:
                    cur.execute("BEGIN TRANSACTION;")
                    row = cur.execute(
                        "SELECT c.RefIndex FROM dbo.tbl_UserDropItemBox b WITH(UPDLOCK,HOLDLOCK) "
                        "JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=b.ItemCategory "
                        f"WHERE b.AccountNumber={number} AND b.UserDropItemIdx={drop_box} "
                        f"AND b.ItemCategory={category} AND b.Amount=1"
                    ).fetchone()
                    if not row:
                        cur.execute("ROLLBACK TRANSACTION;")
                        return 2, None
                    unit = int(row[0])
                    result = cur.execute(f"""
DECLARE @slot int,@instance bigint,@serial bigint,@max tinyint,@r int,@now datetime=GETDATE();
EXEC @r=dbo.sp_CreateUserUnit {number},'Event',{category},{category},@now,0,
     {unit},1,0,0,@slot OUTPUT,@instance OUTPUT,@serial OUTPUT,@max OUTPUT;
SELECT @r,@slot,@instance,@serial,@max;
""").fetchone()
                    code = int(result[0])
                    if code:
                        cur.execute("ROLLBACK TRANSACTION;")
                        return code, None
                    deleted = cur.execute(
                        "DELETE dbo.tbl_UserDropItemBox "
                        f"WHERE AccountNumber={number} AND UserDropItemIdx={drop_box} "
                        f"AND ItemCategory={category} AND Amount=1"
                    ).rowcount
                    if deleted != 1:
                        cur.execute("ROLLBACK TRANSACTION;")
                        return 3002, None
                    cur.execute("COMMIT TRANSACTION;")
                    return 0, (unit, *(int(value) for value in result[1:]))
                except Exception:
                    with contextlib.suppress(Exception):
                        cur.execute("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;")
                    raise

        if native_build:
            execute("UPDATE sdgo_collection_capsule.State SET Enabled=1;UPDATE dbo.tbl_ItemMachinePrice SET Disable=0 WHERE MachineID=990001;")
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
            legacy = draw(1)
            check(legacy[0] == 0 and unit_for(legacy[1]) != 102
                  and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=1") == 900000,
                  "legacy profiles draw immediately and avoid their currently owned catalog unit")
            # Poison rows would fail delivery validation if they leaked into
            # the pool, so success proves filtering precedes prize selection.
            execute("""INSERT dbo.tbl_Units VALUES
(151,0,0,1,2,1000151,N'Unready record'),
(152,0,1,1,1,1000152,N'Training record'),
(153,0,1,1,2,1010001,N'name');""")
            account(15)
            filtered = draw(15)
            check(filtered[0] == 0 and unit_for(filtered[1]) in {100,102,103},
                  "unready, training, placeholder and independently unusable post-form rows are filtered before drawing")
            execute("DELETE dbo.tbl_Units WHERE UnitNumber IN(151,152,153);")

            account(7)
            result = draw(7)
            first = unit_for(result[1])
            check(result[0] == 0 and result[1][0] > 2147483647 and result[1][2] == 1
                  and scalar(f"SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE UserDropItemIdx={result[1][0]}") == 1
                  and scalar(f"SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=7 AND UnitID={first}") == 1,
                  "successful draw returns the exact activity reward and commits a 64-bit drop box")
            claimed = claim(7, result[1])
            check(claimed[0] == 0 and claimed[1][0] == first
                  and claimed[1][2] > 2147483647 and claimed[1][3] > 2147483647
                  and scalar(f"SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE UserDropItemIdx={result[1][0]}") == 0,
                  "native claim creates the unit and removes its pending drop box atomically")
            execute("DELETE dbo.tbl_UserUnits WHERE AccountNumber=7")
            check(scalar(f"SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=7 AND UnitID={first}") == 1,
                  "selling a committed unit retains its catalog unlock")
            second = draw(7)
            second_unit = unit_for(second[1])
            third = draw(7)
            third_unit = unit_for(third[1])
            check(second[0] == 0 and third[0] == 0
                  and len({first, second_unit, third_unit}) == 3,
                  "pending, claimed and deleted rewards cannot repeat a base family")
            execute(f"DELETE dbo.tbl_UserDropItemBox WHERE UserDropItemIdx={second[1][0]}")
            check(scalar(f"SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=7 AND UnitID={second_unit}") == 1,
                  "deleting an unclaimed pending box retains its catalog unlock")
            refill_first, refill_second = draw(7), draw(7)
            check(refill_first[0] == 0 and refill_second[0] == 0
                  and {unit_for(refill_first[1]), unit_for(refill_second[1])} == {first, second_unit},
                  "after every catalog family is unlocked, only missing inventory families refill")
            check(draw(7)[0] != 0 and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=7") == 500000,
                  "fully held or pending catalog refunds the entire 100000 GP charge")

            account(3)
            execute("UPDATE dbo.fixture_flags SET FailCreate=1")
            pending = draw(3)
            failed_claim = claim(3, pending[1])
            check(pending[0] == 0 and failed_claim[0] == 3
                  and scalar("SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=3") == 1
                  and scalar("SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE AccountNumber=3") == 0
                  and scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=3") == 1,
                  "claim failure keeps the already awarded box and its acquisition reservation")
            execute("UPDATE dbo.fixture_flags SET FailCreate=0")
            check(claim(3, pending[1])[0] == 0
                  and scalar("SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=3") == 0
                  and scalar("SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE AccountNumber=3") == 1,
                  "a later native claim can consume the preserved box")

            account(9)
            check(draw(9, abort_success=True)[0] != 0
                  and scalar("SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox WHERE AccountNumber=9") == 0
                  and scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=9") == 0
                  and scalar("SELECT COUNT(*) FROM dbo.tbl_CapsulePicked WHERE AccountID=9") == 0,
                  "outer caller abort rolls back drop box, catalog unlock and native receipt before refund")

            account(4)
            with contextlib.closing(connect()) as c:
                cur = c.cursor()
                cur.execute("BEGIN TRANSACTION;INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber,UnitNumber) VALUES(4,1,100),(4,2,102),(4,3,103);DELETE dbo.tbl_UserUnits WHERE AccountNumber=4 AND UnitNumber<>100;COMMIT;")
            check(scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.CatalogUnlocks WHERE AccountNumber=4") == 1,
                  "uncommitted bulk gifts removed by starter policy are not marked as received")

            account(5)
            with ThreadPoolExecutor(max_workers=8) as workers:
                results = list(workers.map(lambda _: draw(5), range(8)))
            successful = [unit_for(r[1]) for r in results if r[0] == 0]
            check(len(successful) == len(set(successful)) == 3
                  and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=5") == 700000,
                  "concurrent draws serialize per account without duplicates or lost refunds")

            account(6)
            execute("INSERT dbo.tbl_UserUnits(AccountNumber,UserUnitSlotNumber,UnitNumber) VALUES(6,1,101)")
            results = [draw(6), draw(6)]
            check(all(r[0] == 0 and unit_for(r[1]) != 100 for r in results),
                  "ownership of a transformed form counts toward its base family")

            # No pool rebuild: a newly imported standalone unit is discovered live.
            execute("INSERT dbo.tbl_Units VALUES(110,0,1,2,2,1000110,N'Imported base')")
            before = scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=7")
            check(draw(7)[0] == 3008 and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=7") == before,
                  "a new raw unit without native delivery metadata rejects and fully refunds")
            publish_category(110)
            imported = draw(7)
            check(imported[0] == 0 and unit_for(imported[1]) == 110
                  and scalar("SELECT COUNT(*) FROM sdgo_collection_capsule.Roots WHERE UnitID=110") == 0,
                  "a valid database-only unit absent from the installed catalog participates dynamically without a pool rebuild")
            execute("INSERT dbo.tbl_Units VALUES(111,112,1,2,2,1000111,N'Imported transform base'),(112,111,0,2,2,1000112,N'Imported alternate')")
            publish_category(111)
            imported = draw(7)
            check(imported[0] == 0 and unit_for(imported[1]) == 111,
                  "new unique-root transform families enter dynamically without a pool rebuild")
            execute("INSERT dbo.tbl_Units VALUES(120,121,1,3,2,1000120,N'Imported alternate'),(121,120,1,3,2,1000121,N'Imported base')")
            publish_category(121)
            before = scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=7")
            check(draw(7)[0] == 3008 and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=7") == before,
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
            imported = draw(7)
            check(imported[0] == 0 and unit_for(imported[1]) == 121,
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
            mapped = installation(build_pool(rows), rows, 990001,
                unit_items=[{'unit_id': 100, 'category_id': 1180000, 'name': 'Verified client gift', 'created_by_collection': True}])
            execute(mapped)
            execute(install)
            account(11)
            execute("INSERT sdgo_collection_capsule.CatalogUnlocks(AccountNumber,UnitID,PendingTransactionID) VALUES(11,102,0),(11,103,0),(11,110,0),(11,111,0),(11,121,0)")
            gift = draw(11)
            check(gift[0] == 0 and gift[1][1] == 1180000
                  and scalar("SELECT RefIndex FROM dbo.tbl_ItemCategory WHERE CatalogueID=1100100") == 100,
                  "verified client gift mapping overrides lower server-only categories and survives older package replay")
            collision = installation(build_pool(rows), rows, 990001, unit_items=[
                {'unit_id': 102, 'category_id': 1180000, 'name': 'Collision', 'created_by_collection': True}])
            check(execute_fails(collision)
                  and scalar("SELECT UnitID FROM sdgo_collection_capsule.OwnedUnitItems WHERE CategoryID=1180000") == 100
                  and scalar("SELECT CategoryID FROM sdgo_collection_capsule.UnitDelivery WHERE UnitID=100") == 1180000,
                  "owned permanent category collision rolls back without breaking historical gift mapping")
            execute(disable(990001))
            account(10)
            check(draw(10)[0] != 0 and scalar("SELECT GP FROM dbo.fixture_wallet WHERE AccountNumber=10") == 1000000,
                  "disabling this machine preserves catalog unlocks and fully refunds rejected draws")

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
