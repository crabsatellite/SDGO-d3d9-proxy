import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

import owner.collection_capsule.client as client
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


def table(name, rows=1):
    size = client.TABLE_SIZES[name]
    data = []
    for index in range(rows):
        row = bytearray(size)
        struct.pack_into("<I", row, 0, client.TABLE_BASES[name] + index)
        if name == "0033":
            struct.pack_into("<I", row, 5, 700000 + index)
            row[11] = 6
        data.append(bytes(row))
    return client.encode_gdt(size, data, extra={"0033": 11, "0101": 6, "0154": 30, "0155": 35}[name])


def make_client(root, *, reserved=False):
    root = Path(root)
    root.mkdir()
    write_pe(root / "GOnline.exe")
    resources = {
        "gdrs/0033.gdt": table("0033", 2),
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

    Image.new("RGB", (182, 108), (25, 50, 75)).save(path)


def build_registry(game, cover, output, current=None, enabled=True):
    with patch.object(
        client,
        "patch_capsule_price_defaults",
        return_value=(b"fixture-ui-patched", {"fixture": True}),
    ):
        return client.build_client_registry(
            client_root=game,
            current_registry=current,
            source_units=range(10001, 10007),
            cover=cover if enabled else None,
            output=output,
            enabled=enabled,
        )


def make_legacy_registry(game, output):
    """Compiler-v4-style registry: four GDTs/assets, no UI declaration."""
    output = Path(output)
    output.mkdir()
    stock = client.ArchiveReader(
        Path(game) / "GPackData.zdx",
        Path(game) / "GPackData.zpk",
        loose_root=Path(game),
    )
    resources = {
        f"gdrs/{name}.gdt": stock.read(f"gdrs/{name}.gdt")
        for name in client.TABLE_BASES
    }
    size, rows, extra = client.decode_gdt(resources["gdrs/0033.gdt"])
    other = bytearray(rows[0])
    struct.pack_into("<I", other, 0, client.TABLE_BASES["0033"] + len(rows))
    struct.pack_into("<I", other, 5, 880001)
    rows.append(bytes(other))
    resources["gdrs/0033.gdt"] = client.encode_gdt(size, rows, extra)
    resources["sprs/60000.spr"] = b"legacy unrelated generated sprite"
    zdx, zpk, _ = client.build_archive(resources)
    (output / "overlay.zdx").write_bytes(zdx)
    (output / "overlay.zpk").write_bytes(zpk)
    (output / "data").mkdir()
    for name in client.TABLE_BASES:
        (output / "data" / f"{name}.gdt").write_bytes(resources[f"gdrs/{name}.gdt"])
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
    for name in client.TABLE_BASES:
        raw = resources[f"gdrs/{name}.gdt"]
        lines.extend(
            (
                f"{name}=data\\{name}.gdt",
                f"{name}_sha256={hashlib.sha256(raw).hexdigest()}",
            )
        )
    lines.extend(("", "[counts]", "fixed=3", "coin=1", "custom=1", "candidates=1", ""))
    (output / "runtime.ini").write_text("\n".join(lines), encoding="ascii")
    return rows


def symbol_profile():
    sections = [("host", packaging.HOST_VALUES)]
    sections.extend(packaging.GRID_SYMBOLS.items())
    sections.extend(packaging.CAPSULE_SYMBOLS.items())
    return packaging._render_symbols(sections, "; fixture")


class CollectionClientRegistryTests(unittest.TestCase):
    def test_native_table_and_archive_round_trip(self):
        for name in client.TABLE_SIZES:
            raw = table(name, 3)
            size, rows, extra = client.decode_gdt(raw)
            self.assertEqual(size, client.TABLE_SIZES[name])
            self.assertEqual(len(rows), 3)
            self.assertEqual(
                [struct.unpack_from("<I", row)[0] for row in rows],
                [client.TABLE_BASES[name] + index for index in range(3)],
            )
            self.assertIsInstance(extra, int)
        resources = {
            "gdrs/0033.gdt": table("0033"),
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
                "data/0033.gdt",
                "data/0101.gdt",
                "data/0154.gdt",
                "data/0155.gdt",
                "ui/1161.uir",
            ):
                self.assertEqual((first / relative).read_bytes(), (second / relative).read_bytes())

            reader = client.ArchiveReader(first / "overlay.zdx", first / "overlay.zpk")
            _, rows, _ = client.decode_gdt(reader.read("gdrs/0033.gdt"))
            self.assertEqual(len(rows), 3)
            machine = rows[-1]
            self.assertEqual(struct.unpack_from("<I", machine, 5)[0], 990001)
            self.assertEqual(struct.unpack_from("<I", machine, 15)[0], 100000)
            self.assertEqual(
                [struct.unpack_from("<I", machine, 27 + index * 4)[0] for index in range(6)],
                list(range(10001, 10007)),
            )
            self.assertEqual(
                client._machine_sprites(machine),
                tuple(client.PLACEHOLDER_SPRITES[index % 2] for index in range(12)),
            )
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
            for name in ("sprs/65534.spr", "sprs/65535.spr", "txrs/65534.txr"):
                self.assertFalse(disabled_reader.has(name))

    def test_reserved_stock_resource_and_machine_id_collision_fail_closed(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            cover = root / "cover.png"
            make_cover(cover)
            occupied = make_client(root / "occupied", reserved=True)
            with patch.object(
                client,
                "patch_capsule_price_defaults",
                return_value=(b"fixture-ui-patched", {"fixture": True}),
            ), self.assertRaisesRegex(client.ClientRegistryError, "resource is occupied"):
                client.build_client_registry(
                    client_root=occupied,
                    source_units=range(10001, 10007),
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
                    "gdrs/0033.gdt",
                    "gdrs/0101.gdt",
                    "gdrs/0154.gdt",
                    "gdrs/0155.gdt",
                    "uirs/1161.uir",
                )
            }
            size, rows, extra = client.decode_gdt(resources["gdrs/0033.gdt"])
            mutable = bytearray(rows[0])
            struct.pack_into("<I", mutable, 5, 990001)
            rows[0] = bytes(mutable)
            resources["gdrs/0033.gdt"] = client.encode_gdt(size, rows, extra)
            zdx, zpk, _ = client.build_archive(resources)
            (game / "GPackData.zdx").write_bytes(zdx)
            (game / "GPackData.zpk").write_bytes(zpk)
            with patch.object(
                client,
                "patch_capsule_price_defaults",
                return_value=(b"fixture-ui-patched", {"fixture": True}),
            ), self.assertRaisesRegex(client.ClientRegistryError, "occupied by another"):
                client.build_client_registry(
                    client_root=game,
                    source_units=range(10001, 10007),
                    cover=cover,
                    output=root / "out2",
                )

    def test_compiler_v4_registry_without_ui_is_upgraded_without_losing_rows_or_assets(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = make_client(root / "game")
            cover = root / "cover.png"
            make_cover(cover)
            legacy = root / "legacy"
            legacy_rows = make_legacy_registry(game, legacy)
            output = root / "upgraded"
            result = build_registry(game, cover, output, current=legacy)
            self.assertEqual(result["action"], "added")
            reader = client.ArchiveReader(
                output / "overlay.zdx", output / "overlay.zpk"
            )
            _, rows, _ = client.decode_gdt(reader.read("gdrs/0033.gdt"))
            self.assertEqual(rows[:-1], legacy_rows)
            self.assertEqual(struct.unpack_from("<I", rows[-1], 5)[0], 990001)
            self.assertEqual(
                reader.read("sprs/60000.spr"),
                b"legacy unrelated generated sprite",
            )
            self.assertEqual(reader.read("uirs/1161.uir"), b"fixture-ui-patched")
            runtime = (output / "runtime.ini").read_text(encoding="ascii")
            self.assertIn("[ui]", runtime)
            self.assertIn("1161=ui\\1161.uir", runtime)


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
            support.write_bytes(b"CURRENT CAPSULE SUPPORT")
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
                b"CURRENT CAPSULE SUPPORT",
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
