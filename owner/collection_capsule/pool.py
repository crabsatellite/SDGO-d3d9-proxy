"""Build a conservative pool of independently ownable, pre-transform units.

No smallest-ID/name heuristic is used to choose a form. A mutually reachable
group with multiple ownable forms is excluded until reviewed explicitly.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
from typing import Iterable


def ownable(row: dict) -> bool:
    return (type(row.get("UnitNumber")) is int and row["UnitNumber"] > 0
            and row.get("SallyReady") == 1
            and row.get("UnitGradeNumber") in (1, 2, 3, 4)
            and row.get("UnitGradeNumber2") in (2, 3, 4, 5))


def placeholder(row: dict) -> bool:
    return (str(row.get("UnitModelName", "")).strip().lower() == "name"
            and row.get("UnitGraph") == 1010001)


def forward(start: int, units: dict[int, dict]) -> set[int]:
    result = set()
    current = start
    while current in units and current not in result:
        result.add(current)
        current = units[current].get("UnitNumber_Transform") or 0
    return result


def build_pool(rows: Iterable[dict], *, verified_bases: dict[int, str] | None = None) -> dict:
    rows = list(rows)
    from .metadata import validate_source
    validate_source(rows)
    rows.sort(key=lambda row: row["UnitNumber"])
    units = {row["UnitNumber"]: row for row in rows}
    if len(units) != len(rows) or any(type(uid) is not int or uid <= 0 for uid in units):
        raise ValueError("Unit IDs must be unique positive integers")
    verified_bases = verified_bases or {}
    if any(uid not in units or not ownable(units[uid]) or not isinstance(note, str)
           or not note.strip() for uid, note in verified_bases.items()):
        raise ValueError("Verified bases need an ownable unit and a review/source note")
    # Some real legacy families have placeholder server names/graphs but valid
    # native client definitions. Verified content metadata admits their root;
    # unverified stubs are still not treated as playable units.
    candidates = {uid for uid, row in units.items()
                  if ownable(row) and (not placeholder(row) or uid in verified_bases)}
    paths = {uid: forward(uid, units) for uid in candidates}
    predecessors = {uid: set() for uid in candidates}
    for uid, reached in paths.items():
        for other in (reached & candidates) - {uid}:
            predecessors[other].add(uid)
    selected = set()
    excluded = []
    for uid in sorted(units):
        if uid not in candidates:
            reason = "not_independently_ownable"
        else:
            mutual = {other for other in predecessors[uid] if other in paths[uid]} | {uid}
            upstream = predecessors[uid] - mutual
            reviewed = mutual & verified_bases.keys()
            if len(reviewed) > 1:
                raise ValueError(f"Multiple reviewed base forms in one transform cycle: {sorted(reviewed)}")
            if upstream:
                reason = "post_transform_state"
            elif len(mutual) > 1 and not reviewed:
                reason = "ambiguous_transform_cycle"
            elif reviewed and uid not in reviewed:
                reason = "reviewed_alternate_form"
            else:
                selected.add(uid)
                continue
        excluded.append({"unit_id": uid, "reason": reason})
    aliases = sorted({(form, base) for base in selected for form in paths[base]})
    pool = [{"unit_id": uid, "rank": units[uid]["UnitGradeNumber"],
             "subgrade": units[uid]["UnitGradeNumber2"],
             "name": str(units[uid].get("UnitModelName", uid)),
             "weight": 1,
             "basis": verified_bases.get(uid, "unique ownable transform root")}
            for uid in sorted(selected)]
    source = json.dumps(rows, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return {"schema_version": 1, "source_sha256": hashlib.sha256(source.encode()).hexdigest(),
            "policy": "never_acquired_base_forms", "units": pool,
            "form_aliases": [{"unit_id": form, "base_unit_id": base} for form, base in aliases],
            "excluded": excluded}


def load_units(snapshot: Path) -> list[dict]:
    root = snapshot / "GOnlineGame"
    files = sorted(root.glob("dbo.tbl_Units.part*.json"))
    single = root / "dbo.tbl_Units.json"
    if single.exists():
        if files:
            raise ValueError("Both complete and split unit tables are present")
        files = [single]
    if not files:
        raise ValueError("The configuration snapshot has no tbl_Units export")
    meta_path = snapshot / "_meta.json"
    metadata = json.loads(meta_path.read_text(encoding="utf-8-sig")) if meta_path.exists() else {}
    result = []
    for file in files:
        raw = file.read_bytes()
        expected = metadata.get("files", {}).get(file.relative_to(snapshot).as_posix(), {})
        if expected and hashlib.sha256(raw).hexdigest() != expected["sha256"]:
            raise ValueError("Unit configuration differs from its snapshot receipt")
        result.extend(json.loads(raw.decode("utf-8-sig")))
    return result
