#!/usr/bin/env python3
"""Owner AI roster audit and paired process-local catalog generation. No database writes.

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
REQUIREMENTS = ("generate_paired_catalog", "install_optional_mod", "manual_spawn_combat_respawn_acceptance")
MAGIC = b"SDGOAI1\0"
HEADER = struct.Struct("<8sIIII")
RECORD = struct.Struct("<IIIIIQ89s38s598s")
# Packed native BattleServer UnitData prefix (0x59 bytes). Names, custom stats,
# UI-only fields and linked-list pointers are not part of this prefix.
UNIT_FIELDS = (
    ("UnitNumber", "I"), ("Influence1", "B"), ("Influence2", "B"),
    ("UnitGradeNumber", "B"), ("UnitGradeNumber2", "B"), ("MasteredWeaponType", "B"),
    ("BattleCost", "H"), ("Star", "B"), ("UnitNumber_Transform", "I"), ("HP", "I"),
    ("Shield", "I"), ("ShieldType", "B"), ("ShieldRate", "I"), ("DashType", "B"),
    ("MoveLevel", "B"), ("DashLevel", "B"), ("QuickStepLevel", "B"), ("MotivePower", "B"),
    ("BoosterIncrease", "H"), ("BoosterDecrease", "H"), ("FatalAttackIncrease", "H"),
    ("AdaptEnvironType", "B"), ("AdaptEnvironSpace", "h"), ("AdaptEnvironGround", "h"),
    ("AdaptEnvironUnderwater", "h"), ("AdaptEnvironDesert", "h"), ("AdaptEnvironSky", "h"),
    ("Sight", "H"), ("SkillNumber_1", "I"), ("SkillNumber_2", "I"), ("SkillNumber_3", "I"),
    ("StrikingPower", "H"), ("DefensivePower", "H"), ("MasteredWeaponIX", "I"),
    ("WeaponNumber_1", "I"), ("WeaponNumber_2", "I"), ("WeaponNumber_3", "I"),
    ("WeaponNumber_FatalAttack", "I"),
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
    if not isinstance(rules, dict) or set(rules) - {"exclude", "levels", "templates"}:
        raise ValueError("rules must contain only exclude, levels and templates")
    exclude, levels = rules.get("exclude", []), rules.get("levels", {})
    if not isinstance(exclude, list) or not isinstance(levels, dict):
        raise ValueError("invalid rules structure")
    for number in exclude:
        integer(number, "excluded unit id", 1, 9999999)
    for number, level in levels.items():
        if not number.isascii() or not number.isdecimal() or str(int(number)) != number:
            raise ValueError("override keys must be canonical decimal unit ids")
        integer(level, "level override", 1, 5)
    templates = rules.get("templates", {})
    if not isinstance(templates, dict):
        raise ValueError("templates must be an object")
    for number, donor in templates.items():
        if not number.isascii() or not number.isdecimal() or str(int(number)) != number:
            raise ValueError("template keys must be canonical decimal unit ids")
        integer(donor, "NPC template", NPC_FIRST, NPC_LAST)
    return rules


def fnv(data, bits):
    value, prime = (2166136261, 16777619) if bits == 32 else (14695981039346656037, 1099511628211)
    mask = (1 << bits) - 1
    for byte in data:
        value = ((value ^ byte) * prime) & mask
    return value


def native_unit_prefix(unit):
    values = []
    for key, kind in UNIT_FIELDS:
        maximum = {"I": 0xFFFFFFFF, "H": 0xFFFF, "h": 32767, "B": 255}[kind]
        values.append(integer(unit.get(key), key, -32768 if kind == "h" else 0, maximum))
    return struct.pack("<" + "".join(kind for _, kind in UNIT_FIELDS), *values)


def native_npc(bot, npc_id, unit_id, hp):
    unsigned = ("NpcPoint", "ObjectHpUp", "ItemBuffNumber", "CreateUnitHp", "DestroyUnitHp")
    values = {key: integer(bot.get(key), key, 0, 0x7FFFFFFF) for key in unsigned}
    exp = integer(bot.get("AddUnitEXP"), "AddUnitEXP", -32768, 32767)
    point = integer(bot.get("AddPoint"), "AddPoint", -32768, 32767)
    boss = integer(bot.get("BossFlag"), "BossFlag", 0, 255)
    level = integer(bot.get("NpcUnitLevel"), "NpcUnitLevel", 0, 255)
    return struct.pack("<IIIIIhhBBIII", npc_id, unit_id, hp, values["NpcPoint"], values["ObjectHpUp"],
                       exp, point, boss, level, values["ItemBuffNumber"], values["CreateUnitHp"], values["DestroyUnitHp"])


def previous_ids(path):
    if path is None:
        return {}
    try:
        manifest = json.loads(path.read_text(encoding="utf-8-sig"))
    except (UnicodeError, json.JSONDecodeError):
        raise ValueError("use the previous catalog.json allocation ledger, not catalog.bin") from None
    allocations = manifest.get("allocations") if isinstance(manifest, dict) else None
    if not isinstance(manifest, dict) or manifest.get("schema_version") != 1 or not isinstance(allocations, dict):
        raise ValueError("previous catalog manifest has no supported allocation ledger")
    result, used = {}, set()
    for key, npc in allocations.items():
        if not key.isascii() or not key.isdecimal() or str(int(key)) != key:
            raise ValueError("invalid previous allocation key")
        unit = integer(int(key), "previous unit id", 1, 9999999)
        integer(npc, "previous NPC id", NPC_FIRST, NPC_LAST)
        if unit in result or npc in used:
            raise ValueError("previous catalog has invalid or duplicate identities")
        result[unit] = npc
        used.add(npc)
    return result


def generate_catalog(tables, client, report, rules=None, previous=None):
    """Generate paired process-local data, never SQL or changes to a game archive."""
    if client is None or client["missing_files"]:
        raise ValueError("generation requires the complete supported client gdrs tables")
    if not report["npc_capacity_ok"] or not report["candidates"]:
        raise ValueError("no candidates or insufficient NPC capacity")
    rules, previous = rules or {}, previous or {}
    units = unique(tables["tbl_Units"], "UnitNumber", "tbl_Units")
    npc_units = unique(tables["tbl_UnitNpc"], "UnitNumber", "tbl_UnitNpc")
    npcs = unique(tables["tbl_NpcData"], "NpcDataID", "tbl_NpcData")
    templates = []
    for number, bot in sorted(npcs.items()):
        record = client["npcs"].get(number)
        donor = npc_units.get(bot["UseUnitID"])
        if not NPC_FIRST <= number <= NPC_LAST or record is None or donor is None:
            continue
        script = record[0x6D:0xD1].decode("utf-16le").split("\0")[0]
        if (not script.startswith(("VSAI_Gunfighter_", "VSAI_Infighter_", "VSAI_Stoker_", "VSAI_Tanker_")) or
            struct.unpack_from("<I", record, 0x69)[0] != bot["UseUnitID"] or
            struct.unpack_from("<I", record, 0xE5)[0] != bot["UseUnitHP"] or
            bot.get("CreateUnitHp", 0) != 0 or bot.get("DestroyUnitHp", 0) != 0 or bot.get("NpcPoint", 0) <= 0):
            continue
        templates.append((number, bot, donor, record, script))
    if not templates:
        raise ValueError("no matching native AI template in the server and client data")
    by_template = {row[0]: row for row in templates}
    template_rules = {int(key): value for key, value in rules.get("templates", {}).items()}
    if set(template_rules) - units.keys() or set(template_rules.values()) - by_template.keys():
        raise ValueError("template override references missing or incompatible data")
    occupied = set(npcs) | set(client["npcs"])
    eligible = {row["unit_id"] for row in report["candidates"]}
    # Reserve even removed rows from a previous generation: old ids must never
    # silently become a different unit when an owner updates a distributed pack.
    if any(npc in occupied for npc in previous.values()):
        raise ValueError("previous catalog id now collides with native data")
    available = iter(i for i in range(NPC_FIRST, NPC_LAST + 1) if i not in occupied and i not in previous.values())
    payload, generated = [], []
    for row in report["candidates"]:
        number = row["unit_id"]
        unit = units[number]
        if row["missing_client_unit_ids"] or row["missing_weapon_ids"]:
            raise ValueError(f"unit {number}: client unit, transform or weapon dependency is missing")
        if row["transform_id"] and row["transform_id"] not in units and row["transform_id"] not in npc_units:
            raise ValueError(f"unit {number}: server transform dependency is missing")
        slots = [unit[field] for field in WEAPONS[:3]]
        preferred = unit["MasteredWeaponIX"]
        if not 1 <= preferred <= 3 or not slots[preferred - 1]:
            preferred = next((i + 1 for i, value in enumerate(slots) if value), None)
        if preferred is None:
            raise ValueError(f"unit {number}: no non-special weapon")
        npc_id = previous.get(number)
        if npc_id is None:
            npc_id = next(available, None)
            if npc_id is None:
                raise ValueError("NPC capacity exhausted after preserving previous identities")
        if number in template_rules:
            template = by_template[template_rules[number]]
        else:
            # Native scripts are a starting policy, not a gameplay certification.
            # Ranged scripts avoid relying on a primary melee slot when it is absent.
            family = "VSAI_Gunfighter_" if not slots[0] or unit["MasteredWeaponType"] == 1 else (
                "VSAI_Infighter_" if unit["MasteredWeaponType"] == 0 else "VSAI_Stoker_")
            template = min(templates, key=lambda item: (
                not item[4].startswith(family), item[2]["MasteredWeaponType"] != unit["MasteredWeaponType"],
                abs(item[2]["UnitGradeNumber"] - unit["UnitGradeNumber"]),
                abs(item[1]["UseUnitHP"] - unit["HP"]), item[0]))
        template_id, bot, _, original_record, script = template
        client_record = bytearray(original_record)
        struct.pack_into("<I", client_record, 0, npc_id)
        struct.pack_into("<I", client_record, 0x69, number)
        struct.pack_into("<I", client_record, 0xE5, unit["HP"])
        client_record[5:105] = client["units"][number][5:105]
        npc_record = native_npc(bot, npc_id, number, unit["HP"])
        prefix = native_unit_prefix(unit)
        payload.append((npc_id, RECORD.pack(npc_id, number, row["level"], template_id, preferred,
                        fnv(client["units"][number], 64), prefix, npc_record, bytes(client_record))))
        generated.append(dict(unit_id=number, npc_id=npc_id, level=row["level"], template_npc_id=template_id,
                              script=script, preferred_weapon=preferred, empty_weapon_slots=row["empty_weapon_slots"]))
    payload = b"".join(data for _, data in sorted(payload))
    binary = HEADER.pack(MAGIC, 1, RECORD.size, len(generated), fnv(payload, 32)) + payload
    allocations = dict(previous)
    allocations.update({row["unit_id"]: row["npc_id"] for row in generated})
    manifest = {"schema_version": 1, "mode": "process_local_catalog", "count": len(generated),
                "sha256": hashlib.sha256(binary).hexdigest(), "levels": report["levels"],
                "database_writes": False, "game_archive_writes": False, "gameplay_verified": False,
                "native_script_policy": "heuristic; owner overrides supported; manual gameplay acceptance required",
                "retired_unit_ids": sorted(set(previous) - eligible),
                "allocations": {str(key): value for key, value in sorted(allocations.items())}, "entries": generated}
    return binary, manifest


def write_bundle(report, output, binary, manifest):
    write_plan(report, output)
    for role in ("client", "server"):
        directory = output / role / "sdgo/mods/ai_catalog"
        directory.mkdir(parents=True)
        (directory / "catalog.bin").write_bytes(binary)
        if hashlib.sha256((directory / "catalog.bin").read_bytes()).hexdigest() != manifest["sha256"]:
            raise OSError("catalog readback failed")
    (output / "catalog.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "README.txt").write_text(
        "Paired AI catalog data. Install the ai_catalog mod in server and every client.\n"
        "Use the same catalog.bin on both sides; start games with the normal launcher.\n"
        "No SQL or game archive changes are needed. Toggle the mod with a process restart.\n"
        "Existing AI gameplay mods are required. Native script selection still needs manual gameplay acceptance.\n",
        encoding="utf-8")


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
        "reason": "Audit only; use --generate with matching client data to build an optional ai_catalog payload.",
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
    parser.add_argument("--generate", action="store_true", help="generate paired data for the optional ai_catalog mod")
    parser.add_argument("--previous-catalog", type=Path, help="previous catalog.json allocation ledger; preserve ids across updates")
    parser.add_argument("--out", required=True, type=Path, help="new output directory; never overwritten")
    args = parser.parse_args(argv)
    try:
        rules = rules_from_json(args.rules)
        tables = read_exports(args.db_export) if args.db_export else read_database(args.connection_env)
        client = read_client(args.client_gdrs) if args.client_gdrs else None
        report = prepare(tables, client, rules)
        report["source_kind"] = "json_catalog_exports" if args.db_export else "odbc_catalog_selects"
        if args.generate:
            binary, manifest = generate_catalog(tables, client, report, rules, previous_ids(args.previous_catalog))
            report["mode"] = "generated_process_local_catalog"
            report["ready_to_install"] = True
            report["reason"] = "Paired data generated for ai_catalog; manual gameplay acceptance remains pending."
            report["remaining_requirements"] = ["matching_mod_installations", "manual_gameplay_acceptance"]
            identities = {entry["unit_id"]: entry["npc_id"] for entry in manifest["entries"]}
            for row in report["candidates"]:
                row["proposed_npc_id"] = identities[row["unit_id"]]
                row["status"] = "generated_not_gameplay_verified"
            write_bundle(report, args.out, binary, manifest)
        else:
            if args.previous_catalog:
                raise ValueError("--previous-catalog requires --generate")
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
