"""Compile owner reward policy and verify native settlement heartbeats."""
from __future__ import annotations

from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import threading
import uuid

from . import model


STATUS_SCHEMA_VERSION = 1
DEFAULT_HEARTBEAT_TIMEOUT_SECONDS = 15
_INSTANCE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}")


def _decimal(value, maximum=1_000_000):
    return model.text(model.number(value, maximum))


def compile_ini(policy):
    """Return the complete native policy snapshot as stable ASCII INI bytes."""
    model.validate(policy)
    revision = model.revision(policy)
    pvp = policy["pvp"]
    pve = policy["pve"]
    rows = [
        ("meta", (
            ("SchemaVersion", str(policy["schema_version"])),
            ("PolicyRevision", revision),
        )),
        ("timing", (
            ("FullRewardSeconds", str(policy["full_reward_seconds"])),
            ("MinimumActiveSeconds", str(policy["minimum_active_seconds"])),
        )),
        ("multipliers", (
            ("Base", _decimal(policy["base_multiplier"], 100)),
            ("ExtraHuman", _decimal(policy["extra_human_multiplier"], 2)),
        )),
        ("pvp", (
            ("BaseWinGb", _decimal(pvp["base"]["win"]["gb"])),
            ("BaseWinMb", _decimal(pvp["base"]["win"]["mb"])),
            ("BaseLoseGb", _decimal(pvp["base"]["lose"]["gb"])),
            ("BaseLoseMb", _decimal(pvp["base"]["lose"]["mb"])),
            ("BaseDrawGb", _decimal(pvp["base"]["draw"]["gb"])),
            ("BaseDrawMb", _decimal(pvp["base"]["draw"]["mb"])),
            ("KillGb", _decimal(pvp["kill"]["gb"])),
            ("KillMb", _decimal(pvp["kill"]["mb"])),
            ("AssistGb", _decimal(pvp["assist"]["gb"])),
            ("AssistMb", _decimal(pvp["assist"]["mb"])),
            ("DamagePer1000Gb", _decimal(pvp["damage_per_1000"]["gb"])),
            ("DamagePer1000Mb", _decimal(pvp["damage_per_1000"]["mb"])),
            ("ObjectiveGb", _decimal(pvp["objective"]["gb"])),
            ("ObjectiveMb", _decimal(pvp["objective"]["mb"])),
            ("ContributionCapGb", _decimal(pvp["contribution_cap"]["gb"])),
            ("ContributionCapMb", _decimal(pvp["contribution_cap"]["mb"])),
            ("MvpGb", _decimal(pvp["mvp"]["gb"])),
            ("MvpMb", _decimal(pvp["mvp"]["mb"])),
        )),
        ("pve", (
            ("BaseClearGb", _decimal(pve["base"]["clear"]["gb"])),
            ("BaseClearMb", _decimal(pve["base"]["clear"]["mb"])),
            ("BaseFailGb", _decimal(pve["base"]["fail"]["gb"])),
            ("BaseFailMb", _decimal(pve["base"]["fail"]["mb"])),
            ("MinionGb", _decimal(pve["minion"]["gb"])),
            ("MinionMb", _decimal(pve["minion"]["mb"])),
            ("BossCreditGb", _decimal(pve["boss_credit"]["gb"])),
            ("BossCreditMb", _decimal(pve["boss_credit"]["mb"])),
            ("DamagePer1000Gb", _decimal(pve["damage_per_1000"]["gb"])),
            ("DamagePer1000Mb", _decimal(pve["damage_per_1000"]["mb"])),
            ("ObjectiveGb", _decimal(pve["objective"]["gb"])),
            ("ObjectiveMb", _decimal(pve["objective"]["mb"])),
            ("ContributionCapGb", _decimal(pve["contribution_cap"]["gb"])),
            ("ContributionCapMb", _decimal(pve["contribution_cap"]["mb"])),
            ("TotalCap1xGb", _decimal(pve["total_cap_1x"]["gb"])),
            ("TotalCap1xMb", _decimal(pve["total_cap_1x"]["mb"])),
            ("DifficultyNormal", _decimal(pve["difficulty"]["normal"], 3)),
            ("DifficultyHard", _decimal(pve["difficulty"]["hard"], 3)),
            ("DifficultyExpert", _decimal(pve["difficulty"]["expert"], 3)),
        )),
    ]
    lines = []
    for section, values in rows:
        if lines:
            lines.append("")
        lines.append(f"[{section}]")
        lines.extend(f"{key}={value}" for key, value in values)
    return ("\r\n".join(lines) + "\r\n").encode("ascii")


def _atomic_write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _utc(value):
    if not isinstance(value, str) or not value.endswith("Z"):
        raise ValueError("heartbeat_utc must be UTC with Z suffix")
    parsed = datetime.fromisoformat(value[:-1] + "+00:00")
    if parsed.tzinfo is None:
        raise ValueError("heartbeat_utc must include a timezone")
    return parsed.astimezone(timezone.utc)


def _optional_diagnostics(value):
    diagnostics = {}
    limits = value.get("limits")
    if limits is not None:
        if not isinstance(limits, dict):
            raise ValueError("limits must be an object")
        for key in ("max_gb", "max_mb"):
            if type(limits.get(key)) is not int or not 0 <= limits[key] <= 2147483647:
                raise ValueError(f"limits.{key} is invalid")
        diagnostics["limits"] = {
            "max_gb": limits["max_gb"],
            "max_mb": limits["max_mb"],
            "max_base_multiplier": _decimal(limits["max_base_multiplier"], 100),
        }
    window = value.get("activity_window")
    if window is not None:
        if not isinstance(window, dict) or type(window.get("active")) is not bool:
            raise ValueError("activity_window is invalid")
        for key in ("start", "end"):
            text = window.get(key)
            if not isinstance(text, str) or not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", text):
                raise ValueError(f"activity_window.{key} is invalid")
        diagnostics["activity_window"] = {
            "start": window["start"],
            "end": window["end"],
            "active": window["active"],
        }
    counters = value.get("payout_counters")
    if counters is not None:
        keys = (
            "matches_seen",
            "settlements_committed",
            "settlements_failed",
            "payouts_committed",
            "payouts_rejected",
            "idempotent_replays",
        )
        if not isinstance(counters, dict):
            raise ValueError("payout_counters must be an object")
        if any(type(counters.get(key)) is not int or not 0 <= counters[key] <= 9223372036854775807
               for key in keys):
            raise ValueError("payout_counters is invalid")
        diagnostics["payout_counters"] = {key: counters[key] for key in keys}
    return diagnostics


class RuntimeControl:
    """Own the compiled policy and derive connectivity from native heartbeats."""

    def __init__(self, workspace_root, runtime_dir=None,
                 heartbeat_timeout_seconds=DEFAULT_HEARTBEAT_TIMEOUT_SECONDS, clock=None):
        if type(heartbeat_timeout_seconds) is not int or not 1 <= heartbeat_timeout_seconds <= 300:
            raise ValueError("heartbeat timeout must be 1..300 seconds")
        self.workspace_root = Path(workspace_root).resolve()
        self.root = Path(runtime_dir).resolve() if runtime_dir else self.workspace_root / "runtime"
        self.policy_path = self.root / "reward-policy.ini"
        self.status_dir = self.root / "status"
        self.heartbeat_timeout_seconds = heartbeat_timeout_seconds
        self.clock = clock or (lambda: datetime.now(timezone.utc))
        self.lock = threading.RLock()

    def publish(self, policy):
        """Atomically publish one native snapshot and return its revision."""
        data = compile_ini(policy)
        with self.lock:
            current = self.policy_path.read_bytes() if self.policy_path.is_file() else None
            if current != data:
                _atomic_write(self.policy_path, data)
        return model.revision(policy)

    def _read_status(self, path, expected_revision, now):
        if path.stat().st_size > 65536:
            raise ValueError("status file is too large")
        value = json.loads(path.read_text(encoding="utf-8-sig"))
        if not isinstance(value, dict) or value.get("schema_version") != STATUS_SCHEMA_VERSION:
            raise ValueError("unsupported status schema")
        instance = value.get("instance_id")
        if not isinstance(instance, str) or not _INSTANCE.fullmatch(instance):
            raise ValueError("invalid instance_id")
        if path.name != instance + ".json":
            raise ValueError("instance_id does not match the status filename")
        if value.get("component") != "battle_reward":
            raise ValueError("invalid status component")
        heartbeat = _utc(value.get("heartbeat_utc"))
        age = (now - heartbeat).total_seconds()
        modified_age = now.timestamp() - path.stat().st_mtime
        reasons = []
        if age < -5 or modified_age < -5:
            reasons.append("clock_in_future")
        if age > self.heartbeat_timeout_seconds or modified_age > self.heartbeat_timeout_seconds:
            reasons.append("heartbeat_stale")
        if value.get("policy_revision") != expected_revision:
            reasons.append("policy_revision_mismatch")
        for key in ("policy_loaded", "settlement_ready", "wallet_worker_healthy"):
            if value.get(key) is not True:
                reasons.append(key + "_false")
        last_error = value.get("last_error")
        if last_error is not None and (not isinstance(last_error, str) or len(last_error) > 1024):
            reasons.append("invalid_last_error")
            last_error = None
        result = {
            "instance_id": instance,
            "connected": not reasons,
            "heartbeat_utc": value["heartbeat_utc"],
            "policy_revision": value.get("policy_revision"),
            "wallet_worker_revision": value.get("wallet_worker_revision"),
            "reasons": reasons,
            "last_error": last_error,
        }
        result.update(_optional_diagnostics(value))
        return result

    def health(self, policy):
        expected = self.publish(policy)
        now = self.clock().astimezone(timezone.utc)
        instances = []
        if self.status_dir.is_dir():
            for path in sorted(self.status_dir.glob("*.json"))[:64]:
                try:
                    instances.append(self._read_status(path, expected, now))
                except (OSError, ValueError, TypeError, KeyError, json.JSONDecodeError) as error:
                    instances.append({
                        "instance_id": path.stem,
                        "connected": False,
                        "reasons": ["invalid_status"],
                        "last_error": str(error),
                    })
        connected = [item["instance_id"] for item in instances if item["connected"]]
        if connected:
            status = "connected"
        elif instances:
            status = "native_unhealthy_or_reloading"
        else:
            status = "waiting_for_native"
        return {
            "delivery_connected": bool(connected),
            "status": status,
            "runtime": {
                "schema_version": 1,
                "policy_revision": expected,
                "policy_file": str(self.policy_path),
                "status_directory": str(self.status_dir),
                "heartbeat_timeout_seconds": self.heartbeat_timeout_seconds,
                "connected_instances": connected,
                "instances": instances,
            },
        }

    def decorate(self, public):
        result = dict(public)
        result.update(self.health(result["policy"]))
        return result
