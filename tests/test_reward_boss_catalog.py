import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.rewards import boss_catalog


class BossCatalogTests(unittest.TestCase):
    def test_canonical_identity_mapping_and_content_hash(self):
        raw = boss_catalog.encode([(88010, 70001), (88006, 50001), (88010, 70001)])
        header, body = raw.split(b"\n", 1)
        self.assertEqual(body, b"88006,50001\n88010,70001\n")
        self.assertEqual(header, b"SDGO_BOSS_REGISTRY_V1 " + hashlib.sha256(body).hexdigest().encode())

    def test_invalid_or_ambiguous_fields_are_never_guessed(self):
        for rows in ([(-1, 1)], [(1, True)], [(1, 2, 3)], [("MissionID", "ModelID")]):
            with self.assertRaises(ValueError): boss_catalog.encode(rows)
        self.assertNotIn(b"BossFlag", boss_catalog.encode([]))

    def test_atomic_export_replaces_only_named_output(self):
        with tempfile.TemporaryDirectory() as root:
            target = Path(root) / "runtime" / "reward-bosses.txt"
            revision = boss_catalog.publish(target, [(1, 2)])
            self.assertEqual(len(revision), 64)
            boss_catalog.publish(target, [(1, 3)])
            self.assertTrue(target.read_bytes().endswith(b"1,3\n"))
            self.assertEqual([p.name for p in target.parent.iterdir()], ["reward-bosses.txt"])

    def test_exporter_executes_select_only_and_checks_native_identity_refs(self):
        class Cursor:
            def __init__(self): self.sql = []
            def execute(self, sql): self.sql.append(sql); return self
            def fetchall(self):
                return [(10, 20, 0, None, 0, 0)] if "RankBoss" in self.sql[-1] else [(20,)]
        class Connection:
            def __init__(self): self.value = Cursor()
            def cursor(self): return self.value
        connection = Connection()
        self.assertEqual(boss_catalog.read_native(connection), [(10, 20)])
        self.assertTrue(all(sql.startswith("SELECT ") for sql in connection.value.sql))


if __name__ == "__main__": unittest.main()
