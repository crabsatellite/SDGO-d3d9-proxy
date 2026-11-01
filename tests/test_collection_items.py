from __future__ import annotations

import json
from pathlib import Path
import struct
import tempfile
import unittest

from owner.collection_capsule.client import encode_gdt
from owner.collection_capsule.items import (
    OWNED_CATEGORY_MIN,
    build_unit_items,
)


def category(
    identity: int,
    unit: int,
    *,
    time_limit: int = 0,
    option1: int = 1,
    option2: int = 2,
) -> dict:
    return {
        "CatalogueID": identity,
        "Name": f"Unit {unit}",
        "RefIndex": unit,
        "TimeLimit": time_limit,
        "Option1": option1,
        "Option2": option2,
    }


def client_category(
    identity: int,
    unit: int,
    *,
    security: int = 0,
    time_limit: int = 0,
    option1: int = 1,
) -> bytes:
    row = bytearray(614)
    struct.pack_into("<I", row, 0, identity)
    row[4] = security
    struct.pack_into("<I", row, 105, unit)
    row[109] = time_limit
    struct.pack_into("<I", row, 110, option1)
    return bytes(row)


class CollectionUnitItemTests(unittest.TestCase):
    def fixture(
        self,
        root: Path,
        *,
        server: list[dict],
        client: list[bytes],
        owned: list[dict] | None = None,
    ) -> tuple[Path, Path]:
        snapshot = root / "snapshot"
        game = snapshot / "GOnlineGame"
        game.mkdir(parents=True)
        (game / "dbo.tbl_ItemCategory.json").write_text(
            json.dumps(server), encoding="utf-8"
        )
        if owned is not None:
            (game / "collection.unit-items.json").write_text(
                json.dumps(owned), encoding="utf-8"
            )
        gdrs = root / "gdrs"
        gdrs.mkdir()
        (gdrs / "0031.gdt").write_bytes(encode_gdt(614, client, 0))
        return snapshot, gdrs

    def build(
        self,
        root: Path,
        roots: list[dict],
        *,
        server: list[dict],
        client: list[bytes],
        owned: list[dict] | None = None,
    ) -> list[dict]:
        snapshot, gdrs = self.fixture(
            root, server=server, client=client, owned=owned
        )
        return build_unit_items(snapshot, roots, client_gdrs=gdrs)

    def test_reuses_only_a_matching_permanent_native_unit_category(self):
        with tempfile.TemporaryDirectory(prefix="collection item native ") as raw:
            result = self.build(
                Path(raw),
                [{"unit_id": 501, "name": "Native base"}],
                server=[category(1100501, 501)],
                client=[client_category(1100501, 501)],
            )
        self.assertEqual(
            result,
            [
                {
                    "unit_id": 501,
                    "category_id": 1100501,
                    "name": "Native base",
                    "created_by_collection": False,
                }
            ],
        )

    def test_unsafe_native_candidates_allocate_owned_metadata_instead(self):
        cases = {
            "server wrong unit": (
                category(1100601, 999),
                client_category(1100601, 601),
            ),
            "server rental": (
                category(1100601, 601, time_limit=7),
                client_category(1100601, 601),
            ),
            "server delivery options": (
                category(1100601, 601, option2=9),
                client_category(1100601, 601),
            ),
            "client security row": (
                category(1100601, 601),
                client_category(1100601, 601, security=1),
            ),
            "client wrong unit": (
                category(1100601, 601),
                client_category(1100601, 999),
            ),
            "client rental": (
                category(1100601, 601),
                client_category(1100601, 601, time_limit=7),
            ),
            "client delivery option": (
                category(1100601, 601),
                client_category(1100601, 601, option1=9),
            ),
        }
        for index, (label, (server, client)) in enumerate(cases.items()):
            with self.subTest(label=label):
                with tempfile.TemporaryDirectory(
                    prefix=f"collection item unsafe {index} "
                ) as raw:
                    result = self.build(
                        Path(raw),
                        [{"unit_id": 601, "name": "Imported base"}],
                        server=[server],
                        client=[client],
                    )
                self.assertEqual(result[0]["category_id"], OWNED_CATEGORY_MIN)
                self.assertTrue(result[0]["created_by_collection"])

    def test_dynamic_allocation_avoids_server_and_client_ids_and_is_stable(self):
        roots = [
            {"unit_id": 703, "name": "Later import"},
            {"unit_id": 701, "name": "Earlier import"},
        ]
        server = [category(OWNED_CATEGORY_MIN, 900)]
        client = [
            client_category(OWNED_CATEGORY_MIN + 1, 901),
            client_category(1100702, 702),
        ]
        with tempfile.TemporaryDirectory(prefix="collection item ids ") as raw:
            root = Path(raw)
            forward = self.build(
                root / "forward",
                roots,
                server=server,
                client=client,
            )
            reverse = self.build(
                root / "reverse",
                list(reversed(roots)),
                server=server,
                client=client,
            )
        self.assertEqual(forward, reverse)
        self.assertEqual(
            [(row["unit_id"], row["category_id"]) for row in forward],
            [
                (701, OWNED_CATEGORY_MIN + 2),
                (703, OWNED_CATEGORY_MIN + 3),
            ],
        )
        self.assertTrue(all(row["created_by_collection"] for row in forward))

    def test_owned_history_is_retained_after_unit_leaves_current_roots(self):
        owned = [
            {
                "UnitID": 801,
                "CategoryID": OWNED_CATEGORY_MIN,
                "Name": "Pending historical unit",
            }
        ]
        with tempfile.TemporaryDirectory(prefix="collection item history ") as raw:
            result = self.build(
                Path(raw),
                [{"unit_id": 802, "name": "Current import"}],
                server=[category(OWNED_CATEGORY_MIN, 801)],
                client=[],
                owned=owned,
            )
        self.assertEqual(
            [(row["unit_id"], row["category_id"]) for row in result],
            [(801, OWNED_CATEGORY_MIN), (802, OWNED_CATEGORY_MIN + 1)],
        )
        self.assertEqual(result[0]["name"], "Pending historical unit")
        self.assertTrue(result[0]["created_by_collection"])

    def test_removed_or_edited_owned_metadata_is_rejected(self):
        owned = [
            {
                "UnitID": 901,
                "CategoryID": OWNED_CATEGORY_MIN,
                "Name": "Historical unit",
            }
        ]
        cases = {
            "removed": [category(1101902, 1902)],
            "wrong ref": [category(OWNED_CATEGORY_MIN, 999)],
            "rental": [category(OWNED_CATEGORY_MIN, 901, time_limit=1)],
            "wrong option1": [category(OWNED_CATEGORY_MIN, 901, option1=9)],
            "wrong option2": [category(OWNED_CATEGORY_MIN, 901, option2=9)],
        }
        for index, (label, server) in enumerate(cases.items()):
            with self.subTest(label=label):
                with tempfile.TemporaryDirectory(
                    prefix=f"collection item edited {index} "
                ) as raw:
                    root = Path(raw)
                    snapshot, gdrs = self.fixture(
                        root,
                        server=server,
                        client=[],
                        owned=owned,
                    )
                    with self.assertRaisesRegex(
                        ValueError, "removed or externally edited"
                    ):
                        build_unit_items(
                            snapshot, [], client_gdrs=gdrs
                        )

    def test_invalid_or_duplicate_category_sources_are_rejected(self):
        server_cases = {
            "empty": [],
            "boolean ID": [
                {
                    **category(1101001, 1001),
                    "CatalogueID": True,
                }
            ],
            "duplicate ID": [
                category(1101001, 1001),
                category(1101001, 1002),
            ],
        }
        for index, (label, server) in enumerate(server_cases.items()):
            with self.subTest(source="server", label=label):
                with tempfile.TemporaryDirectory(
                    prefix=f"collection item bad server {index} "
                ) as raw:
                    snapshot, gdrs = self.fixture(
                        Path(raw),
                        server=server,
                        client=[],
                    )
                    with self.assertRaises(ValueError):
                        build_unit_items(snapshot, [], client_gdrs=gdrs)

        with tempfile.TemporaryDirectory(
            prefix="collection item duplicate client "
        ) as raw:
            snapshot, gdrs = self.fixture(
                Path(raw),
                server=[category(1101001, 1001)],
                client=[
                    client_category(1101001, 1001),
                    client_category(1101001, 1001),
                ],
            )
            with self.assertRaisesRegex(
                ValueError, "Duplicate native item category ID"
            ):
                build_unit_items(
                    snapshot,
                    [{"unit_id": 1001, "name": "Fixture"}],
                    client_gdrs=gdrs,
                )


if __name__ == "__main__":
    unittest.main()
