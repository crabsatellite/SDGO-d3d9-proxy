"""Durable owner-side MB jobs. GB is delivered by the original game path.

Only native-captured jobs in the configured owner directory are accepted. This
worker has no HTTP payout endpoint and never trusts browser combat telemetry.
"""
from __future__ import annotations
import configparser
import hashlib
import os
from pathlib import Path
import re
import time
import uuid
from .runtime_control import _atomic_write
from .sql_repository import validate_request

WORKER_REVISION = "sdgo-native-gc-ledger-v1"
JOB_NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,159}\.ini\Z")


def ini_bytes(section, values):
    rows = [f"[{section}]"]
    for key, value in values.items():
        text = str(value)
        if any(c in text for c in "\r\n\x00"):
            raise ValueError("Runtime value contains a line break")
        rows.append(f"{key}={text}")
    return ("\r\n".join(rows) + "\r\n").encode("ascii")


def read_job(path):
    path = Path(path)
    if not JOB_NAME.fullmatch(path.name) or path.is_symlink() or path.stat().st_size > 16384:
        raise ValueError("Native reward job identity/size is invalid")
    raw = path.read_bytes()
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    parser.read_string(raw.decode("ascii"))
    if parser.sections() != ["reward"]:
        raise ValueError("Native reward job requires one reward section")
    values = dict(parser["reward"])
    expected = {"SchemaVersion", "MatchId", "AccountNumber", "PlanHash", "Mb", "PolicyRevision", "ExpectedScope"}
    if set(values) != expected or values["SchemaVersion"] != "1":
        raise ValueError("Native reward job schema is invalid")
    if not values["AccountNumber"].isdigit() or not values["Mb"].isdigit():
        raise ValueError("Native reward job amounts are invalid")
    match, account, plan, amount = validate_request(values["MatchId"], int(values["AccountNumber"]), values["PlanHash"], int(values["Mb"]))
    if account > 2147483647 or amount > 2147483647 or not re.fullmatch(r"[a-f0-9]{64}", values["PolicyRevision"]):
        raise ValueError("Native reward job exceeds wallet or revision bounds")
    scope = uuid.UUID(values["ExpectedScope"])
    return {"match_id": match, "account_number": account, "plan_hash": plan, "mb": amount,
            "expected_scope": scope, "policy_revision": values["PolicyRevision"],
            "source_hash": hashlib.sha256(raw).hexdigest()}


class WalletWorker:
    def __init__(self, runtime, repository, clock=time.time):
        self.root = Path(runtime).resolve()
        self.outbox = self.root / "outbox"
        self.receipts = self.root / "receipts"
        self.completed = self.root / "jobs-completed"
        self.status_path = self.root / "wallet-status.ini"
        self.repository = repository
        self.clock = clock
        self.scope = repository.scope_id()
        self.last_error = ""
        self.processed = 0
        self.replays = 0
        for directory in (self.root, self.outbox, self.receipts, self.completed):
            if directory.is_symlink():
                raise ValueError("Linked reward runtime directories are unsupported")
            directory.mkdir(parents=True, exist_ok=True)

    def heartbeat(self, healthy):
        _atomic_write(self.status_path, ini_bytes("wallet", {
            "SchemaVersion": 1, "Healthy": int(healthy), "HeartbeatUnix": int(self.clock()),
            "ScopeId": str(self.scope), "Revision": WORKER_REVISION, "LastError": self.last_error,
            "Processed": self.processed, "Replays": self.replays,
        }))

    def process(self, path):
        path = Path(path)
        if path.parent.resolve() != self.outbox:
            raise ValueError("Reward job is outside the native outbox")
        job = read_job(path)
        if job["expected_scope"] != self.scope:
            raise ValueError("Reward job belongs to a different wallet scope")
        receipt_path = self.receipts / path.name
        if receipt_path.exists():
            old = configparser.ConfigParser(interpolation=None)
            old.read(receipt_path, encoding="ascii")
            if old.get("receipt", "SourceHash", fallback="") != job["source_hash"]:
                raise ValueError("Native reward job changed after its durable receipt")
            self._archive(path)
            return False
        result = self.repository.apply_mb(**{k: job[k] for k in ("match_id", "account_number", "plan_hash", "mb", "expected_scope")})
        current_mb = self.repository.current_mb(job["account_number"])
        if type(current_mb) is not int or not 0 <= current_mb <= 2147483647:
            raise ValueError("Native cash query returned an invalid current balance")
        # The SQL ledger commits first. If this process loses power before the
        # atomic receipt write, the exact same job safely retries through SQL.
        _atomic_write(receipt_path, ini_bytes("receipt", {
            "SchemaVersion": 1, "MatchId": result.match_id, "AccountNumber": result.account_number,
            "PlanHash": result.plan_hash, "Mb": result.mb_delta, "MbAfter": result.mb_after,
            "NativeRetVal": result.native_retval, "AlreadyApplied": int(result.already_applied),
            "PolicyRevision": job["policy_revision"], "ScopeId": str(self.scope), "SourceHash": job["source_hash"],
            "CurrentMb": current_mb, "PublishedUnix": int(self.clock()),
        }))
        self.processed += 1
        self.replays += int(result.already_applied)
        self._archive(path)
        return True

    def _archive(self, path):
        destination = self.completed / path.name
        if destination.exists():
            if destination.read_bytes() != path.read_bytes():
                raise ValueError("Completed native job identity conflicts with a new job")
            path.unlink()
        else:
            os.replace(path, destination)

    def poll(self):
        healthy = True
        self.last_error = ""
        try:
            if self.repository.scope_id() != self.scope:
                raise ValueError("Wallet scope changed while the worker was running")
            for path in sorted(self.outbox.glob("*.ini"))[:256]:
                self.process(path)
        except Exception as error:
            healthy = False
            # Drivers can include server/account details in their messages.
            # Status exposes only a class/SQLSTATE, never a connection string.
            state = error.args[0] if error.args else ""
            state = str(state) if re.fullmatch(r"[0-9A-Z]{5}", str(state)) else ""
            self.last_error = "wallet_job_failed:" + type(error).__name__ + (":" + state if state else "")
        self.heartbeat(healthy)
        return healthy
