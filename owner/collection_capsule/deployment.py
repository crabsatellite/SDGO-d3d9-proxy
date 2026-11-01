"""Local owner connection and history import. No credentials enter packages."""
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


def registration_baseline(server_ini: Path) -> int:
    """Read only the existing account-ID high water mark; never export accounts."""
    with contextlib.closing(connection(server_ini, section="DB Member")) as member:
        value = member.cursor().execute("SELECT ISNULL(MAX(f_idx),0) FROM dbo.tbl_Account").fetchone()[0]
    if type(value) is not int or not 0 <= value <= 2147483647:
        raise ValueError("Account registration baseline is invalid")
    return value


def import_history(db, payload: dict) -> dict:
    """Import a complete prior collection export, not a current-inventory reset.

    Completeness is an explicit assertion of the exporting system. Missing
    pre-install history cannot be reconstructed from current ownership alone.
    """
    account, values = payload.get("account"), payload.get("units")
    if (payload.get("schema_version") != 1 or payload.get("coverage") != "from-account-creation"
            or payload.get("complete") is not True or not payload.get("source")
            or type(account) is not int or not 0 < account <= 2147483647
            or not isinstance(values, list) or len(values) > 100000
            or any(type(uid) is not int or not 0 < uid <= 2147483647 for uid in values)):
        raise ValueError("A complete, source-identified collection history export is required")
    units = sorted(set(values))
    digest = hashlib.sha256(json.dumps(payload, sort_keys=True, ensure_ascii=False,
                                      separators=(",", ":")).encode()).hexdigest()
    cursor = db.cursor()
    try:
        cursor.execute("SET XACT_ABORT ON;BEGIN TRANSACTION;")
        cursor.execute("SELECT AccountNumber FROM sdgo_collection_capsule.Coverage WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber=?", account)
        if cursor.fetchone() is None:
            raise ValueError("Account profile is not registered in collection history")
        known = {row[0] for row in cursor.execute("SELECT UnitID FROM sdgo_collection_capsule.Seen WHERE AccountNumber=?", account)}
        if not known.issubset(units):
            raise ValueError("History import omits units already known to have been acquired")
        for uid in units:
            cursor.execute("""IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen WHERE AccountNumber=? AND UnitID=?)
INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source) VALUES(?,?,0,'imported_full_history')""",
                           account, uid, account, uid)
        cursor.execute("UPDATE sdgo_collection_capsule.Coverage SET Complete=1,Source='imported_full_history',ReceiptHash=? WHERE AccountNumber=?",
                       digest, account)
        cursor.execute("COMMIT TRANSACTION;")
    except Exception:
        with contextlib.suppress(Exception):
            cursor.execute("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;")
        raise
    finally:
        cursor.close()
    return {"account": account, "unit_count": len(units), "receipt_sha256": digest}


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
