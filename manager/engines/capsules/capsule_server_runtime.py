"""Build a fail-closed SQL Server runtime adapter for capsule configuration.

This tool only reads a server bundle and writes deployment artifacts.  It does
not connect to SQL Server, inspect player data, restart a service, or execute a
draw.  The generated adapter intercepts only ``sp_GetNextCapsule1`` for fixed
machines whose policy is ``independent_weighted``.  The native
``sp_CreateCapsule`` procedure therefore keeps ownership of limit checks,
inventory creation, result rows, and error codes, while WorldServer keeps
ownership of charging and compensation.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re


GRADE_SETS = (
    (2,), (5,), (4,), (3,), (2, 5), (2, 4), (2, 3), (5, 4),
    (5, 3), (4, 3), (2, 5, 4), (2, 5, 3), (2, 4, 3),
    (5, 4, 3), (2, 5, 4, 3),
)
GRADE_SET_INDEX = {values: index for index, values in enumerate(GRADE_SETS)}
REVISION_RE = re.compile(r"^[0-9a-f]{64}$")
WORLD_SERVER_SHA256 = "82d3c4cf09679a431b9bfa639795b148f932642d8774ff66021785115dc53d46"
SQL_INT_MAX = 2_147_483_647
SQL_SMALLINT_MAX = 32_767


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2,
                       allow_nan=False) + "\n").encode("utf-8")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def write_bytes(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_bytes() == data:
        return False
    pending = path.with_name(path.name + ".pending")
    if pending.exists():
        pending.unlink()
    with pending.open("xb") as handle:
        handle.write(data)
    pending.replace(path)
    return True


def require(condition, message):
    if not condition:
        raise ValueError(message)


def validate_bundle(bundle):
    require(bundle.get("schema_version") == 1, "Unsupported server bundle schema")
    require(bundle.get("role") == "server", "Expected a server bundle")
    revision = bundle.get("catalog_revision")
    require(isinstance(revision, str) and REVISION_RE.fullmatch(revision),
            "Invalid catalog revision")
    units = bundle.get("units")
    require(isinstance(units, list), "Missing unit catalog")
    require(all(type(unit.get("id")) is int
                and 0 < unit["id"] <= SQL_INT_MAX for unit in units),
            "Unit IDs must be positive SQL INT values")
    unit_ids = {unit["id"] for unit in units}
    require(len(unit_ids) == len(units),
            "Duplicate or invalid unit ID")
    unit_by_id = {unit["id"]: unit for unit in units}
    require(all(type(unit.get("rank")) is int and 1 <= unit["rank"] <= 4
                and type(unit.get("subgrade")) is int
                and 1 <= unit["subgrade"] <= 5
                for unit in units),
            "Unit rank or subgrade is invalid")
    if "custom_candidate_policy" in bundle:
        from .capsule_catalog import validate as validate_catalog
        validate_catalog(bundle)

    fixed = bundle.get("fixed_machines")
    custom = bundle.get("custom_machines")
    require(isinstance(fixed, list) and isinstance(custom, list),
            "Missing machine collections")
    all_ids = set()
    for machine in fixed:
        machine_id = machine.get("id")
        require(type(machine_id) is int and 0 < machine_id <= SQL_INT_MAX
                and machine_id not in all_ids,
                "Invalid or duplicate fixed machine ID")
        all_ids.add(machine_id)
        require(type(machine.get("enabled")) is bool, "Fixed enabled must be boolean")
        require(type(machine.get("price")) is int
                and 0 <= machine["price"] <= SQL_INT_MAX,
                "Fixed price must be a nonnegative integer")
        require(machine.get("currency") == "GP", "Runtime currently supports GP fixed machines")
        policy = machine.get("draw_policy")
        require(policy in ("native_sequence", "independent_weighted"),
                "Unsupported fixed draw policy")
        owner = machine.get("owner")
        require(isinstance(owner, str) and owner, "Fixed machine owner is required")
        if owner != "native":
            require(policy == "independent_weighted",
                    "New fixed machines require independent_weighted")
        slots = machine.get("slots")
        require(isinstance(slots, list) and len(slots) == 6,
                "Fixed machines require six slots")
        seen = set()
        total = 0
        for slot in slots:
            unit_id = slot.get("unit_id")
            weight = slot.get("weight")
            group = slot.get("native_draw_group")
            require(type(unit_id) is int and unit_id in unit_ids and unit_id not in seen,
                    "Fixed slot has a duplicate or unknown unit")
            require(type(weight) is int and weight > 0, "Fixed weight must be positive")
            require(type(group) is int and 0 <= group <= 255,
                    "Native draw group must fit TINYINT")
            seen.add(unit_id)
            total += weight
        require(total == 1000, "Fixed weights must total 1000")

    custom_ids = set()
    for machine in custom:
        machine_id = machine.get("id")
        require(type(machine_id) is int and 0 < machine_id <= SQL_INT_MAX
                and machine_id not in custom_ids,
                "Invalid or duplicate custom machine ID")
        custom_ids.add(machine_id)
        require(type(machine.get("enabled")) is bool, "Custom enabled must be boolean")
        require(isinstance(machine.get("owner"), str) and machine["owner"],
                "Custom machine owner is required")
        require(type(machine.get("price")) is int
                and 0 <= machine["price"] <= SQL_INT_MAX,
                "Custom price must be a nonnegative integer")
        require(machine.get("currency_type") in (0, 1),
                "Custom currency type must be 0 (GP) or 1 (coin)")
        require(isinstance(machine.get("name"), str)
                and 0 < len(machine["name"]) <= 100,
                "Custom machine title must contain 1 to 100 characters")
        require(machine.get("draw_policy") == "native_mileage",
                "Custom machines must retain native_mileage")
        rules = machine.get("slot_rules")
        require(isinstance(rules, list) and len(rules) == 8,
                "Custom machines require eight slot rules")
        for expected, rule in enumerate(rules, 1):
            require(rule.get("slot") == expected, "Custom slot rules must be ordered")
            require(type(rule.get("rank_max")) is int and 1 <= rule["rank_max"] <= 4,
                    "Custom rank maximum is invalid")
            subgrades = tuple(rule.get("subgrades", ()))
            require(subgrades in GRADE_SET_INDEX,
                    "Custom subgrade set has no native Grade code")
            require(bool(rule.get("extension")) == (expected >= 7),
                    "Only custom slots 7 and 8 are extension slots")
        candidates = machine.get("candidates")
        require(isinstance(candidates, list)
                and (not machine["enabled"] or len(candidates) > 0),
                "Enabled custom machines require candidates")
        seen = set()
        for candidate in candidates:
            unit_id = candidate.get("unit_id")
            require(type(unit_id) is int and unit_id in unit_ids and unit_id not in seen,
                    "Custom candidate has a duplicate or unknown unit")
            seen.add(unit_id)
            require(type(candidate.get("basic_weight")) is int
                    and 0 < candidate["basic_weight"] <= SQL_SMALLINT_MAX,
                    "Custom basic weight must fit positive SQL SMALLINT")
            mileage = candidate.get("mileage")
            require(isinstance(mileage, (int, float)) and math.isfinite(mileage)
                    and mileage >= 0, "Custom mileage is invalid")
            require(type(candidate.get("extra_price")) is int
                    and 0 <= candidate["extra_price"] <= SQL_INT_MAX,
                    "Custom extra price is invalid")
            require(type(candidate.get("reset_mileage")) is bool,
                    "Custom mileage reset flag must be boolean")
            unit = unit_by_id[unit_id]
            require(any(unit.get("rank") <= rule["rank_max"]
                        and unit.get("subgrade") in rule["subgrades"]
                        for rule in rules),
                    "Custom candidate has no permitted native slot")
    return bundle


def verify_input_manifest(bundle_path, bundle):
    path = Path(bundle_path)
    manifest_path = path.with_name("manifest.json")
    if not manifest_path.exists():
        return None
    manifest = read_json(manifest_path)
    require(manifest.get("catalog_revision") == bundle["catalog_revision"],
            "Bundle manifest revision differs")
    expected = manifest.get("files", {}).get(path.name)
    require(expected == sha256(path.read_bytes()), "Server bundle hash differs from manifest")
    return manifest


def sql_text(value):
    return "N'" + str(value).replace("'", "''") + "'"


def sql_float(value):
    value = float(value)
    require(math.isfinite(value), "Non-finite SQL number")
    return format(value, ".17g")


def insert_rows(table, columns, rows, chunk_size=500):
    if not rows:
        return ""
    output = []
    for offset in range(0, len(rows), chunk_size):
        chunk = rows[offset:offset + chunk_size]
        output.append(
            f"INSERT INTO {table} ({', '.join(columns)}) VALUES\n  "
            + ",\n  ".join("(" + ", ".join(row) + ")" for row in chunk)
            + ";\n"
        )
    return "".join(output)


def stage_sql(bundle):
    fixed_rows = []
    fixed_slots = []
    native_slots = []
    for machine in bundle["fixed_machines"]:
        policy = 1 if machine["draw_policy"] == "independent_weighted" else 0
        fixed_rows.append((
            str(machine["id"]),
            "1" if machine["enabled"] else "0",
            str(machine["price"]),
            "1" if machine["owner"] == "native" else "0",
            str(policy),
        ))
        lower = 0
        for ordinal, slot in enumerate(machine["slots"], 1):
            if policy:
                upper = lower + slot["weight"]
                fixed_slots.append((
                    str(machine["id"]), str(ordinal), str(slot["unit_id"]),
                    str(slot["weight"]), str(lower), str(upper),
                    str(slot["native_draw_group"]),
                ))
                lower = upper
            else:
                native_slots.append((
                    str(machine["id"]), str(ordinal), str(slot["unit_id"]),
                    str(slot["weight"]), str(slot["native_draw_group"]),
                ))

    custom_rows = []
    candidate_rows = []
    for machine in bundle["custom_machines"]:
        rules = machine["slot_rules"]
        row = [
            str(machine["id"]),
            "1" if machine["enabled"] else "0",
            "1" if machine["owner"] == "native" else "0",
            str(machine["currency_type"]),
            str(machine["price"]),
            sql_text(machine["name"]),
        ]
        for rule in rules:
            row.extend((str(rule["rank_max"]),
                        str(GRADE_SET_INDEX[tuple(rule["subgrades"])])))
        custom_rows.append(tuple(row))
        for candidate in machine["candidates"]:
            candidate_rows.append((
                str(machine["id"]), str(candidate["unit_id"]),
                str(candidate["basic_weight"]), sql_float(candidate["mileage"]),
                str(candidate["extra_price"]),
                "1" if candidate["reset_mileage"] else "0",
            ))

    custom_columns = [
        "MachineID", "Enabled", "OwnerNative", "CostType", "BasicCost", "MachineTitle"
    ]
    for index in range(1, 9):
        custom_columns.extend((f"Rank{index}", f"Grade{index}"))
    return f"""\
IF OBJECT_ID(N'tempdb..#Candidate', N'U') IS NOT NULL DROP TABLE #Candidate;
IF OBJECT_ID(N'tempdb..#Custom', N'U') IS NOT NULL DROP TABLE #Custom;
IF OBJECT_ID(N'tempdb..#NativeSlot', N'U') IS NOT NULL DROP TABLE #NativeSlot;
IF OBJECT_ID(N'tempdb..#FixedSlot', N'U') IS NOT NULL DROP TABLE #FixedSlot;
IF OBJECT_ID(N'tempdb..#Fixed', N'U') IS NOT NULL DROP TABLE #Fixed;
IF OBJECT_ID(N'tempdb..#CaptureCustom', N'U') IS NOT NULL DROP TABLE #CaptureCustom;
CREATE TABLE #Fixed (
  MachineID INT NOT NULL PRIMARY KEY,
  Enabled BIT NOT NULL,
  Price INT NOT NULL,
  OwnerNative BIT NOT NULL,
  DrawPolicy TINYINT NOT NULL
);
CREATE TABLE #FixedSlot (
  MachineID INT NOT NULL,
  SlotOrdinal TINYINT NOT NULL,
  UnitID INT NOT NULL,
  Weight INT NOT NULL,
  LowerBound INT NOT NULL,
  UpperBound INT NOT NULL,
  NativeDrawGroup TINYINT NOT NULL,
  PRIMARY KEY (MachineID, SlotOrdinal)
);
CREATE TABLE #NativeSlot (
  MachineID INT NOT NULL,
  SlotOrdinal TINYINT NOT NULL,
  UnitID INT NOT NULL,
  Weight INT NOT NULL,
  NativeDrawGroup TINYINT NOT NULL,
  PRIMARY KEY (MachineID, SlotOrdinal)
);
CREATE TABLE #Custom (
  MachineID INT NOT NULL PRIMARY KEY,
  Enabled BIT NOT NULL,
  OwnerNative BIT NOT NULL,
  CostType TINYINT NOT NULL,
  BasicCost INT NOT NULL,
  MachineTitle VARCHAR(100) COLLATE DATABASE_DEFAULT NOT NULL,
  Rank1 TINYINT NOT NULL, Grade1 TINYINT NOT NULL,
  Rank2 TINYINT NOT NULL, Grade2 TINYINT NOT NULL,
  Rank3 TINYINT NOT NULL, Grade3 TINYINT NOT NULL,
  Rank4 TINYINT NOT NULL, Grade4 TINYINT NOT NULL,
  Rank5 TINYINT NOT NULL, Grade5 TINYINT NOT NULL,
  Rank6 TINYINT NOT NULL, Grade6 TINYINT NOT NULL,
  Rank7 TINYINT NOT NULL, Grade7 TINYINT NOT NULL,
  Rank8 TINYINT NOT NULL, Grade8 TINYINT NOT NULL
);
CREATE TABLE #Candidate (
  MachineID INT NOT NULL,
  UnitID INT NOT NULL,
  BasicProb INT NOT NULL,
  Mileage FLOAT NOT NULL,
  AdditionalCost INT NOT NULL,
  MileageResetFlag BIT NOT NULL,
  PRIMARY KEY (MachineID, UnitID)
);
CREATE TABLE #CaptureCustom (MachineID INT NOT NULL PRIMARY KEY);
{insert_rows("#Fixed", ["MachineID", "Enabled", "Price", "OwnerNative", "DrawPolicy"], fixed_rows)}
{insert_rows("#FixedSlot", ["MachineID", "SlotOrdinal", "UnitID", "Weight",
                            "LowerBound", "UpperBound", "NativeDrawGroup"], fixed_slots)}
{insert_rows("#NativeSlot", ["MachineID", "SlotOrdinal", "UnitID", "Weight",
                              "NativeDrawGroup"], native_slots)}
{insert_rows("#Custom", custom_columns, custom_rows)}
{insert_rows("#Candidate", ["MachineID", "UnitID", "BasicProb", "Mileage",
                             "AdditionalCost", "MileageResetFlag"], candidate_rows)}
"""


WRAPPER_SQL = r"""
CREATE PROCEDURE dbo.sp_GetNextCapsule1
  @v_AccountNum INT,
  @v_MachineID INT,
  @v_OutputUnitID INT OUTPUT,
  @v_OutputUnitGradeNo TINYINT OUTPUT
AS
BEGIN
  SET NOCOUNT ON;
  DECLARE @Enabled BIT;
  SELECT @Enabled = Enabled
    FROM dbo.sdgo_mod_CapsuleMachine WITH (READCOMMITTEDLOCK)
    WHERE MachineID = @v_MachineID;

  IF @Enabled IS NULL
  BEGIN
    DECLARE @NativeReturn INT;
    EXEC @NativeReturn = dbo.sdgo_native_sp_GetNextCapsule1
      @v_AccountNum, @v_MachineID,
      @v_OutputUnitID OUTPUT, @v_OutputUnitGradeNo OUTPUT;
    RETURN @NativeReturn;
  END;

  IF @Enabled = 0
    RETURN 3002;

  DECLARE @RandomBytes VARBINARY(4);
  DECLARE @RandomValue BIGINT;
  DECLARE @Roll INT;
  WHILE 1 = 1
  BEGIN
    SET @RandomBytes = CRYPT_GEN_RANDOM(4);
    SET @RandomValue = CONVERT(BIGINT, @RandomBytes);
    IF @RandomValue < CONVERT(BIGINT, 4294967000)
      BREAK;
  END;
  SET @Roll = CONVERT(INT, @RandomValue % 1000);

  SELECT @v_OutputUnitID = UnitID,
         @v_OutputUnitGradeNo = NativeDrawGroup
    FROM dbo.sdgo_mod_CapsuleSlot WITH (READCOMMITTEDLOCK)
    WHERE MachineID = @v_MachineID
      AND @Roll >= LowerBound
      AND @Roll < UpperBound;

  IF @@ROWCOUNT <> 1
    RETURN 3002;
  RETURN 0;
END
""".strip()


def dynamic_exec(sql):
    return "EXEC(N'" + sql.replace("'", "''") + "');"


def common_header(bundle, purpose):
    return f"""\
-- SDGO capsule runtime adapter: {purpose}
-- Catalog revision: {bundle["catalog_revision"]}
-- Runtime host evidence: WorldServer.exe SHA-256 {WORLD_SERVER_SHA256}
-- Generated offline. This script reads/writes configuration tables only.
-- It never queries player inventory, account, mileage, or draw-history tables.
SET NOCOUNT ON;
SET XACT_ABORT ON;
"""


def install_sql(bundle):
    revision = bundle["catalog_revision"]
    wrapper_exec = dynamic_exec(WRAPPER_SQL)
    return common_header(bundle, "install or update") + stage_sql(bundle) + f"""
BEGIN TRY
  BEGIN TRANSACTION;

  IF OBJECT_ID(N'dbo.tbl_MachinePrice', N'U') IS NULL
     OR OBJECT_ID(N'dbo.tbl_CoinMachinePrice', N'U') IS NULL
     OR OBJECT_ID(N'dbo.tbl_MachineDice', N'U') IS NULL
     OR OBJECT_ID(N'dbo.tbl_CustomCapsuleList', N'U') IS NULL
     OR OBJECT_ID(N'dbo.tbl_CustomCapsuleInfo', N'U') IS NULL
     OR OBJECT_ID(N'dbo.tbl_Units', N'U') IS NULL
     OR (OBJECT_ID(N'dbo.sp_GetNextCapsule1', N'P') IS NULL
         AND OBJECT_ID(N'dbo.sdgo_native_sp_GetNextCapsule1', N'P') IS NULL)
    RAISERROR(N'Capsule runtime prerequisites are missing', 16, 1);

  IF EXISTS (
    SELECT 1 FROM #Fixed f
    WHERE f.OwnerNative = 1
      AND NOT EXISTS (SELECT 1 FROM dbo.tbl_MachinePrice p WHERE p.MachineID = f.MachineID)
  )
    RAISERROR(N'A native fixed machine is missing from tbl_MachinePrice', 16, 1);

  IF EXISTS (
    SELECT 1
      FROM #NativeSlot e
      FULL OUTER JOIN (
        SELECT d.MachineID,
               ROW_NUMBER() OVER (PARTITION BY d.MachineID ORDER BY d.Ident) AS SlotOrdinal,
               d.UnitID, d.Prob1 AS Weight,
               d.UnitGradeNumber AS NativeDrawGroup
          FROM dbo.tbl_MachineDice d
          JOIN #Fixed f ON f.MachineID=d.MachineID AND f.DrawPolicy=0
      ) n ON n.MachineID=e.MachineID AND n.SlotOrdinal=e.SlotOrdinal
      WHERE e.MachineID IS NULL OR n.MachineID IS NULL
         OR n.UnitID<>e.UnitID OR n.Weight<>e.Weight
         OR n.NativeDrawGroup<>e.NativeDrawGroup
  )
    RAISERROR(
      N'Native-sequence slots differ from tbl_MachineDice; promote the machine to independent_weighted',
      16, 1
    );

  IF EXISTS (
    SELECT 1 FROM #FixedSlot s
    LEFT JOIN dbo.tbl_Units u ON u.UnitNumber=s.UnitID
    WHERE u.UnitNumber IS NULL
  ) OR EXISTS (
    SELECT 1 FROM #NativeSlot s
    LEFT JOIN dbo.tbl_Units u ON u.UnitNumber=s.UnitID
    WHERE u.UnitNumber IS NULL
  ) OR EXISTS (
    SELECT 1 FROM #Candidate c
    LEFT JOIN dbo.tbl_Units u ON u.UnitNumber=c.UnitID
    WHERE u.UnitNumber IS NULL
  )
    RAISERROR(N'A configured capsule unit is missing from tbl_Units', 16, 1);

  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleRuntimeState', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleRuntimeState (
      Singleton TINYINT NOT NULL PRIMARY KEY,
      CatalogRevision CHAR(64) NOT NULL,
      InstalledUtc DATETIME NOT NULL,
      CONSTRAINT CK_sdgo_mod_CapsuleRuntimeState_Singleton CHECK (Singleton = 1)
    );
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleMachine', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleMachine (
      MachineID INT NOT NULL PRIMARY KEY,
      CatalogRevision CHAR(64) NOT NULL,
      Enabled BIT NOT NULL,
      Price INT NOT NULL,
      TotalWeight INT NOT NULL,
      CONSTRAINT CK_sdgo_mod_CapsuleMachine_Weight CHECK (TotalWeight = 1000)
    );
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleSlot', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleSlot (
      MachineID INT NOT NULL,
      SlotOrdinal TINYINT NOT NULL,
      UnitID INT NOT NULL,
      Weight INT NOT NULL,
      LowerBound INT NOT NULL,
      UpperBound INT NOT NULL,
      NativeDrawGroup TINYINT NOT NULL,
      CONSTRAINT PK_sdgo_mod_CapsuleSlot PRIMARY KEY (MachineID, SlotOrdinal),
      CONSTRAINT FK_sdgo_mod_CapsuleSlot_Machine FOREIGN KEY (MachineID)
        REFERENCES dbo.sdgo_mod_CapsuleMachine(MachineID),
      CONSTRAINT CK_sdgo_mod_CapsuleSlot_Bounds
        CHECK (Weight > 0 AND LowerBound >= 0 AND UpperBound <= 1000
               AND UpperBound - LowerBound = Weight)
    );
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleCustomMachine', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleCustomMachine (
      MachineID INT NOT NULL PRIMARY KEY,
      CatalogRevision CHAR(64) NOT NULL,
      Enabled BIT NOT NULL
    );
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineMachinePrice', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleBaselineMachinePrice (
      MachineID INT NOT NULL PRIMARY KEY,
      Existed BIT NOT NULL,
      Price INT NULL,
      Disable BIT NULL,
      Type INT NULL
    );
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineCoinState', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleBaselineCoinState (
      GPMachineID INT NOT NULL PRIMARY KEY,
      CoinMachineID INT NOT NULL,
      OriginalDisable BIT NOT NULL
    );
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineCustomCaptured', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleBaselineCustomCaptured (
      MachineID INT NOT NULL PRIMARY KEY
    );
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineCustomList', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleBaselineCustomList (
      MachineID INT NOT NULL PRIMARY KEY,
      CostType INT NOT NULL,
      BasicCost INT NOT NULL,
      MachineTitle VARCHAR(100) NOT NULL,
      Rank1 INT NOT NULL, Grade1 INT NOT NULL,
      Rank2 INT NOT NULL, Grade2 INT NOT NULL,
      Rank3 INT NOT NULL, Grade3 INT NOT NULL,
      Rank4 INT NOT NULL, Grade4 INT NOT NULL,
      Rank5 INT NOT NULL, Grade5 INT NOT NULL,
      Rank6 INT NOT NULL, Grade6 INT NOT NULL,
      Rank7 INT NOT NULL, Grade7 INT NOT NULL,
      Rank8 INT NOT NULL, Grade8 INT NOT NULL
    );
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineCustomInfo', N'U') IS NULL
    CREATE TABLE dbo.sdgo_mod_CapsuleBaselineCustomInfo (
      BaselineOrdinal INT IDENTITY(1,1) NOT NULL PRIMARY KEY,
      MachineID INT NOT NULL,
      UnitID INT NOT NULL,
      BasicProb INT NOT NULL,
      Mileage FLOAT NOT NULL,
      AdditionalCost INT NOT NULL,
      MileageResetFlag BIT NOT NULL
    );

  IF EXISTS (
    SELECT 1
      FROM #Fixed f
      JOIN dbo.tbl_MachinePrice p ON p.MachineID=f.MachineID
      LEFT JOIN dbo.sdgo_mod_CapsuleBaselineMachinePrice b
        ON b.MachineID=f.MachineID
      WHERE f.OwnerNative=0 AND (b.MachineID IS NULL OR b.Existed=1)
  )
    RAISERROR(N'A new fixed machine ID collides with an unowned native row',16,1);

  IF EXISTS (
    SELECT 1
      FROM #Fixed f
      JOIN dbo.tbl_CoinMachinePrice c ON c.MachineID=f.MachineID+1
      WHERE f.OwnerNative=0
  )
    RAISERROR(N'A new GP machine ID collides with an existing coin variant',16,1);

  IF EXISTS (
    SELECT 1
      FROM #Custom c
      WHERE c.OwnerNative=1
        AND NOT EXISTS (
          SELECT 1 FROM dbo.tbl_CustomCapsuleList n
          WHERE n.MachineID=c.MachineID
        )
        AND NOT EXISTS (
          SELECT 1 FROM dbo.sdgo_mod_CapsuleBaselineCustomList b
          WHERE b.MachineID=c.MachineID
        )
  )
    RAISERROR(N'A native custom machine has no native configuration to capture',16,1);

  IF EXISTS (
    SELECT 1
      FROM #Custom c
      WHERE c.OwnerNative=0
        AND (
          EXISTS (
            SELECT 1 FROM dbo.tbl_CustomCapsuleList n
            WHERE n.MachineID=c.MachineID
          )
          OR EXISTS (
            SELECT 1 FROM dbo.tbl_CustomCapsuleInfo i
            WHERE i.MachineID=c.MachineID
          )
        )
        AND NOT EXISTS (
          SELECT 1 FROM dbo.sdgo_mod_CapsuleBaselineCustomCaptured b
          WHERE b.MachineID=c.MachineID
        )
  )
    RAISERROR(N'A new custom machine ID collides with unowned native configuration',16,1);

  IF EXISTS (
    SELECT 1
      FROM dbo.sdgo_mod_CapsuleCustomMachine m
      LEFT JOIN #Custom c ON c.MachineID=m.MachineID
      WHERE c.MachineID IS NULL
  )
    RAISERROR(
      N'An adapter-managed custom machine is missing; include it with Enabled=0 to delete it',
      16, 1
    );

  INSERT INTO dbo.sdgo_mod_CapsuleBaselineMachinePrice
    (MachineID, Existed, Price, Disable, Type)
  SELECT f.MachineID, 1, p.Price, p.Disable, p.Type
    FROM #Fixed f
    JOIN dbo.tbl_MachinePrice p ON p.MachineID = f.MachineID
    WHERE NOT EXISTS (
      SELECT 1 FROM dbo.sdgo_mod_CapsuleBaselineMachinePrice b
      WHERE b.MachineID = f.MachineID
    );
  INSERT INTO dbo.sdgo_mod_CapsuleBaselineMachinePrice
    (MachineID, Existed, Price, Disable, Type)
  SELECT f.MachineID, 0, NULL, NULL, NULL
    FROM #Fixed f
    WHERE NOT EXISTS (SELECT 1 FROM dbo.tbl_MachinePrice p WHERE p.MachineID = f.MachineID)
      AND NOT EXISTS (
        SELECT 1 FROM dbo.sdgo_mod_CapsuleBaselineMachinePrice b
        WHERE b.MachineID = f.MachineID
      );

  INSERT INTO dbo.sdgo_mod_CapsuleBaselineCoinState
    (GPMachineID, CoinMachineID, OriginalDisable)
  SELECT f.MachineID, c.MachineID, c.Disable
    FROM #Fixed f
    JOIN dbo.tbl_CoinMachinePrice c ON c.MachineID = f.MachineID + 1
    WHERE f.OwnerNative = 1
      AND NOT EXISTS (
        SELECT 1 FROM dbo.sdgo_mod_CapsuleBaselineCoinState b
        WHERE b.GPMachineID = f.MachineID
      );

  INSERT INTO #CaptureCustom (MachineID)
  SELECT c.MachineID FROM #Custom c
  WHERE NOT EXISTS (
    SELECT 1 FROM dbo.sdgo_mod_CapsuleBaselineCustomCaptured b
    WHERE b.MachineID = c.MachineID
  );
  INSERT INTO dbo.sdgo_mod_CapsuleBaselineCustomList
    (MachineID, CostType, BasicCost, MachineTitle,
     Rank1, Grade1, Rank2, Grade2, Rank3, Grade3, Rank4, Grade4,
     Rank5, Grade5, Rank6, Grade6, Rank7, Grade7, Rank8, Grade8)
  SELECT n.MachineID, n.CostType, n.BasicCost, n.MachineTitle,
         n.Rank1, n.Grade1, n.Rank2, n.Grade2, n.Rank3, n.Grade3,
         n.Rank4, n.Grade4, n.Rank5, n.Grade5, n.Rank6, n.Grade6,
         n.Rank7, n.Grade7, n.Rank8, n.Grade8
    FROM dbo.tbl_CustomCapsuleList n
    JOIN #CaptureCustom c ON c.MachineID = n.MachineID;
  INSERT INTO dbo.sdgo_mod_CapsuleBaselineCustomInfo
    (MachineID, UnitID, BasicProb, Mileage, AdditionalCost, MileageResetFlag)
  SELECT n.MachineID, n.UnitID, n.BasicProb, n.Mileage,
         n.AdditionalCost, n.MileageResetFlag
    FROM dbo.tbl_CustomCapsuleInfo n
    JOIN #CaptureCustom c ON c.MachineID = n.MachineID;
  INSERT INTO dbo.sdgo_mod_CapsuleBaselineCustomCaptured (MachineID)
  SELECT MachineID FROM #CaptureCustom;

  IF OBJECT_ID(N'dbo.sdgo_native_sp_GetNextCapsule1', N'P') IS NULL
  BEGIN
    IF OBJECT_ID(N'dbo.sp_GetNextCapsule1', N'P') IS NULL
      RAISERROR(N'Native sp_GetNextCapsule1 is missing', 16, 1);
    EXEC sys.sp_rename N'dbo.sp_GetNextCapsule1',
                       N'sdgo_native_sp_GetNextCapsule1', N'OBJECT';
  END
  ELSE IF OBJECT_ID(N'dbo.sp_GetNextCapsule1', N'P') IS NOT NULL
  BEGIN
    IF NOT EXISTS (
      SELECT 1 FROM fn_listextendedproperty(
        N'SDGO_CAPSULE_RUNTIME_WRAPPER', N'SCHEMA', N'dbo',
        N'PROCEDURE', N'sp_GetNextCapsule1', NULL, NULL
      ) WHERE CONVERT(NVARCHAR(20), value) = N'1'
    )
      RAISERROR(N'Existing sp_GetNextCapsule1 is not owned by this adapter', 16, 1);
    DROP PROCEDURE dbo.sp_GetNextCapsule1;
  END;
  {wrapper_exec}
  EXEC sys.sp_addextendedproperty
    @name=N'SDGO_CAPSULE_RUNTIME_WRAPPER', @value=N'1',
    @level0type=N'SCHEMA', @level0name=N'dbo',
    @level1type=N'PROCEDURE', @level1name=N'sp_GetNextCapsule1';

  UPDATE p
    SET p.Price = f.Price,
        p.Disable = CASE WHEN f.Enabled = 1 THEN 0 ELSE 1 END
    FROM dbo.tbl_MachinePrice p
    JOIN #Fixed f ON f.MachineID = p.MachineID;
  INSERT INTO dbo.tbl_MachinePrice (MachineID, Price, Disable, Type)
  SELECT f.MachineID, f.Price, CASE WHEN f.Enabled = 1 THEN 0 ELSE 1 END, 0
    FROM #Fixed f
    WHERE f.OwnerNative = 0
      AND NOT EXISTS (
        SELECT 1 FROM dbo.tbl_MachinePrice p WHERE p.MachineID = f.MachineID
      );

  UPDATE c
    SET c.Disable = CASE WHEN f.Enabled = 0 THEN 1 ELSE b.OriginalDisable END
    FROM dbo.tbl_CoinMachinePrice c
    JOIN dbo.sdgo_mod_CapsuleBaselineCoinState b ON b.CoinMachineID = c.MachineID
    JOIN #Fixed f ON f.MachineID = b.GPMachineID
    WHERE f.OwnerNative = 1;

  UPDATE m
    SET m.CatalogRevision = '{revision}', m.Enabled = f.Enabled,
        m.Price = f.Price, m.TotalWeight = 1000
    FROM dbo.sdgo_mod_CapsuleMachine m
    JOIN #Fixed f ON f.MachineID = m.MachineID
    WHERE f.DrawPolicy = 1;
  INSERT INTO dbo.sdgo_mod_CapsuleMachine
    (MachineID, CatalogRevision, Enabled, Price, TotalWeight)
  SELECT f.MachineID, '{revision}', f.Enabled, f.Price, 1000
    FROM #Fixed f
    WHERE f.DrawPolicy = 1
      AND NOT EXISTS (
        SELECT 1 FROM dbo.sdgo_mod_CapsuleMachine m
        WHERE m.MachineID = f.MachineID
      );
  DELETE s
    FROM dbo.sdgo_mod_CapsuleSlot s
    JOIN #Fixed f ON f.MachineID = s.MachineID
    WHERE f.DrawPolicy = 1;
  INSERT INTO dbo.sdgo_mod_CapsuleSlot
    (MachineID, SlotOrdinal, UnitID, Weight,
     LowerBound, UpperBound, NativeDrawGroup)
  SELECT MachineID, SlotOrdinal, UnitID, Weight,
         LowerBound, UpperBound, NativeDrawGroup
    FROM #FixedSlot;

  DELETE s
    FROM dbo.sdgo_mod_CapsuleSlot s
    JOIN #Fixed f ON f.MachineID = s.MachineID
    WHERE f.DrawPolicy = 0;
  DELETE m
    FROM dbo.sdgo_mod_CapsuleMachine m
    JOIN #Fixed f ON f.MachineID = m.MachineID
    WHERE f.DrawPolicy = 0;
  UPDATE m SET Enabled = 0
    FROM dbo.sdgo_mod_CapsuleMachine m
    WHERE NOT EXISTS (
      SELECT 1 FROM #Fixed f
      WHERE f.MachineID = m.MachineID AND f.DrawPolicy = 1
    );
  UPDATE p SET Disable = 1
    FROM dbo.tbl_MachinePrice p
    JOIN dbo.sdgo_mod_CapsuleMachine m ON m.MachineID = p.MachineID
    WHERE m.Enabled = 0;

  DELETE n
    FROM dbo.tbl_CustomCapsuleInfo n
    JOIN #Custom c ON c.MachineID = n.MachineID;
  DELETE n
    FROM dbo.tbl_CustomCapsuleList n
    JOIN #Custom c ON c.MachineID = n.MachineID
    WHERE c.Enabled = 0;
  UPDATE n
    SET n.CostType=c.CostType, n.BasicCost=c.BasicCost,
        n.MachineTitle=c.MachineTitle,
        n.Rank1=c.Rank1, n.Grade1=c.Grade1,
        n.Rank2=c.Rank2, n.Grade2=c.Grade2,
        n.Rank3=c.Rank3, n.Grade3=c.Grade3,
        n.Rank4=c.Rank4, n.Grade4=c.Grade4,
        n.Rank5=c.Rank5, n.Grade5=c.Grade5,
        n.Rank6=c.Rank6, n.Grade6=c.Grade6,
        n.Rank7=c.Rank7, n.Grade7=c.Grade7,
        n.Rank8=c.Rank8, n.Grade8=c.Grade8
    FROM dbo.tbl_CustomCapsuleList n
    JOIN #Custom c ON c.MachineID=n.MachineID
    WHERE c.Enabled=1;
  INSERT INTO dbo.tbl_CustomCapsuleList
    (MachineID, CostType, BasicCost, MachineTitle,
     Rank1, Grade1, Rank2, Grade2, Rank3, Grade3, Rank4, Grade4,
     Rank5, Grade5, Rank6, Grade6, Rank7, Grade7, Rank8, Grade8)
  SELECT c.MachineID, c.CostType, c.BasicCost, c.MachineTitle,
         c.Rank1, c.Grade1, c.Rank2, c.Grade2, c.Rank3, c.Grade3,
         c.Rank4, c.Grade4, c.Rank5, c.Grade5, c.Rank6, c.Grade6,
         c.Rank7, c.Grade7, c.Rank8, c.Grade8
    FROM #Custom c
    WHERE c.Enabled=1
      AND NOT EXISTS (
        SELECT 1 FROM dbo.tbl_CustomCapsuleList n WHERE n.MachineID=c.MachineID
      );
  INSERT INTO dbo.tbl_CustomCapsuleInfo
    (MachineID, UnitID, BasicProb, Mileage, AdditionalCost, MileageResetFlag)
  SELECT x.MachineID, x.UnitID, x.BasicProb, x.Mileage,
         x.AdditionalCost, x.MileageResetFlag
    FROM #Candidate x
    JOIN #Custom c ON c.MachineID=x.MachineID
    WHERE c.Enabled=1;

  UPDATE m
    SET m.CatalogRevision='{revision}', m.Enabled=c.Enabled
    FROM dbo.sdgo_mod_CapsuleCustomMachine m
    JOIN #Custom c ON c.MachineID=m.MachineID;
  INSERT INTO dbo.sdgo_mod_CapsuleCustomMachine
    (MachineID, CatalogRevision, Enabled)
  SELECT c.MachineID, '{revision}', c.Enabled
    FROM #Custom c
    WHERE NOT EXISTS (
      SELECT 1 FROM dbo.sdgo_mod_CapsuleCustomMachine m
      WHERE m.MachineID=c.MachineID
    );

  IF EXISTS (SELECT 1 FROM dbo.sdgo_mod_CapsuleRuntimeState WHERE Singleton=1)
    UPDATE dbo.sdgo_mod_CapsuleRuntimeState
      SET CatalogRevision='{revision}', InstalledUtc=GETUTCDATE()
      WHERE Singleton=1;
  ELSE
    INSERT INTO dbo.sdgo_mod_CapsuleRuntimeState
      (Singleton, CatalogRevision, InstalledUtc)
      VALUES (1, '{revision}', GETUTCDATE());

  COMMIT TRANSACTION;
END TRY
BEGIN CATCH
  IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION;
  DECLARE @ErrorMessage NVARCHAR(4000);
  SET @ErrorMessage = ERROR_MESSAGE();
  RAISERROR(@ErrorMessage, 16, 1);
  RETURN;
END CATCH;

SELECT CatalogRevision, InstalledUtc,
       (SELECT COUNT(*) FROM dbo.sdgo_mod_CapsuleMachine) AS WeightedMachines,
       (SELECT COUNT(*) FROM dbo.sdgo_mod_CapsuleCustomMachine) AS CustomMachines
  FROM dbo.sdgo_mod_CapsuleRuntimeState WHERE Singleton=1;
"""


def verify_sql(bundle):
    revision = bundle["catalog_revision"]
    return common_header(bundle, "read-only verification") + stage_sql(bundle) + f"""
BEGIN TRY
  IF OBJECT_ID(N'dbo.sdgo_native_sp_GetNextCapsule1', N'P') IS NULL
     OR OBJECT_ID(N'dbo.sp_GetNextCapsule1', N'P') IS NULL
    RAISERROR(N'Runtime procedure pair is incomplete', 16, 1);
  IF NOT EXISTS (
    SELECT 1 FROM fn_listextendedproperty(
      N'SDGO_CAPSULE_RUNTIME_WRAPPER', N'SCHEMA', N'dbo',
      N'PROCEDURE', N'sp_GetNextCapsule1', NULL, NULL
    ) WHERE CONVERT(NVARCHAR(20), value)=N'1'
  )
    RAISERROR(N'Runtime wrapper ownership marker is missing', 16, 1);
  IF NOT EXISTS (
    SELECT 1 FROM dbo.sdgo_mod_CapsuleRuntimeState
    WHERE Singleton=1 AND CatalogRevision='{revision}'
  )
    RAISERROR(N'Installed catalog revision differs', 16, 1);
  IF EXISTS (
    SELECT 1 FROM #Fixed f
    LEFT JOIN dbo.tbl_MachinePrice p ON p.MachineID=f.MachineID
    WHERE p.MachineID IS NULL OR p.Price<>f.Price
       OR CONVERT(INT,p.Disable)<>CASE WHEN f.Enabled=1 THEN 0 ELSE 1 END
  )
    RAISERROR(N'Fixed machine price or enabled state differs', 16, 1);
  IF EXISTS (
    SELECT 1 FROM #Fixed f
    JOIN dbo.tbl_CoinMachinePrice c ON c.MachineID=f.MachineID+1
    WHERE f.OwnerNative=0
  )
    RAISERROR(N'A new GP machine unexpectedly has a coin variant', 16, 1);
  IF EXISTS (
    SELECT 1 FROM #Fixed f
    JOIN dbo.sdgo_mod_CapsuleBaselineCoinState b ON b.GPMachineID=f.MachineID
    JOIN dbo.tbl_CoinMachinePrice c ON c.MachineID=b.CoinMachineID
    WHERE CONVERT(INT,c.Disable)<>
      CASE WHEN f.Enabled=0 THEN 1 ELSE CONVERT(INT,b.OriginalDisable) END
  )
    RAISERROR(N'Existing coin variant state differs from physical-machine policy',16,1);
  IF EXISTS (
    SELECT 1 FROM #Fixed f
    LEFT JOIN dbo.sdgo_mod_CapsuleMachine m ON m.MachineID=f.MachineID
    WHERE f.DrawPolicy=1 AND
      (m.MachineID IS NULL OR m.CatalogRevision<>'{revision}'
       OR m.Enabled<>f.Enabled OR m.TotalWeight<>1000)
  )
    RAISERROR(N'Weighted machine metadata differs', 16, 1);
  IF EXISTS (
    SELECT 1 FROM #FixedSlot e
    LEFT JOIN dbo.sdgo_mod_CapsuleSlot a
      ON a.MachineID=e.MachineID AND a.SlotOrdinal=e.SlotOrdinal
    WHERE a.MachineID IS NULL OR a.UnitID<>e.UnitID OR a.Weight<>e.Weight
       OR a.LowerBound<>e.LowerBound OR a.UpperBound<>e.UpperBound
       OR a.NativeDrawGroup<>e.NativeDrawGroup
  ) OR EXISTS (
    SELECT 1 FROM dbo.sdgo_mod_CapsuleSlot a
    JOIN #Fixed f ON f.MachineID=a.MachineID AND f.DrawPolicy=1
    LEFT JOIN #FixedSlot e
      ON e.MachineID=a.MachineID AND e.SlotOrdinal=a.SlotOrdinal
    WHERE e.MachineID IS NULL
  )
    RAISERROR(N'Weighted machine slots differ', 16, 1);
  IF EXISTS (
    SELECT 1 FROM dbo.sdgo_mod_CapsuleSlot
    GROUP BY MachineID
    HAVING COUNT(*)<>6 OR MIN(LowerBound)<>0 OR MAX(UpperBound)<>1000
           OR SUM(Weight)<>1000
  )
    RAISERROR(N'Weighted slot boundaries are not continuous and complete',16,1);
  IF EXISTS (
    SELECT 1
      FROM #NativeSlot e
      FULL OUTER JOIN (
        SELECT d.MachineID,
               ROW_NUMBER() OVER (PARTITION BY d.MachineID ORDER BY d.Ident) AS SlotOrdinal,
               d.UnitID, d.Prob1 AS Weight,
               d.UnitGradeNumber AS NativeDrawGroup
          FROM dbo.tbl_MachineDice d
          JOIN #Fixed f ON f.MachineID=d.MachineID AND f.DrawPolicy=0
      ) n ON n.MachineID=e.MachineID AND n.SlotOrdinal=e.SlotOrdinal
      WHERE e.MachineID IS NULL OR n.MachineID IS NULL
         OR n.UnitID<>e.UnitID OR n.Weight<>e.Weight
         OR n.NativeDrawGroup<>e.NativeDrawGroup
  )
    RAISERROR(N'Native-sequence slots differ from tbl_MachineDice',16,1);
  IF EXISTS (
    SELECT 1 FROM #Custom c
    LEFT JOIN dbo.tbl_CustomCapsuleList n ON n.MachineID=c.MachineID
    WHERE (c.Enabled=1 AND n.MachineID IS NULL)
       OR (c.Enabled=0 AND n.MachineID IS NOT NULL)
  )
    RAISERROR(N'Custom machine enabled state differs',16,1);
  IF EXISTS (
    SELECT 1 FROM #Custom c
    JOIN dbo.tbl_CustomCapsuleList n ON n.MachineID=c.MachineID
    WHERE c.Enabled=1 AND (
      n.CostType<>c.CostType OR n.BasicCost<>c.BasicCost
      OR n.MachineTitle<>c.MachineTitle
      OR n.Rank1<>c.Rank1 OR n.Grade1<>c.Grade1
      OR n.Rank2<>c.Rank2 OR n.Grade2<>c.Grade2
      OR n.Rank3<>c.Rank3 OR n.Grade3<>c.Grade3
      OR n.Rank4<>c.Rank4 OR n.Grade4<>c.Grade4
      OR n.Rank5<>c.Rank5 OR n.Grade5<>c.Grade5
      OR n.Rank6<>c.Rank6 OR n.Grade6<>c.Grade6
      OR n.Rank7<>c.Rank7 OR n.Grade7<>c.Grade7
      OR n.Rank8<>c.Rank8 OR n.Grade8<>c.Grade8
    )
  )
    RAISERROR(N'Custom machine title, price, currency, or slot rules differ',16,1);
  IF EXISTS (
    SELECT 1 FROM #Custom c
    LEFT JOIN dbo.sdgo_mod_CapsuleCustomMachine m ON m.MachineID=c.MachineID
    WHERE m.MachineID IS NULL OR m.CatalogRevision<>'{revision}'
       OR m.Enabled<>c.Enabled
  )
    RAISERROR(N'Custom machine adapter metadata differs',16,1);
  IF EXISTS (
    SELECT 1 FROM #Candidate e
    JOIN #Custom c ON c.MachineID=e.MachineID AND c.Enabled=1
    LEFT JOIN dbo.tbl_CustomCapsuleInfo a
      ON a.MachineID=e.MachineID AND a.UnitID=e.UnitID
    WHERE a.MachineID IS NULL OR a.BasicProb<>e.BasicProb
       OR ABS(a.Mileage-e.Mileage)>0.000001
       OR a.AdditionalCost<>e.AdditionalCost
       OR CONVERT(INT,a.MileageResetFlag)<>CONVERT(INT,e.MileageResetFlag)
  )
    RAISERROR(N'Custom candidate configuration differs',16,1);
  IF EXISTS (
    SELECT 1 FROM dbo.tbl_CustomCapsuleInfo a
    JOIN #Custom c ON c.MachineID=a.MachineID
    LEFT JOIN #Candidate e ON e.MachineID=a.MachineID AND e.UnitID=a.UnitID
    WHERE c.Enabled=0 OR e.MachineID IS NULL
  )
    RAISERROR(N'Custom candidate set has stale rows',16,1);
  IF EXISTS (
    SELECT 1
      FROM dbo.tbl_CustomCapsuleInfo a
      JOIN #Custom c ON c.MachineID=a.MachineID AND c.Enabled=1
      GROUP BY a.MachineID, a.UnitID
      HAVING COUNT(*)<>1
  )
    RAISERROR(N'Custom candidate set contains duplicate rows',16,1);
END TRY
BEGIN CATCH
  DECLARE @ErrorMessage NVARCHAR(4000);
  SET @ErrorMessage=ERROR_MESSAGE();
  RAISERROR(@ErrorMessage,16,1);
  RETURN;
END CATCH;

SELECT '{revision}' AS CatalogRevision,
       (SELECT COUNT(*) FROM #Fixed WHERE DrawPolicy=1) AS WeightedMachines,
       (SELECT COUNT(*) FROM #FixedSlot) AS WeightedSlots,
       (SELECT COUNT(*) FROM #Custom WHERE Enabled=1) AS EnabledCustomMachines,
       (SELECT COUNT(*) FROM #Candidate c JOIN #Custom m
          ON m.MachineID=c.MachineID WHERE m.Enabled=1) AS EnabledCustomCandidates,
       CAST(1 AS BIT) AS Verified;
"""


def rollback_sql(bundle):
    return common_header(bundle, "rollback to captured native configuration") + r"""
IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleRuntimeState', N'U') IS NULL
   AND OBJECT_ID(N'dbo.sdgo_native_sp_GetNextCapsule1', N'P') IS NULL
BEGIN
  SELECT CAST(1 AS BIT) AS AlreadyRolledBack;
  RETURN;
END;

BEGIN TRY
  BEGIN TRANSACTION;
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineMachinePrice', N'U') IS NULL
     OR OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineCustomCaptured', N'U') IS NULL
     OR OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineCustomList', N'U') IS NULL
     OR OBJECT_ID(N'dbo.sdgo_mod_CapsuleBaselineCustomInfo', N'U') IS NULL
    RAISERROR(N'Runtime baseline is incomplete; rollback refused',16,1);

  IF OBJECT_ID(N'dbo.sp_GetNextCapsule1', N'P') IS NOT NULL
  BEGIN
    IF NOT EXISTS (
      SELECT 1 FROM fn_listextendedproperty(
        N'SDGO_CAPSULE_RUNTIME_WRAPPER', N'SCHEMA', N'dbo',
        N'PROCEDURE', N'sp_GetNextCapsule1', NULL, NULL
      ) WHERE CONVERT(NVARCHAR(20),value)=N'1'
    )
      RAISERROR(N'Current sp_GetNextCapsule1 is not adapter-owned',16,1);
    DROP PROCEDURE dbo.sp_GetNextCapsule1;
  END;
  IF OBJECT_ID(N'dbo.sdgo_native_sp_GetNextCapsule1', N'P') IS NULL
    RAISERROR(N'Captured native selection procedure is missing',16,1);
  EXEC sys.sp_rename N'dbo.sdgo_native_sp_GetNextCapsule1',
                     N'sp_GetNextCapsule1', N'OBJECT';

  DELETE p FROM dbo.tbl_MachinePrice p
    JOIN dbo.sdgo_mod_CapsuleBaselineMachinePrice b ON b.MachineID=p.MachineID
    WHERE b.Existed=0;
  UPDATE p SET p.Price=b.Price, p.Disable=b.Disable, p.Type=b.Type
    FROM dbo.tbl_MachinePrice p
    JOIN dbo.sdgo_mod_CapsuleBaselineMachinePrice b ON b.MachineID=p.MachineID
    WHERE b.Existed=1;
  UPDATE c SET c.Disable=b.OriginalDisable
    FROM dbo.tbl_CoinMachinePrice c
    JOIN dbo.sdgo_mod_CapsuleBaselineCoinState b ON b.CoinMachineID=c.MachineID;

  DELETE i FROM dbo.tbl_CustomCapsuleInfo i
    JOIN dbo.sdgo_mod_CapsuleBaselineCustomCaptured c ON c.MachineID=i.MachineID;
  DELETE l FROM dbo.tbl_CustomCapsuleList l
    JOIN dbo.sdgo_mod_CapsuleBaselineCustomCaptured c ON c.MachineID=l.MachineID;
  INSERT INTO dbo.tbl_CustomCapsuleList
    (MachineID, CostType, BasicCost, MachineTitle,
     Rank1, Grade1, Rank2, Grade2, Rank3, Grade3, Rank4, Grade4,
     Rank5, Grade5, Rank6, Grade6, Rank7, Grade7, Rank8, Grade8)
  SELECT MachineID, CostType, BasicCost, MachineTitle,
         Rank1, Grade1, Rank2, Grade2, Rank3, Grade3, Rank4, Grade4,
         Rank5, Grade5, Rank6, Grade6, Rank7, Grade7, Rank8, Grade8
    FROM dbo.sdgo_mod_CapsuleBaselineCustomList;
  INSERT INTO dbo.tbl_CustomCapsuleInfo
    (MachineID, UnitID, BasicProb, Mileage, AdditionalCost, MileageResetFlag)
  SELECT MachineID, UnitID, BasicProb, Mileage, AdditionalCost, MileageResetFlag
    FROM dbo.sdgo_mod_CapsuleBaselineCustomInfo
    ORDER BY BaselineOrdinal;

  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleSlot', N'U') IS NOT NULL
    DROP TABLE dbo.sdgo_mod_CapsuleSlot;
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleMachine', N'U') IS NOT NULL
    DROP TABLE dbo.sdgo_mod_CapsuleMachine;
  IF OBJECT_ID(N'dbo.sdgo_mod_CapsuleCustomMachine', N'U') IS NOT NULL
    DROP TABLE dbo.sdgo_mod_CapsuleCustomMachine;
  DROP TABLE dbo.sdgo_mod_CapsuleRuntimeState;
  DROP TABLE dbo.sdgo_mod_CapsuleBaselineCustomInfo;
  DROP TABLE dbo.sdgo_mod_CapsuleBaselineCustomList;
  DROP TABLE dbo.sdgo_mod_CapsuleBaselineCustomCaptured;
  DROP TABLE dbo.sdgo_mod_CapsuleBaselineCoinState;
  DROP TABLE dbo.sdgo_mod_CapsuleBaselineMachinePrice;

  COMMIT TRANSACTION;
END TRY
BEGIN CATCH
  IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION;
  DECLARE @ErrorMessage NVARCHAR(4000);
  SET @ErrorMessage=ERROR_MESSAGE();
  RAISERROR(@ErrorMessage,16,1);
  RETURN;
END CATCH;

SELECT CAST(1 AS BIT) AS RolledBack;
"""


def weighted_pick(slots, roll):
    """Pure model of the generated SQL boundary selection, for tests/tools."""
    require(type(roll) is int and 0 <= roll < 1000, "Roll must be in [0, 1000)")
    lower = 0
    for slot in slots:
        upper = lower + slot["weight"]
        if lower <= roll < upper:
            return slot["unit_id"], slot["native_draw_group"]
        lower = upper
    raise ValueError("Slots do not cover the 1000-point range")


def build(bundle_path, output):
    bundle_path = Path(bundle_path)
    bundle = validate_bundle(read_json(bundle_path))
    verify_input_manifest(bundle_path, bundle)
    output = Path(output)
    artifacts = {
        "install.sql": install_sql(bundle).replace("\r\n", "\n").encode("utf-8"),
        "verify.sql": verify_sql(bundle).replace("\r\n", "\n").encode("utf-8"),
        "rollback.sql": rollback_sql(bundle).replace("\r\n", "\n").encode("utf-8"),
    }
    manifest = {
        "schema_version": 1,
        "catalog_revision": bundle["catalog_revision"],
        "source_bundle_sha256": sha256(bundle_path.read_bytes()),
        "runtime_host": {
            "file": "WorldServer.exe",
            "sha256": WORLD_SERVER_SHA256,
            "entry": "dbo.sp_GetNextCapsule1",
            "native_delegate": "dbo.sdgo_native_sp_GetNextCapsule1",
        },
        "counts": {
            "fixed": len(bundle["fixed_machines"]),
            "weighted_fixed": sum(m["draw_policy"] == "independent_weighted"
                                  for m in bundle["fixed_machines"]),
            "custom": len(bundle["custom_machines"]),
            "custom_candidates": sum(len(m["candidates"])
                                     for m in bundle["custom_machines"]),
        },
        "files": {name: sha256(data) for name, data in artifacts.items()},
        "safety": {
            "executes_sql": False,
            "reads_player_tables": False,
            "new_coin_variants": False,
            "native_charge_and_grant_path_preserved": True,
        },
    }
    artifacts["manifest.json"] = json_bytes(manifest)
    for name, data in artifacts.items():
        write_bytes(output / name, data)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)
    command = sub.add_parser("build", help="Generate install, verify, and rollback SQL")
    command.add_argument("--bundle", type=Path, required=True)
    command.add_argument("--output", type=Path, required=True)
    command = sub.add_parser("validate", help="Validate a server bundle without writing")
    command.add_argument("--bundle", type=Path, required=True)
    args = parser.parse_args()
    if args.command == "validate":
        bundle = validate_bundle(read_json(args.bundle))
        verify_input_manifest(args.bundle, bundle)
        print(json.dumps({"valid": True, "catalog_revision": bundle["catalog_revision"]}))
        return
    manifest = build(args.bundle, args.output)
    print(json.dumps(manifest, ensure_ascii=False, sort_keys=True))


