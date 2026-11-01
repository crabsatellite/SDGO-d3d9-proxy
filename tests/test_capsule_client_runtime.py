import gzip
import hashlib
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
REVISION = "1" * 64
GDT_NAMES = ("0033", "0101", "0154", "0155")


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def archive(resources, *, duplicate=False, bad_range=False):
    chunks = []
    entries = []
    for name, raw in sorted(resources.items()):
        payload = b"ZOAGZIP1" + struct.pack("<I", len(raw)) + gzip.compress(raw, mtime=0)
        first = len(chunks)
        count = (len(payload) + 511) // 512
        for offset in range(count):
            index = len(chunks)
            block = payload[offset * 512:(offset + 1) * 512].ljust(512, b"\0")
            chunks.append(struct.pack("<II", index + 1, max(0, index - 1)) + block)
        entries.append((name, len(payload), first, count))
    if duplicate:
        entries.append(entries[0])
    if bad_range:
        name, stored, first, count = entries[-1]
        entries[-1] = (name, stored, len(chunks), count)
    zdx = bytearray(struct.pack("<6I", 1, 512, len(chunks), 0, len(entries), 0))
    for name, stored, first, count in entries:
        for char in name.replace("/", "\\").lower() + "\0":
            zdx += struct.pack("<H", ord(char) ^ 0xAA)
        zdx += struct.pack("<III", stored, first, count) + bytes(24)
    return bytes(zdx), b"".join(chunks)


def write_bundle(folder, *, duplicate=False, bad_range=False, asset_id=60000,
                 with_ui=False, declare_ui=True):
    folder = Path(folder)
    (folder / "data").mkdir(parents=True)
    resources = {}
    files = {}
    for table in GDT_NAMES:
        data = ("table-" + table).encode("ascii")
        path = folder / "data" / (table + ".gdt")
        path.write_bytes(data)
        resources["gdrs/" + table + ".gdt"] = data
        files[table] = sha(path)
    resources[f"sprs/{asset_id}.spr"] = b"sprite"
    resources[f"txrs/{asset_id}.txr"] = b"texture"
    if with_ui:
        resources["uirs/1161.uir"] = b"capsule-ui-fixture"
        (folder / "ui").mkdir()
        (folder / "ui/1161.uir").write_bytes(resources["uirs/1161.uir"])
    zdx, zpk = archive(resources, duplicate=duplicate, bad_range=bad_range)
    (folder / "overlay.zdx").write_bytes(zdx)
    (folder / "overlay.zpk").write_bytes(zpk)
    lines = [
        "[runtime]",
        "schema=1",
        f"catalog_revision={REVISION}",
        "asset_root=assets",
        "overlay=overlay",
        f"overlay_zdx_sha256={sha(folder / 'overlay.zdx')}",
        f"overlay_zpk_sha256={sha(folder / 'overlay.zpk')}",
        "",
        "[gdt]",
    ]
    for table in GDT_NAMES:
        lines += [f"{table}=data\\{table}.gdt", f"{table}_sha256={files[table]}"]
    if with_ui and declare_ui:
        lines += ["", "[ui]", "1161=ui\\1161.uir",
                  f"1161_sha256={sha(folder / 'ui/1161.uir')}"]
    lines += ["", "[counts]", "fixed=104", "coin=78", "custom=2", "candidates=156"]
    (folder / "runtime.ini").write_text("\n".join(lines) + "\n", encoding="ascii")


def harness():
    override = os.environ.get("CAPSULE_RUNTIME_HARNESS")
    candidates = [
        Path(override) if override else None,
        ROOT / "build" / "Debug" / "capsule_client_runtime_harness.exe",
        ROOT / "build" / "Release" / "capsule_client_runtime_harness.exe",
        ROOT / "build-capsule-runtime-test" / "Debug" / "capsule_client_runtime_harness.exe",
    ]
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    raise RuntimeError("capsule_client_runtime_harness.exe has not been built")


class CapsuleClientRuntimeTests(unittest.TestCase):
    def test_declared_capsule_ui_overlay_is_loaded_without_expanding_checksum_gate(self):
        with tempfile.TemporaryDirectory() as temp:
            write_bundle(temp, with_ui=True)
            result = self.run_harness(temp)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("FIND uirs\\1161.uir 1", result.stdout)
            self.assertIn("CHECKSUM_GATE 1", result.stdout)

    def test_undeclared_capsule_ui_overlay_is_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            write_bundle(temp, with_ui=True, declare_ui=False)
            result = self.run_harness(temp)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("UI overlay declaration differs", result.stdout)

    def test_modified_capsule_ui_file_is_rejected_by_its_declared_hash(self):
        with tempfile.TemporaryDirectory() as temp:
            write_bundle(temp, with_ui=True)
            (Path(temp) / "ui/1161.uir").write_bytes(b"modified")
            result = self.run_harness(temp)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("SHA-256 mismatch", result.stdout)

    def run_harness(self, *folders):
        return subprocess.run(
            [str(harness()), *(str(folder) for folder in folders)],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

    def test_valid_bundle_routes_only_declared_native_entries(self):
        with tempfile.TemporaryDirectory() as temp:
            write_bundle(temp)
            result = self.run_harness(temp)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("CHECKSUM_GATE 1", result.stdout)
            self.assertIn(f"OK {REVISION} 6", result.stdout)
            self.assertIn("FIND gdrs\\0033.gdt 1", result.stdout)
            self.assertIn("FIND sprs\\60000.spr 1", result.stdout)
            self.assertIn("FIND txrs\\60000.txr 1", result.stdout)

    def test_hash_failure_rejects_whole_bundle_and_preserves_previous(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            write_bundle(first)
            write_bundle(second)
            with (Path(second) / "data" / "0033.gdt").open("ab") as stream:
                stream.write(b"corrupt")
            result = self.run_harness(first, second)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn("SECOND_FAIL SHA-256 mismatch for data\\0033.gdt", result.stdout)
            self.assertIn("PRESERVED 1", result.stdout)

    def test_parent_traversal_is_rejected_before_file_access(self):
        with tempfile.TemporaryDirectory() as temp:
            write_bundle(temp)
            ini = Path(temp) / "runtime.ini"
            text = ini.read_text("ascii").replace("0033=data\\0033.gdt", "0033=..\\0033.gdt")
            ini.write_text(text, encoding="ascii")
            result = self.run_harness(temp)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("unsafe path", result.stdout)

    def test_zdx_duplicate_and_out_of_bounds_entries_are_rejected(self):
        for option, message in (({"duplicate": True}, "duplicate"), ({"bad_range": True}, "chunk range")):
            with self.subTest(option=option), tempfile.TemporaryDirectory() as temp:
                write_bundle(temp, **option)
                result = self.run_harness(temp)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn(message, result.stdout)

    def test_assets_outside_reserved_overlay_range_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            write_bundle(temp, asset_id=59999)
            result = self.run_harness(temp)
            self.assertEqual(result.returncode, 1, result.stdout)
            self.assertIn("entry path", result.stdout)


if __name__ == "__main__":
    unittest.main()
