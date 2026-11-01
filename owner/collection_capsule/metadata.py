"""Consume inventory-root declarations produced by unit-content importers.

These are unit-family metadata, not a capsule prize allowlist. The capsule still
enumerates the live catalog and applies acquisition history for every draw.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path


def imported_roots(rows: list[dict], manifests: list[dict]) -> dict[int, str]:
    units = {row["UnitNumber"]: row for row in rows}
    roots = {}
    claimed = {}
    for manifest in manifests:
        if (manifest.get("protocol") != "sdgo.unit-family"
                or manifest.get("protocol_version") != 1 or manifest.get("installable") is not True):
            raise ValueError("Expected an installable sdgo.unit-family v1 declaration")
        package = manifest.get("package_id")
        family = manifest.get("family", {})
        forms = family.get("forms", [])
        if not isinstance(package, str) or not 1 <= len(package) <= 128 or not 1 <= len(forms) <= 32:
            raise ValueError("Unit family identity/forms are missing or too large")
        by_key = {}
        ids = set()
        for form in forms:
            uid = form.get("id")
            key = form.get("key")
            if (type(uid) is not int or uid not in units or uid in ids
                    or not isinstance(key, str) or not key or key in by_key):
                raise ValueError("Imported family has duplicate, unknown or invalid forms")
            if form.get("transform_target") != (units[uid].get("UnitNumber_Transform") or 0):
                raise ValueError("Imported form transform reference differs from the live catalog")
            if uid in claimed and claimed[uid] != package:
                raise ValueError("Two imported families claim the same native form ID")
            ids.add(uid)
            by_key[key] = uid
            claimed[uid] = package
        root = family.get("root_form")
        if isinstance(root, str):
            root = by_key.get(root)
        if type(root) is not int or root not in ids or units[root].get("SallyReady") != 1:
            raise ValueError("Imported family must identify one independently ownable inventory root")
        if any((units[uid].get("UnitNumber_Transform") or 0) not in ids | {0} for uid in ids):
            raise ValueError("Imported family is missing a referenced transform form")
        if root in roots:
            raise ValueError("An inventory root was declared more than once")
        digest = hashlib.sha256(json.dumps(manifest, sort_keys=True, ensure_ascii=False,
                                          separators=(",", ":")).encode()).hexdigest()
        roots[root] = f"unit-family:{package}:{digest}"
    return roots


def validate_source(rows: list[dict]) -> None:
    if not isinstance(rows, list) or not 1 <= len(rows) <= 20000:
        raise ValueError("Unit source must contain 1..20000 records")
    keys = ("UnitNumber", "UnitNumber_Transform", "SallyReady", "UnitGradeNumber", "UnitGradeNumber2", "UnitGraph")
    seen = set()
    for row in rows:
        for key in keys:
            value = row.get(key)
            if key == "UnitNumber_Transform" and value is None:
                value = 0
            if type(value) is not int or not 0 <= value <= 2147483647:
                raise ValueError(f"Native {key} must be a non-negative SQL INT")
        if row["UnitNumber"] == 0 or row["UnitNumber"] in seen:
            raise ValueError("Unit source IDs must be positive and unique")
        if not isinstance(row.get("UnitModelName"), str) or len(row["UnitModelName"]) > 2000:
            raise ValueError("Native unit model name is invalid")
        seen.add(row["UnitNumber"])


def registered_imports(rows: list[dict], snapshot: Path, *, ignore_packages: set[str] | None = None) -> tuple[dict[int, str], set[int]]:
    """Recover published importer metadata during feature updates without a new manifest."""
    path = snapshot / "GOnlineGame/collection.imported-forms.json"
    if not path.is_file():
        return {}, set()
    raw = path.read_bytes()
    meta = json.loads((snapshot / "_meta.json").read_text(encoding="utf-8-sig"))
    expected = meta.get("files", {}).get(path.relative_to(snapshot).as_posix(), {}).get("sha256")
    if not expected or hashlib.sha256(raw).hexdigest() != expected:
        raise ValueError("Imported form registry differs from its snapshot receipt")
    units = {row["UnitNumber"]: row for row in rows}
    groups = {}
    claimed = set()
    for item in json.loads(raw.decode("utf-8-sig")):
        if item.get("PackageID") in (ignore_packages or set()):
            continue
        uid, root = item.get("UnitID"), item.get("RootUnitID")
        if type(uid) is not int or type(root) is not int or uid not in units or root not in units or uid in claimed:
            raise ValueError("Invalid published importer form identity")
        source = units[uid]
        if any(item.get(key) != value for key, value in {
            "TransformTo": source.get("UnitNumber_Transform") or 0, "Ready": source["SallyReady"],
            "UnitRank": source["UnitGradeNumber"], "UnitSubgrade": source["UnitGradeNumber2"], "UnitGraph": source["UnitGraph"]}.items()):
            raise ValueError("Published importer form metadata is stale")
        package, revision = item.get("PackageID"), item.get("RevisionHash")
        if (not isinstance(package, str) or not 1 <= len(package) <= 128 or not isinstance(revision, str)
                or len(revision) != 64 or any(c not in "0123456789abcdef" for c in revision)):
            raise ValueError("Invalid published importer revision")
        key = (package, revision, root)
        groups.setdefault(key, set()).add(uid)
        claimed.add(uid)
    roots = {}
    for (package, revision, root), ids in groups.items():
        if root not in ids or units[root]["SallyReady"] != 1:
            raise ValueError("Published importer root is missing or unownable")
        if any((units[uid].get("UnitNumber_Transform") or 0) not in ids | {0} for uid in ids):
            raise ValueError("Published importer family is incomplete")
        if root in roots:
            raise ValueError("A published importer root has conflicting declarations")
        roots[root] = f"unit-family:{package}:{revision}"
    return roots, claimed
