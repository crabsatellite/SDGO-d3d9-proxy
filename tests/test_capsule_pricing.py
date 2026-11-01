import copy
from collections import Counter
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import capsule_catalog as c


WEIGHTS = [20, 100, 250, 250, 240, 140]
GROUPS = [1, 3, 5, 5, 5, 3]


def unit(uid, rank, subgrade=2, *, sprites=True):
    suffix = {2: "", 3: "S", 4: "R", 5: "U"}[subgrade]
    return {
        "id": uid,
        "name": str(uid),
        "rank": rank,
        "subgrade": subgrade,
        "grade": {1: "C", 2: "B", 3: "A", 4: "S"}[rank] + suffix,
        "eligible": True,
        "has_blueprint_recipe": False,
        "transform_to": 0,
        "capsule_sprites": (
            [f"{uid}-shown.spr", f"{uid}-hidden.spr"] if sprites else []),
    }


def tables():
    return {
        name: []
        for name in [
            "tbl_ItemCategory",
            "tbl_MixTreeFile",
            "tbl_CoinMachinePrice",
            "tbl_CoinCapsuleList",
            "tbls_StoreItems",
            "tbls_SubItems",
            "tbl_QuestInfo",
            "tbl_QuestPrerequest",
            "tbl_MachinePrice",
            "tbl_BattleResultItemDropDice",
            "tbl_BattleResultItemDropSetting",
        ]
    }


def catalog_for(rank, model_id, model_price, missing_ids, *, blocked_id=None):
    native_units = [unit(uid, rank) for uid in range(1, 7)]
    missing_units = [unit(uid, rank) for uid in missing_ids]
    units = native_units + missing_units
    catalog = {
        "schema_version": 1,
        "units": units,
        "fixed_machines": [{
            "id": model_id,
            "name": "native template",
            "enabled": True,
            "price": model_price,
            "owner": "native",
            "draw_policy": "native_sequence",
            "slots": [{
                "unit_id": uid,
                "weight": weight,
                "native_draw_group": group,
                "sprites": [f"{uid}-shown.spr", f"{uid}-hidden.spr"],
            } for uid, weight, group in zip(range(1, 7), WEIGHTS, GROUPS)],
        }],
        "custom_machines": [],
    }
    if blocked_id is not None:
        catalog["custom_candidate_policy"] = {
            "version": 1,
            "native_candidate_unit_ids": [],
            "new_candidate_grades": ["SR", "SS-without-blueprint-recipe"],
        }
        catalog["units"].append(unit(blocked_id, rank, 3, sprites=False))
    return catalog


def initialize(catalog, price=None):
    fixture_tables = tables()
    with patch.object(
            c, "table",
            side_effect=lambda root, database, name, **kwargs: fixture_tables[name]):
        return c.initialize("fixture", catalog, price)


class InitializationPricingTests(unittest.TestCase):
    def test_largest_remainder_matches_native_price_distribution(self):
        self.assertEqual(
            c._largest_remainder_quotas(
                {1200: 17, 1400: 4, 1600: 40, 7500: 5, 14800: 12},
                35,
            ),
            {1200: 8, 1400: 2, 1600: 18, 7500: 2, 14800: 5},
        )

    def test_all_a_and_all_b_inherit_selected_native_template_price(self):
        all_a, _ = initialize(catalog_for(3, 3601, 14800, [100]))
        new_a = next(m for m in all_a["fixed_machines"]
                     if m["owner"] == "initialization")
        self.assertEqual(new_a["price"], 14800)
        self.assertEqual(new_a["template_price"], 14800)
        self.assertEqual(new_a["pricing_source"], "template")
        self.assertEqual(new_a["template_id"], 3601)

        all_b, _ = initialize(catalog_for(2, 2001, 7500, [200]))
        new_b = next(m for m in all_b["fixed_machines"]
                     if m["owner"] == "initialization")
        self.assertEqual(new_b["price"], 7500)
        self.assertEqual(new_b["template_price"], 7500)
        self.assertEqual(new_b["pricing_source"], "template")
        self.assertEqual(new_b["template_id"], 2001)

    def test_explicit_override_is_recorded_without_changing_template_price(self):
        result, report = initialize(
            catalog_for(3, 3601, 14800, [100]), price=1200)
        machine = next(m for m in result["fixed_machines"]
                       if m["owner"] == "initialization")
        self.assertEqual(machine["price"], 1200)
        self.assertEqual(machine["template_price"], 14800)
        self.assertEqual(machine["pricing_source"], "override")
        self.assertEqual(report["fixed_machine_mix"][0]["price"], 1200)
        self.assertEqual(
            report["fixed_machine_mix"][0]["pricing_source"], "override")

    def test_second_initialization_preserves_manual_price_edit(self):
        result, _ = initialize(catalog_for(3, 3601, 14800, [100]))
        edited = copy.deepcopy(result)
        machine = next(m for m in edited["fixed_machines"]
                       if m["owner"] == "initialization")
        machine["price"] = 9999
        again, report = initialize(edited)
        self.assertEqual(again, edited)
        self.assertEqual(
            next(m for m in again["fixed_machines"]
                 if m["owner"] == "initialization")["price"],
            9999,
        )
        self.assertEqual(report["new_fixed_machine_ids"], [])

    def test_invalid_price_override_type_or_value_is_rejected(self):
        catalog = catalog_for(3, 3601, 14800, [100])
        for invalid in (-1, 1.5, "1200", True, False):
            with self.subTest(invalid=invalid):
                with self.assertRaisesRegex(
                        ValueError, "nonnegative integer or null"):
                    initialize(catalog, invalid)


class InitializationDiversityTests(unittest.TestCase):
    def test_strict_mix_uses_five_non_old_units_and_reports_all_usage(self):
        base = catalog_for(3, 3601, 14800, range(100, 110))
        for row in base["units"]:
            row["has_inventory_category"] = True
            row["collection"] = True
            row["review_placeholder"] = False
        base["custom_candidate_policy"] = {
            "version": 1,
            "native_candidate_unit_ids": [],
            "new_candidate_grades": [
                "SR",
                "SS-without-blueprint-recipe",
            ],
        }

        result, report = initialize(base)
        generated = [
            machine for machine in result["fixed_machines"]
            if machine["owner"] == "initialization"
        ]
        self.assertEqual(len(generated), 2)
        native_ids = set(range(1, 7))
        for machine in generated:
            ids = [slot["unit_id"] for slot in machine["slots"]]
            self.assertEqual(len(set(ids)), 6)
            self.assertEqual(sum(uid in native_ids for uid in ids), 1)
            self.assertEqual(
                Counter(slot["initialization_role"]
                        for slot in machine["slots"]),
                Counter({"new_root": 5, "filler_native_overlap": 1}),
            )

        plan = report["fixed_initialization_plan"]
        self.assertEqual(plan["machine_count"], 2)
        self.assertEqual(plan["price_quotas"], {14800: 2})
        self.assertEqual(plan["actual_template_price_counts"], {14800: 2})
        self.assertEqual(plan["native_overlap_slot_count"], 2)
        self.assertEqual(plan["native_overlap_rate"], 1 / 6)
        self.assertEqual(plan["distinct_template_count"], 1)
        self.assertEqual(plan["maximum_template_reuse"], 2)
        self.assertEqual(
            {row["unit_id"]: row["uses"]
             for row in report["fixed_non_native_usage"]},
            {uid: 1 for uid in range(100, 110)},
        )
        self.assertTrue(all(
            row["passes"] for row in report["new_fixed_rarity_audit"]))

    def test_repeated_pattern_rotates_fillers_and_never_uses_blocked_root(self):
        base = catalog_for(
            3, 3601, 14800, range(100, 120), blocked_id=999)
        first, report = initialize(base)
        second, second_report = initialize(copy.deepcopy(base))
        self.assertEqual(first, second)
        self.assertEqual(
            report["fixed_machine_mix"], second_report["fixed_machine_mix"])

        generated = [m for m in first["fixed_machines"]
                     if m["owner"] == "initialization"]
        self.assertEqual(len(generated), 5)
        filler_sets = []
        actual_usage = Counter()
        for machine in generated:
            roles = Counter(slot["initialization_role"]
                            for slot in machine["slots"])
            self.assertEqual(roles["new_root"], 4)
            self.assertGreaterEqual(roles["filler_baseline"], 1)
            self.assertEqual(
                roles["filler_baseline"] + roles["filler_initialized"], 2)
            self.assertEqual(len({slot["unit_id"]
                                  for slot in machine["slots"]}), 6)
            self.assertNotIn(999, [slot["unit_id"]
                                   for slot in machine["slots"]])
            fillers = tuple(slot["unit_id"] for slot in machine["slots"]
                            if slot["initialization_role"] != "new_root")
            filler_sets.append(fillers)
            actual_usage.update(fillers)
        self.assertGreater(len(set(filler_sets)), 1)
        self.assertTrue(any(
            slot["initialization_role"] == "filler_initialized"
            for machine in generated for slot in machine["slots"]))
        self.assertEqual(
            {row["unit_id"]: row["uses"]
             for row in report["fixed_filler_usage"]},
            dict(actual_usage),
        )
        blocked = {row["unit_id"]: row["reason"]
                   for row in report["unconfigured_seeds"]}
        self.assertEqual(blocked[999], "missing_native_capsule_card")

        again, repeat_report = initialize(first)
        self.assertEqual(first, again)
        self.assertEqual(repeat_report["new_fixed_machine_ids"], [])


if __name__ == "__main__":
    unittest.main()
