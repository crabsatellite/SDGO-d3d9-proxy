import configparser
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.rewards.sql_repository import MbCreditReceipt
from owner.rewards.worker import WalletWorker, ini_bytes, read_job


MATCH_ID = "battle:interop:20261101:42"
ACCOUNT = 123456789
PLAN_HASH = "a" * 64
POLICY_REVISION = "b" * 64
MB = 37
HISTORICAL_MB = 1037
CURRENT_MB = 2048
SCOPE = uuid.UUID("12345678-1234-4abc-8def-123456789abc")


class FakeNativeLedger:
    def __init__(self):
        self.apply_calls = []
        self.current_calls = []

    def scope_id(self):
        return SCOPE

    def apply_mb(self, **values):
        self.apply_calls.append(values)
        assert values == {
            "match_id": MATCH_ID,
            "account_number": ACCOUNT,
            "plan_hash": PLAN_HASH,
            "mb": MB,
            "expected_scope": SCOPE,
        }
        return MbCreditReceipt(
            MATCH_ID,
            ACCOUNT,
            PLAN_HASH,
            MB,
            HISTORICAL_MB,
            0,
            False,
        )

    def current_mb(self, account):
        self.current_calls.append(account)
        assert account == ACCOUNT
        return CURRENT_MB


def run_native(executable, stage, runtime):
    completed = subprocess.run(
        [str(executable), stage, str(runtime), str(SCOPE)],
        cwd=ROOT,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=30,
    )
    sys.stdout.write(completed.stdout)
    sys.stderr.write(completed.stderr)
    if completed.returncode:
        raise AssertionError(
            f"native wallet stage {stage!r} failed with {completed.returncode}"
        )


def parse_ini(path, section):
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    parser.read(path, encoding="ascii")
    assert parser.sections() == [section]
    return dict(parser[section])


def canonical_job():
    return ini_bytes("reward", {
        "SchemaVersion": 1,
        "MatchId": MATCH_ID,
        "AccountNumber": ACCOUNT,
        "PlanHash": PLAN_HASH,
        "Mb": MB,
        "PolicyRevision": POLICY_REVISION,
        "ExpectedScope": SCOPE,
    })


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_reward_job_interop.py TEST_EXE")
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="sdgo reward job interop ") as directory:
        runtime = Path(directory) / "native runtime with spaces"
        run_native(executable, "emit", runtime)

        expected_name = hashlib.sha256(
            f"{MATCH_ID}:{ACCOUNT}".encode("ascii")
        ).hexdigest() + ".ini"
        jobs = list((runtime / "outbox").glob("*.ini"))
        assert [path.name for path in jobs] == [expected_name]
        source = jobs[0].read_bytes()
        assert source == canonical_job()
        expected_source_hash = hashlib.sha256(source).hexdigest()
        parsed_job = read_job(jobs[0])
        assert parsed_job == {
            "match_id": MATCH_ID,
            "account_number": ACCOUNT,
            "plan_hash": PLAN_HASH,
            "mb": MB,
            "expected_scope": SCOPE,
            "policy_revision": POLICY_REVISION,
            "source_hash": expected_source_hash,
        }

        repository = FakeNativeLedger()
        worker = WalletWorker(runtime, repository)
        before = int(time.time())
        assert worker.poll()
        after = int(time.time())
        assert len(repository.apply_calls) == 1
        assert repository.current_calls == [ACCOUNT]
        assert not list((runtime / "outbox").glob("*.ini"))
        completed = runtime / "jobs-completed" / expected_name
        receipt_path = runtime / "receipts" / expected_name
        assert completed.read_bytes() == source
        receipt = parse_ini(receipt_path, "receipt")
        assert receipt == {
            "SchemaVersion": "1",
            "MatchId": MATCH_ID,
            "AccountNumber": str(ACCOUNT),
            "PlanHash": PLAN_HASH,
            "Mb": str(MB),
            "MbAfter": str(HISTORICAL_MB),
            "NativeRetVal": "0",
            "AlreadyApplied": "0",
            "PolicyRevision": POLICY_REVISION,
            "ScopeId": str(SCOPE),
            "SourceHash": expected_source_hash,
            "CurrentMb": str(CURRENT_MB),
            "PublishedUnix": receipt["PublishedUnix"],
        }
        assert before <= int(receipt["PublishedUnix"]) <= after
        status = parse_ini(runtime / "wallet-status.ini", "wallet")
        assert status == {
            "SchemaVersion": "1",
            "Healthy": "1",
            "HeartbeatUnix": status["HeartbeatUnix"],
            "ScopeId": str(SCOPE),
            "Revision": "sdgo-native-gc-ledger-v2",
            "LastError": "",
            "Processed": "1",
            "Replays": "0",
        }
        assert before <= int(status["HeartbeatUnix"]) <= after

        run_native(executable, "receipt", runtime)

        status["HeartbeatUnix"] = str(int(time.time()) - 16)
        (runtime / "wallet-status.ini").write_bytes(
            ini_bytes("wallet", status)
        )
        run_native(executable, "stale", runtime)
        v2 = runtime / 'v2'
        run_native(executable, 'emit-v2', v2)
        job = read_job(next((v2/'outbox').glob('*.ini')))
        assert job['mb'] == 0 and job['completed_unix'] == 1793008800
        assert job['active_seconds'] == 180 and job['contributed'] is True
        print(
            "native publish -> Python wallet worker -> native receipt/health "
            "interop passed"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
