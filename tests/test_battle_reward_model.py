from fractions import Fraction
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
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
from owner.rewards.workspace import Workspace
from assistant_modules.rewards import attach


def pvp(**changes):
    context = dict(mode="pvp", outcome="win", human_count=1, event_multiplier="1", duration_seconds=180)
    stats = dict(kind="human", active_seconds=180, kills=3, damage=8000, is_mvp=False)
    context.update(changes)
    return context, stats


class CalculationTests(unittest.TestCase):
    def setUp(self):
        self.policy = model.defaults()

    def calculate(self, context, stats):
        return model.preview(self.policy, context, stats)

    def test_ten_and_double_multiply_once(self):
        context, stats = pvp()
        normal = self.calculate(context, stats)
        context["event_multiplier"] = "2"
        doubled = self.calculate(context, stats)
        self.assertEqual(normal["amount"], dict(gb=8000, mb=34))
        self.assertEqual(doubled["amount"], dict(gb=16000, mb=68))
        self.assertEqual(doubled["multipliers"]["effective"], "20")

    def test_native_default_does_not_advertise_uncollected_objective_rewards(self):
        self.assertEqual(self.policy["pvp"]["objective"], {"gb": "0", "mb": "0"})
        self.assertEqual(self.policy["pve"]["objective"], {"gb": "0", "mb": "0"})

    def test_compounded_humans_and_ai_same_tier(self):
        context, stats = pvp()
        solo = self.calculate(context, stats)
        context["ai_count"] = 7
        self.assertEqual(self.calculate(context, stats)["amount"], solo["amount"])
        for humans, expected in ((1, "1"), (2, "1.1"), (3, "1.21"), (4, "1.331"), (8, "1.9487171"), (12, "2.85311670611")):
            context["human_count"] = humans
            self.assertEqual(self.calculate(context, stats)["multipliers"]["humans"], expected)

    def test_mvp_and_all_factors_apply_to_both_currencies(self):
        context, stats = pvp(human_count=4, event_multiplier="2")
        stats["is_mvp"] = True
        result = self.calculate(context, stats)
        self.assertEqual(result["amount"], dict(gb=26620, mb=250))
        self.assertEqual(result["multipliers"]["effective"], "26.62")
        self.assertEqual(result["base_1x"], dict(gb="1000", mb="9.4"))

    def test_winner_and_loser_keep_participation_base(self):
        context, stats = pvp()
        stats.update(kills=0, damage=0)
        self.assertEqual(self.calculate(context, stats)["amount"]["gb"], 5000)
        context["outcome"] = "lose"
        self.assertEqual(self.calculate(context, stats)["amount"]["gb"], 3000)

    def test_minions_and_boss_damage_credit(self):
        context = dict(mode="pve", outcome="clear", human_count=4, event_multiplier="2", duration_seconds=180, bosses_defeated=1)
        stats = dict(active_seconds=180, minion_kills=15, boss_credit="1", damage=8000)
        result = self.calculate(context, stats)
        self.assertEqual(result["amount"], dict(gb=4871, mb=13))
        self.assertEqual(result["base_1x"], dict(gb="183", mb="0.5"))
        stats["boss_credit"] = "0.5"
        self.assertLess(self.calculate(context, stats)["amount"]["gb"], result["amount"]["gb"])
        stats["boss_credit"] = "2"
        with self.assertRaises(ValueError): self.calculate(context, stats)

    def test_pvp_typical_returns_far_above_mission(self):
        context, stats = pvp(human_count=4, event_multiplier="2")
        versus = self.calculate(context, stats)
        task = self.calculate(dict(mode="pve", outcome="clear", human_count=4, event_multiplier="2", duration_seconds=180, bosses_defeated=1),
                              dict(active_seconds=180, minion_kills=15, boss_credit="1", damage=8000))
        self.assertGreater(versus["amount"]["gb"], task["amount"]["gb"] * 4)
        self.assertGreater(versus["amount"]["mb"], task["amount"]["mb"] * 6)

    def test_fractional_duration_has_no_float_or_decimal_floor_drift(self):
        context, stats = pvp(duration_seconds=60)
        stats.update(active_seconds=60, kills=255, damage=100000)
        # (500 + capped 400) * 10 / 3 is exactly 3000.
        self.assertEqual(self.calculate(context, stats)["amount"]["gb"], 3000)

    def test_default_extreme_case_is_bounded_and_below_uint16_gb(self):
        context, stats = pvp(human_count=12, event_multiplier="2")
        stats.update(kills=255, assists=255, damage=100000000, objectives=1000, is_mvp=True)
        result = self.calculate(context, stats)
        self.assertEqual(result["base_1x"]["gb"], "1100")
        self.assertLessEqual(result["amount"]["gb"], 65535)

    def test_pve_cap_and_mvp_scope(self):
        context = dict(mode="pve", outcome="clear", human_count=1, event_multiplier="1", duration_seconds=180, bosses_defeated=100, difficulty="expert")
        stats = dict(active_seconds=180, minion_kills=10000, damage=100000000, boss_credit="100")
        result = self.calculate(context, stats)
        stats["is_mvp"] = True
        self.assertEqual(self.calculate(context, stats)["amount"], result["amount"])
        self.assertLessEqual(Fraction(result["base_1x"]["gb"]), 300)

    def test_no_wallet_for_ai_observers_or_too_short_sessions(self):
        context, stats = pvp()
        for kind in ("ai", "observer"):
            stats["kind"] = kind
            self.assertEqual(self.calculate(context, stats)["amount"], dict(gb=0, mb=0))
        stats.update(kind="human", active_seconds=20)
        self.assertFalse(self.calculate(context, stats)["eligible"])

    def test_invalid_counts_event_and_multiplier_rejected(self):
        context, stats = pvp()
        for changes in (dict(human_count=0), dict(human_count=13), dict(human_count=True), dict(event_multiplier="4"), dict(duration_seconds=1)):
            with self.assertRaises(ValueError): self.calculate({**context, **changes}, stats)
        for value in (True, -1, "NaN", "Infinity", "1/2", "100.01"):
            with self.assertRaises(ValueError): model.with_multiplier(self.policy, value)
        with self.assertRaises(ValueError): self.calculate(context, {**stats, "damage": -1})

    def test_batch_derives_unique_human_count_and_never_pays_bots(self):
        context, stats = pvp(human_count=12)
        players = [dict(participant_id="one", account_id=1001, stats=stats),
                   dict(participant_id="two", account_id=1002, stats=stats),
                   dict(participant_id="bot", stats={**stats, "kind": "ai"})]
        result = model.preview_match(self.policy, context, players)
        self.assertEqual(result["human_count"], 2)
        self.assertEqual(result["participants"][0]["multipliers"]["humans"], "1.1")
        self.assertEqual(result["participants"][2]["amount"], dict(gb=0, mb=0))
        players[1]["account_id"] = 1001
        with self.assertRaises(ValueError): model.preview_match(self.policy, context, players)

    def test_batch_boss_credit_cannot_be_claimed_twice(self):
        context = dict(mode="pve", outcome="clear", duration_seconds=180, bosses_defeated=1)
        players = [dict(participant_id="a", account_id=1, stats=dict(active_seconds=180, boss_credit="0.6")),
                   dict(participant_id="b", account_id=2, stats=dict(active_seconds=180, boss_credit="0.6"))]
        with self.assertRaises(ValueError): model.preview_match(self.policy, context, players)
        players[1]["stats"]["boss_credit"] = "0.4"
        self.assertEqual(model.preview_match(self.policy, context, players)["human_count"], 2)


class WorkspaceTests(unittest.TestCase):
    def test_revision_history_and_preview_rate_do_not_write_live_data(self):
        with tempfile.TemporaryDirectory() as root:
            workspace = Workspace(root)
            initial = workspace.public()
            context, stats = pvp()
            result = workspace.preview(dict(context=context, stats=stats, preview_multiplier="20"))
            self.assertEqual(result["amount"]["gb"], 16000)
            self.assertEqual(workspace.public()["revision"], initial["revision"])
            saved = workspace.multiplier("20", initial["revision"])
            self.assertNotEqual(saved["revision"], initial["revision"])
            self.assertFalse(saved["delivery_connected"])
            self.assertEqual(len(list((Path(root) / "history").glob("*.json"))), 1)
            with self.assertRaises(ValueError): workspace.multiplier("5", initial["revision"])


class HttpModuleTests(unittest.TestCase):
    def test_same_port_existing_module_and_protected_updates(self):
        class Existing(BaseHTTPRequestHandler):
            def log_message(self, *args): pass
            def json_response(self, status, value):
                body = json.dumps(value).encode(); self.send_response(status)
                self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
            def do_GET(self):
                self.json_response(200, {"modules": [{"id": "existing"}]} if self.path == "/api/modules" else {"existing": True})
            def do_POST(self): self.json_response(200, {"existing_post": True})
        with tempfile.TemporaryDirectory() as root:
            server = attach(ThreadingHTTPServer(("127.0.0.1", 0), Existing), Workspace(root))
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            origin = f"http://127.0.0.1:{server.server_port}"
            try:
                def read(path):
                    with urlopen(origin + path, timeout=3) as response: return json.load(response)
                self.assertEqual([x["id"] for x in read("/api/modules")["modules"]], ["existing", "rewards"])
                self.assertTrue(read("/existing")["existing"])
                state = read("/api/rewards")
                payload = json.dumps({"base_multiplier": "20"}).encode()
                headers = {"Content-Type": "application/json", "Origin": origin,
                           "X-Assistant-Token": state["session_token"], "X-Reward-Revision": state["revision"]}
                with urlopen(Request(origin + "/api/rewards/multiplier", data=payload, headers=headers), timeout=3) as response:
                    self.assertEqual(json.load(response)["policy"]["base_multiplier"], "20")
                headers["Origin"] = "https://unrelated.invalid"
                with self.assertRaises(HTTPError) as rejected:
                    urlopen(Request(origin + "/api/rewards/multiplier", data=payload, headers=headers), timeout=3)
                self.assertEqual(rejected.exception.code, 403)
                rejected.exception.close()
                with urlopen(origin + "/rewards", timeout=3) as response:
                    self.assertIn("未连接游戏结算", response.read().decode())
            finally:
                server.shutdown(); server.server_close(); thread.join(timeout=3)


if __name__ == "__main__": unittest.main()
