from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.rewards import model
from owner.rewards.runtime_control import RuntimeControl, compile_ini


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: runtime_state_probe.py TEST_EXE")
    executable = Path(sys.argv[1]).resolve()
    policy = model.defaults()
    revision = model.revision(policy)
    with tempfile.TemporaryDirectory() as directory:
        owner = Path(directory)
        runtime = owner / "runtime"
        runtime.mkdir()
        (runtime / "reward-policy.ini").write_bytes(compile_ini(policy))
        completed = subprocess.run(
            [str(executable), str(owner), revision],
            cwd=ROOT,
            check=False,
            text=True,
            capture_output=True,
        )
        sys.stdout.write(completed.stdout)
        sys.stderr.write(completed.stderr)
        if completed.returncode:
            return completed.returncode

        status_path = runtime / "status" / "battle-1.json"
        value = json.loads(status_path.read_text(encoding="utf-8"))
        assert value["schema_version"] == 1
        assert value["component"] == "battle_reward"
        assert value["policy_revision"] == revision
        assert value["limits"]["max_gb"] == 65535
        assert value["limits"]["max_mb"] == 2147483647
        assert value["limits"]["max_base_multiplier"] != "0"
        assert value["payout_counters"]["idempotent_replays"] == 2
        assert not list((runtime / "status").glob(".*.tmp"))

        control = RuntimeControl(
            owner,
            clock=lambda: datetime.now(timezone.utc),
        )
        health = control.health(policy)
        assert health["delivery_connected"], health
        assert health["runtime"]["connected_instances"] == ["battle-1"]
        instance = health["runtime"]["instances"][0]
        assert instance["wallet_worker_revision"] == "wallet-schema-1"
        assert instance["activity_window"]["active"] is False
        print(
            "native heartbeat accepted by RuntimeControl; "
            f"max Base={instance['limits']['max_base_multiplier']}"
        )

        unsafe = model.defaults()
        unsafe["base_multiplier"] = "100"
        unsafe_revision = model.revision(unsafe)
        (runtime / "reward-policy.ini").write_bytes(compile_ini(unsafe))
        completed = subprocess.run(
            [str(executable), str(owner), unsafe_revision, "unsafe"],
            cwd=ROOT,
            check=False,
            text=True,
            capture_output=True,
        )
        sys.stdout.write(completed.stdout)
        sys.stderr.write(completed.stderr)
        if completed.returncode:
            return completed.returncode
        health = control.health(unsafe)
        assert not health["delivery_connected"], health
        instances = {
            item["instance_id"]: item for item in health["runtime"]["instances"]
        }
        rejected = instances["battle-unsafe"]
        assert "policy_loaded_false" in rejected["reasons"]
        assert rejected["policy_revision"] == unsafe_revision
        assert "configured Base exceeds native safe maximum" in rejected["last_error"]
        assert rejected["limits"]["max_base_multiplier"] == "10.440894"
        print("unsafe Base=100 rejected and reported fail-closed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
