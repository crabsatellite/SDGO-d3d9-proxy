import configparser
from pathlib import Path
import sys
import tempfile
import unittest
import uuid
from unittest.mock import patch
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.rewards.sql_repository import MbCreditReceipt
from owner.rewards.worker import WalletWorker, ini_bytes, read_job


class Ledger:
    def __init__(self):
        self.scope = uuid.uuid4(); self.balance = 1000; self.receipts = {}; self.calls = 0
    def scope_id(self): return self.scope
    def current_mb(self, account): return self.balance
    def apply_mb(self, **value):
        self.calls += 1
        key = value["match_id"], value["account_number"]
        old = self.receipts.get(key)
        if old:
            if old.plan_hash != value["plan_hash"] or old.mb_delta != value["mb"]:
                raise ValueError("changed retry")
            return MbCreditReceipt(old.match_id, old.account_number, old.plan_hash, old.mb_delta, old.mb_after, 0, True)
        self.balance += value["mb"]
        receipt = MbCreditReceipt(*key, value["plan_hash"], value["mb"], self.balance, 0, False)
        self.receipts[key] = receipt
        return receipt


class WorkerTests(unittest.TestCase):
    def job(self, worker, name="job-one.ini", amount=250):
        path = worker.outbox / name
        path.write_bytes(ini_bytes("reward", {"SchemaVersion": 1, "MatchId": name[:-4], "AccountNumber": 1001,
                                            "PlanHash": "a" * 64, "Mb": amount, "PolicyRevision": "b" * 64,
                                            "ExpectedScope": str(worker.scope)}))
        return path

    def test_native_job_to_durable_absolute_balance_receipt(self):
        with tempfile.TemporaryDirectory() as root:
            ledger = Ledger(); worker = WalletWorker(root, ledger, clock=lambda: 100)
            path = self.job(worker)
            self.assertTrue(worker.poll())
            self.assertEqual(ledger.balance, 1250)
            self.assertFalse(path.exists())
            self.assertTrue((worker.completed / path.name).exists())
            parser = configparser.ConfigParser(); parser.read(worker.receipts / path.name)
            self.assertEqual(parser["receipt"]["MbAfter"], "1250")
            self.assertEqual(parser["receipt"]["Mb"], "250")
            self.assertEqual(parser["receipt"]["CurrentMb"], "1250")
            self.assertEqual(parser["receipt"]["PublishedUnix"], "100")
            self.assertEqual(parser["receipt"]["SourceHash"], read_job(worker.completed / path.name)["source_hash"])

    def test_loss_after_sql_commit_retries_without_duplicate_money(self):
        with tempfile.TemporaryDirectory() as root:
            ledger = Ledger(); worker = WalletWorker(root, ledger)
            path = self.job(worker)
            with patch("owner.rewards.worker._atomic_write", side_effect=OSError("simulated lost receipt")):
                with self.assertRaises(OSError): worker.process(path)
            self.assertEqual(ledger.balance, 1250)
            self.assertTrue(path.exists())
            ledger.balance -= 100 # player spends after credit but before the lost response is retried
            self.assertTrue(worker.poll())
            self.assertEqual(ledger.balance, 1150)
            self.assertEqual(worker.replays, 1)
            self.assertFalse(path.exists())
            parsed = configparser.ConfigParser(); parsed.read(worker.receipts / path.name)
            self.assertEqual(parsed["receipt"]["MbAfter"], "1250")
            self.assertEqual(parsed["receipt"]["CurrentMb"], "1150")

    def test_more_than_poll_limit_does_not_starve_new_matches(self):
        with tempfile.TemporaryDirectory() as root:
            ledger = Ledger(); worker = WalletWorker(root, ledger)
            for index in range(260): self.job(worker, f"match-{index:04}.ini", 1)
            self.assertTrue(worker.poll()); self.assertTrue(worker.poll())
            self.assertEqual(ledger.balance, 1260)
            self.assertEqual(len(list(worker.receipts.glob("*.ini"))), 260)
            self.assertFalse(list(worker.outbox.glob("*.ini")))

    def test_different_scope_or_malformed_job_never_calls_wallet(self):
        with tempfile.TemporaryDirectory() as root:
            ledger = Ledger(); worker = WalletWorker(root, ledger)
            path = self.job(worker)
            path.write_bytes(path.read_bytes().replace(str(worker.scope).encode(), str(uuid.uuid4()).encode()))
            self.assertFalse(worker.poll()); self.assertEqual(ledger.calls, 0)
            self.assertNotIn("different wallet", worker.last_error)
            path.write_bytes(b"[reward]\nSchemaVersion=1\nMb=1\n")
            self.assertFalse(worker.poll()); self.assertEqual(ledger.calls, 0)

    def test_existing_receipt_and_same_job_do_not_repeat_sql(self):
        with tempfile.TemporaryDirectory() as root:
            ledger = Ledger(); worker = WalletWorker(root, ledger)
            path = self.job(worker); worker.process(path)
            path.write_bytes((worker.completed / path.name).read_bytes())
            self.assertFalse(worker.process(path)); self.assertEqual(ledger.calls, 1)
            self.assertEqual(ledger.balance, 1250)


if __name__ == "__main__": unittest.main()
