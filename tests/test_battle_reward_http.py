from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import configparser
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))

from owner.rewards import model
from owner.rewards.runtime_control import RuntimeControl, compile_ini
from owner.rewards.workspace import Workspace
from assistant_modules.rewards import attach


NOW = datetime(2026, 11, 1, 3, 4, 5, tzinfo=timezone.utc)


def iso(value=NOW):
    return value.isoformat().replace("+00:00", "Z")


def heartbeat(revision, **changes):
    value = {
        "schema_version": 1,
        "instance_id": "battle-1",
        "component": "battle_reward",
        "heartbeat_utc": iso(),
        "policy_revision": revision,
        "policy_loaded": True,
        "settlement_ready": True,
        "wallet_worker_healthy": True,
        "wallet_worker_revision": "wallet-schema-1",
        "last_error": None,
    }
    value.update(changes)
    return value


def write_status(control, value, modified=NOW):
    control.status_dir.mkdir(parents=True, exist_ok=True)
    path = control.status_dir / (value["instance_id"] + ".json")
    path.write_text(json.dumps(value), encoding="utf-8")
    os.utime(path, (modified.timestamp(), modified.timestamp()))
    return path


class RuntimePolicyTests(unittest.TestCase):
    def test_compiled_policy_is_complete_stable_ascii_ini(self):
        policy = model.defaults()
        raw = compile_ini(policy)
        self.assertEqual(raw.decode("ascii").encode("ascii"), raw)
        self.assertTrue(raw.endswith(b"\r\n"))
        self.assertNotIn(b"\n", raw.replace(b"\r\n", b""))
        parser = configparser.ConfigParser()
        parser.optionxform = str
        parser.read_file(io.StringIO(raw.decode("ascii")))
        self.assertEqual(parser["meta"]["SchemaVersion"], "1")
        self.assertEqual(parser["meta"]["PolicyRevision"], model.revision(policy))
        self.assertEqual(parser["multipliers"]["Base"], "10")
        self.assertEqual(parser["multipliers"]["ExtraHuman"], "1.1")
        self.assertEqual(parser["pvp"]["BaseWinGb"], "500")
        self.assertEqual(parser["pvp"]["MvpMb"], "6")
        self.assertEqual(parser["pve"]["BossCreditMb"], "0.5")
        self.assertEqual(parser["pve"]["DifficultyExpert"], "1.5")
        self.assertEqual(len(parser["pvp"]), 18)
        self.assertEqual(len(parser["pve"]), 19)
        self.assertEqual(
            set(parser.sections()),
            {"meta", "timing", "multipliers", "pvp", "pve"},
        )

    def test_connection_requires_current_fresh_complete_native_handshake(self):
        with tempfile.TemporaryDirectory() as root:
            control = RuntimeControl(root, heartbeat_timeout_seconds=15, clock=lambda: NOW)
            workspace = Workspace(root, control)
            revision = workspace.public()["revision"]
            self.assertTrue(control.policy_path.is_file())
            self.assertFalse(workspace.public()["delivery_connected"])

            value = heartbeat(
                revision,
                limits={"max_gb": 65535, "max_mb": 2147483647, "max_base_multiplier": "10"},
                activity_window={"start": "18:00", "end": "22:00", "active": False},
                payout_counters={
                    "matches_seen": 8,
                    "settlements_committed": 7,
                    "settlements_failed": 1,
                    "payouts_committed": 20,
                    "payouts_rejected": 1,
                    "idempotent_replays": 2,
                },
            )
            path = write_status(control, value)
            state = workspace.public()
            self.assertTrue(state["delivery_connected"])
            self.assertEqual(state["runtime"]["connected_instances"], ["battle-1"])
            self.assertEqual(state["runtime"]["instances"][0]["limits"]["max_gb"], 65535)
            self.assertEqual(state["runtime"]["instances"][0]["payout_counters"]["idempotent_replays"], 2)

            value["policy_revision"] = "0" * 64
            write_status(control, value)
            state = workspace.public()
            self.assertFalse(state["delivery_connected"])
            self.assertIn("policy_revision_mismatch", state["runtime"]["instances"][0]["reasons"])

            value.update(policy_revision=revision, wallet_worker_healthy=False)
            write_status(control, value)
            state = workspace.public()
            self.assertFalse(state["delivery_connected"])
            self.assertIn("wallet_worker_healthy_false", state["runtime"]["instances"][0]["reasons"])

            value.update(wallet_worker_healthy=True, heartbeat_utc=iso(NOW - timedelta(seconds=16)))
            write_status(control, value, NOW - timedelta(seconds=16))
            state = workspace.public()
            self.assertFalse(state["delivery_connected"])
            self.assertIn("heartbeat_stale", state["runtime"]["instances"][0]["reasons"])
            self.assertFalse(any(control.root.glob(".*.tmp")))
            self.assertEqual(path.name, "battle-1.json")

    def test_invalid_status_is_diagnostic_and_never_connects(self):
        with tempfile.TemporaryDirectory() as root:
            control = RuntimeControl(root, clock=lambda: NOW)
            workspace = Workspace(root, control)
            control.status_dir.mkdir(parents=True)
            path = control.status_dir / "broken.json"
            path.write_text('{"schema_version": 1,', encoding="utf-8")
            os.utime(path, (NOW.timestamp(), NOW.timestamp()))
            state = workspace.public()
            self.assertFalse(state["delivery_connected"])
            self.assertEqual(state["runtime"]["instances"][0]["reasons"], ["invalid_status"])

    def test_failed_runtime_publish_rolls_back_canonical_policy(self):
        class FailingControl(RuntimeControl):
            fail = False

            def publish(self, policy):
                if self.fail:
                    raise OSError("fixture publish failure")
                return super().publish(policy)

        with tempfile.TemporaryDirectory() as root:
            control = FailingControl(root, clock=lambda: NOW)
            workspace = Workspace(root, control)
            initial = workspace.public()
            control.fail = True
            with self.assertRaises(OSError):
                workspace.multiplier("20", initial["revision"])
            self.assertEqual(workspace.read()["base_multiplier"], "10")
            self.assertIn(b"Base=10\r\n", control.policy_path.read_bytes())


class HttpRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.control = RuntimeControl(self.temp.name, clock=lambda: NOW)
        self.workspace = Workspace(self.temp.name, self.control)

        class Existing(BaseHTTPRequestHandler):
            def log_message(self, *_):
                pass

            def json_response(self, status, value):
                body = json.dumps(value).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/api/modules":
                    return self.json_response(200, {"modules": [
                        {"id": "existing"},
                        {"id": "rewards", "name": "old"},
                    ]})
                return self.json_response(200, {"existing": True})

            def do_POST(self):
                return self.json_response(200, {"existing_post": True})

        self.server = attach(ThreadingHTTPServer(("127.0.0.1", 0), Existing), self.workspace)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()
        self.origin = f"http://127.0.0.1:{self.server.server_port}"

    def tearDown(self):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join(timeout=3)
        self.temp.cleanup()

    def read(self, path):
        with urlopen(self.origin + path, timeout=3) as response:
            return json.load(response)

    def post_multiplier(self, state, multiplier, **headers):
        values = {
            "Content-Type": "application/json",
            "Origin": self.origin,
            "X-Assistant-Token": state["session_token"],
            "X-Reward-Revision": state["revision"],
        }
        values.update(headers)
        return urlopen(Request(
            self.origin + "/api/rewards/multiplier",
            data=json.dumps({"base_multiplier": multiplier}).encode(),
            headers=values,
        ), timeout=3)

    def test_shared_port_preserves_routes_and_replaces_duplicate_module(self):
        modules = self.read("/api/modules")["modules"]
        self.assertEqual([item["id"] for item in modules], ["existing", "rewards"])
        self.assertEqual(self.read("/existing"), {"existing": True})
        with urlopen(Request(self.origin + "/existing-post", data=b"{}"), timeout=3) as response:
            self.assertEqual(json.load(response), {"existing_post": True})

    def test_save_publishes_new_revision_and_waits_for_native_reload(self):
        initial = self.read("/api/rewards")
        self.assertFalse(initial["delivery_connected"])
        write_status(self.control, heartbeat(initial["revision"]))
        connected = self.read("/api/rewards")
        self.assertTrue(connected["delivery_connected"])

        with self.post_multiplier(connected, "20") as response:
            saved = json.load(response)
        self.assertEqual(saved["policy"]["base_multiplier"], "20")
        self.assertNotEqual(saved["revision"], initial["revision"])
        self.assertFalse(saved["delivery_connected"])
        self.assertIn(
            "policy_revision_mismatch",
            saved["runtime"]["instances"][0]["reasons"],
        )
        ini = self.control.policy_path.read_bytes()
        self.assertIn(b"Base=20\r\n", ini)
        self.assertIn(("PolicyRevision=" + saved["revision"]).encode("ascii"), ini)

        write_status(self.control, heartbeat(saved["revision"]))
        self.assertTrue(self.read("/api/rewards")["delivery_connected"])

        with self.assertRaises(HTTPError) as stale:
            self.post_multiplier(initial, "30")
        self.assertEqual(stale.exception.code, 400)
        stale.exception.close()

    def test_cross_origin_or_wrong_token_cannot_change_policy(self):
        state = self.read("/api/rewards")
        with self.assertRaises(HTTPError) as foreign:
            self.post_multiplier(state, "20", Origin="https://unrelated.invalid")
        self.assertEqual(foreign.exception.code, 403)
        foreign.exception.close()
        with self.assertRaises(HTTPError) as wrong:
            self.post_multiplier(state, "20", **{"X-Assistant-Token": "wrong"})
        self.assertEqual(wrong.exception.code, 403)
        wrong.exception.close()
        with self.assertRaises(HTTPError) as bad_host:
            self.post_multiplier(state, "20", Host="unrelated.invalid")
        self.assertEqual(bad_host.exception.code, 403)
        bad_host.exception.close()
        self.assertEqual(self.workspace.read()["base_multiplier"], "10")


if __name__ == "__main__":
    unittest.main()
