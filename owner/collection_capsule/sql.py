"""Generate owner-installable SQL without exporting credentials or player data."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path

HERE = Path(__file__).with_name("sql")
OWNER = "collection_capsule/v1"
MODULES = {
    "dbo.sdgo_collection_capture_profiles": "capture_profiles.sql",
    "dbo.sdgo_collection_capture_units": "capture_units.sql",
    "sdgo_collection_capsule.Draw": "draw.sql",
}


def literal(value: str) -> str:
    return "N'" + value.replace("'", "''") + "'"


def dynamic(sql: str) -> str:
    return "EXEC(" + literal(sql) + ");\n"


def insert(table: str, columns: tuple[str, ...], rows: list[tuple]) -> str:
    result = []
    for offset in range(0, len(rows), 300):
        chunk = rows[offset:offset + 300]
        values = ["(" + ",".join(str(value) for value in row) + ")" for row in chunk]
        result.append(f"INSERT {table}({','.join(columns)}) VALUES\n" + ",\n".join(values) + ";\n")
    return "".join(result)


def pool_hash(pool: dict) -> str:
    return hashlib.sha256(json.dumps(pool, sort_keys=True, ensure_ascii=False,
                                     separators=(",", ":")).encode()).hexdigest()


def installation(pool: dict, source_rows: list[dict], machine_id: int, *, legacy_account_max: int | None = None) -> str:
    from .metadata import validate_source
    validate_source(source_rows)
    source = {r["UnitNumber"]: r for r in source_rows}
    if type(machine_id) is not int or not 0 < machine_id <= 2_147_483_647:
        raise ValueError("Machine ID must be a positive SQL INT")
    if legacy_account_max is not None and (type(legacy_account_max) is not int or not 0 <= legacy_account_max <= 2147483647):
        raise ValueError("Invalid registration baseline")
    if not pool.get("units") or len(pool["units"]) > 20000:
        raise ValueError("Collection pool is empty or too large")
    root_ids = set()
    for root in pool["units"]:
        uid = root.get("unit_id")
        if (type(uid) is not int or uid not in source or uid in root_ids
                or root.get("rank") != source[uid]["UnitGradeNumber"]
                or root.get("subgrade") != source[uid]["UnitGradeNumber2"]
                or not isinstance(root.get("basis"), str)):
            raise ValueError("Pool root metadata differs from its unit source")
        root_ids.add(uid)
    if any(type(f.get("unit_id")) is not int or f["unit_id"] not in source
           or type(f.get("base_unit_id")) is not int or f["base_unit_id"] not in root_ids
           for f in pool.get("form_aliases", [])):
        raise ValueError("Invalid form metadata")
    checksum = pool_hash(pool)
    sql = """SET NOCOUNT ON;
SET ANSI_NULLS ON;
SET QUOTED_IDENTIFIER ON;
SET XACT_ABORT ON;
BEGIN TRY
BEGIN TRANSACTION;
DECLARE @installLock int;
EXEC @installLock=sys.sp_getapplock @Resource=N'sdgo.collection_capsule.install.v1',
     @LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000;
IF @installLock<0 RAISERROR('Collection install is busy',16,1);
IF OBJECT_ID(N'dbo.tbl_Users',N'U') IS NULL OR OBJECT_ID(N'dbo.tbl_UserUnits',N'U') IS NULL
 OR OBJECT_ID(N'dbo.tbl_MachinePrice',N'U') IS NULL OR OBJECT_ID(N'dbo.sp_CreateUserUnit',N'P') IS NULL
 OR OBJECT_ID(N'dbo.tbl_CapsulePicked',N'U') IS NULL
 RAISERROR('Required native capsule tables/procedure are missing',16,1);
DECLARE @transactionProbe bigint;
SELECT @transactionProbe=transaction_id FROM sys.dm_tran_current_transaction;
IF @transactionProbe IS NULL RAISERROR('Transaction tracking permission is missing',16,1);
"""
    sql += HERE.joinpath("schema.sql").read_text(encoding="utf-8") + "\n"
    sql += f"""
IF EXISTS(SELECT 1 FROM sdgo_collection_capsule.State WHERE SchemaVersion<>1 OR MachineID<>{machine_id})
 RAISERROR('Installed collection schema or machine ID differs',16,1);
IF EXISTS(SELECT 1 FROM dbo.tbl_MachinePrice WHERE MachineID={machine_id})
 AND NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.State s JOIN dbo.tbl_MachinePrice p ON p.MachineID=s.MachineID
                WHERE p.MachineID={machine_id} AND p.Price=s.PriceGP AND p.Disable=CASE WHEN s.Enabled=1 THEN 0 ELSE 1 END AND p.Type=0)
 RAISERROR('Machine ID is occupied or its price was externally edited',16,1);
IF OBJECT_ID(N'dbo.tbl_MachineList',N'U') IS NOT NULL AND EXISTS(SELECT 1 FROM dbo.tbl_MachineList WHERE MachineID={machine_id})
 RAISERROR('Collection machine must not have a native fallback sequence',16,1);
IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleMachine',N'U') IS NOT NULL
BEGIN
 DECLARE @foreignMachine int=0;
 EXEC sys.sp_executesql N'SELECT @n=COUNT(*) FROM dbo.sdgo_mod_CapsuleMachine WHERE MachineID=@id',
      N'@id int,@n int OUTPUT',@id={machine_id},@n=@foreignMachine OUTPUT;
 IF @foreignMachine<>0 RAISERROR('Collection machine ID belongs to another capsule provider',16,1);
END;
"""
    for name, filename in MODULES.items():
        body = HERE.joinpath(filename).read_text(encoding="utf-8").replace("\r\n", "\n").strip()
        kind = "TRIGGER" if filename.startswith("capture") else "PROCEDURE"
        # Module checks use the same NVARCHAR bytes SQL Server stores, retaining
        # SQL 2014 compatibility (no UTF-8 collation/CREATE OR ALTER dependency).
        sha = hashlib.sha256(body.encode("utf-16-le")).hexdigest()
        sql += f"""
IF OBJECT_ID({literal(name)}) IS NOT NULL
BEGIN
 IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Modules WHERE ObjectName={literal(name)})
  RAISERROR('Collection module name belongs to another owner',16,1);
 IF CONVERT(varbinary(max),OBJECT_DEFINITION(OBJECT_ID({literal(name)})))
    <>(SELECT CONVERT(varbinary(max),InstalledDefinition) FROM sdgo_collection_capsule.Modules WHERE ObjectName={literal(name)})
    OR EXISTS(SELECT 1 FROM sys.sql_modules WHERE object_id=OBJECT_ID({literal(name)}) AND (uses_ansi_nulls<>1 OR uses_quoted_identifier<>1))
  RAISERROR('Collection module was externally edited',16,1);
 DROP {kind} {name};
END;
"""
        sql += dynamic(body)
        sql += f"DELETE sdgo_collection_capsule.Modules WHERE ObjectName={literal(name)};\n"
        sql += f"INSERT sdgo_collection_capsule.Modules VALUES({literal(name)},{literal(sha)},{literal(body)});\n"
    sql += """
CREATE TABLE #CollectionRootInput(UnitID int PRIMARY KEY,UnitRank int,UnitSubgrade int,Origin varchar(16) COLLATE DATABASE_DEFAULT);
CREATE TABLE #CollectionSourceInput(UnitID int PRIMARY KEY,TransformTo int,Ready int,UnitRank int,UnitSubgrade int,UnitGraph int,Placeholder bit);
CREATE TABLE #CollectionImportInput(UnitID int PRIMARY KEY,RootUnitID int,PackageID nvarchar(128) COLLATE DATABASE_DEFAULT,RevisionHash char(64) COLLATE DATABASE_DEFAULT);
"""
    roots = [u for u in pool["units"] if u.get("basis") != "unique ownable transform root"]
    for root in roots:
        origin = "import" if root["basis"].startswith("unit-family:") else "native"
        sql += f"INSERT #CollectionRootInput VALUES({root['unit_id']},{root['rank']},{root['subgrade']},'{origin}');\n"
        if origin == "import":
            package, revision = root["basis"][len("unit-family:"):].rsplit(":", 1)
            if not package or len(package) > 128 or len(revision) != 64 or any(c not in "0123456789abcdef" for c in revision):
                raise ValueError("Invalid imported family metadata revision")
            for form in pool["form_aliases"]:
                if form["base_unit_id"] == root["unit_id"]:
                    sql += f"INSERT #CollectionImportInput VALUES({form['unit_id']},{root['unit_id']},{literal(package)},{literal(revision)});\n"
    sql += insert("#CollectionSourceInput",
                  ("UnitID", "TransformTo", "Ready", "UnitRank", "UnitSubgrade", "UnitGraph", "Placeholder"),
                  [(r["UnitNumber"], int(r.get("UnitNumber_Transform") or 0), r["SallyReady"],
                    r["UnitGradeNumber"], r["UnitGradeNumber2"], r["UnitGraph"],
                    int(str(r.get("UnitModelName", "")).strip().lower() == "name" and r["UnitGraph"] == 1010001))
                   for r in source_rows])
    sql += """
IF EXISTS(SELECT 1 FROM #CollectionImportInput n JOIN sdgo_collection_capsule.ImportedForms old ON old.UnitID=n.UnitID WHERE old.PackageID<>n.PackageID)
 RAISERROR('Imported form ID belongs to another package',16,1);
DELETE r FROM sdgo_collection_capsule.Roots r WHERE r.Origin='native'
 OR r.UnitID IN(SELECT old.UnitID FROM sdgo_collection_capsule.ImportedForms old
               WHERE old.PackageID IN(SELECT PackageID FROM #CollectionImportInput));
DELETE s FROM sdgo_collection_capsule.UnitSource s WHERE s.Origin='native'
 OR s.UnitID IN(SELECT UnitID FROM #CollectionImportInput);
DELETE old FROM sdgo_collection_capsule.ImportedForms old
 WHERE old.PackageID IN(SELECT PackageID FROM #CollectionImportInput);
INSERT sdgo_collection_capsule.ImportedForms SELECT UnitID,RootUnitID,PackageID,RevisionHash FROM #CollectionImportInput;
INSERT sdgo_collection_capsule.Roots(UnitID,UnitRank,UnitSubgrade,Origin)
 SELECT r.UnitID,r.UnitRank,r.UnitSubgrade,r.Origin FROM #CollectionRootInput r
 WHERE r.Origin='import' OR NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.ImportedForms f WHERE f.UnitID=r.UnitID);
INSERT sdgo_collection_capsule.UnitSource(UnitID,TransformTo,Ready,UnitRank,UnitSubgrade,UnitGraph,Placeholder,Origin)
 SELECT n.UnitID,n.TransformTo,n.Ready,n.UnitRank,n.UnitSubgrade,n.UnitGraph,n.Placeholder,
        CASE WHEN EXISTS(SELECT 1 FROM sdgo_collection_capsule.ImportedForms f WHERE f.UnitID=n.UnitID) THEN 'import' ELSE 'native' END
 FROM #CollectionSourceInput n
 WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.UnitSource existing WHERE existing.UnitID=n.UnitID);
"""
    sql += HERE.joinpath("seed_history.sql").read_text(encoding="utf-8") + "\n"
    sql += f"""
IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.State)
 INSERT sdgo_collection_capsule.State(Singleton,SchemaVersion,MachineID,PriceGP,PoolHash,Enabled)
 VALUES(1,1,{machine_id},100000,{literal(checksum)},1);
ELSE UPDATE sdgo_collection_capsule.State SET PoolHash={literal(checksum)},Enabled=1 WHERE Singleton=1;
IF NOT EXISTS(SELECT 1 FROM dbo.tbl_MachinePrice WHERE MachineID={machine_id})
 INSERT dbo.tbl_MachinePrice(MachineID,Price,Disable,Type) VALUES({machine_id},100000,0,0);
ELSE UPDATE dbo.tbl_MachinePrice SET Disable=0 WHERE MachineID={machine_id};
"""
    if legacy_account_max is not None:
        sql += f"UPDATE sdgo_collection_capsule.State SET LegacyAccountMax={legacy_account_max},RegistrationBaseline=1 WHERE Singleton=1 AND RegistrationBaseline=0;\n"
    sql += """
COMMIT;
END TRY
BEGIN CATCH
 IF XACT_STATE()<>0 ROLLBACK;
 DECLARE @error nvarchar(2048)=ERROR_MESSAGE();
 RAISERROR('%s',16,1,@error);
END CATCH;
"""
    return sql


def disable(machine_id: int) -> str:
    if type(machine_id) is not int or not 0 < machine_id <= 2_147_483_647:
        raise ValueError("Invalid machine ID")
    return f"""SET XACT_ABORT ON;BEGIN TRANSACTION;
IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.State WHERE Singleton=1 AND MachineID={machine_id})
BEGIN ROLLBACK;RAISERROR('Collection ownership differs',16,1);RETURN;END;
UPDATE sdgo_collection_capsule.State SET Enabled=0 WHERE Singleton=1;
UPDATE dbo.tbl_MachinePrice SET Disable=1 WHERE MachineID={machine_id};
COMMIT;
-- Keep history and capture triggers so re-enabling does not permit repeats.
"""
