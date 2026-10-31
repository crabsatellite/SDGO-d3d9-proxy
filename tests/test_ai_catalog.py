import copy
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch


spec = importlib.util.spec_from_file_location("ai_catalog", Path(__file__).resolve().parents[1] / "backend/ai_catalog.py")
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


def unit(number=10001, grade=1, variant=2, ready=1):
    return dict(UnitNumber=number, UnitModelName="Example", UnitGradeNumber=grade,
                UnitGradeNumber2=variant, SallyReady=ready, HP=12000,
                UnitNumber_Transform=0, MasteredWeaponType=1,
                WeaponNumber_1=101, WeaponNumber_2=102,
                WeaponNumber_3=103, WeaponNumber_FatalAttack=104)


def tables():
    return {"tbl_Units": [unit()], "tbl_UnitNpc": [], "tbl_NpcData": [],
            "tbl_UnitGrades": [dict(UnitGradeNumber=g, UnitGradeNumber2=v, UnitGradeName=f"{g}-{v}")
                               for g in range(1, 5) for v in range(1, 6)],
            "tbl_WeaponsRev": [{"WeaponNo": i} for i in range(101, 105)]}


def record(number, size, security=0):
    result = bytearray(size)
    struct.pack_into("<I", result, 0, number)
    result[4] = security
    return result


def gdt(records):
    size, count, extra = len(records[0]), len(records), 17
    key_size = (size + count + extra) % 65 + 192
    key = bytes(reversed(range(key_size)))
    output = bytearray(b"\xf9\xf8" + struct.pack("<iii", size, count, extra) + key)
    for row in records:
        for offset in range(0, size, key_size):
            encoded = bytearray(key_size)
            for index, value in enumerate(row[offset:offset + key_size]):
                encoded[key[index]] = value ^ key[(0x300 + 3 * index) % key_size]
            output.extend(encoded)
    return bytes(output)


class AiCatalogTests(unittest.TestCase):
    def test_grouping_includes_five_distinct_levels_and_excludes_non_sortie_rows(self):
        source = tables()
        source["tbl_Units"] = [unit(10000 + g, g) for g in range(1, 5)] + [unit(10005, 4, 3), unit(10006, ready=0)]
        before = copy.deepcopy(source)
        report = catalog.prepare(source)
        self.assertEqual(report["levels"], {str(i): 1 for i in range(1, 6)})
        self.assertEqual(report["skipped"], [{"unit_id": 10006, "reason": "not_sortie_ready"}])
        self.assertFalse(report["ready_to_install"])
        self.assertEqual(source, before)

    def test_owner_exclusion_and_level_override(self):
        source = tables()
        source["tbl_Units"].append(unit(10002))
        report = catalog.prepare(source, rules={"exclude": [10001], "levels": {"10002": 5}})
        self.assertEqual(report["candidate_count"], 1)
        self.assertEqual(report["candidates"][0]["level"], 5)
        with self.assertRaises(ValueError):
            catalog.prepare(source, rules={"exclude": [99999]})

    def test_slot_suggestions_avoid_both_client_and_database_npc_ids(self):
        source = tables()
        source["tbl_NpcData"] = [{"NpcDataID": 3604001, "UseUnitID": 3607001, "UseUnitHP": 14000}]
        client = dict(units={}, npcs={3604002: bytes(record(3604002, 598))},
                      sha256={}, missing_files=[], security_excluded={})
        report = catalog.prepare(source, client)
        self.assertEqual(report["candidates"][0]["proposed_npc_id"], 3604003)
        self.assertEqual(report["npc_slots_free"], 2998)

    def test_exhausted_capacity_does_not_emit_partial_assignments(self):
        source = tables()
        source["tbl_Units"].append(unit(10002))
        source["tbl_NpcData"] = [dict(NpcDataID=i, UseUnitID=3607001, UseUnitHP=100)
                                 for i in range(catalog.NPC_FIRST, catalog.NPC_LAST)]
        report = catalog.prepare(source)
        self.assertFalse(report["npc_capacity_ok"])
        self.assertTrue(all(row["proposed_npc_id"] is None for row in report["candidates"]))

    def test_transform_weapon_and_client_dependencies_are_reported(self):
        source = tables()
        source["tbl_Units"][0].update(UnitNumber_Transform=10002, WeaponNumber_1=999, WeaponNumber_3=0)
        client = dict(units={10001: bytes(record(10001, 4976))}, npcs={},
                      sha256={}, missing_files=[], security_excluded={})
        row = catalog.prepare(source, client)["candidates"][0]
        self.assertEqual(row["missing_client_unit_ids"], [10002])
        self.assertEqual(row["missing_server_npc_unit_ids"], [10001, 10002])
        self.assertEqual(row["missing_weapon_ids"], [999])
        self.assertEqual(row["empty_weapon_slots"], [3])

    def test_npc_unit_and_hp_need_to_match_the_client(self):
        source = tables()
        source["tbl_NpcData"] = [dict(NpcDataID=3604001, UseUnitID=3607001, UseUnitHP=15000)]
        npc = record(3604001, 598)
        struct.pack_into("<I", npc, 0x69, 3607001)
        struct.pack_into("<I", npc, 0xE5, 14000)
        client = dict(units={}, npcs={3604001: bytes(npc)}, sha256={}, missing_files=[], security_excluded={})
        self.assertFalse(catalog.prepare(source, client)["existing_bots"][0]["client_npc_unit_and_hp_match"])
        struct.pack_into("<I", npc, 0xE5, 15000)
        client["npcs"][3604001] = bytes(npc)
        self.assertTrue(catalog.prepare(source, client)["existing_bots"][0]["client_npc_unit_and_hp_match"])

    def test_legacy_level_buffer_overflow_is_detected(self):
        source = tables()
        source["tbl_Units"] = [unit(10000 + i) for i in range(150)]
        report = catalog.prepare(source)
        self.assertEqual(report["levels_exceeding_legacy_buffer"], [1])
        self.assertEqual(report["proposed_pool_value_bytes"]["1"], 1199)

    def test_malformed_or_duplicate_catalog_rows_fail(self):
        source = tables()
        source["tbl_Units"].append(unit())
        with self.assertRaises(ValueError):
            catalog.prepare(source)
        source = tables()
        source["tbl_Units"][0]["SallyReady"] = True
        with self.assertRaises(ValueError):
            catalog.prepare(source)

    def test_strict_gdt_reader_handles_multiple_chunks_and_records(self):
        rows = [record(10001, 4976), record(10002, 4976)]
        for index, row in enumerate(rows):
            row[100:] = bytes((i * 7 + index) % 256 for i in range(4876))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "0237.gdt"
            path.write_bytes(gdt(rows))
            self.assertEqual(catalog.read_gdt(path, 4976), rows)
            complete = path.read_bytes()
            for data in (complete[:-1], complete + b"extra", b"xx" + complete[2:]):
                path.write_bytes(data)
                with self.assertRaises(ValueError):
                    catalog.read_gdt(path, 4976)

    def test_client_skips_security_records_but_rejects_conflicting_ids(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "0237.gdt"
            path.write_bytes(gdt([record(10001, 4976), record(10002, 4976, 1)]))
            result = catalog.read_client(root)
            self.assertEqual(set(result["units"]), {10001})
            self.assertEqual(result["security_excluded"]["0237.gdt"], 1)
            self.assertIn("0198.gdt", result["missing_files"])
            other = record(10001, 4976)
            other[80] = 1
            (root / "0242.gdt").write_bytes(gdt([other]))
            with self.assertRaises(ValueError):
                catalog.read_client(root)

    def test_export_reader_requires_contiguous_parts_and_does_not_read_accounts(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = tables()
            for table, rows in source.items():
                (root / f"dbo.{table}.json").write_text(json.dumps(rows), encoding="utf-8")
            (root / "dbo.tbl_Users.json").write_text("PRIVATE INVALID JSON", encoding="utf-8")
            self.assertEqual(catalog.read_exports(root), source)
            path = root / "dbo.tbl_Units.json"
            path.rename(root / "dbo.tbl_Units.part2.json")
            with self.assertRaises(ValueError):
                catalog.read_exports(root)

    def test_odbc_reader_uses_only_catalog_selects_and_closes_connection(self):
        statements = []
        class Connection:
            closed = False
            description = [("UnitNumber",)]
            def cursor(self): return self
            def execute(self, sql): statements.append(sql)
            def fetchall(self): return []
            def close(self): self.closed = True
        connection = Connection()
        fake = types.SimpleNamespace(connect=lambda *a, **k: connection, Error=RuntimeError)
        with patch.dict(sys.modules, {"pyodbc": fake}), patch.dict(os.environ, {"CATALOG_TEST": "PRIVATE"}):
            catalog.read_database("CATALOG_TEST")
        self.assertEqual(set(statements), {f"SELECT * FROM dbo.[{t}]" for t in catalog.TABLES})
        self.assertTrue(connection.closed)

    def test_portable_output_is_explicitly_a_plan_and_never_overwrites(self):
        source = tables()
        source["tbl_Units"][0]["UnitModelName"] = "=malicious formula"
        report = catalog.prepare(source)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "arbitrary owner layout" / "roster"
            catalog.write_plan(report, root)
            self.assertEqual(len(list(root.iterdir())), 7)
            self.assertIn("'=malicious", (root / "level-1.csv").read_text(encoding="utf-8-sig"))
            self.assertFalse(json.loads((root / "audit.json").read_text(encoding="utf-8"))["ready_to_install"])
            self.assertEqual(list(root.glob("*.sql")), [])
            with self.assertRaises(FileExistsError):
                catalog.write_plan(report, root)

    def test_rules_reject_unknown_settings_and_invalid_levels(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rules.json"
            for value in ({"password": "PRIVATE"}, {"levels": {"10001": 6}},
                          {"levels": {"010001": 2}}, {"exclude": [True]}):
                path.write_text(json.dumps(value), encoding="utf-8")
                with self.assertRaises(ValueError):
                    catalog.rules_from_json(path)

    @unittest.skipUnless(sys.platform == "win32", "release packager uses Windows PowerShell")
    def test_release_package_contains_only_backend_sources(self):
        repo = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "portable release fixture"
            for name in ("tools", "backend", "docs", "third_party/minhook",
                         "build/stage/Release/client", "build/stage/Release/server"):
                (root / name).mkdir(parents=True)
            script = root / "tools/package.ps1"
            script.write_bytes((repo / "tools/package.ps1").read_bytes())
            for name in ("ai_catalog.py", "ai_catalog.rules.example.json", "README.zh-CN.md"):
                (root / "backend" / name).write_bytes((repo / "backend" / name).read_bytes())
            (root / "backend/private.json").write_text("PRIVATE DO NOT PACKAGE")
            for name in ("README.md", "README.zh-CN.md", "THIRD_PARTY_NOTICES.md",
                         "tools/install_client.ps1", "tools/install_server.ps1", "third_party/minhook/LICENSE.txt"):
                (root / name).write_text("fixture")
            (root / "VERSION").write_text("0.0.1")
            environment = dict(os.environ)
            modules = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/Modules"
            environment["PSModulePath"] = str(modules) + os.pathsep + environment.get("PSModulePath", "")
            run = subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script)],
                                 capture_output=True, text=True, timeout=30, env=environment)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            import zipfile
            with zipfile.ZipFile(root / "dist/SDGO-ModFramework-0.0.1.zip") as archive:
                backend = {name for name in archive.namelist() if name.startswith("backend/")}
                self.assertEqual(backend, {"backend/ai_catalog.py", "backend/ai_catalog.rules.example.json",
                                           "backend/README.zh-CN.md"})
                self.assertFalse(any(b"PRIVATE" in archive.read(name) for name in archive.namelist()
                                     if not name.endswith("/")))


if __name__ == "__main__":
    unittest.main()
