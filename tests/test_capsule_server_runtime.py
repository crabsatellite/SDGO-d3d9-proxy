import copy
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import capsule_server_runtime as runtime


def fixture():
    units = [
        {"id": index, "name": f"Unit {index}", "rank": 1, "subgrade": 2}
        for index in range(1, 13)
    ]
    fixed = {
        "id": 7901,
        "name": "Weighted",
        "enabled": True,
        "owner": "initialization",
        "currency": "GP",
        "price": 1200,
        "draw_policy": "independent_weighted",
        "slots": [
            {"unit_id": index, "weight": weight, "native_draw_group": group}
            for index, weight, group in zip(
                range(1, 7), (20, 100, 245, 245, 250, 140), (1, 3, 5, 5, 5, 5)
            )
        ],
    }
    custom = {
        "id": 1,
        "name": "Custom",
        "enabled": True,
        "owner": "native",
        "currency_type": 0,
        "price": 2000,
        "draw_policy": "native_mileage",
        "slot_rules": [
            {
                "slot": index,
                "rank_max": 4 if index <= 3 or index >= 7 else 1,
                "subgrades": [2, 5, 4, 3] if index <= 3 or index >= 7 else [2],
                "extension": index >= 7,
            }
            for index in range(1, 9)
        ],
        "candidates": [
            {
                "unit_id": 7,
                "basic_weight": 800,
                "mileage": 0.25,
                "extra_price": 50,
                "reset_mileage": False,
            },
            {
                "unit_id": 8,
                "basic_weight": 1500,
                "mileage": 0.0,
                "extra_price": 0,
                "reset_mileage": True,
            },
        ],
    }
    return {
        "schema_version": 1,
        "catalog_revision": hashlib.sha256(b"fixture").hexdigest(),
        "requires_runtime_adapter": True,
        "fixed_machines": [fixed],
        "custom_machines": [custom],
        "units": units,
        "role": "server",
    }


class ValidationTests(unittest.TestCase):
    def test_source_bound_custom_policy_is_enforced_by_server_builder(self):
        bundle = fixture()
        bundle["custom_candidate_policy"] = {"version": 1, "native_candidate_unit_ids": [7, 8]}
        bundle["units"][8].update(rank=4, subgrade=2, has_blueprint_recipe=False)
        bundle["custom_machines"][0]["candidates"].append({
            "unit_id": 9, "basic_weight": 1, "mileage": 0,
            "extra_price": 0, "reset_mileage": False,
        })
        with self.assertRaises(ValueError):
            runtime.validate_bundle(bundle)
        bundle["units"][8]["subgrade"] = 4
        runtime.validate_bundle(bundle)
        bundle["units"][8].update(subgrade=3, has_blueprint_recipe=True)
        with self.assertRaises(ValueError):
            runtime.validate_bundle(bundle)
        bundle["units"][8]["has_blueprint_recipe"] = False
        runtime.validate_bundle(bundle)

    def test_rejects_new_machine_without_weighted_policy(self):
        bundle = fixture()
        bundle["fixed_machines"][0]["draw_policy"] = "native_sequence"
        with self.assertRaisesRegex(ValueError, "independent_weighted"):
            runtime.validate_bundle(bundle)

    def test_rejects_weight_gap_and_unknown_custom_grade_set(self):
        bundle = fixture()
        bundle["fixed_machines"][0]["slots"][0]["weight"] += 1
        with self.assertRaisesRegex(ValueError, "total 1000"):
            runtime.validate_bundle(bundle)
        bundle = fixture()
        bundle["custom_machines"][0]["slot_rules"][0]["subgrades"] = [2, 5, 3, 4]
        with self.assertRaisesRegex(ValueError, "native Grade code"):
            runtime.validate_bundle(bundle)

    def test_rejects_unsupported_custom_currency_and_smallint_overflow(self):
        bundle = fixture()
        bundle["custom_machines"][0]["currency_type"] = 2
        with self.assertRaisesRegex(ValueError, "0 \\(GP\\) or 1 \\(coin\\)"):
            runtime.validate_bundle(bundle)
        bundle = fixture()
        bundle["custom_machines"][0]["candidates"][0]["basic_weight"] = 32768
        with self.assertRaisesRegex(ValueError, "SQL SMALLINT"):
            runtime.validate_bundle(bundle)

    def test_companion_manifest_must_match_exact_server_bytes(self):
        bundle = fixture()
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            server = root / "server.json"
            server.write_text(json.dumps(bundle), encoding="utf-8")
            (root / "manifest.json").write_text(json.dumps({
                "catalog_revision": bundle["catalog_revision"],
                "files": {"server.json": "0" * 64},
            }), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "hash differs"):
                runtime.verify_input_manifest(server, bundle)


class SamplingTests(unittest.TestCase):
    def test_every_boundary_maps_to_exact_weight_interval(self):
        slots = fixture()["fixed_machines"][0]["slots"]
        expected = [
            (0, 19, 1), (20, 119, 2), (120, 364, 3),
            (365, 609, 4), (610, 859, 5), (860, 999, 6),
        ]
        for lower, upper, unit_id in expected:
            self.assertEqual(runtime.weighted_pick(slots, lower)[0], unit_id)
            self.assertEqual(runtime.weighted_pick(slots, upper)[0], unit_id)
        with self.assertRaises(ValueError):
            runtime.weighted_pick(slots, 1000)

class GenerationTests(unittest.TestCase):
    def build(self, bundle):
        temp = tempfile.TemporaryDirectory()
        root = Path(temp.name)
        source = root / "server.json"
        source.write_text(json.dumps(bundle, ensure_ascii=False), encoding="utf-8")
        output = root / "runtime"
        manifest = runtime.build(source, output)
        return temp, output, manifest

    def test_output_is_content_deterministic_and_hash_verified(self):
        bundle = fixture()
        first_temp, first, first_manifest = self.build(bundle)
        self.addCleanup(first_temp.cleanup)
        second_temp, second, second_manifest = self.build(bundle)
        self.addCleanup(second_temp.cleanup)
        self.assertEqual(first_manifest, second_manifest)
        for name, expected in first_manifest["files"].items():
            self.assertEqual((first / name).read_bytes(), (second / name).read_bytes())
            self.assertEqual(hashlib.sha256((first / name).read_bytes()).hexdigest(), expected)

    def test_install_intercepts_selection_only_and_preserves_native_delegate(self):
        temp, output, manifest = self.build(fixture())
        self.addCleanup(temp.cleanup)
        sql = (output / "install.sql").read_text(encoding="utf-8")
        self.assertIn("CREATE PROCEDURE dbo.sp_GetNextCapsule1", sql)
        self.assertIn("EXEC @NativeReturn = dbo.sdgo_native_sp_GetNextCapsule1", sql)
        self.assertIn("RETURN @NativeReturn", sql)
        self.assertIn("CRYPT_GEN_RANDOM(4)", sql)
        self.assertIn("CONVERT(BIGINT, @RandomBytes)", sql)
        self.assertIn("@RandomValue < CONVERT(BIGINT, 4294967000)", sql)
        self.assertIn("@RandomValue % 1000", sql)
        self.assertNotIn("CHECKSUM(NEWID())", sql)
        self.assertIn("RAISERROR(@ErrorMessage, 16, 1);\n  RETURN;", sql)
        self.assertIn("sp_CreateCapsule", runtime.__doc__)
        self.assertNotIn("tbl_UserUnits", sql)
        self.assertNotIn("tbl_UserCustomCapsuleRecord", sql)
        self.assertNotIn("INSERT INTO dbo.tbl_CoinMachinePrice", sql)
        self.assertTrue(manifest["safety"]["native_charge_and_grant_path_preserved"])

    def test_coin_variant_uses_captured_disable_state_and_new_machine_gets_none(self):
        bundle = fixture()
        native = copy.deepcopy(bundle["fixed_machines"][0])
        native.update({"id": 101, "owner": "native", "draw_policy": "native_sequence",
                       "enabled": False})
        bundle["fixed_machines"].insert(0, native)
        temp, output, _ = self.build(bundle)
        self.addCleanup(temp.cleanup)
        install = (output / "install.sql").read_text(encoding="utf-8")
        rollback = (output / "rollback.sql").read_text(encoding="utf-8")
        self.assertIn("OriginalDisable", install)
        self.assertIn("CASE WHEN f.Enabled = 0 THEN 1 ELSE b.OriginalDisable END", install)
        self.assertIn("SET c.Disable=b.OriginalDisable", rollback)
        self.assertNotIn("INSERT INTO dbo.tbl_CoinMachinePrice", install)

    def test_custom_create_update_disable_and_candidate_replacement_are_staged(self):
        bundle = fixture()
        disabled = copy.deepcopy(bundle["custom_machines"][0])
        disabled["id"] = 2
        disabled["enabled"] = False
        bundle["custom_machines"].append(disabled)
        temp, output, _ = self.build(bundle)
        self.addCleanup(temp.cleanup)
        install = (output / "install.sql").read_text(encoding="utf-8")
        verify = (output / "verify.sql").read_text(encoding="utf-8")
        self.assertIn("DELETE n\n    FROM dbo.tbl_CustomCapsuleInfo", install)
        self.assertIn("INSERT INTO dbo.tbl_CustomCapsuleList", install)
        self.assertIn("INSERT INTO dbo.tbl_CustomCapsuleInfo", install)
        self.assertIn("WHERE c.Enabled=0 OR e.MachineID IS NULL", verify)
        self.assertNotIn("tbl_UserCustomCapsuleRecord", install)

    def test_completely_new_custom_machine_is_created_and_can_be_soft_deleted(self):
        bundle = fixture()
        created = copy.deepcopy(bundle["custom_machines"][0])
        created.update({
            "id": 3,
            "name": "New Custom 3",
            "price": 3456,
            "owner": "user",
        })
        created["candidates"] = [copy.deepcopy(created["candidates"][0])]
        bundle["custom_machines"].append(created)
        temp, output, _ = self.build(bundle)
        self.addCleanup(temp.cleanup)
        install = (output / "install.sql").read_text(encoding="utf-8")
        self.assertIn("(3, 1, 0, 0, 3456, N'New Custom 3'", install)
        self.assertIn("INSERT INTO dbo.tbl_CustomCapsuleList", install)
        self.assertIn("INSERT INTO dbo.tbl_CustomCapsuleInfo", install)
        self.assertIn("include it with Enabled=0 to delete it", install)
        self.assertIn("collides with unowned native configuration", install)

        created["enabled"] = False
        temp, output, _ = self.build(bundle)
        self.addCleanup(temp.cleanup)
        install = (output / "install.sql").read_text(encoding="utf-8")
        self.assertIn("(3, 0, 0, 0, 3456, N'New Custom 3'", install)
        self.assertIn("WHERE c.Enabled = 0", install)

    def test_native_sequence_slots_are_fail_closed_against_machine_dice(self):
        bundle = fixture()
        native = bundle["fixed_machines"][0]
        native.update({"id": 101, "owner": "native", "draw_policy": "native_sequence"})
        temp, output, _ = self.build(bundle)
        self.addCleanup(temp.cleanup)
        install = (output / "install.sql").read_text(encoding="utf-8")
        verify = (output / "verify.sql").read_text(encoding="utf-8")
        self.assertIn("CREATE TABLE #NativeSlot", install)
        self.assertIn("FROM dbo.tbl_MachineDice", install)
        self.assertIn("promote the machine to independent_weighted", install)
        self.assertIn("Native-sequence slots differ from tbl_MachineDice", verify)

    def test_rollback_restores_captured_configuration_and_native_name(self):
        temp, output, _ = self.build(fixture())
        self.addCleanup(temp.cleanup)
        sql = (output / "rollback.sql").read_text(encoding="utf-8")
        self.assertIn("N'dbo.sdgo_native_sp_GetNextCapsule1'", sql)
        self.assertIn("N'sp_GetNextCapsule1'", sql)
        self.assertIn("sdgo_mod_CapsuleBaselineMachinePrice", sql)
        self.assertIn("sdgo_mod_CapsuleBaselineCustomInfo", sql)


if __name__ == "__main__":
    unittest.main()
