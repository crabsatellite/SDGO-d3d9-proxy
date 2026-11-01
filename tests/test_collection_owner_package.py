import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from tools import package_collection_capsule as packager


def fake_x86_dll():
    raw = bytearray(256)
    raw[:2] = b"MZ"
    struct.pack_into("<I", raw, 0x3C, 0x40)
    raw[0x40:0x44] = b"PE\0\0"
    struct.pack_into("<H", raw, 0x44, 0x14C)
    struct.pack_into("<H", raw, 0x40 + 22, 0x2102)
    return bytes(raw)


class OwnerPackageFixture:
    def __init__(self, root):
        self.root = Path(root)
        self.repo = self.root / "repo"
        self.stage = self.root / "stage"
        self.repo.mkdir()
        self.stage.mkdir()
        self._source()
        self._stage()

    @staticmethod
    def _write(path, data=b"fixture\n"):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)

    def _source(self):
        self._write(self.repo / "VERSION", b"1.1.4\n")
        for name in ("__init__.py", "client.py", "native_catalog.py", "pool.py"):
            self._write(self.repo / "owner/collection_capsule" / name)
        for name in ("schema.sql", "draw.sql"):
            self._write(self.repo / "owner/collection_capsule/sql" / name)
        for name in packager.TOOL_FILES:
            self._write(self.repo / "tools" / name)
        for name in packager.SUPPORT_FILES:
            self._write(self.repo / "support/capsule_registry" / name)
        for name in packager.ASSET_FILES:
            self._write(self.repo / "mods/collection_capsule/assets" / name)
        for name in packager.DOCUMENTS:
            self._write(self.repo / "docs" / name)
        self._write(self.repo / "LICENSE")
        self._write(self.repo / "THIRD_PARTY_NOTICES.md")
        self._write(self.repo / "third_party/minhook/LICENSE.txt")
        self._write(self.repo / "owner/collection_capsule/private.ini", b"PASSWORD=x")
        self._write(self.repo / "owner/collection_capsule/history.json", b"ACCOUNT=7")
        self._write(self.repo / "snapshot/model.json", b"PRIVATE MODEL")
        self._write(self.repo / "build.log", b"PRIVATE BUILD LOG")

    def _stage(self):
        dll = fake_x86_dll()
        for destination, relative in packager.STAGE_FILES.items():
            self._write(
                self.stage.joinpath(*relative.split("/")),
                dll if destination.endswith(".dll") else b"public fixture\n",
            )
        self._write(self.stage / "server/private.ini", b"PASSWORD=x")
        self._write(self.stage / "server/test_probe.exe", b"TEST")
        self._write(
            self.stage / "server/sdgo/mods/other/other.dll",
            dll,
        )


class CollectionOwnerPackageTests(unittest.TestCase):
    def test_allowlist_inventory_crc_and_fresh_output(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = OwnerPackageFixture(directory)
            out = Path(directory) / "owner-kit"
            with patch.object(
                packager, "file_version", return_value=(1, 1, 4, 0)
            ):
                archive = packager.package(fixture.repo, fixture.stage, out)
            self.assertEqual(archive, out.with_name("owner-kit.zip"))
            self.assertTrue(out.is_dir())
            self.assertTrue(archive.is_file())

            names = {
                path.relative_to(out).as_posix()
                for path in out.rglob("*")
                if path.is_file()
            }
            forbidden = {
                "owner/collection_capsule/private.ini",
                "owner/collection_capsule/history.json",
                "snapshot/model.json",
                "build.log",
                "framework/server/private.ini",
                "framework/server/test_probe.exe",
                "framework/server/sdgo/mods/other/other.dll",
            }
            self.assertTrue(forbidden.isdisjoint(names))
            self.assertIn(
                "framework/support/capsule-registry/capsule_catalog.dll",
                names,
            )
            self.assertIn("owner/collection_capsule/native_catalog.py", names)
            self.assertIn("SOURCE_PROVENANCE.json", names)

            inventory = json.loads((out / "SHA256.json").read_text())
            self.assertEqual(set(inventory), names - {"SHA256.json"})
            for name, expected in inventory.items():
                self.assertEqual(
                    hashlib.sha256((out / name).read_bytes()).hexdigest(),
                    expected,
                )
            with zipfile.ZipFile(archive) as reader:
                self.assertIsNone(reader.testzip())
                self.assertEqual(reader.namelist(), sorted(names))
                for name in names:
                    self.assertEqual(reader.read(name), (out / name).read_bytes())

            with patch.object(
                packager, "file_version", return_value=(1, 1, 4, 0)
            ):
                with self.assertRaises(FileExistsError):
                    packager.package(fixture.repo, fixture.stage, out)

    def test_directory_only_and_deterministic_archives(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = OwnerPackageFixture(directory)
            with patch.object(
                packager, "file_version", return_value=(1, 1, 4, 0)
            ):
                directory_result = packager.package(
                    fixture.repo,
                    fixture.stage,
                    Path(directory) / "directory-kit",
                    directory_only=True,
                )
                first = packager.package(
                    fixture.repo, fixture.stage, Path(directory) / "first"
                )
                second = packager.package(
                    fixture.repo, fixture.stage, Path(directory) / "second"
                )
            self.assertTrue(directory_result.is_dir())
            self.assertFalse(
                directory_result.with_name(directory_result.name + ".zip").exists()
            )
            self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_dll_version_must_match_repository_version(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = OwnerPackageFixture(directory)
            out = Path(directory) / "stale"
            with patch.object(
                packager, "file_version", return_value=(1, 1, 3, 0)
            ):
                with self.assertRaisesRegex(
                    packager.OwnerPackageError, "version differs"
                ):
                    packager.package(fixture.repo, fixture.stage, out)
            self.assertFalse(out.exists())
            self.assertFalse(out.with_name("stale.zip").exists())

    def test_non_x86_or_non_dll_stage_binary_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            fixture = OwnerPackageFixture(directory)
            bad = fixture.stage / "client/d3d9.dll"
            raw = bytearray(bad.read_bytes())
            struct.pack_into("<H", raw, 0x44, 0x8664)
            bad.write_bytes(raw)
            with patch.object(
                packager, "file_version", return_value=(1, 1, 4, 0)
            ):
                with self.assertRaisesRegex(
                    packager.OwnerPackageError, "not an x86 DLL"
                ):
                    packager.package(
                        fixture.repo,
                        fixture.stage,
                        Path(directory) / "bad-machine",
                    )


if __name__ == "__main__":
    unittest.main()
