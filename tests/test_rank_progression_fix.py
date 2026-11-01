from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import rank_progression_fix as fix


def definition(name=fix.TARGETS[0], condition=None):
    condition = condition or "IF(@UserLevelNumber < @newlevel OR @UserLevelStep < @NewLevelStep)"
    return (f"CREATE PROCEDURE dbo.[{name}] AS\n"
            "DECLARE @UserLevelNumber tinyint, @newlevel int, @UserLevelStep int, @NewLevelStep int;\n"
            + condition + "\nBEGIN PRINT N'promotion'; END\n")


class RankPlanTests(unittest.TestCase):
    def test_only_comparison_changes_for_both_native_entrypoints(self):
        for name in fix.TARGETS:
            original = definition(name)
            p = fix.plan(name, original)
            self.assertEqual(p.state, "affected")
            self.assertEqual(p.updated, original.replace(
                "IF(@UserLevelNumber < @newlevel OR @UserLevelStep < @NewLevelStep)",
                "IF(@UserLevelNumber < @newlevel OR (@UserLevelNumber = @newlevel AND @UserLevelStep < @NewLevelStep))"))
            self.assertNotEqual(p.original_hash, p.installed_hash)
            self.assertEqual(fix.plan(name, p.updated).state, "already_fixed")

    def test_comments_and_literals_cannot_look_like_active_comparison(self):
        original = definition()
        condition = fix.OLD.search(original).group(0)
        extras = ("-- " + condition + "\n/* outer /* " + condition + " */ nested */\n"
                  "PRINT N'" + condition + "';\nPRINT 'it''s a string';\n")
        original = extras[:extras.index("PRINT")] + original + extras[extras.index("PRINT"):]
        p = fix.plan(fix.TARGETS[0], original)
        self.assertIn("-- " + condition, p.updated)
        self.assertIn("PRINT N'" + condition + "'", p.updated)
        self.assertEqual(len(fix.OLD.findall(fix.sql_code(p.updated))), 0)

    def test_sql_identifier_case_and_unicode_are_preserved(self):
        original = definition(condition="if ( @USERLEVELNUMBER < @NEWLEVEL or @USERLEVELSTEP < @NEWLEVELSTEP )")
        original += "-- 中文 군衔 ขั้น\n"
        p = fix.plan(fix.TARGETS[0], original)
        self.assertIn("@USERLEVELNUMBER = @NEWLEVEL", p.updated)
        self.assertTrue(p.updated.endswith("-- 中文 군衔 ขั้น\n"))

    def test_unknown_or_ambiguous_versions_refuse(self):
        for text in (definition(condition="IF(1=1)"), definition() + fix.OLD.search(definition()).group(0)):
            with self.assertRaises(ValueError):
                fix.plan(fix.TARGETS[0], text)

    def test_procedure_identity_and_scope_are_verified(self):
        with self.assertRaises(ValueError):
            fix.plan(fix.TARGETS[1], definition(fix.TARGETS[0]))
        with self.assertRaises(ValueError):
            fix.plan("sp_UpdateUserInfo", definition())
        with self.assertRaises(ValueError):
            fix.plan(fix.TARGETS[0], "SELECT 1;\n" + definition())

    def test_create_and_alter_hash_same_but_body_edits_differ(self):
        original = definition()
        altered = fix.as_alter(original, fix.TARGETS[0])
        self.assertTrue(altered.startswith("ALTER PROCEDURE"))
        self.assertEqual(fix.module_hash(original, fix.TARGETS[0]), fix.module_hash(altered, fix.TARGETS[0]))
        self.assertNotEqual(fix.module_hash(original, fix.TARGETS[0]), fix.module_hash(original+"\n-- later edit", fix.TARGETS[0]))

    def test_mask_preserves_every_utf16_independent_python_string_offset(self):
        text = "/* hi */ SELECT 'it''s', [an]]id], \"quoted\" -- tail\nIF(1=1)"
        masked = fix.sql_code(text)
        self.assertEqual(len(text), len(masked))
        self.assertEqual(masked.index("IF(1=1)"), text.index("IF(1=1)"))
        for invalid in ("/* missing", "SELECT 'missing", "SELECT [missing"):
            with self.assertRaises(ValueError):
                fix.sql_code(invalid)


if __name__ == "__main__":
    unittest.main()
