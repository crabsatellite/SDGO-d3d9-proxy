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


def final_catalog_for_c_rebalance():
    native_c_ids = list(range(30000, 30085))
    native_a_ids = list(range(40000, 40035))
    repeated_c_ids = [10203, 10086, 10218]
    root_ids = list(range(50000, 50119))
    units = (
        [unit(uid, 1) for uid in native_c_ids + repeated_c_ids]
        + [unit(uid, 3) for uid in native_a_ids]
        + [unit(uid, 3 if index % 2 else 2)
           for index, uid in enumerate(root_ids)]
    )
    unit_map = {row["id"]: row for row in units}
    native = []
    for index in range(78):
        ids = [
            native_c_ids[(index * 3 + offset) % len(native_c_ids)]
            for offset in range(3)
        ] + [
            native_a_ids[(index * 3 + offset) % len(native_a_ids)]
            for offset in range(3)
        ]
        native.append({
            "id": 101 + index * 100,
            "name": f"native-{index}",
            "enabled": True,
            "price": 1200,
            "owner": "native",
            "draw_policy": "native_sequence",
            "slots": [{
                "unit_id": uid,
                "weight": WEIGHTS[slot_index],
                "native_draw_group": GROUPS[slot_index],
                "sprites": unit_map[uid]["capsule_sprites"],
            } for slot_index, uid in enumerate(ids)],
        })

    c_sequence = [repeated_c_ids[index % 3] for index in range(56)]
    prices = [1400] * 2 + [1200] * 8 + [1600] * 18
    prices += [7500] * 2 + [14800] * 5
    generated = []
    root_cursor = 0
    c_cursor = 0
    for index, machine_price in enumerate(prices):
        old_uid = native_a_ids[index]
        if index < 28:
            current_c = c_sequence[c_cursor:c_cursor + 2]
            c_cursor += 2
            current_roots = root_ids[root_cursor:root_cursor + 3]
            root_cursor += 3
            ids = [old_uid, *current_c, *current_roots]
            roles = [
                "filler_native_overlap",
                "filler_non_native",
                "filler_non_native",
                "new_root",
                "new_root",
                "new_root",
            ]
        else:
            current_roots = root_ids[root_cursor:root_cursor + 5]
            root_cursor += 5
            ids = [old_uid, *current_roots]
            roles = ["filler_native_overlap"] + ["new_root"] * 5
        old_ids = [old_uid]
        non_old_fillers = [
            uid for uid, role in zip(ids, roles)
            if role == "filler_non_native"
        ]
        generated.append({
            "id": 7901 + index * 100,
            "name": f"generated-{index}",
            "enabled": True,
            "price": machine_price,
            "owner": "initialization",
            "draw_policy": "independent_weighted",
            "initialization_mix": {
                "new_root_unit_ids": list(current_roots),
                "baseline_filler_unit_ids": list(old_ids),
                "initialized_filler_unit_ids": list(non_old_fillers),
                "native_overlap_unit_ids": list(old_ids),
                "non_native_filler_unit_ids": list(non_old_fillers),
                "native_overlap_slot_count": 1,
                "non_native_slot_count": 5,
            },
            "slots": [{
                "unit_id": uid,
                "weight": WEIGHTS[slot_index],
                "native_draw_group": GROUPS[slot_index],
                "sprites": unit_map[uid]["capsule_sprites"],
                "template_grade": unit_map[uid]["grade"],
                "initialization_role": role,
            } for slot_index, (uid, role) in enumerate(zip(ids, roles))],
        })
    return {
        "schema_version": 1,
        "units": units,
        "fixed_machines": native + generated,
        "custom_machines": [
            {"id": 1, "name": "custom-one"},
            {"id": 2, "name": "custom-two"},
        ],
    }, set(root_ids[:100]), set(native_c_ids + native_a_ids)


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
    def test_c_rebalance_uses_global_budget_and_preserves_existing_content(self):
        catalog, required, original_fixed_units = (
            final_catalog_for_c_rebalance())
        native_before = copy.deepcopy(catalog["fixed_machines"][:78])
        custom_before = copy.deepcopy(catalog["custom_machines"])
        units = {row["id"]: row for row in catalog["units"]}
        generated = catalog["fixed_machines"][78:]
        ab_before = {
            (machine["id"], index):
                (slot["unit_id"], slot["weight"])
            for machine in generated
            for index, slot in enumerate(machine["slots"])
            if units[slot["unit_id"]]["grade"] in ("A", "B")
        }

        report = c.rebalance_initialization_c_fillers(
            catalog,
            [machine["id"] for machine in generated],
            original_fixed_units,
            required,
        )

        self.assertEqual(catalog["fixed_machines"][:78], native_before)
        self.assertEqual(catalog["custom_machines"], custom_before)
        self.assertEqual(report["total_generated_slot_count"], 210)
        self.assertEqual(report["old_overlap_slot_count_before"], 35)
        self.assertEqual(report["maximum_old_overlap_slots"], 62)
        self.assertEqual(report["old_overlap_slot_count_after"], 62)
        self.assertLess(report["old_overlap_rate_after"], 0.3)
        self.assertEqual(report["replacement_count"], 27)
        self.assertEqual(report["c_max_use_before"], 19)
        self.assertEqual(report["c_max_use_after"], 10)
        self.assertEqual(report["c_unique_before"], 3)
        self.assertEqual(report["c_unique_after"], 30)
        self.assertEqual(report["retained_required_root_count"], 100)
        self.assertEqual(report["missing_required_unit_ids"], [])
        self.assertTrue(all(
            row["price"] in (1200, 1400, 1600)
            for row in report["replacements"]))

        generated_ids = {
            slot["unit_id"] for machine in generated
            for slot in machine["slots"]
        }
        self.assertTrue(required <= generated_ids)
        self.assertTrue(all(
            row["old_overlap_slot_count"] <= 2
            for row in report["per_machine"]))
        self.assertTrue(all(
            len({slot["unit_id"] for slot in machine["slots"]}) == 6
            for machine in generated))
        self.assertEqual(
            {
                (machine["id"], index):
                    (slot["unit_id"], slot["weight"])
                for machine in generated
                for index, slot in enumerate(machine["slots"])
                if units[slot["unit_id"]]["grade"] in ("A", "B")
            },
            ab_before,
        )
        c_after = {
            row["unit_id"]: row["uses"]
            for row in report["c_usage_after"]
        }
        self.assertEqual(c_after[10203], 10)

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
