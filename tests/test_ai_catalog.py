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
sys.modules.setdefault("ai_catalog", catalog)
install_spec = importlib.util.spec_from_file_location("install_ai_catalog", Path(__file__).resolve().parents[1] / "backend/install_ai_catalog.py")
installer = importlib.util.module_from_spec(install_spec)
install_spec.loader.exec_module(installer)
sys.modules.setdefault("install_ai_catalog", installer)
export_spec = importlib.util.spec_from_file_location("export_ai_catalog", Path(__file__).resolve().parents[1] / "backend/export_ai_catalog.py")
exporter = importlib.util.module_from_spec(export_spec)
export_spec.loader.exec_module(exporter)


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


def generation_fixture(count=3):
    source = tables()
    source["tbl_Units"] = []
    client = dict(units={}, npcs={}, sha256={}, missing_files=[], security_excluded={})
    for index in range(count):
        row = {key: 0 for key, _ in catalog.UNIT_FIELDS}
        row.update(unit(10001 + index), MasteredWeaponIX=2, AdaptEnvironUnderwater=-8)
        source["tbl_Units"].append(row)
        client["units"][row["UnitNumber"]] = bytes(record(row["UnitNumber"], 4976))
    donor = dict(source["tbl_Units"][0], UnitNumber=3607001)
    source["tbl_UnitNpc"] = [donor]
    bot = dict(NpcDataID=3604001, UseUnitID=3607001, UseUnitHP=14000, NpcPoint=4, ObjectHpUp=0,
               AddUnitEXP=0, AddPoint=0, BossFlag=0, NpcUnitLevel=5, ItemBuffNumber=0, CreateUnitHp=0, DestroyUnitHp=0)
    source["tbl_NpcData"] = [bot]
    npc = record(3604001, 598)
    struct.pack_into("<I", npc, 0x69, 3607001)
    struct.pack_into("<I", npc, 0xE5, 14000)
    script = "VSAI_Gunfighter_01\0".encode("utf-16le")
    npc[0x6D:0x6D + len(script)] = script
    client["npcs"][3604001] = bytes(npc)
    return source, client


def fake_host(path, role):
    _, stamp, size = installer.HOSTS[role]
    binary = bytearray(256); binary[:2] = b"MZ"
    struct.pack_into("<I", binary, 0x3C, 0x80)
    binary[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", binary, 0x84, 0x14C)
    struct.pack_into("<I", binary, 0x88, stamp)
    struct.pack_into("<I", binary, 0x80 + 24 + 56, size)
    path.write_bytes(binary)


def installer_fixture(root, role):
    target, package = root / "different layout" / "game", root / "package"
    target.mkdir(parents=True)
    fake_host(target / installer.HOSTS[role][0], role)
    (target / "sdgo").mkdir()
    (target / "sdgo/core.dll").write_bytes(b"original shared core")
    (target / "sdgo/sdgo.ini").write_bytes(b"[mods]\nunrelated_feature=0\n")
    if role == "server":
        for mod in ("coop_ai", "pvp_ai"):
            directory = target / "sdgo/mods" / mod
            directory.mkdir(parents=True)
            (directory / "mod.ini").write_bytes(b"[settings]\nLevel1=3604001\nKeepUserSetting=YES\n")
            (directory / f"{mod}.dll").write_bytes(b"original " + mod.encode())
            directory = package / role / "sdgo/mods" / mod
            directory.mkdir(parents=True)
            (directory / f"{mod}.dll").write_bytes(b"bridge " + mod.encode())
    own = package / role / "sdgo/mods/ai_catalog"
    own.mkdir(parents=True)
    (own / "mod.ini").write_bytes(b"[mod]\nid=ai_catalog\nenabled=0\nhosts=DEFAULT\n")
    (own / "ai_catalog.dll").write_bytes(b"catalog code")
    _, stamp, size = installer.HOSTS[role]
    name = f"{Path(installer.HOSTS[role][0]).stem}-{stamp:08X}-{size:08X}.ini"
    symbols = package / role / "sdgo/symbols" / name
    symbols.parent.mkdir()
    symbols.write_text(f"[host]\ntimestamp=0x{stamp:08X}\nsize_of_image=0x{size:08X}\n" +
                       "".join(f"[{section}]\nkind=code\nrva=123\nexpect=55 8B EC 90 90\n" for section in installer.SECTIONS[role]), encoding="utf-8")
    installed_symbols = target / "sdgo/symbols" / name
    installed_symbols.parent.mkdir()
    installed_symbols.write_text(f"; preserve this comment\n[host]\ntimestamp=0x{stamp:08X}\nsize_of_image=0x{size:08X}\n[other.feature]\nkind=code\nrva=321\nexpect=55 8B EC 90 90\n", encoding="utf-8")
    source, client = generation_fixture()
    binary, manifest = catalog.generate_catalog(source, client, catalog.prepare(source, client))
    data = root / "owner-catalog.bin"; data.write_bytes(binary)
    return target, package, data


def file_snapshot(root):
    return {path.relative_to(root).as_posix(): path.read_bytes() for path in root.rglob("*") if path.is_file()}


class AiCatalogTests(unittest.TestCase):
    def test_standalone_addon_package_is_disabled_and_contains_no_owner_or_game_data(self):
        import shutil
        import zipfile
        repo = Path(__file__).resolve().parents[1]
        package_spec = importlib.util.spec_from_file_location("package_ai_catalog", repo / "tools/package_ai_catalog.py")
        packager = importlib.util.module_from_spec(package_spec); package_spec.loader.exec_module(packager)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            stage = root / "stage"
            for role in ("client", "server"):
                _, package, _ = installer_fixture(root / role, role)
                shutil.copytree(package / role, stage / role)
            (stage / "client/private.json").write_text("PRIVATE OWNER DATA")
            (stage / "client/GOnline.exe").write_text("GAME MUST NOT BE INCLUDED")
            output = root / "addon.zip"
            packager.package(stage, output)
            with zipfile.ZipFile(output) as archive:
                self.assertIsNone(archive.testzip())
                self.assertFalse(any(name.endswith((".exe", ".bin", ".pyc")) or "private" in name for name in archive.namelist()))
                for role in ("client", "server"):
                    self.assertEqual(installer.ini(archive.read(f"{role}/sdgo/mods/ai_catalog/mod.ini")).getint("mod", "enabled"), 0)
                self.assertIn("backend/install_ai_catalog.py", archive.namelist())
                self.assertIn("backend/export_ai_catalog.py", archive.namelist())
                self.assertIn("docs/ai-catalog.zh-CN.md", archive.namelist())
                self.assertNotIn("client/sdgo/core.dll", archive.namelist())

    def test_hashes_match_independent_fnv_vectors(self):
        self.assertEqual(catalog.fnv(b"hello", 32), 0x4F9F2CAB)
        self.assertEqual(catalog.fnv(b"hello", 64), 0xA430D84680AABD0B)

    def test_player_export_contains_only_public_client_mod_and_no_backend(self):
        import zipfile
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "client")
            (package / "private.json").write_text("PRIVATE SERVER CREDENTIALS")
            output = Path(directory) / "different export folder/players.zip"
            exporter.export(package, data, output)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(set(archive.namelist()), {"README.txt", "manifest.json",
                    "Install-AiCatalog.ps1", "sdgo/mods/ai_catalog/client-symbols.ini",
                    "sdgo/mods/ai_catalog/ai_catalog.dll", "sdgo/mods/ai_catalog/mod.ini", "sdgo/mods/ai_catalog/catalog.bin"})
                self.assertTrue(all(b"PRIVATE SERVER" not in archive.read(name) for name in archive.namelist()))
                self.assertEqual(archive.read("sdgo/mods/ai_catalog/catalog.bin"), data.read_bytes())
                self.assertEqual(installer.ini(archive.read("sdgo/mods/ai_catalog/mod.ini")).getint("mod", "enabled"), 1)
            with self.assertRaises(FileExistsError): exporter.export(package, data, output)

    @unittest.skipUnless(sys.platform == "win32", "player setup uses Windows PowerShell")
    def test_player_setup_merges_symbols_idempotently_and_rejects_conflicts(self):
        import zipfile
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "client")
            output = Path(directory) / "players.zip"
            exporter.export(package, data, output)
            with zipfile.ZipFile(output) as archive: archive.extractall(target)
            symbol = target / "sdgo/symbols/GOnline-54BE1568-00C97000.ini"
            original = symbol.read_bytes()
            unrelated = (target / "sdgo/sdgo.ini").read_bytes()
            def setup():
                return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    str(target / "Install-AiCatalog.ps1"), "-GameDir", str(target)], capture_output=True, timeout=30)
            result = setup()
            self.assertEqual(result.returncode, 0, result.stderr.decode(errors="replace"))
            merged = symbol.read_bytes()
            self.assertTrue(merged.startswith(original))
            self.assertTrue(all(installer.ini(merged).has_section(section) for section in installer.SECTIONS["client"]))
            self.assertEqual(Path(str(symbol) + ".ai-catalog-before").read_bytes(), original)
            self.assertEqual(setup().returncode, 0)
            self.assertEqual(symbol.read_bytes(), merged)
            self.assertEqual((target / "sdgo/sdgo.ini").read_bytes(), unrelated)
            symbol.write_bytes(merged.replace(b"rva=123", b"rva=999", 1))
            conflicting = symbol.read_bytes()
            self.assertNotEqual(setup().returncode, 0)
            self.assertEqual(symbol.read_bytes(), conflicting)

    def test_update_retains_first_install_originals_for_uninstall(self):
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "server")
            original = file_snapshot(target)
            overlay = installer.Overlay(target)
            installer.install(overlay, "server", package, data)
            source, client = generation_fixture()
            rules = {"exclude": [10001]}
            binary, _ = catalog.generate_catalog(source, client, catalog.prepare(source, client, rules), rules)
            data.write_bytes(binary)
            installer.install(overlay, "server", package, data)
            self.assertEqual((target / installer.OWN / "catalog.bin").read_bytes(), binary)
            installer.uninstall(overlay)
            self.assertEqual(file_snapshot(target), original)

    def test_update_preserves_disabled_state_and_refuses_bridge_downgrades(self):
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "server")
            overlay = installer.Overlay(target)
            installer.install(overlay, "server", package, data)
            installer.toggle(overlay, False)
            installer.install(overlay, "server", package, data)
            self.assertEqual(installer.ini((target / installer.OWN / "mod.ini").read_bytes()).getint("mod", "enabled"), 0)
            before = file_snapshot(target)
            def versions(path): return (99, 0, 0, 0) if path.is_relative_to(target) else (1, 0, 0, 0)
            with patch.object(installer, "file_version", side_effect=versions):
                with self.assertRaises(ValueError): installer.install(overlay, "server", package, data)
            self.assertEqual(file_snapshot(target), before)
    def test_generation_produces_paired_native_and_client_records(self):
        source, client = generation_fixture()
        before = copy.deepcopy((source, client))
        binary, manifest = catalog.generate_catalog(source, client, catalog.prepare(source, client))
        magic, version, size, count, checksum = catalog.HEADER.unpack_from(binary)
        self.assertEqual((magic, version, size, count), (catalog.MAGIC, 1, 753, 3))
        self.assertEqual(checksum, catalog.fnv(binary[catalog.HEADER.size:], 32))
        self.assertEqual((source, client), before)
        self.assertEqual(manifest["count"], 3)
        for offset in range(catalog.HEADER.size, len(binary), size):
            npc, unit_id, level, donor, preferred, fingerprint, prefix, native, client_record = catalog.RECORD.unpack_from(binary, offset)
            self.assertEqual(struct.unpack_from("<II", native), (npc, unit_id))
            self.assertEqual(struct.unpack_from("<I", client_record, 0x69)[0], unit_id)
            self.assertEqual(struct.unpack_from("<I", native, 8)[0], struct.unpack_from("<I", client_record, 0xE5)[0])
            self.assertEqual(preferred, 2)
            self.assertEqual(struct.unpack_from("<h", prefix, 0x2D)[0], -8)
            self.assertEqual(fingerprint, catalog.fnv(client["units"][unit_id], 64))

    def test_generation_refuses_missing_client_or_weapon_dependencies(self):
        source, client = generation_fixture()
        for change in (lambda s, c:c["missing_files"].append("0198.gdt"),
                       lambda s, c:c["units"].pop(10001),
                       lambda s, c:s["tbl_Units"][0].update(WeaponNumber_1=999)):
            data, frontend = copy.deepcopy((source, client)); change(data, frontend)
            with self.assertRaises(ValueError):
                catalog.generate_catalog(data, frontend, catalog.prepare(data, frontend))

    def test_generation_repairs_only_ai_initial_weapon_and_supports_template_override(self):
        source, client = generation_fixture()
        source["tbl_Units"][0].update(WeaponNumber_1=0, MasteredWeaponIX=1)
        binary, manifest = catalog.generate_catalog(source, client, catalog.prepare(source, client),
                                                   {"templates": {"10001": 3604001}})
        self.assertEqual(manifest["entries"][0]["preferred_weapon"], 2)
        self.assertEqual(source["tbl_Units"][0]["MasteredWeaponIX"], 1)
        row = catalog.RECORD.unpack_from(binary, catalog.HEADER.size)
        self.assertEqual(struct.unpack_from("<I", row[6], 0x45)[0], 1)

    def test_allocation_ledger_preserves_retired_ids_across_multiple_updates(self):
        source, client = generation_fixture(3)
        _, first = catalog.generate_catalog(source, client, catalog.prepare(source, client))
        previous = {int(k): v for k, v in first["allocations"].items()}
        rule = {"exclude": [10001]}
        _, second = catalog.generate_catalog(source, client, catalog.prepare(source, client, rule), rule, previous)
        self.assertEqual(second["allocations"], first["allocations"])
        self.assertEqual(second["retired_unit_ids"], [10001])
        _, third = catalog.generate_catalog(source, client, catalog.prepare(source, client), previous=previous)
        self.assertEqual(third["allocations"], first["allocations"])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "catalog.json"
            path.write_text(json.dumps(second), encoding="utf-8")
            self.assertEqual(catalog.previous_ids(path), previous)

    def test_generated_bundle_uses_identical_data_on_both_sides(self):
        source, client = generation_fixture()
        report = catalog.prepare(source, client)
        binary, manifest = catalog.generate_catalog(source, client, report)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "owner bundle"
            catalog.write_bundle(report, output, binary, manifest)
            self.assertEqual((output / "client/sdgo/mods/ai_catalog/catalog.bin").read_bytes(), binary)
            self.assertEqual((output / "server/sdgo/mods/ai_catalog/catalog.bin").read_bytes(), binary)
            self.assertFalse(list(output.rglob("*.sql")))

    def test_install_toggle_update_uninstall_restores_all_original_file_bytes(self):
        for role in ("client", "server"):
            with self.subTest(role=role), tempfile.TemporaryDirectory() as directory:
                target, package, data = installer_fixture(Path(directory), role)
                original = file_snapshot(target)
                overlay = installer.Overlay(target)
                self.assertEqual(installer.install(overlay, role, package, data), "installed")
                installed = file_snapshot(target)
                self.assertEqual(installer.install(overlay, role, package, data), "unchanged")
                self.assertEqual(file_snapshot(target), installed)
                self.assertEqual(installer.toggle(overlay, False), "disabled")
                disabled = file_snapshot(target)
                self.assertEqual(installer.toggle(overlay, False), "unchanged")
                self.assertEqual(file_snapshot(target), disabled)
                self.assertEqual(installer.toggle(overlay, True), "enabled")
                self.assertEqual(file_snapshot(target), installed)
                self.assertEqual(installer.uninstall(overlay), "uninstalled")
                self.assertEqual(file_snapshot(target), original)
                self.assertEqual(installer.uninstall(overlay), "unchanged")

    def test_stock_disabled_framework_scaffold_can_be_installed_and_restored(self):
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "client")
            own = target / installer.OWN; own.mkdir(parents=True)
            for filename in ("mod.ini", "ai_catalog.dll"):
                (own / filename).write_bytes((package / "client" / installer.OWN / filename).read_bytes())
            original = file_snapshot(target)
            overlay = installer.Overlay(target)
            installer.install(overlay, "client", package, data)
            installer.uninstall(overlay)
            self.assertEqual(file_snapshot(target), original)

    def test_uninstall_conflict_preserves_another_updaters_work_and_allows_disable(self):
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "server")
            overlay = installer.Overlay(target)
            installer.install(overlay, "server", package, data)
            changed = target / "sdgo/mods/pvp_ai/pvp_ai.dll"
            changed.write_bytes(b"another updater owns this change")
            before = file_snapshot(target)
            with self.assertRaises(ValueError): installer.uninstall(overlay)
            self.assertEqual(file_snapshot(target), before)
            self.assertEqual(installer.toggle(overlay, False), "disabled")
            self.assertEqual(changed.read_bytes(), b"another updater owns this change")

    def test_interrupted_install_recovery_is_scoped_and_refuses_external_changes(self):
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "client")
            overlay = installer.Overlay(target)
            original = file_snapshot(target)
            real_write = overlay.write
            failed = False
            def failing_write(relative, value):
                nonlocal failed
                if relative == installer.OWN + "mod.ini" and value is not None and not failed:
                    failed = True; raise OSError("injected copy failure")
                return real_write(relative, value)
            with patch.object(overlay, "write", side_effect=failing_write):
                with self.assertRaises(OSError): installer.install(overlay, "client", package, data)
            self.assertEqual(file_snapshot(target), original)
            pending = {"previous_state": None, "changes": {installer.OWN + "mod.ini": {"before": None, "after": installer.digest(b"ours")}}}
            overlay.write(installer.PENDING, json.dumps(pending).encode())
            overlay.write(installer.OWN + "mod.ini", b"external")
            before = file_snapshot(target)
            with self.assertRaises(ValueError): overlay.recover()
            self.assertEqual(file_snapshot(target), before)

    def test_installer_rejects_path_escape_and_a_running_target_before_writes(self):
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "server")
            overlay = installer.Overlay(target)
            for path in ("../foreign", "sdgo/mods/ai_catalog/../../foreign/mod.ini", "sdgo/sdgo.ini"):
                with self.assertRaises(ValueError): overlay.path(path)
            before = file_snapshot(target)
            with patch.object(installer, "running", return_value=True):
                with self.assertRaises(ValueError): installer.install(overlay, "server", package, data)
            self.assertEqual(file_snapshot(target), before)

    def test_installer_preserves_global_disable_and_unrelated_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            target, package, data = installer_fixture(Path(directory), "client")
            overlay = installer.Overlay(target)
            installer.install(overlay, "client", package, data)
            installer.toggle(overlay, False)
            (target / "sdgo/sdgo.ini").write_bytes(b"[mods]\nai_catalog=0\nunrelated=1\n")
            before = file_snapshot(target)
            with self.assertRaises(ValueError): installer.toggle(overlay, True)
            self.assertEqual(file_snapshot(target), before)

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
            for name in ("ai_catalog.py", "install_ai_catalog.py", "export_ai_catalog.py", "ai_catalog.rules.example.json", "README.zh-CN.md"):
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
                self.assertEqual(backend, {"backend/ai_catalog.py", "backend/install_ai_catalog.py", "backend/export_ai_catalog.py", "backend/ai_catalog.rules.example.json",
                                           "backend/README.zh-CN.md"})
                self.assertFalse(any(b"PRIVATE" in archive.read(name) for name in archive.namelist()
                                     if not name.endswith("/")))


if __name__ == "__main__":
    unittest.main()
