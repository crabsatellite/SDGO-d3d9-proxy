import math
from pathlib import Path
import struct
import tempfile
import unittest

from owner.collection_capsule.native_catalog import (
    CATALOG_RECORD_SIZE,
    UNIT_FILES,
    UNIT_RECORD_SIZE,
    NativeCatalogError,
    resolve_native_roots,
)


def encode_gdt(size, rows, extra=0):
    if any(len(row) != size for row in rows):
        raise ValueError("fixture row size differs")
    count = len(rows)
    key_size = (size + count + extra) % 65 + 192
    key = bytes(range(key_size))
    stride = math.ceil(size / key_size) * key_size
    result = bytearray(b"\xf9\xf8" + struct.pack("<III", size, count, extra) + key)
    mask = bytes(key[(0x300 + index * 3) % key_size] for index in range(key_size))
    for row in rows:
        padded = row.ljust(stride, b"\0")
        for offset in range(0, stride, key_size):
            block = padded[offset : offset + key_size]
            encoded = bytearray(key_size)
            for index in range(key_size):
                encoded[key[index]] = block[index] ^ mask[index]
            result.extend(encoded)
    return bytes(result)


def unit_row(unit_id, target=0, security=0):
    row = bytearray(UNIT_RECORD_SIZE)
    struct.pack_into("<I", row, 0, unit_id)
    row[4] = security
    struct.pack_into("<I", row, 316, target)
    return bytes(row)


def catalog_row(entry_id, unit_id, *, security=0, enabled=1):
    row = bytearray(CATALOG_RECORD_SIZE)
    struct.pack_into("<I", row, 0, entry_id)
    row[4] = security
    row[5] = enabled
    struct.pack_into("<I", row, 6, unit_id)
    return bytes(row)


def sql_unit(unit_id, target=0, *, ready=1, name="unit", graph=None):
    return {
        "UnitNumber": unit_id,
        "UnitNumber_Transform": target,
        "SallyReady": ready,
        "UnitGradeNumber": 4,
        "UnitGradeNumber2": 4,
        "UnitGraph": graph if graph is not None else 1_000_000 + unit_id,
        "UnitModelName": name,
    }


class FixtureGdrs:
    def __init__(self, root, units, catalog, *, catalog_size=CATALOG_RECORD_SIZE):
        self.root = Path(root)
        self.root.mkdir()
        (self.root / "0220.gdt").write_bytes(encode_gdt(catalog_size, catalog))
        for index, name in enumerate(UNIT_FILES):
            rows = units if index == 0 else []
            (self.root / f"{name}.gdt").write_bytes(
                encode_gdt(UNIT_RECORD_SIZE, rows)
            )


class NativeCatalogTests(unittest.TestCase):
    def resolve(self, rows, units, catalog):
        with tempfile.TemporaryDirectory() as directory:
            gdrs = Path(directory) / "gdrs"
            FixtureGdrs(gdrs, units, catalog)
            return resolve_native_roots(rows, client_gdrs=gdrs)

    def test_catalog_entry_resolves_larger_reversed_id_without_name_heuristic(self):
        rows = [
            sql_unit(90002, 90001, name="MISLEADING (AFTER)"),
            sql_unit(90001, 90002, name="MISLEADING"),
        ]
        result = self.resolve(
            rows,
            [unit_row(90002, 90001), unit_row(90001, 90002)],
            [catalog_row(3_600_001, 90002)],
        )
        self.assertEqual(set(result["roots"]), {90002})
        self.assertRegex(
            result["roots"][90002],
            rf"^native-catalog:{result['revision']}:entry3600001$",
        )
        self.assertEqual(result["audit"]["metadata_needed_families"], 1)
        self.assertEqual(result["audit"]["unresolved_families"], 0)

    def test_legitimate_placeholder_root_requires_and_receives_catalog_proof(self):
        rows = [
            sql_unit(81000, name="name", graph=1010001),
            sql_unit(82000, name="ordinary"),
        ]
        result = self.resolve(
            rows,
            [unit_row(81000), unit_row(82000)],
            [catalog_row(3_600_002, 81000), catalog_row(3_600_003, 82000)],
        )
        self.assertEqual(set(result["roots"]), {81000})

    def test_security_filtered_and_disabled_entries_do_not_become_roots(self):
        rows = [sql_unit(70002, 70001), sql_unit(70001, 70002)]
        result = self.resolve(
            rows,
            [unit_row(70002, 70001), unit_row(70001, 70002)],
            [
                catalog_row(3_600_004, 70002, security=1),
                catalog_row(3_600_005, 70001, enabled=0),
            ],
        )
        self.assertEqual(result["roots"], {})
        self.assertEqual(result["audit"]["security_filtered_catalog_records"], 1)
        self.assertEqual(result["audit"]["disabled_catalog_records"], 1)
        self.assertEqual(result["audit"]["unresolved_families"], 1)
        self.assertEqual(
            result["audit"]["unresolved"][0]["reason"],
            "no_enabled_catalog_entry",
        )

    def test_new_catalog_family_is_discovered_without_a_frozen_id_list(self):
        rows = [
            sql_unit(50002, 50001),
            sql_unit(50001, 50002),
            sql_unit(99004, 99003),
            sql_unit(99003, 99004),
        ]
        result = self.resolve(
            rows,
            [
                unit_row(50002, 50001),
                unit_row(50001, 50002),
                unit_row(99004, 99003),
                unit_row(99003, 99004),
            ],
            [
                catalog_row(3_600_006, 50002),
                catalog_row(3_699_999, 99004),
            ],
        )
        self.assertEqual(set(result["roots"]), {50002, 99004})
        self.assertEqual(result["audit"]["resolved_families"], 2)

    def test_enabled_catalog_entry_must_join_a_native_unit_definition(self):
        with tempfile.TemporaryDirectory() as directory:
            gdrs = Path(directory) / "gdrs"
            FixtureGdrs(
                gdrs,
                [unit_row(60002, 60001), unit_row(60001, 60002)],
                [
                    catalog_row(3_600_007, 60002),
                    catalog_row(3_600_008, 77777),
                ],
            )
            with self.assertRaisesRegex(
                NativeCatalogError, "lack client unit definitions"
            ):
                resolve_native_roots(
                    [sql_unit(60002, 60001), sql_unit(60001, 60002)],
                    client_gdrs=gdrs,
                )

    def test_selected_family_transform_must_match_live_sql(self):
        rows = [sql_unit(61002, 61001), sql_unit(61001, 61002)]
        with self.assertRaisesRegex(
            NativeCatalogError, "transform references differ"
        ):
            self.resolve(
                rows,
                [unit_row(61002, 0), unit_row(61001, 61002)],
                [catalog_row(3_600_009, 61002)],
            )

    def test_wrong_catalog_record_stride_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            gdrs = Path(directory) / "gdrs"
            bad = bytes(CATALOG_RECORD_SIZE - 1)
            FixtureGdrs(
                gdrs,
                [unit_row(62002, 62001), unit_row(62001, 62002)],
                [bad],
                catalog_size=CATALOG_RECORD_SIZE - 1,
            )
            with self.assertRaisesRegex(
                NativeCatalogError, "unsupported record layout"
            ):
                resolve_native_roots(
                    [sql_unit(62002, 62001), sql_unit(62001, 62002)],
                    client_gdrs=gdrs,
                )


if __name__ == "__main__":
    unittest.main()
