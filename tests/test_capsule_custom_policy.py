import copy
from pathlib import Path
import sys
import unittest
from unittest.mock import patch


sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import capsule_catalog as c


def unit(uid, rank, subgrade, *, recipe=False):
    suffix = {2: "", 3: "S", 4: "R", 5: "U"}[subgrade]
    return {
        "id": uid,
        "name": str(uid),
        "rank": rank,
        "subgrade": subgrade,
        "grade": {1: "C", 2: "B", 3: "A", 4: "S"}[rank] + suffix,
        "eligible": True,
        "has_blueprint_recipe": recipe,
        "transform_to": 0,
        "capsule_sprites": [],
    }


def recipe(rid, output, inputs):
    return {
        "Idx": rid,
        "OutUnitID": output,
        **{f"UnitID{i}": inputs[i - 1] if i <= len(inputs) else 0
           for i in range(1, 8)},
    }


def fixture():
    units = [
        unit(1, 3, 2),
        unit(2, 2, 2),
        unit(3, 1, 2),
        unit(4, 1, 2),
        unit(5, 2, 2),
        unit(6, 3, 2),
        unit(10, 3, 3),
        unit(11, 2, 3),
        unit(12, 1, 3),
        unit(13, 3, 3),
        unit(20, 4, 2),
        unit(21, 4, 4),
        unit(22, 4, 3),
        unit(23, 4, 3, recipe=True),
        unit(24, 4, 4),
        unit(25, 4, 2),
    ]
    by_id = {row["id"]: row for row in units}
    for uid in (10, 11, 12):
        by_id[uid]["capsule_sprites"] = [
            f"{uid}-native-shown", f"{uid}-native-hidden"]
    by_id[20]["transform_to"] = 25
    by_id[25]["transform_to"] = 20
    weights = [20, 100, 250, 250, 240, 140]
    groups = [1, 3, 5, 5, 5, 3]
    fixed = {
        "id": 101,
        "name": "native",
        "enabled": True,
        "price": 1200,
        "owner": "native",
        "draw_policy": "native_sequence",
        "slots": [
            {
                "unit_id": uid,
                "weight": weight,
                "native_draw_group": group,
                "sprites": [f"{uid}-shown", f"{uid}-hidden"],
            }
            for uid, weight, group in zip(range(1, 7), weights, groups)
        ],
    }
    custom = {
        "id": 1,
        "name": "native custom",
        "enabled": True,
        "price": 2000,
        "owner": "native",
        "candidates": [
            {
                "unit_id": 1,
                "basic_weight": 1500,
                "mileage": 0.0,
                "extra_price": 0,
                "reset_mileage": False,
            },
            {
                "unit_id": 24,
                "basic_weight": 25,
                "mileage": 0.5,
                "extra_price": 100,
                "reset_mileage": True,
            },
        ],
        "slot_rules": [
            {
                "slot": i,
                "rank_max": 4,
                "subgrades": [2, 3, 4, 5],
                "extension": i >= 7,
            }
            for i in range(1, 9)
        ],
    }
    catalog = {
        "schema_version": 1,
        "custom_candidate_policy": {
            "version": 1,
            "native_candidate_unit_ids": [1, 24],
            "new_candidate_grades": ["SR", "SS-without-blueprint-recipe"],
        },
        "units": units,
        "fixed_machines": [fixed],
        "custom_machines": [custom],
    }
    tables = {
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
    tables["tbl_MixTreeFile"] = [recipe(200023, 23, [999])]
    return catalog, tables


def candidate(uid):
    return {
        "unit_id": uid,
        "basic_weight": 25,
        "mileage": 0.5,
        "extra_price": 100,
        "reset_mileage": True,
    }


class CustomCandidatePolicyTests(unittest.TestCase):
    def test_helper_and_validation_keep_native_and_allow_only_sr_or_recipe_less_ss(self):
        catalog, _ = fixture()
        units = {u["id"]: u for u in catalog["units"]}
        self.assertTrue(c.custom_candidate_allowed(catalog, units[21]))
        self.assertTrue(c.custom_candidate_allowed(catalog, units[22]))
        self.assertFalse(c.custom_candidate_allowed(catalog, units[20]))
        self.assertFalse(c.custom_candidate_allowed(catalog, units[23]))
        self.assertTrue(c.native_capsule_card_available(units[10]))
        self.assertFalse(c.native_capsule_card_available(units[13]))

        candidates = catalog["custom_machines"][0]["candidates"]
        candidates.extend([candidate(21), candidate(22)])
        c.validate(catalog)
        self.assertEqual(candidates[0]["unit_id"], 1)

        for prohibited in (20, 23, 10):
            invalid = copy.deepcopy(catalog)
            invalid["custom_machines"][0]["candidates"].append(candidate(prohibited))
            with self.assertRaisesRegex(ValueError, "Custom candidate policy"):
                c.validate(invalid)

    def test_manual_crud_obeys_policy_and_legacy_catalogs_remain_compatible(self):
        catalog, _ = fixture()
        allowed = copy.deepcopy(catalog["custom_machines"][0]["candidates"])
        allowed.append(candidate(21))
        updated = c.mutate(catalog, "custom_machines", "update", 1,
                           {"candidates": allowed})
        self.assertIn(21, [row["unit_id"]
                          for row in updated["custom_machines"][0]["candidates"]])

        prohibited = copy.deepcopy(allowed)
        prohibited.append(candidate(20))
        with self.assertRaisesRegex(ValueError, "Custom candidate policy"):
            c.mutate(catalog, "custom_machines", "update", 1,
                     {"candidates": prohibited})

        legacy = copy.deepcopy(catalog)
        legacy.pop("custom_candidate_policy")
        legacy["custom_machines"][0]["candidates"].append(candidate(20))
        c.validate(legacy)

    def test_initialize_uses_derived_fixed_slots_and_reports_blocked_high_ranks(self):
        catalog, tables = fixture()
        with patch.object(c, "table",
                          side_effect=lambda root, db, name, **kw: tables[name]):
            result, report = c.initialize("fixture", catalog)
            again, second = c.initialize("fixture", result)

        self.assertEqual(result, again)
        self.assertEqual(len(report["derived_fixed_machine_ids"]), 1)
        derived = next(
            machine for machine in result["fixed_machines"]
            if machine["id"] in report["derived_fixed_machine_ids"]
        )
        self.assertEqual(derived["template_match"], "base_rank_derived")
        self.assertEqual(derived["derived_template_units"], [10, 11, 12])
        derived_slots = {slot["unit_id"]: slot for slot in derived["slots"]}
        self.assertEqual(
            {
                uid: (
                    derived_slots[uid]["weight"],
                    derived_slots[uid]["native_draw_group"],
                    derived_slots[uid]["template_unit_id"],
                    derived_slots[uid]["template_grade"],
                    derived_slots[uid]["template_sprites"],
                )
                for uid in (10, 11, 12)
            },
            {
                10: (20, 1, 1, "A", ["1-shown", "1-hidden"]),
                11: (100, 3, 2, "B", ["2-shown", "2-hidden"]),
                12: (250, 5, 3, "C", ["3-shown", "3-hidden"]),
            },
        )

        added = {row["unit_id"] for row in report["custom_candidate_additions"]}
        self.assertEqual(added, {21, 22})
        self.assertNotIn(20, added)
        self.assertNotIn(23, added)
        self.assertEqual(report["remaining"], [13, 20, 23, 25])

        exclusions = {
            row["unit_id"]: row for row in report["custom_candidate_exclusions"]
        }
        self.assertEqual(
            exclusions[20]["reason"], "ordinary_s_not_custom_eligible")
        self.assertEqual(exclusions[23]["reason"], "ss_has_blueprint_recipe")
        self.assertEqual(
            exclusions[13]["reason"], "missing_native_capsule_card")
        self.assertEqual(
            [row["unit_id"] for row in report["derived_fixed_art_blocked"]],
            [13],
        )
        self.assertEqual(exclusions[23]["recipe_ids"], [200023])
        self.assertEqual(
            exclusions[23]["blocked_recipes"],
            [{
                "recipe_id": 200023,
                "output": 23,
                "blueprint_available": False,
                "missing_materials": [999],
            }],
        )
        remaining = {row["unit_id"]: row for row in report["remaining_details"]}
        self.assertEqual(
            remaining[25],
            {
                **next(u for u in result["units"] if u["id"] == 25),
                "unit_id": 25,
                "reason": "transform_family_depends_on_excluded_root",
                "explanation":
                    "This transform state has no source because its linked root was excluded.",
                "blocked_by_unit_id": 20,
                "blocked_reason": "ordinary_s_not_custom_eligible",
            },
        )
        self.assertEqual(
            {row["unit_id"] for row in report["seeds"]},
            {10, 11, 12, 21, 22},
        )
        self.assertEqual(
            {row["unit_id"] for row in report["unconfigured_seeds"]},
            {13, 20, 23},
        )
        self.assertEqual(
            {row["unit_id"] for row in report["suggested_seeds"]},
            {10, 11, 12, 13, 20, 21, 22, 23},
        )
        self.assertEqual(second["seeds"], [])
        self.assertEqual(
            {row["unit_id"] for row in second["suggested_seeds"]},
            {13, 20, 23},
        )
        self.assertEqual(second["remaining"], [13, 20, 23, 25])
        self.assertEqual(
            {row["unit_id"]: row["reason"]
             for row in second["custom_candidate_exclusions"]},
            {
                13: "missing_native_capsule_card",
                20: "ordinary_s_not_custom_eligible",
                23: "ss_has_blueprint_recipe",
            },
        )


if __name__ == "__main__":
    unittest.main()
