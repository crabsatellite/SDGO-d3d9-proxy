#!/usr/bin/env python3
"""Read-only AI roster audit and LV1..LV5 expansion plan. No database writes.

Use the owner's catalog exports or an ODBC connection supplied in an environment
variable. Output contains catalog data only, never accounts or connection settings.
An expansion plan is deliberately not an installation or gameplay certification.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import struct
import sys


NPC_FIRST, NPC_LAST = 3604001, 3607000
TABLES = {
    "tbl_Units": "UnitNumber",
    "tbl_UnitNpc": "UnitNumber",
    "tbl_NpcData": "NpcDataID",
    "tbl_UnitGrades": None,
    "tbl_WeaponsRev": "WeaponNo",
}
# Files loaded by GOnline-54BE1568-00C97000, in its native load order.
UNIT_FILES = ("0237", "0242", "0243", "0244", "0246", "0245")
NPC_FILES = ("0020", "0105", "0128", "0162", "0198", "0222")
WEAPONS = ("WeaponNumber_1", "WeaponNumber_2", "WeaponNumber_3", "WeaponNumber_FatalAttack")
REQUIREMENTS = (
    "server_npc_and_unit_data",
    "client_npc_records_and_existing_unit_assets",
    "compatible_ai_scripts_and_weapon_slots",
    "large_level_pool_reader",
    "manual_spawn_combat_respawn_acceptance",
)


def integer(value, label, minimum=0, maximum=0x7FFFFFFF):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"{label}: expected integer in {minimum}..{maximum}")
    return value


def unique(rows, key, label):
    result = {}
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError(f"{label}: each row must be an object")
        number = integer(row.get(key), f"{label}.{key}", 1, 9999999)
        if number in result:
            raise ValueError(f"{label}: duplicate {key} {number}")
        result[number] = row
    return result


def read_exports(directory):
    """Only read the five public catalog tables, never glob the whole backup."""
    tables = {}
    for table in TABLES:
        files = sorted(directory.glob(f"dbo.{table}.json"))
        parts = sorted(directory.glob(f"dbo.{table}.part[0-9]*.json"))
        if files and parts:
            raise ValueError(f"{table}: both full and split exports exist")
        if not files and not parts:
            raise ValueError(f"{table}: missing catalog export")
        if parts:
            numbers = sorted(int(f.stem.rsplit("part", 1)[1]) for f in parts)
            if numbers != list(range(1, len(numbers) + 1)):
                raise ValueError(f"{table}: incomplete split export")
        rows = []
        for path in files or parts:
            value = json.loads(path.read_text(encoding="utf-8-sig"))
            if not isinstance(value, list):
                raise ValueError(f"{table}: expected an array of rows")
            rows.extend(value)
        tables[table] = rows
    return tables


def read_database(environment_name):
    """Fixed SELECT statements only; credentials are not CLI arguments or output."""
    connection_string = os.environ.get(environment_name)
    if not connection_string:
        raise ValueError("connection environment variable is empty")
    try:
        import pyodbc
    except ImportError:
        raise ValueError("ODBC mode needs pyodbc; JSON export mode uses Python only") from None
    try:
        connection = pyodbc.connect(connection_string, timeout=5, autocommit=True)
        try:
            connection.timeout = 15
            result = {}
            for table in TABLES:
                cursor = connection.cursor()
                cursor.execute(f"SELECT * FROM dbo.[{table}]")
                columns = [column[0] for column in cursor.description]
                result[table] = [dict(zip(columns, row)) for row in cursor.fetchall()]
            return result
        finally:
            connection.close()
    except pyodbc.Error as error:
        # Driver messages can contain server, account and connection information.
        raise ValueError(f"catalog SELECT failed (SQLSTATE {str(error.args[0])[:5]})") from None


def read_gdt(path, expected_size):
    """Strict offline reader of the supported client's F9F8 record format.

    Each record consists of key-sized chunks; the permutation and XOR counter
    restart for each chunk. Reject incomplete files rather than treating a partial
    client table as complete. This never changes a GPackData or GDT file.
    """
    data = path.read_bytes()
    if len(data) < 14 or data[:2] != b"\xf9\xf8":
        raise ValueError(f"{path.name}: unsupported GDT header")
    size, count, extra = struct.unpack_from("<iii", data, 2)
    if size != expected_size or not 0 <= count <= 100000 or extra < 0:
        raise ValueError(f"{path.name}: unsupported record layout")
    key_size = (size + count + extra) % 65 + 192
    key = data[14:14 + key_size]
    stride = ((size + key_size - 1) // key_size) * key_size
    if len(data) != 14 + key_size + count * stride:
        raise ValueError(f"{path.name}: truncated file or unexpected trailing bytes")
    if len(set(key)) != key_size or set(key) != set(range(key_size)):
        raise ValueError(f"{path.name}: invalid permutation key")
    result = []
    mask = [key[(0x300 + i * 3) % key_size] for i in range(key_size)]
    for record_index in range(count):
        start = 14 + key_size + record_index * stride
        record = bytearray()
        for offset in range(0, stride, key_size):
            chunk = data[start + offset:start + offset + key_size]
            record.extend(chunk[key[i]] ^ mask[i] for i in range(min(key_size, size - len(record))))
        record_id = struct.unpack_from("<I", record)[0]
        integer(record_id, f"{path.name} grid id", 1, 9999999)
        result.append(bytes(record))
    return result


def read_client(directory):
    units, npcs, hashes, missing, security_excluded = {}, {}, {}, [], {}
    for names, size, output in ((UNIT_FILES, 4976, units), (NPC_FILES, 598, npcs)):
        for name in names:
            path = directory / (name + ".gdt")
            if not path.is_file():
                missing.append(path.name)
                continue
            records = read_gdt(path, size)
            security_excluded[path.name] = sum(record[4] != 0 for record in records)
            # Keep conflicting definitions explicit; never silently choose a version.
            for record in records:
                if record[4] != 0:  # the release client's loader skips these records
                    continue
                number = struct.unpack_from("<I", record)[0]
                if number in output and output[number] != record:
                    raise ValueError(f"{path.name}: conflicting client record {number}")
                output[number] = record
            hashes[path.name] = hashlib.sha256(path.read_bytes()).hexdigest()
    return {"units": units, "npcs": npcs, "sha256": hashes, "missing_files": missing,
            "security_excluded": security_excluded}


def default_level(unit):
    """Roster grouping, not learned AI difficulty or a unit's experience level."""
    grade = integer(unit.get("UnitGradeNumber"), "UnitGradeNumber", 1, 4)
    variant = integer(unit.get("UnitGradeNumber2"), "UnitGradeNumber2", 1, 5)
    return 5 if grade == 4 and variant in (3, 4) else grade


def rules_from_json(path):
    if path is None:
        return {}
    rules = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(rules, dict) or set(rules) - {"exclude", "levels"}:
        raise ValueError("rules must contain only exclude and levels")
    exclude, levels = rules.get("exclude", []), rules.get("levels", {})
    if not isinstance(exclude, list) or not isinstance(levels, dict):
        raise ValueError("invalid rules structure")
    for number in exclude:
        integer(number, "excluded unit id", 1, 9999999)
    for number, level in levels.items():
        if not number.isascii() or not number.isdecimal() or str(int(number)) != number:
            raise ValueError("override keys must be canonical decimal unit ids")
        integer(level, "level override", 1, 5)
    return rules


def prepare(tables, client=None, rules=None):
    rules = rules or {}
    units = unique(tables["tbl_Units"], "UnitNumber", "tbl_Units")
    npc_units = unique(tables["tbl_UnitNpc"], "UnitNumber", "tbl_UnitNpc")
    npcs = unique(tables["tbl_NpcData"], "NpcDataID", "tbl_NpcData")
    weapons = unique(tables["tbl_WeaponsRev"], "WeaponNo", "tbl_WeaponsRev")
    grades = {}
    for row in tables["tbl_UnitGrades"]:
        if not isinstance(row, dict):
            raise ValueError("tbl_UnitGrades: each row must be an object")
        pair = (integer(row.get("UnitGradeNumber"), "grade", 1, 4),
                integer(row.get("UnitGradeNumber2"), "grade variant", 1, 5))
        if pair in grades:
            raise ValueError("duplicate grade definition")
        grades[pair] = str(row.get("UnitGradeName", ""))
    excluded = set(rules.get("exclude", []))
    overrides = {int(k): v for k, v in rules.get("levels", {}).items()}
    if (excluded | overrides.keys()) - units.keys():
        raise ValueError("rules reference a unit absent from tbl_Units")
    bots = {k: v for k, v in npcs.items() if NPC_FIRST <= k <= NPC_LAST}
    occupied = set(npcs)
    if client:
        occupied.update(client["npcs"])
    free = [number for number in range(NPC_FIRST, NPC_LAST + 1) if number not in occupied]
    candidates, skipped, counts = [], [], {str(level): 0 for level in range(1, 6)}
    for number, unit in sorted(units.items()):
        integer(unit.get("SallyReady"), "SallyReady", 0, 1)
        hp = integer(unit.get("HP"), "HP")
        if number in excluded or unit["SallyReady"] != 1 or hp == 0:
            skipped.append({"unit_id": number, "reason": "owner_excluded" if number in excluded
                            else "not_sortie_ready" if unit["SallyReady"] != 1 else "zero_hp"})
            continue
        level = overrides.get(number, default_level(unit))
        integer(level, "LV group", 1, 5)
        pair = (unit["UnitGradeNumber"], unit["UnitGradeNumber2"])
        if pair not in grades:
            raise ValueError(f"unit {number}: grade definition is missing")
        transform = integer(unit.get("UnitNumber_Transform"), "UnitNumber_Transform", 0, 9999999)
        slots = [integer(unit.get(field), field, 0, 9999999) for field in WEAPONS]
        missing_weapons = [value for value in slots if value and value not in weapons]
        dependencies = [number] + ([transform] if transform else [])
        client_missing = None if client is None else [i for i in dependencies if i not in client["units"]]
        candidates.append({
            "unit_id": number, "name": str(unit.get("UnitModelName", "")).strip(),
            "grade": grades[pair], "level": level, "hp": hp,
            "mastered_weapon_type": integer(unit.get("MasteredWeaponType"), "MasteredWeaponType", 0, 255),
            "transform_id": transform,
            "missing_server_npc_unit_ids": [i for i in dependencies if i not in npc_units],
            "missing_client_unit_ids": client_missing,
            "missing_weapon_ids": missing_weapons,
            "empty_weapon_slots": [index + 1 for index, value in enumerate(slots) if not value],
            "status": "planned_unverified",
        })
        counts[str(level)] += 1
    capacity_ok = len(candidates) <= len(free)
    for index, row in enumerate(candidates):
        # Suggestions only. Regenerate before applying; this is not a stable allocation ledger.
        row["proposed_npc_id"] = free[index] if capacity_ok else None
    pool_bytes = {str(level): len(",".join(str(row["proposed_npc_id"]) for row in candidates
                                           if row["level"] == level and row["proposed_npc_id"] is not None))
                  for level in range(1, 6)}
    bot_checks = []
    for number, bot in sorted(bots.items()):
        unit_id = integer(bot.get("UseUnitID"), "UseUnitID", 1, 9999999)
        record = client["npcs"].get(number) if client else None
        match = None if client is None else record is not None and (
            struct.unpack_from("<I", record, 0x69)[0] == unit_id
            and struct.unpack_from("<I", record, 0xE5)[0] == bot["UseUnitHP"])
        bot_checks.append({"npc_id": number, "unit_id": unit_id,
                           "server_npc_unit_exists": unit_id in npc_units,
                           "client_npc_unit_and_hp_match": match,
                           "client_unit_exists": None if client is None else unit_id in client["units"]})
    return {
        "schema_version": 1, "mode": "read_only_plan", "ready_to_install": False,
        "reason": "New NPC client loading and AI compatibility must be implemented and validated before installation.",
        "source_counts": {table: len(rows) for table, rows in tables.items()},
        "existing_bot_count": len(bots), "existing_bots": bot_checks,
        "candidate_count": len(candidates), "skipped_count": len(skipped),
        "levels": counts, "npc_slots_free": len(free), "npc_capacity_ok": capacity_ok,
        "legacy_pool_buffer_bytes": 1024, "proposed_pool_value_bytes": pool_bytes,
        "levels_exceeding_legacy_buffer": [int(k) for k, size in pool_bytes.items() if size >= 1023],
        "client": None if client is None else {
            "unit_count": len(client["units"]), "npc_count": len(client["npcs"]),
            "missing_files": client["missing_files"], "sha256": client["sha256"],
            "security_excluded": client["security_excluded"],
        },
        "remaining_requirements": list(REQUIREMENTS), "candidates": candidates, "skipped": skipped,
    }


def write_plan(report, output):
    # No overwrite and no installers/SQL: cannot accidentally deploy proposed NPC ids.
    if output.exists():
        raise FileExistsError("output directory already exists")
    output.mkdir(parents=True)
    (output / "audit.json").write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    for level in range(1, 6):
        with (output / f"level-{level}.csv").open("w", encoding="utf-8-sig", newline="") as stream:
            columns = ("unit_id", "name", "grade", "level", "hp", "proposed_npc_id", "status")
            writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
            writer.writeheader()
            for row in report["candidates"]:
                if row["level"] == level:
                    # Prevent spreadsheet formulas in owner-provided display names.
                    row = dict(row)
                    if row["name"].lstrip().startswith(("=", "+", "-", "@")):
                        row["name"] = "'" + row["name"]
                    writer.writerow(row)
    (output / "README.txt").write_text(
        "AI roster expansion PLAN ONLY. No database or client was modified.\n"
        "Proposed NPC ids and LV groups are unverified suggestions.\n"
        "Do not paste these ids into LevelN settings: the client NPC records do not exist yet.\n"
        "See audit.json for content mismatches and remaining runtime work.\n", encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--db-export", type=Path, help="directory containing dbo catalog JSON exports")
    source.add_argument("--connection-env", help="name of environment variable holding owner's ODBC connection string")
    parser.add_argument("--client-gdrs", type=Path, help="owner-extracted gdrs directory; never game installation files")
    parser.add_argument("--rules", type=Path, help="optional owner exclude and LV overrides JSON")
    parser.add_argument("--out", required=True, type=Path, help="new output directory; never overwritten")
    args = parser.parse_args(argv)
    try:
        rules = rules_from_json(args.rules)
        tables = read_exports(args.db_export) if args.db_export else read_database(args.connection_env)
        client = read_client(args.client_gdrs) if args.client_gdrs else None
        report = prepare(tables, client, rules)
        report["source_kind"] = "json_catalog_exports" if args.db_export else "odbc_catalog_selects"
        write_plan(report, args.out)
    except (ValueError, OSError, KeyError, TypeError, struct.error) as error:
        # Paths and values may contain private deployment information: detailed data
        # diagnostics are local-only; never print a raw driver/connection exception.
        print(f"AI catalog: {error}", file=sys.stderr)
        return 1
    print(json.dumps({key: report[key] for key in (
        "mode", "ready_to_install", "candidate_count", "skipped_count", "levels",
        "npc_slots_free", "npc_capacity_ok", "levels_exceeding_legacy_buffer")}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
