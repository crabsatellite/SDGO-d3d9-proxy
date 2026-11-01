"""Resolve native inventory roots from the shipped client catalog.

The native ``0220.gdt`` catalog identifies the initial/acquirable unit record
for transform families.  This module joins those entries to the six native unit
tables and the live SQL unit graph.  It returns metadata only for families that
need an explicit root; ordinary unambiguous units remain dynamic in ``pool``.
"""
from __future__ import annotations

from collections import deque
import hashlib
import json
import math
from pathlib import Path
import struct

from .client import (
    ArchiveReader,
    HOST_SIZE_OF_IMAGE,
    HOST_TIMESTAMP,
    NATIVE_CONTRACT,
    _assert_existing_directory,
    _assert_existing_file,
    _read_pe_host,
)
from .metadata import validate_source
from .pool import ownable, placeholder


CATALOG_FILE = "0220"
UNIT_FILES = ("0237", "0242", "0243", "0244", "0246", "0245")
CATALOG_RECORD_SIZE = 614
UNIT_RECORD_SIZE = 4976
MAX_GDT_BYTES = 64 * 1024 * 1024
MAX_RECORDS = 100_000


class NativeCatalogError(ValueError):
    """The native catalog cannot safely identify inventory roots."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _canonical_sha256(value) -> str:
    raw = json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    ).encode("utf-8")
    return _sha256(raw)


def _decode_gdt(data: bytes, expected_size: int, label: str) -> list[bytes]:
    if len(data) > MAX_GDT_BYTES:
        raise NativeCatalogError(f"{label} exceeds the supported size")
    if len(data) < 14 or data[:2] != b"\xf9\xf8":
        raise NativeCatalogError(f"{label} has an unsupported GDT header")
    size, count, extra = struct.unpack_from("<III", data, 2)
    if size != expected_size or count > MAX_RECORDS:
        raise NativeCatalogError(f"{label} has an unsupported record layout")
    key_size = (size + count + extra) % 65 + 192
    key = data[14 : 14 + key_size]
    if (
        len(key) != key_size
        or len(set(key)) != key_size
        or set(key) != set(range(key_size))
    ):
        raise NativeCatalogError(f"{label} has an invalid permutation key")
    stride = math.ceil(size / key_size) * key_size
    if len(data) != 14 + key_size + count * stride:
        raise NativeCatalogError(f"{label} is truncated or has trailing bytes")
    mask = bytes(key[(0x300 + index * 3) % key_size] for index in range(key_size))
    rows = []
    for record_index in range(count):
        start = 14 + key_size + record_index * stride
        record = bytearray()
        for offset in range(0, stride, key_size):
            chunk = data[start + offset : start + offset + key_size]
            remaining = min(key_size, size - len(record))
            record.extend(chunk[key[index]] ^ mask[index] for index in range(remaining))
        rows.append(bytes(record))
    return rows


def _read_loose(path: Path, label: str) -> bytes:
    path = _assert_existing_file(path, label)
    if path.stat().st_size > MAX_GDT_BYTES:
        raise NativeCatalogError(f"{label} exceeds the supported size")
    return path.read_bytes()


def _client_resources(
    *, client_root: Path | None, client_gdrs: Path | None
) -> tuple[dict[str, bytes], dict]:
    if (client_root is None) == (client_gdrs is None):
        raise NativeCatalogError("Specify exactly one of client_root or client_gdrs")
    names = (CATALOG_FILE,) + UNIT_FILES
    if client_gdrs is not None:
        gdrs = _assert_existing_directory(client_gdrs, "client GDRS directory")
        return (
            {
                name: _read_loose(gdrs / f"{name}.gdt", f"{name}.gdt")
                for name in names
            },
            {"mode": "loose-gdrs"},
        )

    root = _assert_existing_directory(client_root, "client root")
    executable = _assert_existing_file(root / "GOnline.exe", "GOnline.exe")
    executable_raw = executable.read_bytes()
    executable_sha256 = _sha256(executable_raw)
    if executable_sha256 != NATIVE_CONTRACT["host_sha256"]:
        raise NativeCatalogError(
            "Unsupported GOnline.exe bytes for native catalog resolution"
        )
    timestamp, image_size = _read_pe_host(executable)
    if timestamp != HOST_TIMESTAMP or image_size != HOST_SIZE_OF_IMAGE:
        raise NativeCatalogError(
            "Unsupported GOnline.exe host profile for native catalog resolution"
        )
    reader = ArchiveReader(
        root / "GPackData.zdx",
        root / "GPackData.zpk",
        loose_root=root,
    )
    return (
        {
            name: reader.read(f"gdrs/{name}.gdt", limit=MAX_GDT_BYTES)
            for name in names
        },
        {
            "mode": "client-pack",
            "host_sha256": executable_sha256,
            "host_timestamp": f"{timestamp:08x}",
            "host_size_of_image": image_size,
            "archive_index_sha256": reader.index_sha256,
        },
    )


def _catalog_entries(
    records: list[bytes],
) -> tuple[dict[int, dict], dict[str, int]]:
    enabled = {}
    entry_ids = set()
    security_filtered = 0
    disabled = 0
    released = 0
    for index, record in enumerate(records):
        entry_id = struct.unpack_from("<I", record, 0)[0]
        if entry_id == 0 or entry_id in entry_ids:
            raise NativeCatalogError("0220.gdt has an invalid or duplicate entry ID")
        entry_ids.add(entry_id)
        if record[4] != 0:
            security_filtered += 1
            continue
        released += 1
        if record[5] not in (0, 1):
            raise NativeCatalogError("0220.gdt has an invalid enabled byte")
        if record[5] == 0:
            disabled += 1
            continue
        unit_id = struct.unpack_from("<I", record, 6)[0]
        if not 0 < unit_id <= 0x7FFFFFFF:
            raise NativeCatalogError("0220.gdt has an invalid unit ID")
        if unit_id in enabled:
            raise NativeCatalogError(
                f"0220.gdt enables unit {unit_id} more than once"
            )
        enabled[unit_id] = {
            "entry_id": entry_id,
            "record_index": index,
            "record_sha256": _sha256(record),
        }
    return enabled, {
        "catalog_records": len(records),
        "released_catalog_records": released,
        "security_filtered_catalog_records": security_filtered,
        "disabled_catalog_records": disabled,
        "enabled_catalog_entries": len(enabled),
    }


def _client_units(
    resources: dict[str, bytes],
) -> tuple[dict[int, bytes], dict[str, str], int]:
    units = {}
    hashes = {}
    security_filtered = 0
    for name in UNIT_FILES:
        data = resources[name]
        hashes[f"{name}.gdt"] = _sha256(data)
        for record in _decode_gdt(data, UNIT_RECORD_SIZE, f"{name}.gdt"):
            if record[4] != 0:
                security_filtered += 1
                continue
            unit_id = struct.unpack_from("<I", record, 0)[0]
            if not 0 < unit_id <= 0x7FFFFFFF:
                raise NativeCatalogError(f"{name}.gdt has an invalid unit ID")
            previous = units.get(unit_id)
            if previous is not None and previous != record:
                raise NativeCatalogError(
                    f"Native unit {unit_id} has conflicting client definitions"
                )
            units[unit_id] = record
    return units, hashes, security_filtered


def _components(units: dict[int, dict]) -> list[list[int]]:
    graph = {unit_id: set() for unit_id in units}
    for unit_id, row in units.items():
        target = int(row.get("UnitNumber_Transform") or 0)
        if target in units:
            graph[unit_id].add(target)
            graph[target].add(unit_id)
    result = []
    remaining = set(units)
    while remaining:
        start = min(remaining)
        queue = deque([start])
        component = set()
        while queue:
            unit_id = queue.popleft()
            if unit_id in component:
                continue
            component.add(unit_id)
            queue.extend(graph[unit_id] - component)
        remaining -= component
        result.append(sorted(component))
    return result


def _source_digest(rows: list[dict]) -> str:
    normalized = [
        {
            "unit_id": row["UnitNumber"],
            "transform_to": int(row.get("UnitNumber_Transform") or 0),
            "ready": row["SallyReady"],
            "rank": row["UnitGradeNumber"],
            "subgrade": row["UnitGradeNumber2"],
            "graph": row["UnitGraph"],
            "name": row["UnitModelName"],
        }
        for row in sorted(rows, key=lambda item: item["UnitNumber"])
    ]
    return _canonical_sha256(normalized)


def resolve_native_roots(
    rows: list[dict],
    *,
    client_root: Path | None = None,
    client_gdrs: Path | None = None,
) -> dict:
    """Return native roots that need explicit acquisition metadata.

    ``client_root`` reads the verified shipped client pack.  ``client_gdrs`` is
    the offline/fixture path for already extracted tables and therefore has no
    executable provenance claim.
    """
    validate_source(rows)
    if len({row["UnitNumber"] for row in rows}) != len(rows):
        raise NativeCatalogError("Live unit source contains duplicate IDs")
    sql_units = {row["UnitNumber"]: row for row in rows}
    resources, source_audit = _client_resources(
        client_root=client_root, client_gdrs=client_gdrs
    )
    catalog_raw = resources[CATALOG_FILE]
    catalog_records = _decode_gdt(
        catalog_raw, CATALOG_RECORD_SIZE, f"{CATALOG_FILE}.gdt"
    )
    catalog, catalog_counts = _catalog_entries(catalog_records)
    client_units, unit_hashes, security_filtered_units = _client_units(resources)

    missing_client = sorted(set(catalog) - set(client_units))
    if missing_client:
        raise NativeCatalogError(
            "Enabled native catalog entries lack client unit definitions: "
            + ",".join(map(str, missing_client[:16]))
        )

    resolved_entries = {}
    unresolved = []
    metadata_needed = 0
    for component in _components(sql_units):
        ownable_ids = [
            unit_id for unit_id in component if ownable(sql_units[unit_id])
        ]
        needs_metadata = len(ownable_ids) > 1 or any(
            ownable(sql_units[unit_id]) and placeholder(sql_units[unit_id])
            for unit_id in component
        )
        if not needs_metadata:
            continue
        metadata_needed += 1

        missing_targets = sorted(
            {
                int(sql_units[unit_id].get("UnitNumber_Transform") or 0)
                for unit_id in component
                if int(sql_units[unit_id].get("UnitNumber_Transform") or 0)
                and int(sql_units[unit_id].get("UnitNumber_Transform") or 0)
                not in sql_units
            }
        )
        if missing_targets:
            unresolved.append(
                {
                    "forms": component,
                    "reason": "missing_live_transform_target",
                    "targets": missing_targets,
                }
            )
            continue

        client_missing = [unit_id for unit_id in component if unit_id not in client_units]
        if client_missing:
            unresolved.append(
                {
                    "forms": component,
                    "reason": "missing_client_form_definition",
                    "forms_missing": client_missing,
                }
            )
            continue
        mismatched = []
        for unit_id in component:
            client_target = struct.unpack_from("<I", client_units[unit_id], 316)[0]
            sql_target = int(sql_units[unit_id].get("UnitNumber_Transform") or 0)
            if client_target != sql_target:
                mismatched.append(
                    {
                        "unit_id": unit_id,
                        "sql_target": sql_target,
                        "client_target": client_target,
                    }
                )
        if mismatched:
            raise NativeCatalogError(
                "Live/client transform references differ for metadata-needed "
                f"family {component}: {mismatched}"
            )

        entries = [unit_id for unit_id in component if unit_id in catalog]
        ownable_entries = [
            unit_id for unit_id in entries if ownable(sql_units[unit_id])
        ]
        if len(entries) != 1:
            unresolved.append(
                {
                    "forms": component,
                    "reason": (
                        "no_enabled_catalog_entry"
                        if not entries
                        else "multiple_enabled_catalog_entries"
                    ),
                    "catalog_forms": entries,
                }
            )
            continue
        if len(ownable_entries) != 1:
            unresolved.append(
                {
                    "forms": component,
                    "reason": "catalog_entry_is_not_ownable",
                    "catalog_forms": entries,
                }
            )
            continue
        root = ownable_entries[0]
        resolved_entries[root] = catalog[root]

    source_sha256 = _source_digest(rows)
    catalog_sha256 = _sha256(catalog_raw)
    revision_input = {
        "schema": 1,
        "catalog_sha256": catalog_sha256,
        "unit_table_sha256": unit_hashes,
        "source_sha256": source_sha256,
        "resolved_entries": {
            str(unit_id): resolved_entries[unit_id]["entry_id"]
            for unit_id in sorted(resolved_entries)
        },
        "unresolved": unresolved,
    }
    revision = _canonical_sha256(revision_input)
    roots = {
        unit_id: (
            f"native-catalog:{revision}:entry"
            f"{resolved_entries[unit_id]['entry_id']}"
        )
        for unit_id in sorted(resolved_entries)
    }
    audit = {
        **source_audit,
        **catalog_counts,
        "catalog_sha256": catalog_sha256,
        "unit_table_sha256": unit_hashes,
        "source_sha256": source_sha256,
        "client_unit_records": len(client_units),
        "security_filtered_unit_records": security_filtered_units,
        "metadata_needed_families": metadata_needed,
        "resolved_families": len(roots),
        "unresolved_families": len(unresolved),
        "unresolved": unresolved,
    }
    return {"roots": roots, "revision": revision, "audit": audit}
