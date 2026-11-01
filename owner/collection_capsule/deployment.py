"""Local owner connection, catalog policy upgrades and form registration. No credentials enter packages."""
from __future__ import annotations

import configparser
import contextlib
import hashlib
import json
import os
from pathlib import Path
from datetime import datetime, timezone

from .metadata import imported_roots, validate_source


def connection(server_ini: Path, section: str = "DB Game"):
    import pyodbc
    raw = server_ini.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = raw.decode("utf-16")
    else:
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            if os.name != "nt":
                raise ValueError("Server INI must be UTF-8/UTF-16 on this platform") from None
            text = raw.decode("mbcs")
    cfg = configparser.ConfigParser(interpolation=None)
    try:
        cfg.read_string(text)
        db = cfg[section]
    except (configparser.Error, KeyError):
        raise ValueError("Unable to parse host database configuration") from None

    def escaped(value):
        return "{" + str(value).replace("}", "}}") + "}"

    value = ("DRIVER={SQL Server};SERVER=" + escaped(db["DBServerName"])
             + ";DATABASE=" + escaped(db["DBName"]) + ";UID=" + escaped(db["ID"])
             + ";PWD=" + escaped(db["PWD"]) + ";")
    try:
        result = pyodbc.connect(value, autocommit=True, timeout=10)
        result.timeout = 30
        return result
    except pyodbc.Error as error:
        raise RuntimeError(f"Owner SQL connection failed (SQLSTATE {str(error.args[0])[:5]})") from None


def upgrade_catalog(db):
    """Apply the owned, data-only catalog policy upgrade without a World restart."""
    from .sql import catalog_upgrade
    cursor = db.cursor()
    try:
        cursor.execute(catalog_upgrade())
        while cursor.nextset():
            pass
        row = cursor.execute("SELECT SchemaVersion,MachineID,PriceGP,Enabled FROM sdgo_collection_capsule.State WHERE Singleton=1").fetchone()
        if row is None or row[0] != 3 or row[2] != 100000:
            raise RuntimeError("Catalog policy upgrade readback differs")
        old = cursor.execute("SELECT COUNT(*) FROM sys.tables WHERE schema_id=SCHEMA_ID('sdgo_collection_capsule') AND name IN ('Seen','Coverage','DrawLog')").fetchone()[0]
        if old:
            raise RuntimeError("Obsolete history tables remain after catalog upgrade")
        return {"applied": True, "schema_version": 3, "machine_id": row[1],
                "price_gp": row[2], "enabled": bool(row[3]),
                "collection_policy": "catalog_then_inventory", "history_tables_removed": True,
                "world_restart_required": False}
    finally:
        cursor.close()


def register_unit_family(db, manifest: dict, *, in_transaction: bool = False) -> dict:
    """Importer callback: publish root metadata in the unit-install transaction.

    No capsule ID list or DLL rebuild is needed. The caller supplies the content
    package's explicit inventory root and its complete live transform family.
    """
    forms = manifest.get("family", {}).get("forms", [])
    ids = [form.get("id") for form in forms]
    if not 1 <= len(ids) <= 32 or any(type(uid) is not int or not 0 < uid <= 2147483647 for uid in ids):
        raise ValueError("Invalid imported form IDs")
    cursor = db.cursor()
    owned = False
    try:
        count = cursor.execute("SELECT @@TRANCOUNT").fetchone()[0]
        if in_transaction and count == 0:
            raise ValueError("The unit importer must publish metadata in its active transaction")
        if not in_transaction:
            if count:
                raise ValueError("Caller transaction already active; use in_transaction=True")
            cursor.execute("SET XACT_ABORT ON;BEGIN TRANSACTION;")
            owned = True
        marker = cursor.execute("""SELECT CONVERT(nvarchar(128),value) FROM sys.extended_properties
WHERE class=3 AND major_id=SCHEMA_ID(N'sdgo_collection_capsule') AND name=N'sdgo_owner'""").fetchone()
        if not marker or marker[0] != "collection_capsule/v1":
            raise ValueError("Collection schema ownership differs")
        placeholders = ",".join("?" for _ in ids)
        cursor.execute("SELECT UnitNumber,UnitNumber_Transform,SallyReady,UnitGradeNumber,UnitGradeNumber2,UnitGraph,UnitModelName "
                       "FROM dbo.tbl_Units WITH(UPDLOCK,HOLDLOCK) WHERE UnitNumber IN (" + placeholders + ")", *ids)
        columns = [column[0] for column in cursor.description]
        rows = [dict(zip(columns, row)) for row in cursor.fetchall()]
        validate_source(rows)
        roots = imported_roots(rows, [manifest])
        root = next(iter(roots))
        package = manifest["package_id"]
        revision = roots[root].rsplit(":", 1)[1]
        conflicts = cursor.execute("SELECT UnitID FROM sdgo_collection_capsule.ImportedForms WHERE UnitID IN ("
                                   + placeholders + ") AND PackageID<>?", *ids, package).fetchall()
        if conflicts:
            raise ValueError("Imported form ID belongs to another content package")
        old = [r[0] for r in cursor.execute("SELECT UnitID FROM sdgo_collection_capsule.ImportedForms WHERE PackageID=?", package)]
        affected = sorted(set(ids + old))
        cursor.execute("DELETE sdgo_collection_capsule.Roots WHERE UnitID IN (" + ",".join("?" for _ in affected) + ")", *affected)
        cursor.execute("DELETE sdgo_collection_capsule.ImportedForms WHERE PackageID=?", package)
        for uid in ids:
            cursor.execute("INSERT sdgo_collection_capsule.ImportedForms VALUES(?,?,?,?)", uid, root, package, revision)
        for row in rows:
            values = (int(row.get("UnitNumber_Transform") or 0), row["SallyReady"], row["UnitGradeNumber"],
                      row["UnitGradeNumber2"], row["UnitGraph"],
                      int(row["UnitModelName"].strip().lower() == "name" and row["UnitGraph"] == 1010001))
            cursor.execute("""UPDATE sdgo_collection_capsule.UnitSource SET TransformTo=?,Ready=?,UnitRank=?,UnitSubgrade=?,UnitGraph=?,Placeholder=?,Origin='import'
WHERE UnitID=?; IF @@ROWCOUNT=0 INSERT sdgo_collection_capsule.UnitSource(UnitID,TransformTo,Ready,UnitRank,UnitSubgrade,UnitGraph,Placeholder,Origin) VALUES(?,?,?,?,?,?,?,'import');""",
                           *values, row["UnitNumber"], row["UnitNumber"], *values)
        item = next(row for row in rows if row["UnitNumber"] == root)
        cursor.execute("INSERT sdgo_collection_capsule.Roots(UnitID,UnitRank,UnitSubgrade,Origin) VALUES(?,?,?,'import')",
                       root, item["UnitGradeNumber"], item["UnitGradeNumber2"])
        if owned:
            cursor.execute("COMMIT TRANSACTION;")
            owned = False
    except Exception:
        if owned:
            with contextlib.suppress(Exception):
                cursor.execute("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;")
        raise
    finally:
        cursor.close()
    return {"root_form": root, "forms": len(ids), "dll_rebuild_required": False,
            "metadata_revision": roots[root].rsplit(":", 1)[1]}


def export_configuration(db, output: Path) -> dict:
    """Export unit definitions and public content metadata, never player rows."""
    output.mkdir(parents=True, exist_ok=False)
    game = output / "GOnlineGame"
    game.mkdir()
    files = {}

    def save(name, value):
        raw = (json.dumps(value, ensure_ascii=False, default=str, allow_nan=False) + "\n").encode()
        path = game / name
        path.write_bytes(raw)
        files[path.relative_to(output).as_posix()] = {"sha256": hashlib.sha256(raw).hexdigest(), "rows": len(value)}

    cursor = db.cursor()
    try:
        for table in ("tbl_Units", "tbl_MachinePrice", "tbl_ItemMachinePrice", "tbl_ItemCategory"):
            cursor.execute("SELECT * FROM dbo.[" + table + "]")
            keys = [column[0] for column in cursor.description]
            rows = [dict(zip(keys, row)) for row in cursor.fetchall()]
            save("dbo." + table + ".json", rows)
        marker = cursor.execute("SELECT CASE WHEN OBJECT_ID(N'sdgo_collection_capsule.ImportedForms',N'U') IS NULL THEN 0 ELSE 1 END").fetchone()[0]
        imported = []
        if marker:
            cursor.execute("""SELECT f.UnitID,f.RootUnitID,f.PackageID,f.RevisionHash,s.TransformTo,s.Ready,s.UnitRank,s.UnitSubgrade,s.UnitGraph,s.Placeholder
FROM sdgo_collection_capsule.ImportedForms f JOIN sdgo_collection_capsule.UnitSource s ON s.UnitID=f.UnitID""")
            keys = [column[0] for column in cursor.description]
            imported = [dict(zip(keys, row)) for row in cursor.fetchall()]
        save("collection.imported-forms.json", imported)
        owned = []
        if cursor.execute("SELECT CASE WHEN OBJECT_ID(N'sdgo_collection_capsule.OwnedUnitItems',N'U') IS NULL THEN 0 ELSE 1 END").fetchone()[0]:
            cursor.execute('SELECT UnitID,CategoryID,Name FROM sdgo_collection_capsule.OwnedUnitItems')
            keys = [column[0] for column in cursor.description]
            owned = [dict(zip(keys, row)) for row in cursor.fetchall()]
        save('collection.unit-items.json', owned)
    finally:
        cursor.close()
    meta = {"schema_version": 1, "scope": "public unit and machine definitions only; no player data",
            "exported_at": datetime.now(timezone.utc).isoformat(), "files": files}
    (output / "_meta.json").write_text(json.dumps(meta, indent=2) + "\n", encoding="utf-8")
    return {"snapshot": str(output), "files": len(files)}
