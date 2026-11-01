from pathlib import Path
import copy
import importlib.util
import json
import shutil
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("mod_catalog", ROOT / "tools/mod_catalog.py")
catalog = importlib.util.module_from_spec(spec)
spec.loader.exec_module(catalog)


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        for name in ("mods", "docs"):
            shutil.copytree(ROOT / name, self.root / name)
        for name in ("README.md", "README.zh-CN.md", "CMakeLists.txt"):
            shutil.copyfile(ROOT / name, self.root / name)

    def tearDown(self):
        self.temp.cleanup()

    def replace(self, path, before, after):
        p = self.root / path
        p.write_text(p.read_text(encoding="utf-8").replace(before, after), encoding="utf-8")

    def test_complete_source_catalog(self):
        c = catalog.read_catalog(self.root)
        self.assertTrue({"findwindow_fix", "ime_fix", "startup_crash_fix", "pvp_ai", "match_ai", "coop_ai", "mission_ai", "net_probe"}.issubset({m["id"] for m in c["mods"]}))
        self.assertEqual(set(c["features"]["pvp-ai"]["mods"]), {"pvp_ai", "match_ai"})
        self.assertEqual(c["features"]["pvp-ai"]["channel"], "beta")
        self.assertEqual(c["features"]["room-random"]["mods"], ["room_random"])
        self.assertEqual(c["features"]["room-random"]["category"], "gameplay")
        self.assertEqual(c["features"]["room-random"]["activation"], "room-title")
        self.assertFalse(next(m for m in c["mods"] if m["id"] == "net_probe")["enabled_by_default"])

    def test_base_has_only_fixes(self):
        c = catalog.select_catalog(catalog.read_catalog(self.root), "base")
        self.assertTrue({"findwindow_fix", "ime_fix", "startup_crash_fix"}.issubset({m["id"] for m in c["mods"]}))
        self.assertTrue(all(m["category"] == "fixes" for m in c["mods"]))

    def test_starter_policy_is_a_default_server_fix(self):
        c = catalog.select_catalog(catalog.read_catalog(self.root), "base")
        mod = next(m for m in c["mods"] if m["id"] == "new_account_profile")
        self.assertEqual(mod["target"], "server")
        self.assertTrue(mod["enabled_by_default"])
        self.assertEqual(mod["config_files"], ["config.ini"])
        self.assertEqual(mod["hosts"], ["WorldServer-548A86E3-0010C000"])

    def test_catalog_and_package_contents_follow_category_order(self):
        c = catalog.read_catalog(self.root)
        order = {key: i for i, key in enumerate(c["categories"])}
        groups = [order[feature["category"]] for feature in c["features"].values()]
        self.assertEqual(groups, sorted(groups))

    def test_selected_feature_brings_both_halves_and_fixes(self):
        c = catalog.select_catalog(catalog.read_catalog(self.root), features=["pvp-ai"])
        self.assertTrue({"match_ai", "pvp_ai"}.issubset({m["id"] for m in c["mods"]}))
        self.assertTrue(all(m["category"] == "fixes" or m["feature"] == "pvp-ai" for m in c["mods"]))
        self.assertEqual(c["profile"], "custom")

    def test_unknown_feature_is_rejected(self):
        with self.assertRaises(ValueError):
            catalog.select_catalog(catalog.read_catalog(self.root), features=["random-unit-not-bundled"])

    def test_operator_feature_preserves_live_config_and_is_optional(self):
        source=catalog.read_catalog(self.root)
        selected=catalog.select_catalog(source,features=['operator-assistant'])
        mod=next(m for m in selected['mods'] if m['id']=='operator_catalog')
        self.assertEqual(mod['target'],'client')
        self.assertEqual(set(mod['config_files']),{'display.ini','definitions.json'})
        self.assertTrue(all(m['category']=='fixes' or m['id']=='operator_catalog' for m in selected['mods']))
        self.assertNotIn('operator_catalog',{m['id'] for m in catalog.select_catalog(source,'base')['mods']})

    def test_bad_profile_is_rejected(self):
        with self.assertRaises(ValueError):
            catalog.select_catalog(catalog.read_catalog(self.root), "unknown")

    def test_category_is_independent_of_beta(self):
        c = catalog.read_catalog(self.root)
        self.assertEqual(c["features"]["pvp-ai"]["category"], "gameplay")
        self.assertNotIn("beta", c["categories"])

    def test_feature_halves_must_agree(self):
        self.replace("mods/pvp_ai/mod.ini", "channel=beta", "channel=stable")
        with self.assertRaisesRegex(ValueError, "disagrees"):
            catalog.read_catalog(self.root)

    def test_missing_manifest_category_fails(self):
        self.replace("mods/ime_fix/mod.ini", "category=fixes\n", "")
        with self.assertRaises(KeyError):
            catalog.read_catalog(self.root)

    def test_unknown_category_fails(self):
        self.replace("mods/ime_fix/mod.ini", "category=fixes", "category=misc")
        with self.assertRaises(ValueError):
            catalog.read_catalog(self.root)

    def test_diagnostics_cannot_silently_start_on(self):
        self.replace("mods/net_probe/mod.ini", "enabled=0", "enabled=1")
        with self.assertRaisesRegex(ValueError, "disabled"):
            catalog.read_catalog(self.root)

    def test_cmake_registration_matches_manifests(self):
        self.replace("CMakeLists.txt", "sdgo_add_mod(ime_fix ", "sdgo_add_mod(missing_fix ")
        with self.assertRaisesRegex(ValueError, "Build/catalog"):
            catalog.read_catalog(self.root)

    def test_missing_guide_fails(self):
        (self.root / "docs/pvp-ai.md").unlink()
        with self.assertRaisesRegex(ValueError, "guide"):
            catalog.read_catalog(self.root)

    def test_readme_category_and_feature_halves_match(self):
        self.replace("README.zh-CN.md", "`pvp_ai`、`match_ai`", "`pvp_ai`")
        with self.assertRaisesRegex(ValueError, "README"):
            catalog.read_catalog(self.root)

    def test_readme_beta_label_is_checked(self):
        self.replace("README.md", "PvP AI (Beta)", "PvP AI")
        with self.assertRaisesRegex(ValueError, "channel"):
            catalog.read_catalog(self.root)

    def test_network_tools_share_a_complete_feature(self):
        c = catalog.read_catalog(self.root)
        self.assertEqual(set(c["features"]["frp-distribution"]["mods"]), {"net_route", "agent_udp_pool"})
        self.assertEqual(c["features"]["frp-distribution"]["toolkit"], "frp")
        self.assertEqual(next(m for m in c["mods"] if m["id"] == "agent_udp_pool")["target"], "server")
    def test_all_categories_have_readme_blocks(self):
        catalog.read_catalog(self.root)

    def test_unsafe_and_reserved_payloads(self):
        for bad in ("../other.ini", "C:/config.ini", "/root.ini", "a//x.ini", "a/./x.ini", "x.ini:stream", "x.ini."):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                catalog.safe_file(bad)
        self.assertEqual(catalog.safe_file("settings\\owner.json"), "settings/owner.json")

    def test_config_cannot_preserve_code_or_manifest(self):
        for bad in ("ime_fix.dll", "mod.ini", "../sdgo.ini"):
            path = self.root / "mods/ime_fix/mod.ini"
            original = path.read_text(encoding="utf-8")
            path.write_text(original.replace("id=ime_fix", f"id=ime_fix\nconfig_files={bad}"), encoding="utf-8")
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                catalog.read_catalog(self.root)
            path.write_text(original, encoding="utf-8")

    def test_missing_declared_data_fails(self):
        self.replace("mods/mission_ai/mod.ini", "files=mission_ai.lua", "files=missing.lua")
        with self.assertRaisesRegex(ValueError, "payload"):
            catalog.read_catalog(self.root)

    def test_future_data_config_contract_is_supported(self):
        self.replace("mods/mission_ai/mod.ini", "files=mission_ai.lua", "files=mission_ai.lua\nconfig_files=config.ini,settings/roster.json")
        c = catalog.read_catalog(self.root)
        mod = next(m for m in c["mods"] if m["id"] == "mission_ai")
        self.assertEqual(mod["config_files"], ["config.ini", "settings/roster.json"])

    def test_generated_catalog_is_stable_and_portable(self):
        c = catalog.read_catalog(self.root)
        path = self.root / "output/catalog.json"
        catalog.write_catalog(c, path)
        stamp = path.stat().st_mtime_ns
        catalog.write_catalog(c, path)
        self.assertEqual(stamp, path.stat().st_mtime_ns)
        self.assertNotIn(str(self.root), path.read_text(encoding="utf-8"))
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["schema_version"], 1)

    def test_staging_requires_declared_payload_and_current_manifest(self):
        c = catalog.select_catalog(catalog.read_catalog(self.root), features=["coop-wingmen"])
        stage = self.root / "stage"
        for side, loader in (("client", "d3d9.dll"), ("server", "winmm.dll")):
            for name in (loader, "sdgo/core.dll", "sdgo/sdgo.ini"):
                p = stage / side / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b"fixture")
        for mod in c["mods"]:
            out = stage / mod["target"] / "sdgo/mods" / mod["id"]
            out.mkdir(parents=True)
            (out / f"{mod['id']}.dll").write_bytes(b"fixture")
            for name in ("mod.ini", *mod["files"]):
                shutil.copyfile(self.root / "mods" / mod["id"] / name, out / name)
        catalog.validate_stage(self.root, stage, c)
        script = stage / "client/sdgo/mods/mission_ai/mission_ai.lua"
        script.write_text("old script", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Stale"):
            catalog.validate_stage(self.root, stage, c)
        script.unlink()
        with self.assertRaisesRegex(ValueError, "Missing"):
            catalog.validate_stage(self.root, stage, c)


if __name__ == "__main__":
    unittest.main()
