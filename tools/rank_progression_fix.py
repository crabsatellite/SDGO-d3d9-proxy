"""Audit/apply/restore the native SDGO battle rank comparison repair.

Only two existing GOnlineGame procedures are changed. Their signatures and all
other SQL are retained. No player rows, experience values or rank tables are
modified by this installer. SQL access requires pyodbc and a local SQL driver.
"""
from __future__ import annotations

import argparse
import configparser
from dataclasses import dataclass
import datetime
import hashlib
import json
from pathlib import Path
import re
import uuid


TARGETS = ("sp_SetBattleExp_V4", "sp_SetGridBattleExp_V2")
VERSION = 1
OLD = re.compile(
    r"\bIF\s*\(\s*(?P<major>@UserLevelNumber)\s*<\s*(?P<new_major>@newlevel)"
    r"\s+OR\s+(?P<step>@UserLevelStep)\s*<\s*(?P<new_step>@NewLevelStep)\s*\)", re.I)
FIXED = re.compile(
    r"\bIF\s*\(\s*@UserLevelNumber\s*<\s*@newlevel\s+OR\s*\(\s*"
    r"@UserLevelNumber\s*=\s*@newlevel\s+AND\s*@UserLevelStep\s*<\s*@NewLevelStep\s*\)\s*\)", re.I)
HEADER = re.compile(r"\b(?P<action>CREATE|ALTER)\s+(?:PROC|PROCEDURE)\b", re.I)
NAME = re.compile(r"\s*(?:(?:\[dbo\]|dbo)\s*\.\s*)?(?:\[(?P<bracket>\w+)\]|(?P<plain>\w+))", re.I)


def sql_code(text: str) -> str:
    """Blank comments, strings and quoted identifiers without moving offsets."""
    chars = list(text)
    i, length = 0, len(text)
    while i < length:
        start = i
        if text.startswith("--", i):
            i = text.find("\n", i)
            if i < 0:
                i = length
        elif text.startswith("/*", i):
            i += 2
            depth = 1
            while i < length and depth:
                if text.startswith("/*", i):
                    depth += 1
                    i += 2
                elif text.startswith("*/", i):
                    depth -= 1
                    i += 2
                else:
                    i += 1
            if depth:
                raise ValueError("Unterminated SQL block comment")
        elif text[i] in "'\"[":
            close = "]" if text[i] == "[" else text[i]
            i += 1
            while i < length:
                if text[i] == close:
                    i += 1
                    if i < length and text[i] == close:
                        i += 1
                    else:
                        break
                else:
                    i += 1
            else:
                raise ValueError("Unterminated SQL quoted value")
        else:
            i += 1
            continue
        for j in range(start, i):
            if chars[j] not in "\r\n":
                chars[j] = " "
    return "".join(chars)


def module_header(definition: str, name: str):
    if name not in TARGETS:
        raise ValueError("Procedure is outside the repair scope")
    code = sql_code(definition)
    header = HEADER.search(code)
    if header is None or code[:header.start()].strip():
        raise ValueError(f"Unsupported procedure header: {name}")
    named = NAME.match(definition, header.end())
    if named is None or (named.group("bracket") or named.group("plain")).casefold() != name.casefold():
        raise ValueError(f"Procedure definition does not match its identity: {name}")
    return header


def as_alter(definition: str, name: str) -> str:
    h = module_header(definition, name)
    return definition[:h.start("action")] + "ALTER" + definition[h.end("action"):]


def module_hash(definition: str, name: str) -> str:
    h = module_header(definition, name)
    canonical = definition[:h.start("action")] + "CREATE" + definition[h.end("action"):]
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


@dataclass(frozen=True)
class Plan:
    name: str
    state: str
    original: str
    updated: str
    original_hash: str
    installed_hash: str


def plan(name: str, definition: str) -> Plan:
    module_header(definition, name)
    code = sql_code(definition)
    old, fixed = list(OLD.finditer(code)), list(FIXED.finditer(code))
    if not old and len(fixed) == 1:
        h = module_hash(definition, name)
        return Plan(name, "already_fixed", definition, definition, h, h)
    if len(old) != 1 or fixed:
        raise ValueError(f"Unknown or ambiguous rank comparison in {name}; no changes made")
    m = old[0]
    major, new_major, step, new_step = (m.group(k) for k in ("major", "new_major", "step", "new_step"))
    replacement = f"IF({major} < {new_major} OR ({major} = {new_major} AND {step} < {new_step}))"
    updated = definition[:m.start()] + replacement + definition[m.end():]
    if len(FIXED.findall(sql_code(updated))) != 1:
        raise ValueError("Patched comparison could not be verified")
    return Plan(name, "affected", definition, updated, module_hash(definition, name), module_hash(updated, name))


def read_modules(connection):
    modules = {}
    for name in TARGETS:
        row = connection.cursor().execute(
            "SELECT definition, uses_ansi_nulls, uses_quoted_identifier "
            "FROM sys.sql_modules WHERE object_id=OBJECT_ID(?, N'P')", "dbo." + name).fetchone()
        if row is None or row[0] is None:
            raise ValueError(f"Missing or unreadable procedure: {name}")
        modules[name] = {"definition": row[0], "ansi_nulls": bool(row[1]), "quoted_identifier": bool(row[2])}
    return modules


def lock(connection):
    if connection.autocommit:
        raise ValueError("Repair requires an owned non-autocommit connection")
    # ODBC autocommit-off uses IMPLICIT_TRANSACTIONS. An explicit BEGIN at
    # transaction count zero can open two nested transactions on SQL Server;
    # committing just one would leave the repair vulnerable to a later rollback.
    # A table SELECT starts the driver's single implicit transaction instead.
    connection.cursor().execute("SELECT object_id FROM sys.objects WHERE object_id=OBJECT_ID(?)",
                                "dbo." + TARGETS[0]).fetchall()
    if connection.cursor().execute("SELECT @@TRANCOUNT").fetchone()[0] != 1:
        raise ValueError("Repair requires exactly one owned SQL transaction")
    row = connection.cursor().execute(
        "DECLARE @r int; EXEC @r=sys.sp_getapplock "
        "@Resource=N'sdgo.rank_progression_fix.v1', @LockMode='Exclusive', "
        "@LockOwner='Transaction', @LockTimeout=0; SELECT @r").fetchone()
    if row is None or row[0] < 0:
        raise ValueError("Another rank repair transaction is running")


def alter(connection, name, definition, options):
    cursor = connection.cursor()
    cursor.execute("SET ANSI_NULLS " + ("ON" if options["ansi_nulls"] else "OFF"))
    cursor.execute("SET QUOTED_IDENTIFIER " + ("ON" if options["quoted_identifier"] else "OFF"))
    cursor.execute(as_alter(definition, name))


def same_module(current, expected_hash, options, name):
    return (module_hash(current["definition"], name) == expected_hash
            and current["ansi_nulls"] == options["ansi_nulls"]
            and current["quoted_identifier"] == options["quoted_identifier"])


def apply(connection, backup_path: Path):
    """Atomic two-procedure repair. Caller owns a fresh, non-autocommit connection."""
    try:
        lock(connection)
        modules = read_modules(connection)
        plans = [plan(n, modules[n]["definition"]) for n in TARGETS]
        changed = [p for p in plans if p.state == "affected"]
        if not changed:
            connection.rollback()
            return {"changed": 0, "already_fixed": len(plans)}
        database = connection.cursor().execute("SELECT DB_NAME()").fetchone()[0]
        backup = {"fix": "rank_progression_fix", "version": VERSION, "database": database,
                  "created_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                  "procedures": [{"name": p.name, "definition": p.original,
                      "original_hash": p.original_hash, "installed_hash": p.installed_hash,
                      "ansi_nulls": modules[p.name]["ansi_nulls"],
                      "quoted_identifier": modules[p.name]["quoted_identifier"]} for p in changed]}
        backup_path = Path(backup_path)
        backup_path.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation: never replace a previous recovery copy.
        with backup_path.open("x", encoding="utf-8", newline="\n") as f:
            json.dump(backup, f, ensure_ascii=False, indent=2)
            f.write("\n")
            f.flush()
            import os
            os.fsync(f.fileno())
        for p in changed:
            current = read_modules(connection)[p.name]
            if not same_module(current, p.original_hash, modules[p.name], p.name):
                raise ValueError(f"Procedure changed after preflight: {p.name}")
            alter(connection, p.name, p.updated, modules[p.name])
        installed = read_modules(connection)
        if any(not same_module(installed[p.name], p.installed_hash, modules[p.name], p.name) for p in changed):
            raise ValueError("SQL module readback differs from the intended repair")
        connection.commit()
        return {"changed": len(changed), "already_fixed": len(plans)-len(changed), "backup": str(backup_path)}
    except BaseException:
        connection.rollback()
        raise


def rollback(connection, backup_path: Path):
    try:
        backup = json.loads(Path(backup_path).read_text(encoding="utf-8-sig"))
        if backup.get("fix") != "rank_progression_fix" or backup.get("version") != VERSION:
            raise ValueError("Not a supported rank repair backup")
        entries = backup.get("procedures", [])
        names = [e["name"] for e in entries]
        if not names or len(set(names)) != len(names) or any(n not in TARGETS for n in names):
            raise ValueError("Backup procedure scope is invalid")
        if backup["database"] != connection.cursor().execute("SELECT DB_NAME()").fetchone()[0]:
            raise ValueError("Backup belongs to a different database")
        # Validate every entry before executing the first ALTER.
        for e in entries:
            p = plan(e["name"], e["definition"])
            if (p.state != "affected" or p.original_hash != e["original_hash"]
                or p.installed_hash != e["installed_hash"]
                or type(e["ansi_nulls"]) is not bool or type(e["quoted_identifier"]) is not bool):
                raise ValueError("Backup definition or fingerprint is invalid")
        lock(connection)
        modules = read_modules(connection)
        changed = []
        for e in entries:
            current = modules[e["name"]]
            if same_module(current, e["original_hash"], e, e["name"]):
                continue
            if not same_module(current, e["installed_hash"], e, e["name"]):
                raise ValueError(f"Refusing to overwrite a subsequently edited procedure: {e['name']}")
            changed.append(e)
        for e in changed:
            alter(connection, e["name"], e["definition"], e)
        restored = read_modules(connection)
        if any(not same_module(restored[e["name"]], e["original_hash"], e, e["name"]) for e in changed):
            raise ValueError("Restored module readback differs from its backup")
        connection.commit()
        return {"restored": len(changed), "already_restored": len(entries)-len(changed)}
    except BaseException:
        connection.rollback()
        raise


def connect_ini(path):
    import pyodbc
    ini = configparser.ConfigParser(interpolation=None)
    with Path(path).open(encoding="utf-8-sig") as f:
        ini.read_file(f)
    db = ini["DB Game"]
    drivers = [d for d in pyodbc.drivers() if "SQL Server" in d]
    if not drivers:
        raise ValueError("No installed SQL Server ODBC driver")
    def quote(value):
        return "{" + str(value).replace("}", "}}") + "}"
    text = (f"DRIVER={quote(drivers[-1])};SERVER={quote(db['DBServerName'])};"
            f"DATABASE={quote(db['DBName'])};UID={quote(db['ID'])};PWD={quote(db['PWD'])};")
    connection = pyodbc.connect(text, timeout=5, autocommit=False)
    connection.timeout = 30
    return connection


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["audit", "apply", "rollback"], nargs="?", default="audit")
    parser.add_argument("--server-ini", type=Path, required=True)
    parser.add_argument("--backup", type=Path)
    args = parser.parse_args()
    if args.mode == "rollback" and args.backup is None:
        parser.error("Rollback requires --backup")
    if args.mode == "apply" and args.backup is None:
        stamp = datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        args.backup = args.server_ini.resolve().parent / "sdgo" / "backups" / "rank_progression_fix" / (stamp + "-" + uuid.uuid4().hex[:8] + ".json")
    connection = connect_ini(args.server_ini)
    try:
        if args.mode == "audit":
            modules = read_modules(connection)
            result = {"procedures": [{"name": n, "state": plan(n, modules[n]["definition"]).state}
                                      for n in TARGETS]}
            connection.rollback()
        elif args.mode == "apply":
            result = apply(connection, args.backup)
        else:
            result = rollback(connection, args.backup)
        print(json.dumps(result, ensure_ascii=False))
    finally:
        connection.close()


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, KeyError) as exc:
        raise SystemExit(str(exc))
    except Exception as exc:
        # Driver errors can contain connection details. Keep credentials local.
        raise SystemExit(f"SQL operation failed ({type(exc).__name__}); run audit and inspect the retained backup before retrying") from None
