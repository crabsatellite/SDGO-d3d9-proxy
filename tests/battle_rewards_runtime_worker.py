from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import traceback
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.rewards import boss_catalog, model, progression
from owner.rewards.runtime_control import RuntimeControl, compile_ini
from owner.rewards.sql_repository import MbCreditReceipt
from owner.rewards.worker import WalletWorker


class FixtureLedger:
    def __init__(self):
        self.scope = uuid.uuid4()
        self.balances = {}
        self.receipts = {}

    def scope_id(self):
        return self.scope

    def current_mb(self, account_number):
        return self.balances.get(account_number, 1000)

    def apply_mb(self, *, match_id, account_number, plan_hash, mb,
                 expected_scope):
        if expected_scope != self.scope:
            raise ValueError("fixture wallet scope mismatch")
        key = match_id, account_number
        prior = self.receipts.get(key)
        if prior is not None:
            if prior.plan_hash != plan_hash or prior.mb_delta != mb:
                raise ValueError("fixture wallet retry changed")
            return MbCreditReceipt(
                prior.match_id, prior.account_number, prior.plan_hash,
                prior.mb_delta, prior.mb_after, prior.native_retval, True
            )
        current = self.current_mb(account_number) + mb
        self.balances[account_number] = current
        receipt = MbCreditReceipt(
            match_id, account_number, plan_hash, mb, current, 0, False
        )
        self.receipts[key] = receipt
        return receipt

    @property
    def credit_count(self):
        return len(self.receipts)

    def verify(self, expected):
        if self.credit_count != expected:
            raise RuntimeError(
                f"expected {expected} fixture credits, got {self.credit_count}"
            )

    def close(self):
        pass


class SqlFixtureLedger:
    def __init__(self, mode):
        self.mode = mode
        tests = ROOT / "tests"
        sys.path.insert(0, str(tests))
        from test_reward_repository import RewardRepositorySqlIntegrationTests

        self.fixture = RewardRepositorySqlIntegrationTests
        self.fixture.setUpClass()
        self.repository = self.fixture.repository
        self.connection = self.fixture.connection
        self.scope = self.fixture.scope
        self.applied = set()
        self.allowed = {6001} if mode == 'progression' else (
            {4001, 5001}
            if mode == "daily"
            else {1001, 1002, 1101, 1201, 1301, 1401, 1501, 1601,
                  1701, 1801, 1901, 2501, 3001}
        )

    def scope_id(self):
        return self.scope

    def _ensure_account(self, account_number):
        if account_number not in self.allowed:
            raise ValueError("runtime fixture requested an unexpected SQL account")
        cursor = self.connection.cursor()
        cursor.execute(
            "IF NOT EXISTS (SELECT 1 FROM dbo.tbls_AccountPocket "
            "WHERE f_idx=?) INSERT dbo.tbls_AccountPocket"
            "(f_idx,f_GC,f_GP) VALUES(?,1000,0)",
            account_number, account_number,
        )
        self.connection.commit()

    def current_mb(self, account_number):
        return self.repository.current_mb(account_number)

    def apply_mb(self, **value):
        self._ensure_account(value["account_number"])
        result = self.repository.apply_mb(**value)
        self.applied.add((result.match_id, result.account_number))
        return result

    def apply_progression(self, job, policy):
        self._ensure_account(job['account_number'])
        return self.repository.apply_progression(job, policy)

    @property
    def credit_count(self):
        return len(self.applied)

    def verify(self, expected):
        cursor = self.connection.cursor()
        receipt_count = cursor.execute(
            "SELECT COUNT(*) FROM dbo.SdgoBattleRewardMbReceipt"
        ).fetchone()[0]
        log_count = cursor.execute(
            "SELECT COUNT(*) FROM dbo.tbls_LogGCInOut "
            "WHERE f_idx IN (" + ",".join("?" for _ in self.allowed) + ")",
            *sorted(self.allowed),
        ).fetchone()[0]
        self.connection.rollback()
        expected_logs = expected + (2 if self.mode == 'progression' else 0)
        if self.credit_count != expected or receipt_count != expected or \
                log_count != expected_logs:
            raise RuntimeError(
                f"expected {expected} SQL credits, got applied="
                f"{self.credit_count} receipts={receipt_count} logs={log_count}"
            )

    def close(self):
        self.fixture.tearDownClass()


def prepare_runtime(runtime, mode='basic'):
    runtime.mkdir(parents=True, exist_ok=True)
    control = RuntimeControl(ROOT, runtime_dir=runtime)
    alternate = model.with_multiplier(model.defaults(), '5')
    control.publish(alternate)
    (runtime / 'reward-policy-base-five.ini').write_bytes(compile_ini(alternate))
    policy = model.defaults()
    if mode == 'progression':
        policy['progression'] = {**progression.defaults(), 'enabled': True}
    control.publish(policy)
    boss_catalog.publish(runtime / "reward-bosses.txt", [(10, 20), (10, 21)])


def run(args):
    runtime = args.runtime.resolve()
    prepare_runtime(runtime, args.mode)
    use_sql = os.environ.get("SDGO_REWARD_SQL_INTEGRATION") == "1"
    if use_sql and not os.environ.get("SDGO_REWARD_SERVER_INI"):
        raise RuntimeError(
            "SDGO_REWARD_SERVER_INI is required for SQL integration"
        )
    ledger = SqlFixtureLedger(args.mode) if use_sql else FixtureLedger()
    try:
        worker = WalletWorker(runtime, ledger)
        if not worker.poll():
            raise RuntimeError("fixture wallet worker did not start healthy")

        command = [str(args.exe.resolve()), str(runtime), args.mode]
        host_output = runtime / "fixture-host-output.txt"
        process = None
        try:
            with host_output.open(
                    "w", encoding="utf-8", errors="replace") as stream:
                process = subprocess.Popen(
                    command, cwd=args.exe.resolve().parent, stdout=stream,
                    stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                    errors="replace"
                )
                deadline = time.monotonic() + args.timeout
                while process.poll() is None and time.monotonic() < deadline:
                    if not worker.poll():
                        raise RuntimeError(
                            "real WalletWorker rejected a native job"
                        )
                    time.sleep(0.025)
                if process.poll() is None:
                    raise TimeoutError(
                        "battle reward runtime fixture exceeded its timeout"
                    )
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=2)
        output = host_output.read_text(encoding="utf-8", errors="replace")
        print(output, end="")
        if process.returncode != 0:
            raise RuntimeError(
                f"battle reward runtime fixture exited {process.returncode}"
            )
        if not worker.poll():
            raise RuntimeError("fixture wallet worker ended unhealthy")

        expected = 4 if args.mode == 'progression' else 2 if args.mode == "daily" else 13
        completed = list((runtime / "jobs-completed").glob("*.ini"))
        receipts = list((runtime / "receipts").glob("*.ini"))
        outbox = list((runtime / "outbox").glob("*.ini"))
        ledger.verify(expected)
        if len(completed) != expected:
            raise RuntimeError(
                f"expected {expected} archived jobs, got {len(completed)}"
            )
        if len(receipts) != expected or outbox:
            raise RuntimeError("native wallet receipts/outbox are incomplete")
        if "ALL REAL CORE BATTLE REWARD RUNTIME CASES PASSED" not in output:
            raise RuntimeError("runtime fixture success marker is absent")
        backend = "SQL native procedure" if use_sql else "fixture ledger"
        print(
            f"REAL WALLET WORKER CASE PASSED: mode={args.mode} "
            f"credits={expected} backend={backend} scope={ledger.scope}"
        )
    finally:
        ledger.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--mode", choices=("basic", "daily", "progression"), required=True)
    parser.add_argument("--timeout", type=float, default=35.0)
    run(parser.parse_args())


if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc(file=sys.stdout)
        raise SystemExit(1)
