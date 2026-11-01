import copy
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import capsule_catalog as c


def fixture():
    units = [{"id": i, "name": str(i), "rank": 1 if i < 3 else 2,
              "subgrade": 2, "grade": "C" if i < 3 else "B", "eligible": True,
              "transform_to": 0} for i in range(1, 5)]
    custom = {"id": 1, "name": "custom", "enabled": True, "price": 2000, "owner": "native",
              "candidates": [{"unit_id": 1, "basic_weight": 1500, "mileage": 0.0,
                               "extra_price": 0, "reset_mileage": False},
                              {"unit_id": 4, "basic_weight": 800, "mileage": 0.0,
                               "extra_price": 0, "reset_mileage": False}],
              "slot_rules": [{"slot": i, "rank_max": 4, "subgrades": [2, 3, 4, 5],
                              "extension": i >= 7} for i in range(1, 9)]}
    cat = {"schema_version": 1, "units": units, "fixed_machines": [], "custom_machines": [custom]}
    tables = {k: [] for k in ["tbl_ItemCategory", "tbl_MixTreeFile", "tbl_CoinMachinePrice",
        "tbl_CoinCapsuleList", "tbls_StoreItems", "tbls_SubItems", "tbl_QuestInfo",
        "tbl_QuestPrerequest", "tbl_MachinePrice", "tbl_BattleResultItemDropDice",
        "tbl_BattleResultItemDropSetting"]}
    return cat, tables


def recipe(rid, output, inputs):
    return {"Idx": rid, "OutUnitID": output,
            **{f"UnitID{i}": inputs[i-1] if i <= len(inputs) else 0 for i in range(1, 8)}}


def add_plan(tables, rid):
    # Native blueprint category 154xxxx points to recipe 200xxx; it is not
    # classified by the first decimal digit of the category number.
    category = 1540000 + rid - 200000
    tables["tbl_ItemCategory"].append({"CatalogueID": category, "RefIndex": rid, "TimeLimit": 0})
    tables["tbls_StoreItems"].append({"f_itemid": str(rid), "f_display": "1", "f_depth0": 3,
                                    "f_quantity": 1})
    tables["tbls_SubItems"].append({"f_itemid": str(rid), "f_display": "1", "f_categoryNumber": category})


class AcquisitionTests(unittest.TestCase):
    def run_analysis(self, cat, tables, extra=()):
        with patch.object(c, "table", side_effect=lambda root, db, name, **kw: tables[name]):
            return c.acquisition("fixture", cat, extra)

    def test_recipe_requires_blueprint_and_every_material(self):
        cat, tables = fixture()
        tables["tbl_MixTreeFile"] = [recipe(200001, 3, [1, 2])]
        self.assertNotIn(3, self.run_analysis(cat, tables, [2])["known"])
        add_plan(tables, 200001)
        self.assertNotIn(3, self.run_analysis(cat, tables)["known"])
        self.assertIn(3, self.run_analysis(cat, tables, [2])["known"])

    def test_cycle_needs_an_external_root(self):
        cat, tables = fixture()
        tables["tbl_MixTreeFile"] = [recipe(200001, 2, [3]), recipe(200002, 3, [2])]
        add_plan(tables, 200001)
        add_plan(tables, 200002)
        a = self.run_analysis(cat, tables)
        self.assertNotIn(2, a["known"])
        self.assertNotIn(3, a["known"])
        self.assertIn(3, self.run_analysis(cat, tables, [2])["known"])

    def test_alternative_recipe_is_or_materials_are_and(self):
        cat, tables = fixture()
        tables["tbl_MixTreeFile"] = [recipe(200001, 3, [2]), recipe(200002, 3, [1, 4])]
        add_plan(tables, 200002)
        self.assertEqual(self.run_analysis(cat, tables)["known"][3]["recipe_id"], 200002)

    def test_hidden_product_does_not_make_plan_available(self):
        cat, tables = fixture()
        add_plan(tables, 200001)
        tables["tbls_StoreItems"][0]["f_display"] = "0"
        self.assertFalse(self.run_analysis(cat, tables)["blueprints"])

    def test_rental_not_an_inventory_material(self):
        cat, tables = fixture()
        tables["tbl_ItemCategory"] = [{"CatalogueID": 1110001, "RefIndex": 2, "TimeLimit": 1}]
        tables["tbls_StoreItems"] = [{"f_itemid": "rent", "f_display": "1", "f_depth0": 3, "f_quantity": 1}]
        tables["tbls_SubItems"] = [{"f_itemid": "rent", "f_display": "1", "f_categoryNumber": 1110001}]
        self.assertNotIn(2, self.run_analysis(cat, tables)["known"])

    def test_transformation_not_a_second_recipe_material(self):
        cat, tables = fixture()
        cat["units"][0]["transform_to"] = 2
        tables["tbl_MixTreeFile"] = [recipe(200001, 3, [2])]
        add_plan(tables, 200001)
        a = self.run_analysis(cat, tables)
        self.assertIn(2, a["forms"])
        self.assertNotIn(2, a["known"])
        self.assertNotIn(3, a["known"])

    def test_unconnected_drop_pool_does_not_grant_anything(self):
        cat, tables = fixture()
        tables["tbl_ItemCategory"] = [{"CatalogueID": 1120001, "RefIndex": 2, "TimeLimit": 0}]
        tables["tbl_BattleResultItemDropDice"] = [{"ItemMachineID": 9, "CategoryID": 1120001, "Prob": 1}]
        self.assertNotIn(2, self.run_analysis(cat, tables)["known"])
        tables["tbl_BattleResultItemDropSetting"] = [{"NormalMachineID": 9, "RareMachineID": 0, "UniqueMachineID": 0}]
        self.assertIn(2, self.run_analysis(cat, tables)["known"])

    def test_initialization_minimizes_recipe_roots_and_is_idempotent(self):
        cat, tables = fixture()
        tables["tbl_MixTreeFile"] = [recipe(200001, 3, [1, 2])]
        add_plan(tables, 200001)
        with patch.object(c, "table", side_effect=lambda root, db, name, **kw: tables[name]):
            result, report = c.initialize("fixture", cat)
            self.assertEqual([u["id"] for u in report["seeds"]], [2])
            again, receipt = c.initialize("fixture", result)
        self.assertEqual(result, again)
        self.assertEqual(receipt["seeds"], [])
        self.assertEqual(cat["custom_machines"][0]["candidates"][0]["unit_id"], 1)

    def test_later_material_prunes_an_earlier_redundant_seed(self):
        cat, tables = fixture()
        cat["units"][1]["rank"] = 2
        cat["units"][1]["grade"] = "B"
        # Unit 2 is visited before unit 3 but is craftable only after 3 is seeded.
        tables["tbl_MixTreeFile"] = [recipe(200001, 2, [3])]
        add_plan(tables, 200001)
        with patch.object(c, "table", side_effect=lambda root, db, name, **kw: tables[name]):
            result, report = c.initialize("fixture", cat)
        self.assertNotIn(2, [u["id"] for u in report["seeds"]])

    def test_transform_family_is_not_seeded_twice(self):
        cat, tables = fixture()
        cat["units"][1]["transform_to"] = 3
        cat["units"][2]["transform_to"] = 2
        with patch.object(c, "table", side_effect=lambda root, db, name, **kw: tables[name]):
            _, report = c.initialize("fixture", cat)
        self.assertEqual([u["id"] for u in report["seeds"]], [2])


class CatalogTests(unittest.TestCase):
    def test_create_read_update_delete_preserves_other_entries(self):
        cat, _ = fixture()
        item = copy.deepcopy(cat["custom_machines"][0])
        item["id"] = 2
        result = c.mutate(cat, "custom_machines", "create", 2, item)
        self.assertEqual(c.get_machine(result, "custom_machines", 2), item)
        result = c.mutate(result, "custom_machines", "update", 2, {"price": 600})
        self.assertEqual(c.get_machine(result, "custom_machines", 2)["price"], 600)
        result = c.mutate(result, "custom_machines", "delete", 2)
        self.assertFalse(c.get_machine(result, "custom_machines", 2)["enabled"])
        self.assertTrue(c.get_machine(result, "custom_machines", 2)["retired"])
        self.assertEqual(result["custom_machines"][0], cat["custom_machines"][0])
        with self.assertRaises(ValueError):
            c.mutate(result, "custom_machines", "create", 2, item)

    def test_malformed_probability_and_price_rejected(self):
        cat, _ = fixture()
        with self.assertRaises(ValueError):
            c.mutate(cat, "custom_machines", "update", 1, {"price": -1})
        cat["custom_machines"][0]["candidates"][0]["basic_weight"] = 0
        with self.assertRaises(ValueError):
            c.validate(cat)

    def test_client_server_revision_matches_without_local_secrets(self):
        cat, _ = fixture()
        with tempfile.TemporaryDirectory() as temp:
            c.export_bundle(cat, temp)
            client = c.read_json(Path(temp)/"client.json")
            server = c.read_json(Path(temp)/"server.json")
            manifest = c.read_json(Path(temp)/"manifest.json")
            self.assertEqual(client.pop("role"), "client")
            self.assertEqual(server.pop("role"), "server")
            self.assertEqual(client, server)
            self.assertEqual(client["catalog_revision"], manifest["catalog_revision"])
            self.assertTrue(client["requires_runtime_adapter"])
            self.assertNotIn("password", json.dumps(client).lower())

    def test_no_placeholder_or_disabled_form_auto_seed(self):
        base = {"SallyReady": 1, "UnitGradeNumber": 1, "UnitGradeNumber2": 2,
                "UnitModelName": "real unit", "UnitGraph": 1010020}
        self.assertTrue(c.eligible(base))
        self.assertFalse(c.eligible({**base, "SallyReady": 0}))
        self.assertFalse(c.eligible({**base, "UnitGradeNumber2": 1}))
        self.assertFalse(c.eligible({**base, "UnitModelName": "name", "UnitGraph": 1010001}))

    def test_invalid_archive_and_gdt_fail_closed(self):
        with self.assertRaises(ValueError):
            c.gdt_records(b"plain unsupported data")

    def test_fixed_crud_rejects_duplicate_slot_and_bad_weight_sum(self):
        cat, _ = fixture()
        cat["units"] = [{"id": i, "name": str(i)} for i in range(1, 7)]
        cat["custom_machines"] = []
        item = {"id": 7901, "name": "fixed", "enabled": True, "price": 1200,
                "slots": [{"unit_id": i, "weight": w} for i,w in enumerate([20,100,250,250,240,140],1)]}
        result = c.mutate(cat, "fixed_machines", "create", 7901, item)
        c.validate(c.mutate(result, "fixed_machines", "delete", 7901))
        bad = copy.deepcopy(item["slots"])
        bad[0]["weight"] += 1
        with self.assertRaises(ValueError):
            c.mutate(result, "fixed_machines", "update", 7901, {"slots": bad})
        bad[0] = copy.deepcopy(bad[1])
        with self.assertRaises(ValueError):
            c.mutate(result, "fixed_machines", "update", 7901, {"slots": bad})

    def test_editing_stock_pool_switches_to_real_weighted_runtime(self):
        cat, _ = fixture()
        cat["units"] = [{"id": i, "name": str(i)} for i in range(1, 7)]
        cat["custom_machines"] = []
        item = {"id": 101, "name": "stock", "owner": "native", "draw_policy": "native_sequence",
                "enabled": True, "price": 1200,
                "slots": [{"unit_id": i, "weight": w} for i,w in enumerate([20,100,250,250,240,140],1)]}
        cat["fixed_machines"] = [item]
        price_only = c.mutate(cat, "fixed_machines", "update", 101, {"price": 1300})
        self.assertEqual(price_only["fixed_machines"][0]["draw_policy"], "native_sequence")
        slots = copy.deepcopy(item["slots"])
        slots[0]["weight"] += 1
        slots[1]["weight"] -= 1
        changed = c.mutate(cat, "fixed_machines", "update", 101, {"slots": slots})
        self.assertEqual(changed["fixed_machines"][0]["draw_policy"], "independent_weighted")
        new = {**item, "id": 201}
        copied = c.mutate(cat, "fixed_machines", "create", 201, new)
        self.assertEqual(copied["fixed_machines"][1]["draw_policy"], "independent_weighted")
        self.assertEqual(copied["fixed_machines"][1]["owner"], "editor")


if __name__ == "__main__":
    unittest.main()
