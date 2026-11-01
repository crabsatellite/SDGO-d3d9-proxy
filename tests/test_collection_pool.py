import unittest
from owner.collection_capsule.pool import build_pool


def unit(uid, target=0, ready=1, **extra):
    return dict(UnitNumber=uid, UnitNumber_Transform=target, SallyReady=ready,
                UnitGradeNumber=1, UnitGradeNumber2=2, UnitModelName=str(uid),
                UnitGraph=uid + 1000000, **extra)


class PoolTests(unittest.TestCase):
    def ids(self, rows, **kwargs):
        return [row["unit_id"] for row in build_pool(rows, **kwargs)["units"]]

    def test_normal_and_one_way_transform(self):
        self.assertEqual(self.ids([unit(7), unit(8, 9), unit(9, ready=0)]), [7, 8])

    def test_reversible_transform_keeps_only_ownable_base(self):
        result = build_pool([unit(8, 9), unit(9, 8, ready=0)])
        self.assertEqual([r["unit_id"] for r in result["units"]], [8])
        self.assertIn({"unit_id": 9, "base_unit_id": 8}, result["form_aliases"])

    def test_bad_ready_flag_on_one_way_post_form_does_not_make_it_a_prize(self):
        self.assertEqual(self.ids([unit(8, 9), unit(9)]), [8])

    def test_multi_step_transform_does_not_reintroduce_a_ready_post_form(self):
        self.assertEqual(self.ids([unit(8, 9), unit(9, 10, ready=0), unit(10)]), [8])

    def test_multiple_ready_forms_in_cycle_are_not_guessed_by_numeric_id(self):
        result = build_pool([unit(19, 3), unit(3, 19)])
        self.assertFalse(result["units"])
        self.assertEqual({r["reason"] for r in result["excluded"]}, {"ambiguous_transform_cycle"})

    def test_reviewed_base_can_have_larger_id(self):
        self.assertEqual(self.ids([unit(19, 3), unit(3, 19)], verified_bases={19: "verified native acquisition entry"}), [19])

    def test_two_reviewed_bases_in_one_cycle_rejected(self):
        with self.assertRaises(ValueError):
            build_pool([unit(1, 2), unit(2, 1)], verified_bases={1: "a", 2: "b"})

    def test_placeholder_training_and_unready_records_excluded(self):
        placeholder = unit(1)
        placeholder.update(UnitModelName="name", UnitGraph=1010001)
        training = unit(2)
        training["UnitGradeNumber2"] = 1
        self.assertEqual(self.ids([placeholder, training, unit(3, ready=0)]), [])

    def test_disconnected_preform_is_not_removed_by_unownable_reverse_link(self):
        self.assertEqual(self.ids([unit(8), unit(9, 8, ready=0)]), [8])

    def test_duplicate_ids_fail_instead_of_silently_overwriting(self):
        with self.assertRaises(ValueError):
            build_pool([unit(1), unit(1)])

    def test_verified_client_family_admits_real_root_with_placeholder_server_name(self):
        before, after = unit(8, 9), unit(9, 8)
        for row in (before, after):
            row.update(UnitModelName="name", UnitGraph=1010001)
        result = build_pool([before, after], verified_bases={8: "native client family/acquisition proof"})
        self.assertEqual([r["unit_id"] for r in result["units"]], [8])


if __name__ == "__main__":
    unittest.main()
