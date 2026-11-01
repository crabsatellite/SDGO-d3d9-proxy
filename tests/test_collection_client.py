import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import owner.collection_capsule.client as client
import owner.collection_capsule.art as art
import package_collection_capsule_client as packaging


def write_pe(path):
    raw = bytearray(0x300)
    raw[:2] = b"MZ"
    pe = 0x80
    struct.pack_into("<I", raw, 0x3C, pe)
    raw[pe : pe + 4] = b"PE\0\0"
    struct.pack_into("<I", raw, pe + 8, client.HOST_TIMESTAMP)
    struct.pack_into("<H", raw, pe + 0x18, 0x10B)
    struct.pack_into("<I", raw, pe + 0x18 + 0x38, client.HOST_SIZE_OF_IMAGE)
    Path(path).write_bytes(raw)


TABLE_EXTRAS = {
    "0031": 55,
    "0033": 11,
    "0099": 56,
    "0101": 6,
    "0154": 30,
    "0155": 35,
}


def table(name, rows=1):
    size = client.TABLE_SIZES[name]
    data = []
    for index in range(rows):
        row = bytearray(size)
        if name == "0031":
            category_id = client.ITEM_CATEGORY_TEMPLATE_ID + index
            struct.pack_into("<I", row, 0, category_id)
            client._put_wide(row, 5, 100, f"Native unit {index}")
            struct.pack_into("<I", row, 105, 10001 + index)
            row[109] = 0
            struct.pack_into("<I", row, 110, 1)
            client._put_wide(row, 114, 100, r"sprs\3949.spr")
        else:
            struct.pack_into("<I", row, 0, client.TABLE_BASES[name] + index)
        if name == "0033":
            struct.pack_into("<I", row, 5, 700000 + index)
            row[11] = 6
        elif name == "0099":
            struct.pack_into("<I", row, 5, 9000 + index)
            struct.pack_into("<I", row, 15, 10000)
            client._put_wide(row, 19, 160, fr"sprs\{21000 + index}.spr")
        data.append(bytes(row))
    return client.encode_gdt(size, data, extra=TABLE_EXTRAS[name])


def make_client(root, *, reserved=False):
    root = Path(root)
    root.mkdir()
    write_pe(root / "GOnline.exe")
    resources = {
        "gdrs/0031.gdt": table("0031", 2),
        "gdrs/0033.gdt": table("0033", 2),
        "gdrs/0099.gdt": table("0099", 3),
        "gdrs/0101.gdt": table("0101", 1),
        "gdrs/0154.gdt": table("0154", 1),
        "gdrs/0155.gdt": table("0155", 1),
        "uirs/1161.uir": b"fixture-ui",
    }
    if reserved:
        resources["sprs/65534.spr"] = b"occupied"
    zdx, zpk, _ = client.build_archive(resources)
    (root / "GPackData.zdx").write_bytes(zdx)
    (root / "GPackData.zpk").write_bytes(zpk)
    return root


def make_cover(path):
    from PIL import Image

    Image.new("RGB", client.COVER_SIZE, (25, 50, 75)).save(path)


def unit_items():
    return [
        {
            "unit_id": 10001,
            "category_id": client.ITEM_CATEGORY_TEMPLATE_ID,
            "name": "Gundam",
            "created_by_collection": False,
        },
        {
            "unit_id": 90001,
            "category_id": client.COLLECTION_CATEGORY_MIN,
            "name": "Collection Unit",
            "created_by_collection": True,
        },
    ]


def legacy_machine_row(template):
    row = bytearray(template)
    struct.pack_into("<I", row, 5, 990001)
    struct.pack_into("<II", row, 11, 6, 100000)
    row[19:1019] = bytes(1000)
    for index, unit_id in enumerate(range(10001, 10007)):
        struct.pack_into("<I", row, 27 + index * 4, unit_id)
        client._put_wide(
            row,
            59 + index * 120,
            60,
            client.LEGACY_ORDINARY_SPRITES[0],
        )
        client._put_wide(
            row,
            59 + index * 120 + 60,
            60,
            client.LEGACY_ORDINARY_SPRITES[1],
        )
    return bytes(row)


def build_registry(
    game, cover, output, current=None, enabled=True, items=None
):
    return client.build_client_registry(
        client_root=game,
        current_registry=current,
        source_units=range(10001, 10007),
        unit_items=unit_items() if items is None else items,
        cover=cover if enabled else None,
        output=output,
        enabled=enabled,
    )


def make_legacy_registry(
    game, output, *, with_ui=False, foreign_texture_reference=False
):
    """Compiler-v4-style registry with the exact old ordinary collection row."""
    output = Path(output)
    output.mkdir()
    stock = client.ArchiveReader(
        Path(game) / "GPackData.zdx",
        Path(game) / "GPackData.zpk",
        loose_root=Path(game),
    )
    resources = {
        f"gdrs/{name}.gdt": stock.read(f"gdrs/{name}.gdt")
        for name in client.LEGACY_TABLES
    }
    size, rows, extra = client.decode_gdt(resources["gdrs/0033.gdt"])
    other = bytearray(rows[0])
    struct.pack_into("<I", other, 0, client.TABLE_BASES["0033"] + len(rows))
    struct.pack_into("<I", other, 5, 880001)
    rows.append(bytes(other))
    legacy = bytearray(legacy_machine_row(rows[0]))
    struct.pack_into("<I", legacy, 0, client.TABLE_BASES["0033"] + len(rows))
    legacy = bytes(legacy)
    rows.append(legacy)
    resources["gdrs/0033.gdt"] = client.encode_gdt(size, rows, extra)
    resources["sprs/60000.spr"] = b"legacy unrelated generated sprite"
    resources["sprs/65534.spr"] = b"old shown poster"
    resources["sprs/65535.spr"] = b"old hidden poster"
    resources["txrs/65534.txr"] = b"old poster texture"
    if foreign_texture_reference:
        resources["sprs/60001.spr"] = client._standalone_sprite(
            client.COLLECTION_TEXTURE, 1, 1, (0.0, 0.0, 1.0, 1.0)
        )
    if with_ui:
        resources[client.CAPSULE_UI_RESOURCE] = b"legacy patched capsule UI"
    zdx, zpk, _ = client.build_archive(resources)
    (output / "overlay.zdx").write_bytes(zdx)
    (output / "overlay.zpk").write_bytes(zpk)
    (output / "data").mkdir()
    for name in client.LEGACY_TABLES:
        (output / "data" / f"{name}.gdt").write_bytes(resources[f"gdrs/{name}.gdt"])
    if with_ui:
        (output / "ui").mkdir()
        (output / "ui" / "1161.uir").write_bytes(
            resources[client.CAPSULE_UI_RESOURCE]
        )
    lines = [
        "[runtime]",
        "schema=1",
        "catalog_revision=" + "1" * 64,
        "asset_root=assets",
        "overlay=overlay",
        f"overlay_zdx_sha256={hashlib.sha256(zdx).hexdigest()}",
        f"overlay_zpk_sha256={hashlib.sha256(zpk).hexdigest()}",
        "",
        "[gdt]",
    ]
    for name in client.LEGACY_TABLES:
        raw = resources[f"gdrs/{name}.gdt"]
        lines.extend(
            (
                f"{name}=data\\{name}.gdt",
                f"{name}_sha256={hashlib.sha256(raw).hexdigest()}",
            )
        )
    if with_ui:
        ui = resources[client.CAPSULE_UI_RESOURCE]
        lines.extend(
            (
                "",
                "[ui]",
                "1161=ui\\1161.uir",
                f"1161_sha256={hashlib.sha256(ui).hexdigest()}",
            )
        )
    lines.extend(("", "[counts]", "fixed=4", "coin=1", "custom=1", "candidates=1", ""))
    (output / "runtime.ini").write_text("\n".join(lines), encoding="ascii")
    return rows


def symbol_profile():
    sections = [("host", packaging.HOST_VALUES)]
    sections.extend(packaging.GRID_SYMBOLS.items())
    sections.extend(packaging.CAPSULE_SYMBOLS.items())
    return packaging._render_symbols(sections, "; fixture")


class CollectionClientRegistryTests(unittest.TestCase):
    def test_cover_art_uses_native_activity_dimensions(self):
        from PIL import Image

        committed = ROOT / "mods/collection_capsule/assets/cover.png"
        with Image.open(committed) as image:
            self.assertEqual(image.size, client.COVER_SIZE)
            self.assertEqual(image.mode, "RGB")
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.png"
            output = root / "output.png"
            Image.new("RGB", (722, 644), (10, 20, 30)).save(source)
            art.prepare_cover(source, output)
            with Image.open(output) as image:
                self.assertEqual(image.size, client.COVER_SIZE)
                self.assertEqual(image.getpixel((0, 0)), (10, 20, 30))

    def test_native_table_and_archive_round_trip(self):
        for name in client.TABLE_SIZES:
            raw = table(name, 3)
            size, rows, extra = client.decode_gdt(raw)
            self.assertEqual(size, client.TABLE_SIZES[name])
            self.assertEqual(len(rows), 3)
            if name == client.ITEM_CATEGORY_TABLE:
                self.assertEqual(
                    [struct.unpack_from("<I", row)[0] for row in rows],
                    [client.ITEM_CATEGORY_TEMPLATE_ID + index for index in range(3)],
                )
            else:
                self.assertEqual(
                    [struct.unpack_from("<I", row)[0] for row in rows],
                    [client.TABLE_BASES[name] + index for index in range(3)],
                )
            self.assertIsInstance(extra, int)
        resources = {
            "gdrs/0031.gdt": table("0031"),
            "gdrs/0033.gdt": table("0033"),
            "gdrs/0099.gdt": table("0099"),
            "sprs/65535.spr": os.urandom(2048),
            "txrs/60000.txr": b"texture",
        }
        zdx, zpk, _ = client.build_archive(resources)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "overlay.zdx").write_bytes(zdx + b"\xff" * 12)
            (root / "overlay.zpk").write_bytes(zpk)
            reader = client.ArchiveReader(root / "overlay.zdx", root / "overlay.zpk")
            for name, raw in resources.items():
                self.assertEqual(reader.read(name), raw)

    def test_generated_resource_allowlist_uses_exact_numeric_range_and_extension(self):
        client.build_archive({"sprs/60000.spr": b"x", "txrs/65535.txr": b"y"})
        for name in ("sprs/59999.spr", "txrs/65536.txr", "sprs/60000.txr"):
            with self.subTest(name=name), self.assertRaises(client.ClientRegistryError):
                client.build_archive({name: b"x"})

    def test_add_repeat_and_disable_preserve_unrelated_registry(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = make_client(root / "game")
            cover = root / "cover.png"
            make_cover(cover)
            first, second, disabled = root / "first", root / "second", root / "disabled"
            added = build_registry(game, cover, first)
            repeated = build_registry(game, cover, second, current=first)
            removed = build_registry(game, cover, disabled, current=second, enabled=False)
            self.assertEqual(added["action"], "added")
            self.assertEqual(repeated["action"], "unchanged")
            self.assertEqual(removed["action"], "removed")
            self.assertEqual(added["catalog_revision"], repeated["catalog_revision"])
            for relative in (
                "overlay.zdx",
                "overlay.zpk",
                "runtime.ini",
                "data/0031.gdt",
                "data/0033.gdt",
                "data/0099.gdt",
                "data/0101.gdt",
                "data/0154.gdt",
                "data/0155.gdt",
            ):
                self.assertEqual((first / relative).read_bytes(), (second / relative).read_bytes())

            reader = client.ArchiveReader(first / "overlay.zdx", first / "overlay.zpk")
            _, fixed_rows, _ = client.decode_gdt(reader.read("gdrs/0033.gdt"))
            self.assertEqual(len(fixed_rows), 2)
            _, rows, _ = client.decode_gdt(reader.read("gdrs/0099.gdt"))
            self.assertEqual(len(rows), 4)
            machine = rows[-1]
            self.assertEqual(struct.unpack_from("<I", machine, 5)[0], 990001)
            self.assertEqual(struct.unpack_from("<I", machine, 9)[0], 0)
            self.assertEqual(struct.unpack_from("<I", machine, 15)[0], 100000)
            self.assertEqual(
                client._activity_sprite(machine),
                client.COLLECTION_SPRITE,
            )
            self.assertEqual(added["placement_kind"], "activity")
            self.assertEqual(added["counts"]["activity"], 4)
            self.assertEqual(
                added["placement"]["grid_id"],
                client.TABLE_BASES["0099"] + 3,
            )
            sprite = reader.read("sprs/65534.spr")
            self.assertEqual(
                struct.unpack_from("<IIIII", sprite, 0),
                (10000, 1, 20, client.COVER_SIZE[1], client.COVER_SIZE[0]),
            )
            self.assertEqual(len(sprite), 176)
            self.assertFalse(reader.has("sprs/65535.spr"))
            _, categories, _ = client.decode_gdt(reader.read("gdrs/0031.gdt"))
            created = next(
                row
                for row in categories
                if struct.unpack_from("<I", row, 0)[0]
                == client.COLLECTION_CATEGORY_MIN
            )
            self.assertEqual(struct.unpack_from("<I", created, 105)[0], 90001)
            self.assertEqual(created[109], 0)
            self.assertEqual(struct.unpack_from("<I", created, 110)[0], 1)

            disabled_reader = client.ArchiveReader(
                disabled / "overlay.zdx", disabled / "overlay.zpk"
            )
            stock = client.ArchiveReader(
                game / "GPackData.zdx", game / "GPackData.zpk", loose_root=game
            )
            self.assertEqual(
                client.decode_gdt(disabled_reader.read("gdrs/0033.gdt")),
                client.decode_gdt(stock.read("gdrs/0033.gdt")),
            )
            self.assertEqual(
                client.decode_gdt(disabled_reader.read("gdrs/0099.gdt")),
                client.decode_gdt(stock.read("gdrs/0099.gdt")),
            )
            _, retained_categories, _ = client.decode_gdt(
                disabled_reader.read("gdrs/0031.gdt")
            )
            self.assertIn(
                client.COLLECTION_CATEGORY_MIN,
                [struct.unpack_from("<I", row, 0)[0] for row in retained_categories],
            )
            for name in ("sprs/65534.spr", "sprs/65535.spr", "txrs/65534.txr"):
                self.assertFalse(disabled_reader.has(name))

    def test_reserved_stock_resource_and_machine_id_collision_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cover = root / "cover.png"
            make_cover(cover)
            occupied = make_client(root / "occupied", reserved=True)
            with self.assertRaisesRegex(client.ClientRegistryError, "resource is occupied"):
                client.build_client_registry(
                    client_root=occupied,
                    unit_items=unit_items(),
                    cover=cover,
                    output=root / "out",
                )

            game = make_client(root / "machine")
            reader = client.ArchiveReader(
                game / "GPackData.zdx", game / "GPackData.zpk"
            )
            resources = {
                name: reader.read(name)
                for name in (
                    *(f"gdrs/{table}.gdt" for table in client.TABLE_NAMES),
                    "uirs/1161.uir",
                )
            }
            size, rows, extra = client.decode_gdt(resources["gdrs/0099.gdt"])
            mutable = bytearray(rows[0])
            struct.pack_into("<I", mutable, 5, 990001)
            rows[0] = bytes(mutable)
            resources["gdrs/0099.gdt"] = client.encode_gdt(size, rows, extra)
            zdx, zpk, _ = client.build_archive(resources)
            (game / "GPackData.zdx").write_bytes(zdx)
            (game / "GPackData.zpk").write_bytes(zpk)
            with self.assertRaisesRegex(client.ClientRegistryError, "occupied by another"):
                client.build_client_registry(
                    client_root=game,
                    unit_items=unit_items(),
                    cover=cover,
                    output=root / "out2",
                )

            ordinary = make_client(root / "ordinary")
            reader = client.ArchiveReader(
                ordinary / "GPackData.zdx", ordinary / "GPackData.zpk"
            )
            resources = {
                name: reader.read(name)
                for name in (
                    *(f"gdrs/{table}.gdt" for table in client.TABLE_NAMES),
                    "uirs/1161.uir",
                )
            }
            size, rows, extra = client.decode_gdt(resources["gdrs/0033.gdt"])
            mutable = bytearray(rows[0])
            struct.pack_into("<I", mutable, 5, 990001)
            rows[0] = bytes(mutable)
            resources["gdrs/0033.gdt"] = client.encode_gdt(size, rows, extra)
            zdx, zpk, _ = client.build_archive(resources)
            (ordinary / "GPackData.zdx").write_bytes(zdx)
            (ordinary / "GPackData.zpk").write_bytes(zpk)
            with self.assertRaisesRegex(
                client.ClientRegistryError, "another capsule definition"
            ):
                build_registry(ordinary, cover, root / "out3")

    def test_old_ordinary_registry_migrates_without_losing_rows_or_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = make_client(root / "game")
            cover = root / "cover.png"
            make_cover(cover)
            legacy = root / "legacy"
            legacy_rows = make_legacy_registry(game, legacy, with_ui=True)
            output = root / "upgraded"
            result = build_registry(game, cover, output, current=legacy)
            self.assertEqual(result["action"], "added")
            reader = client.ArchiveReader(
                output / "overlay.zdx", output / "overlay.zpk"
            )
            _, fixed_rows, _ = client.decode_gdt(reader.read("gdrs/0033.gdt"))
            self.assertEqual(fixed_rows, legacy_rows[:-1])
            self.assertNotIn(
                990001,
                [struct.unpack_from("<I", row, 5)[0] for row in fixed_rows],
            )
            _, activity_rows, _ = client.decode_gdt(reader.read("gdrs/0099.gdt"))
            self.assertEqual(struct.unpack_from("<I", activity_rows[-1], 5)[0], 990001)
            self.assertEqual(
                reader.read("sprs/60000.spr"),
                b"legacy unrelated generated sprite",
            )
            self.assertFalse(reader.has("sprs/65535.spr"))
            self.assertEqual(
                reader.read("uirs/1161.uir"), b"legacy patched capsule UI"
            )
            runtime = (output / "runtime.ini").read_text(encoding="ascii")
            self.assertIn("0031=data\\0031.gdt", runtime)
            self.assertIn("0099=data\\0099.gdt", runtime)
            self.assertIn("activity=4", runtime)
            self.assertIn("1161=ui\\1161.uir", runtime)

    def test_unrelated_generated_sprite_cannot_share_reserved_texture(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = make_client(root / "game")
            cover = root / "cover.png"
            make_cover(cover)
            legacy = root / "legacy"
            make_legacy_registry(
                game,
                legacy,
                foreign_texture_reference=True,
            )
            with self.assertRaisesRegex(
                client.ClientRegistryError,
                "texture is referenced by another generated sprite",
            ):
                build_registry(
                    game,
                    cover,
                    root / "output",
                    current=legacy,
                )

    def test_unit_item_categories_fail_closed_and_are_never_implicitly_removed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = make_client(root / "game")
            cover = root / "cover.png"
            make_cover(cover)
            first = root / "first"
            build_registry(game, cover, first)
            second = root / "second"
            build_registry(game, cover, second, current=first, items=[])
            reader = client.ArchiveReader(
                second / "overlay.zdx", second / "overlay.zpk"
            )
            _, rows, _ = client.decode_gdt(reader.read("gdrs/0031.gdt"))
            self.assertIn(
                client.COLLECTION_CATEGORY_MIN,
                [struct.unpack_from("<I", row, 0)[0] for row in rows],
            )

            conflicting = [
                {
                    "unit_id": 99999,
                    "category_id": client.ITEM_CATEGORY_TEMPLATE_ID,
                    "name": "Wrong",
                    "created_by_collection": False,
                }
            ]
            with self.assertRaisesRegex(
                client.ClientRegistryError, "conflicts with 0031"
            ):
                build_registry(
                    game,
                    cover,
                    root / "conflict",
                    items=conflicting,
                )


class CollectionClientPackageTests(unittest.TestCase):
    def test_self_contained_player_package_has_current_framework_and_no_private_state(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = make_client(root / "game")
            cover = root / "cover.png"
            make_cover(cover)
            registry = root / "registry"
            built = build_registry(game, cover, registry)
            stage = root / "stage"
            (stage / "sdgo/symbols").mkdir(parents=True)
            (stage / "d3d9.dll").write_bytes(b"CURRENT FRAMEWORK LOADER")
            (stage / "sdgo/core.dll").write_bytes(b"CURRENT FRAMEWORK CORE")
            (stage / "sdgo/sdgo.ini").write_text("[core]\nlog_level=1\n", encoding="utf-8")
            (stage / "sdgo/symbols" / packaging.HOST_SYMBOL).write_bytes(symbol_profile())
            support = root / "capsule_catalog.dll"
            support.write_bytes(
                b"CURRENT CAPSULE SUPPORT SDGO_CAPSULE_REGISTRY_6TABLE_V2"
            )
            output = root / "player"
            result = packaging.package_client_registry(
                registry=registry,
                framework_stage=stage,
                support_dll=support,
                output=output,
            )
            self.assertEqual(result["catalog_revision"], built["catalog_revision"])
            self.assertTrue((output / "Install-Client.cmd").is_file())
            self.assertTrue((output / "Uninstall-Client.cmd").is_file())
            self.assertEqual(
                (output / "client/d3d9.dll").read_bytes(), b"CURRENT FRAMEWORK LOADER"
            )
            self.assertEqual(
                (output / "client/sdgo/mods/capsule_catalog/capsule_catalog.dll").read_bytes(),
                b"CURRENT CAPSULE SUPPORT SDGO_CAPSULE_REGISTRY_6TABLE_V2",
            )
            manifest = json.loads(
                (output / "package-manifest.json").read_text(encoding="utf-8")
            )
            bundle = json.loads(
                (output / "bundle-manifest.json").read_text(encoding="utf-8")
            )
            actual = {
                path.relative_to(output / "client").as_posix()
                for path in (output / "client").rglob("*")
                if path.is_file()
            }
            self.assertEqual(set(manifest["files"]), actual)
            for relative, expected in manifest["files"].items():
                self.assertEqual(
                    hashlib.sha256((output / "client" / relative).read_bytes()).hexdigest(),
                    expected,
                )
            declared_bundle = {
                path.relative_to(output).as_posix()
                for path in output.rglob("*")
                if path.is_file() and path.name != "bundle-manifest.json"
            }
            self.assertEqual(set(bundle["files"]), declared_bundle)
            for relative, expected in bundle["files"].items():
                self.assertEqual(
                    hashlib.sha256((output / relative).read_bytes()).hexdigest(),
                    expected,
                )
            combined = b"\n".join(
                path.read_bytes() for path in output.rglob("*") if path.is_file()
            ).lower()
            for forbidden in (
                b"password=",
                b"server=",
                b"database=",
                b"worldserver.ini",
                b"tbl_user",
            ):
                self.assertNotIn(forbidden, combined)

    @unittest.skipUnless(sys.platform == "win32", "PowerShell parser is Windows-only")
    def test_install_and_uninstall_scripts_parse(self):
        scripts = (
            ROOT / "support/capsule_registry/Install-Client.ps1",
            ROOT / "support/capsule_registry/Uninstall-Client.ps1",
        )
        for script in scripts:
            command = (
                "$e=$null;$t=$null;"
                f"[void][Management.Automation.Language.Parser]::ParseFile('{script}',[ref]$t,[ref]$e);"
                "if($e.Count){$e|ForEach-Object{$_.ToString()};exit 1}"
            )
            result = subprocess.run(
                ["powershell", "-NoProfile", "-NonInteractive", "-Command", command],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
