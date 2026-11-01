from pathlib import Path
import configparser
import contextlib
import importlib.util
import io
import json
import tempfile
import unittest
from unittest import mock
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("game_test", ROOT / "tools/game_test.py")
game_test = importlib.util.module_from_spec(spec)
spec.loader.exec_module(game_test)


TEST_MANIFEST = """[mod]
id=game_test_client
category=development
feature=game-test
channel=experimental
activation=manual
name=fixture observer
version=0.1.0
api=1
dll=game_test_client.dll
enabled=0
order=1000
hosts=GOnline-54BE1568-00C97000
requires=game.app,test.client_init,test.client_update,test.client_set_state
"""

MATCH_MANIFEST = """[mod]
id=match_ai
category=gameplay
feature=pvp-ai
channel=beta
activation=room-title
name=normal match AI
version=0.1.0
api=1
dll=match_ai.dll
order=110
hosts=GOnline-54BE1568-00C97000
requires=game.app,match.runtime
"""

OTHER_MANIFEST = """[mod]
id=other_mod
category=gameplay
feature=other
name=preserved addon
version=1
api=1
dll=other_mod.dll
hosts=*
"""


class FakeProcess:
    next_pid = 5100

    def __init__(self, *_args, **_kwargs):
        self.pid = FakeProcess.next_pid
        FakeProcess.next_pid += 1
        self.killed = False
        self.returncode = None

    def poll(self):
        return -9 if self.killed else self.returncode

    def kill(self):
        self.killed = True
        self.returncode = -9

    def wait(self, timeout=None):
        return self.returncode if self.returncode is not None else 0


class FakeStartupEvent:
    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        return None


class GameTestRunnerTests(unittest.TestCase):
    def test_real_manifest_merges_the_optional_production_observer_profile_alias(self):
        # An older normal installation may have only flow.profile_pointer.
        # match_ai binds game.profile_pointer optionally, but the real test
        # needs it to identify the authenticated pilot and the local team.
        with tempfile.TemporaryDirectory() as directory:
            copied = Path(directory) / "symbols.ini"
            copied.write_text("[host]\nimage=GOnline.exe\n[flow.profile_pointer]\nkind=data\nrva=old\n")
            game_test.merge_required_symbols(
                ROOT / "symbols" / f"{game_test.HOST_KEY}.ini",
                copied,
                [ROOT / "mods/game_test_client/mod.ini", ROOT / "mods/match_ai/mod.ini"],
            )
            merged = game_test.read_ini(copied)
            self.assertIn("game.profile_pointer", merged)
            self.assertEqual(merged["game.profile_pointer"]["rva"], merged["flow.profile_pointer"]["rva"])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        git_source_patch = mock.patch.object(
            game_test,
            "git_source_ref",
            return_value={"commit": "0" * 40, "dirty": False},
        )
        git_source_patch.start()
        self.addCleanup(git_source_patch.stop)
        self.template = self.root / "normal-client"
        self.stage = self.root / "stage"
        self.account_profile = self.root / "persistent" / "account.ini"
        self.template.mkdir()
        self.account_profile.parent.mkdir()
        self.account_profile.write_text(
            """[account]
username=ReuseA1234
password=ReusePass123

[registration]
state=auth_accepted
"""
        )
        self.make_normal_client()
        self.make_stage()

    def tearDown(self):
        self.temp.cleanup()

    def make_normal_client(self):
        (self.template / "GOnline.exe").write_bytes(b"normal-game")
        (self.template / "3_SD敢达快速启动.exe").write_bytes(b"normal-launcher")
        (self.template / "配置文件.ini").write_text("[server]\nhost=127.0.0.1\nport=5001\n")
        (self.template / "config.xml").write_text("<config/>")
        (self.template / "d3d9.dll").write_bytes(b"normal-loader")
        (self.template / "SDPlus.dll").write_bytes(b"normal-localization")
        (self.template / "GPackData.zpk").write_bytes(b"normal-assets")
        (self.template / "BGM").mkdir()
        (self.template / "BGM" / "theme.ogg").write_bytes(b"music")
        (self.template / "old.log").write_text("do not clone")

        sdgo = self.template / "sdgo"
        (sdgo / "mods" / "match_ai").mkdir(parents=True)
        (sdgo / "mods" / "match_ai" / "match_ai.dll").write_bytes(b"normal-match-ai")
        (sdgo / "mods" / "match_ai" / "mod.ini").write_text(MATCH_MANIFEST)
        (sdgo / "mods" / "other_mod").mkdir()
        (sdgo / "mods" / "other_mod" / "other_mod.dll").write_bytes(b"other-addon")
        (sdgo / "mods" / "other_mod" / "mod.ini").write_text(OTHER_MANIFEST)
        (sdgo / "symbols").mkdir()
        (sdgo / "symbols" / f"{game_test.HOST_KEY}.ini").write_text(
            """[host]
image=GOnline.exe
timestamp=old

[game.app]
kind=data
rva=old

[match.runtime]
kind=code
rva=old

[other.symbol]
kind=data
rva=kept
"""
        )
        (sdgo / "gametest" / "private").mkdir(parents=True)
        (sdgo / "gametest" / "events.jsonl").write_text("stale")
        (sdgo / "gametest" / "private" / "account.ini").write_text("stale-secret")
        (sdgo / "logs").mkdir()
        (sdgo / "logs" / "sdgo.log").write_text("stale log")
        (sdgo / "core.dll").write_bytes(b"normal-core")
        (sdgo / "sdgo.ini").write_text(
            """[core]
log_level=1
[categories]
gameplay=0
development=0
[features]
pvp-ai=0
game-test=0
[mods]
match_ai=0
other_mod=1
"""
        )

    def make_stage(self):
        client = self.stage / "client"
        observer = client / "sdgo" / "mods" / "game_test_client"
        observer.mkdir(parents=True)
        (client / "d3d9.dll").write_bytes(b"stage-loader")
        (client / "sdgo" / "core.dll").write_bytes(b"stage-core")
        (client / "sdgo" / "sdgo.ini").write_text("[core]\nlog_level=0\n")
        (observer / "game_test_client.dll").write_bytes(b"actual-observer")
        (observer / "mod.ini").write_text(TEST_MANIFEST)
        symbols = client / "sdgo" / "symbols"
        symbols.mkdir()
        (symbols / f"{game_test.HOST_KEY}.ini").write_text(
            """[host]
image=GOnline.exe
timestamp=new

[game.app]
kind=data
rva=new-app

[match.runtime]
kind=code
rva=new-match

[test.client_init]
kind=code
rva=init

[test.client_update]
kind=code
rva=update

[test.client_set_state]
kind=code
rva=state
"""
        )

    def prepare(self, name="run", **kwargs):
        path = self.root / name
        kwargs.setdefault("account_profile", self.account_profile)
        game_test.prepare(self.template, self.stage, path, **kwargs)
        return path

    def metadata(self, run_dir):
        return game_test.read_json(run_dir / "run.json")

    def private_account(self, run_dir):
        parser = configparser.ConfigParser()
        parser.read(run_dir / "client" / "sdgo" / "gametest" / "private" / "account.ini")
        return parser

    def event(self, metadata, event, case="", status="running", message="", pid=5100):
        return {
            "run_id": metadata["run_id"],
            "role": "client",
            "event": event,
            "case": case,
            "status": status,
            "message": message,
            "host_key": game_test.HOST_KEY,
            "pid": pid,
            "image_path": metadata["client"]["game"],
            "duration_ms": 1,
        }

    def write_events(
        self,
        run_dir,
        cases=(),
        *,
        include_end=False,
        pid=5100,
        end_status="pass",
        selection=None,
    ):
        metadata = self.metadata(run_dir)
        events = [self.event(metadata, "run_begin", pid=pid)]
        if selection is not None:
            observation = self.event(
                metadata,
                "observation",
                status="info",
                message="native automatic server/channel selection observed",
                pid=pid,
            )
            observation.update(selection)
            events.append(observation)
        for case_id in cases:
            events.append(self.event(metadata, "case_end", case_id, "pass", pid=pid))
        if include_end:
            events.append(self.event(metadata, "run_end", status=end_status, pid=pid))
        Path(metadata["client"]["events"]).write_text(
            "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )

    def set_execution(self, run_dir, pid=5100, status="completed"):
        metadata = self.metadata(run_dir)
        metadata["execution"] = {
            "started_utc": "start",
            "finished_utc": "finish",
            "entry_pid": pid,
            "game_pid": pid,
            "game_image_path": metadata["client"]["game"],
            "status": status,
            "timed_out": False,
            "error": None,
        }
        game_test.atomic_write(run_dir / "run.json", json.dumps(metadata))

    def test_prepare_clones_normal_client_and_only_overlays_observer(self):
        run_dir = self.prepare()
        client = run_dir / "client"
        self.assertEqual((client / "GOnline.exe").read_bytes(), b"normal-game")
        self.assertEqual((client / "3_SD敢达快速启动.exe").read_bytes(), b"normal-launcher")
        self.assertTrue((client / "配置文件.ini").is_file())
        self.assertEqual((client / "BGM" / "theme.ogg").read_bytes(), b"music")
        self.assertFalse((client / "old.log").exists())
        self.assertEqual((client / "d3d9.dll").read_bytes(), b"normal-loader")
        self.assertEqual((client / "sdgo" / "core.dll").read_bytes(), b"normal-core")
        self.assertEqual(
            (client / "sdgo" / "mods" / "match_ai" / "match_ai.dll").read_bytes(),
            b"normal-match-ai",
        )
        self.assertEqual(
            (client / "sdgo" / "mods" / "other_mod" / "other_mod.dll").read_bytes(),
            b"other-addon",
        )
        self.assertEqual(
            (client / "sdgo" / "mods" / "game_test_client" / "game_test_client.dll").read_bytes(),
            b"actual-observer",
        )
        self.assertFalse((client / "sdgo" / "gametest" / "events.jsonl").exists())
        self.assertEqual((self.template / "sdgo" / "gametest" / "events.jsonl").read_text(), "stale")

    def test_prepare_merges_required_symbols_and_enables_needed_switches(self):
        run_dir = self.prepare("symbols")
        symbols = game_test.read_ini(
            run_dir / "client" / "sdgo" / "symbols" / f"{game_test.HOST_KEY}.ini"
        )
        self.assertEqual(symbols["test.client_init"]["rva"], "init")
        self.assertEqual(symbols["match.runtime"]["rva"], "new-match")
        self.assertEqual(symbols["other.symbol"]["rva"], "kept")
        settings = game_test.read_ini(run_dir / "client" / "sdgo" / "sdgo.ini")
        self.assertEqual(settings["mods"]["game_test_client"], "1")
        self.assertEqual(settings["mods"]["match_ai"], "1")
        self.assertEqual(settings["mods"]["other_mod"], "1")
        self.assertEqual(settings["features"]["pvp-ai"], "1")
        observer = game_test.read_ini(
            run_dir / "client" / "sdgo" / "mods" / "game_test_client" / "mod.ini"
        )
        self.assertEqual(observer["mod"]["enabled"], "1")

    def test_staged_production_match_ai_dll_replaces_only_owned_copy(self):
        staged = self.stage / "client" / "sdgo" / "mods" / "match_ai"
        staged.mkdir()
        (staged / "match_ai.dll").write_bytes(b"current-production-match-ai")
        original_manifest = (
            self.template / "sdgo" / "mods" / "match_ai" / "mod.ini"
        ).read_bytes()

        run_dir = self.prepare("staged-target")
        copied = run_dir / "client" / "sdgo" / "mods" / "match_ai"
        self.assertEqual(
            (copied / "match_ai.dll").read_bytes(),
            b"current-production-match-ai",
        )
        self.assertEqual((copied / "mod.ini").read_bytes(), original_manifest)
        self.assertEqual(
            (self.template / "sdgo" / "mods" / "match_ai" / "match_ai.dll").read_bytes(),
            b"normal-match-ai",
        )
        hashes = self.metadata(run_dir)["client"]["immutable_hashes"]
        self.assertEqual(
            hashes["sdgo/mods/match_ai/match_ai.dll"],
            game_test.sha256(copied / "match_ai.dll"),
        )

    def test_two_prepares_reuse_persistent_account_without_public_credentials(self):
        first = self.prepare("private-first")
        second = self.prepare("private-second")
        first_account = self.private_account(first)
        second_account = self.private_account(second)
        username = first_account["account"]["username"]
        password = first_account["account"]["password"]
        self.assertEqual(username, "ReuseA1234")
        self.assertEqual(password, "ReusePass123")
        self.assertEqual(second_account["account"]["username"], username)
        self.assertEqual(second_account["account"]["password"], password)
        for run_dir in (first, second):
            account = self.private_account(run_dir)
            self.assertEqual(account["registration"]["retry_count"], "1")
            self.assertEqual(account["registration"]["expect_first_fail"], "0")
            public = (run_dir / "run.json").read_text()
            run_ini = (run_dir / "client" / "sdgo" / "gametest" / "run.ini").read_text()
            self.assertNotIn(username, public)
            self.assertNotIn(password, public)
            self.assertNotIn(username, run_ini)
            self.assertNotIn(password, run_ini)
            self.assertIn("account_path=private/account.ini", run_ini)

    def test_same_persistent_account_blocks_concurrent_runs_across_run_directories(self):
        first = self.prepare("account-lock-first")
        second = self.prepare("account-lock-second")
        first_metadata = self.metadata(first)
        second_metadata = self.metadata(second)
        first_root = Path(first_metadata["client"]["root"])
        second_root = Path(second_metadata["client"]["root"])
        first_lock = game_test.validate_private_account(first_root, first_metadata["plan"])
        second_lock = game_test.validate_private_account(second_root, second_metadata["plan"])
        self.assertEqual(first_lock, second_lock)

        lock, token = game_test.acquire_account_lock(first_root, first_metadata["plan"])
        try:
            with mock.patch.object(game_test.subprocess, "Popen") as popen:
                with self.assertRaisesRegex(game_test.GameTestError, "persistent account lock"):
                    game_test.run_prepared(second, timeout=0, launch_timeout=0)
            popen.assert_not_called()
            self.assertFalse((second / ".run.lock").exists())
            self.assertTrue(lock.is_file())
        finally:
            game_test.release_owned_lock(lock, token)
        self.assertFalse(lock.exists())

    def test_missing_profile_blocks_without_silent_account_generation(self):
        missing = self.root / "missing-profile" / "account.ini"
        run_dir = self.root / "missing-profile-run"
        with self.assertRaisesRegex(game_test.GameTestError, "profile is missing"):
            game_test.prepare(
                self.template,
                self.stage,
                run_dir,
                account_profile=missing,
            )
        self.assertFalse(missing.exists())
        self.assertFalse(run_dir.exists())

    def test_existing_pending_first_login_profile_reuses_identity_with_safe_retry(self):
        profile = self.root / "pending-profile" / "account.ini"
        profile.parent.mkdir()
        profile.write_text(
            """[account]
username=Pending123
password=PendingPass1

[registration]
state=pending_first_login
"""
        )
        first = self.root / "pending-retry-first"
        second = self.root / "pending-retry-second"
        with mock.patch.object(
            game_test,
            "random_account",
            side_effect=AssertionError("existing pending profile must never generate a new identity"),
        ):
            game_test.prepare(
                self.template,
                self.stage,
                first,
                account_profile=profile,
            )
            game_test.prepare(
                self.template,
                self.stage,
                second,
                account_profile=profile,
            )

        for run_dir in (first, second):
            account = self.private_account(run_dir)
            metadata = self.metadata(run_dir)
            self.assertEqual(account["account"]["username"], "Pending123")
            self.assertEqual(account["account"]["password"], "PendingPass1")
            self.assertEqual(account["registration"]["retry_count"], "2")
            self.assertEqual(account["registration"]["expect_first_fail"], "0")
            self.assertEqual(metadata["plan"]["retry_count"], 2)
            self.assertFalse(metadata["plan"]["expect_first_fail"])
        persistent = game_test.load_account_profile(profile)
        self.assertEqual(persistent["account"]["username"], "Pending123")
        self.assertEqual(persistent["account"]["password"], "PendingPass1")

    def test_explicit_first_creation_is_the_only_generation_path(self):
        profile = self.root / "first-create" / "account.ini"
        run_dir = self.root / "first-create-run"
        game_test.prepare(
            self.template,
            self.stage,
            run_dir,
            account_profile=profile,
            create_test_account=True,
        )
        self.assertTrue(profile.is_file())
        persistent = game_test.load_account_profile(profile)
        owned = self.private_account(run_dir)
        self.assertEqual(owned["account"]["username"], persistent["account"]["username"])
        self.assertEqual(owned["account"]["password"], persistent["account"]["password"])
        self.assertEqual(owned["registration"]["retry_count"], "2")
        self.assertEqual(owned["registration"]["expect_first_fail"], "1")
        with self.assertRaisesRegex(game_test.GameTestError, "refusing to overwrite"):
            game_test.prepare(
                self.template,
                self.stage,
                self.root / "first-create-again",
                account_profile=profile,
                create_test_account=True,
            )

    def test_prepare_records_real_normal_flow_without_server_fixture(self):
        run_dir = self.prepare("plan")
        metadata = self.metadata(run_dir)
        self.assertEqual(metadata["plan"]["target_host"], "127.0.0.1")
        self.assertEqual(metadata["plan"]["login_port"], 5001)
        self.assertEqual(metadata["plan"]["mode_sequence"], "1")
        self.assertEqual(metadata["plan"]["server_channel_selection"], "native_automatic")
        self.assertFalse((run_dir / "server").exists())
        run_ini = game_test.read_ini(run_dir / "client" / "sdgo" / "gametest" / "run.ini")
        self.assertEqual(run_ini["run"]["normal_flow"], "1")
        self.assertEqual(run_ini["run"]["server_channel_selection"], "native_automatic")
        self.assertEqual(run_ini["run"]["required_cases"], ",".join(game_test.FLOW_CASES))
        self.assertEqual(run_ini["run"]["process_entry"], "GOnline.exe")
        self.assertRegex(run_ini["run"]["title_nonce"], r"^gt[0-9a-f]{4}$")
        self.assertEqual(
            run_ini["run"]["room_title"],
            f"{run_ini['run']['title_nonce']} !ai e2 a1",
        )
        self.assertEqual(run_ini["run"]["expected_ai"], "3")
        self.assertEqual(run_ini["run"]["auto_nickname"], "account_name")
        self.assertEqual(run_ini["run"]["exit_tutorial"], "1")
        self.assertEqual(run_ini["run"]["first_unit_draw"], "1")
        self.assertEqual(run_ini["run"]["acknowledge_game_windows"], "1")
        self.assertEqual(
            game_test.FLOW_CASES[:3],
            ("framework.bootstrap", "flow.login", "flow.onboarding"),
        )
        self.assertLessEqual(len(run_ini["run"]["room_title"].encode("ascii")), 20)
        self.assertRegex(metadata["source"]["runner_sha256"], r"^[0-9a-f]{64}$")

    def test_deprecated_selection_values_are_record_only_and_reports_use_native_events(self):
        help_output = io.StringIO()
        with contextlib.redirect_stdout(help_output), self.assertRaises(SystemExit) as stopped:
            game_test.parse_args(["prepare", "--help"])
        self.assertEqual(stopped.exception.code, 0)
        help_text = " ".join(help_output.getvalue().split())
        self.assertIn("deprecated compatibility value, recorded only", help_text)
        self.assertIn("native automatic server selection", help_text)
        self.assertIn("native automatic channel selection", help_text)

        run_dir = self.prepare(
            "native-selection",
            target_server="recorded-server",
            target_channel="recorded-channel",
        )
        metadata = self.metadata(run_dir)
        self.assertEqual(metadata["plan"]["target_server"], "recorded-server")
        self.assertEqual(metadata["plan"]["target_channel"], "recorded-channel")
        self.assertEqual(metadata["plan"]["server_channel_selection"], "native_automatic")
        self.set_execution(run_dir)

        self.write_events(run_dir, game_test.FLOW_CASES, include_end=True)
        unobserved = game_test.build_report(run_dir)["layers"]["native"][
            "server_channel_selection"
        ]
        self.assertEqual(unobserved["policy"], "native_automatic")
        self.assertEqual(unobserved["source"], "native_events")
        self.assertEqual(unobserved["status"], "unobserved")
        self.assertIsNone(unobserved["observed_server_id"])
        self.assertIsNone(unobserved["observed_channel_id"])
        self.assertNotIn("recorded-server", json.dumps(unobserved))
        self.assertNotIn("recorded-channel", json.dumps(unobserved))

        self.write_events(
            run_dir,
            game_test.FLOW_CASES,
            include_end=True,
            selection={"server_id": 4, "channel_id": 7},
        )
        observed = game_test.build_report(run_dir)["layers"]["native"][
            "server_channel_selection"
        ]
        self.assertEqual(observed["status"], "observed")
        self.assertEqual(observed["observed_server_id"], 4)
        self.assertEqual(observed["observed_channel_id"], 7)
        self.assertEqual(observed["observation_event_count"], 1)

    def test_mode_matrix_contract_and_ai_count_validation(self):
        self.assertEqual(game_test.required_flow_cases("3"), game_test.FLOW_CASES)
        multi = game_test.required_flow_cases("1,3,6")
        self.assertEqual(multi[:3], game_test.FLOW_CASES[:3])
        self.assertEqual(
            multi[3:],
            tuple(
                f"pvp.mode{mode}.{step}"
                for mode in (1, 3, 6)
                for step in game_test.MODE_FLOW_STEPS
            ),
        )
        for invalid in ("", "1, 3", "2", "01", "10", "1,1", "1,"):
            with self.subTest(mode_sequence=invalid):
                with self.assertRaises(game_test.GameTestError):
                    game_test.required_flow_cases(invalid)

        one = game_test.make_plan("127.0.0.1", 5001, "1", "1", "1", 1, False, 1, None)
        seven = game_test.make_plan("127.0.0.1", 5001, "1", "1", "9", 1, False, 7, None)
        self.assertRegex(one["room_title"], r"^gt[0-9a-f]{4} !ai e1 a0$")
        self.assertRegex(seven["room_title"], r"^gt[0-9a-f]{4} !ai e4 a3$")
        for invalid in (0, 8):
            with self.subTest(expected_ai=invalid):
                with self.assertRaisesRegex(game_test.GameTestError, "between 1 and 7"):
                    game_test.make_plan(
                        "127.0.0.1", 5001, "1", "1", "1", 1, False, invalid, None
                    )

    def test_multimode_receipt_requires_each_modes_complete_sequence(self):
        run_dir = self.prepare("multi-mode", mode_sequence="1,3", expected_ai=7)
        metadata = self.metadata(run_dir)
        required = game_test.required_flow_cases("1,3")
        run_ini = game_test.read_ini(
            run_dir / "client" / "sdgo" / "gametest" / "run.ini"
        )
        self.assertEqual(run_ini["run"]["required_cases"], ",".join(required))
        self.assertRegex(metadata["plan"]["room_title"], r"^gt[0-9a-f]{4} !ai e4 a3$")

        self.set_execution(run_dir, status="running")
        self.write_events(run_dir, required[:-1], include_end=False)
        pending = game_test.build_report(run_dir)
        self.assertEqual(pending["status"], "pending")
        self.assertIn(required[-1], " ".join(pending["layers"]["native"]["pending"]))

        self.write_events(run_dir, required, include_end=True)
        self.set_execution(run_dir)
        complete = game_test.build_report(run_dir)
        self.assertEqual(complete["status"], "pass")
        junit = ET.parse(run_dir / "receipts" / "junit.xml").getroot().find("testsuite")
        self.assertIsNotNone(junit)
        self.assertEqual(junit.get("tests"), str(len(required)))
        self.assertEqual(
            [case.get("name") for case in junit.findall("testcase")],
            list(required),
        )

    def test_prepare_refuses_existing_destination_and_preserves_original(self):
        destination = self.root / "existing"
        destination.mkdir()
        marker = destination / "keep.txt"
        marker.write_text("keep")
        original_core = (self.template / "sdgo" / "core.dll").read_bytes()
        with self.assertRaises(game_test.GameTestError):
            game_test.prepare(
                self.template,
                self.stage,
                destination,
                account_profile=self.account_profile,
            )
        self.assertEqual(marker.read_text(), "keep")
        self.assertEqual((self.template / "sdgo" / "core.dll").read_bytes(), original_core)

    def test_hashes_bind_game_core_symbols_mods_and_assets_but_not_writable_config(self):
        run_dir = self.prepare("hashes")
        hashes = self.metadata(run_dir)["client"]["immutable_hashes"]
        self.assertIn("GOnline.exe", hashes)
        self.assertIn("d3d9.dll", hashes)
        self.assertIn("GPackData.zpk", hashes)
        self.assertIn("sdgo/core.dll", hashes)
        self.assertIn(f"sdgo/symbols/{game_test.HOST_KEY}.ini", hashes)
        self.assertIn("sdgo/mods/game_test_client/game_test_client.dll", hashes)
        self.assertIn("sdgo/mods/match_ai/match_ai.dll", hashes)
        self.assertNotIn("config.xml", hashes)
        self.assertNotIn("配置文件.ini", hashes)
        (run_dir / "client" / "config.xml").write_text("<changed/>")
        game_test.verify_hashes(self.metadata(run_dir))

    def test_tampered_immutable_payloads_fail_before_process_launch(self):
        targets = (
            Path("client/GOnline.exe"),
            Path("client/sdgo/core.dll"),
            Path("client/sdgo/symbols") / f"{game_test.HOST_KEY}.ini",
            Path("client/sdgo/gametest/run.ini"),
        )
        for index, target in enumerate(targets):
            with self.subTest(target=target):
                run_dir = self.prepare(f"tamper-{index}")
                (run_dir / target).write_bytes(b"tampered")
                with mock.patch.object(game_test.subprocess, "Popen") as popen:
                    with self.assertRaises(game_test.GameTestError):
                        game_test.run_prepared(run_dir, timeout=0, launch_timeout=0)
                popen.assert_not_called()

    def test_run_rejects_public_plan_metadata_weakening(self):
        run_dir = self.prepare("plan-tamper")
        metadata = self.metadata(run_dir)
        metadata["plan"]["retry_count"] = 2
        game_test.atomic_write(run_dir / "run.json", json.dumps(metadata))
        with mock.patch.object(game_test.subprocess, "Popen") as popen:
            with self.assertRaisesRegex(game_test.GameTestError, "disagrees"):
                game_test.run_prepared(run_dir, timeout=0, launch_timeout=0)
        popen.assert_not_called()

    def test_default_run_starts_copied_gonline_with_normal_endpoint_and_private_env(self):
        run_dir = self.prepare("direct")
        metadata = self.metadata(run_dir)
        captured = {}
        process = FakeProcess()

        def launch(arguments, **kwargs):
            captured["arguments"] = arguments
            captured["kwargs"] = kwargs
            return process

        def finish_immediately(path):
            self.write_events(run_dir, game_test.FLOW_CASES, include_end=True, pid=process.pid)
            return True

        with mock.patch.object(game_test.subprocess, "Popen", side_effect=launch), mock.patch.object(
            game_test, "NamedStartupEvent", FakeStartupEvent
        ), mock.patch.object(game_test, "has_run_end", side_effect=finish_immediately):
            report = game_test.run_prepared(run_dir, timeout=1, launch_timeout=0)

        self.assertEqual(
            captured["arguments"],
            [metadata["client"]["game"], "127.0.0.1", "5001"],
        )
        environment = captured["kwargs"]["env"]
        self.assertEqual(environment["__COMPAT_LAYER"], "RunAsInvoker")
        self.assertEqual(environment["SDGO_GAME_TEST_RUN"], metadata["run_id"])
        self.assertEqual(captured["kwargs"]["startupinfo"].wShowWindow, 4)
        self.assertTrue(environment["SDGO_GAME_TEST_ACCOUNT"].endswith("private\\account.ini"))
        self.assertNotIn(self.private_account(run_dir)["account"]["password"], str(captured))
        self.assertTrue(process.killed)
        self.assertEqual(report["status"], "pass")

    def test_optional_launcher_accepts_only_owned_descendant_at_exact_capsule_path(self):
        run_dir = self.prepare("launcher", launcher_path="3_SD敢达快速启动.exe")
        metadata = self.metadata(run_dir)
        entry_pid = 7000
        rows = [
            {"pid": 47616, "parent_pid": 1, "image_path": metadata["client"]["game"]},
            {"pid": 7001, "parent_pid": entry_pid, "image_path": r"E:\other\GOnline.exe"},
            {"pid": 7002, "parent_pid": entry_pid, "image_path": metadata["client"]["game"]},
        ]
        selected = game_test.select_owned_game_process(
            rows,
            entry_pid,
            Path(metadata["client"]["game"]),
        )
        self.assertEqual(selected["pid"], 7002)
        self.assertNotEqual(selected["pid"], 47616)

    def test_report_is_pending_until_real_flow_completes(self):
        run_dir = self.prepare("pending")
        report = game_test.build_report(run_dir)
        self.assertEqual(report["status"], "pending")
        self.set_execution(run_dir, status="running")
        self.write_events(run_dir, game_test.FLOW_CASES[:3], include_end=False)
        report = game_test.build_report(run_dir)
        self.assertEqual(report["status"], "pending")
        self.assertIn("missing cases", " ".join(report["layers"]["native"]["pending"]))

    def test_unexpected_exit_with_partial_native_evidence_is_a_failure(self):
        run_dir = self.prepare("unexpected-exit")
        completed_cases = game_test.FLOW_CASES[:4]
        self.set_execution(run_dir, status="game_exited")
        self.write_events(run_dir, completed_cases, include_end=False)

        report = game_test.build_report(run_dir)
        self.assertEqual(report["status"], "fail")
        self.assertEqual(report["layers"]["launch"]["status"], "fail")
        self.assertIn(
            "exited before GameTest completed",
            " ".join(report["layers"]["launch"]["errors"]),
        )
        self.assertEqual(report["layers"]["native"]["status"], "pending")
        self.assertEqual(
            list(report["layers"]["native"]["cases"]),
            list(completed_cases),
        )
        self.assertIn(
            game_test.FLOW_CASES[-1],
            " ".join(report["layers"]["native"]["pending"]),
        )
        suite = ET.parse(run_dir / "receipts" / "junit.xml").getroot().find("testsuite")
        self.assertEqual(suite.get("tests"), str(len(game_test.FLOW_CASES) + 1))
        self.assertEqual(suite.get("failures"), "1")
        framework = suite.findall("testcase")[-1]
        self.assertEqual(framework.get("name"), "framework.infrastructure")
        self.assertIn(
            "exited before GameTest completed",
            framework.find("failure").get("message"),
        )

    def test_only_not_started_or_running_execution_remains_pending(self):
        running = self.prepare("still-running")
        self.set_execution(running, status="running")
        running_report = game_test.build_report(running)
        self.assertEqual(running_report["status"], "pending")
        self.assertEqual(running_report["layers"]["launch"]["status"], "pending")

        for status in ("completed", "timeout", "launch_error", "launch_timeout"):
            with self.subTest(status=status):
                run_dir = self.prepare(f"terminal-{status}")
                self.set_execution(run_dir, status=status)
                metadata = self.metadata(run_dir)
                metadata["execution"]["error"] = f"fixture {status}"
                if status in {"launch_error", "launch_timeout"}:
                    metadata["execution"]["game_pid"] = None
                    metadata["execution"]["game_image_path"] = None
                game_test.atomic_write(run_dir / "run.json", json.dumps(metadata))
                report = game_test.build_report(run_dir)
                self.assertEqual(report["status"], "fail")
                self.assertEqual(report["layers"]["launch"]["status"], "fail")
                if status == "completed":
                    self.assertIn(
                        "completed without the required native GameTest evidence",
                        " ".join(report["layers"]["launch"]["errors"]),
                    )
                else:
                    self.assertIn(f"fixture {status}", report["layers"]["launch"]["errors"])

    def test_junit_fails_for_source_and_terminal_evidence_errors_without_native_case_failure(self):
        source_failure = self.prepare("junit-source-failure")
        self.set_execution(source_failure)
        self.write_events(source_failure, game_test.FLOW_CASES, include_end=True)
        (source_failure / "client" / "sdgo" / "core.dll").write_bytes(b"tampered")
        source_report = game_test.build_report(source_failure)
        self.assertEqual(source_report["status"], "fail")
        self.assertEqual(source_report["layers"]["native"]["status"], "pass")
        source_suite = ET.parse(
            source_failure / "receipts" / "junit.xml"
        ).getroot().find("testsuite")
        self.assertEqual(source_suite.get("failures"), "1")
        self.assertEqual(source_suite.get("tests"), str(len(game_test.FLOW_CASES) + 1))
        self.assertEqual(
            source_suite.findall("testcase")[-1].get("name"),
            "framework.infrastructure",
        )
        self.assertIn(
            "source:",
            source_suite.findall("testcase")[-1].find("failure").get("message"),
        )

        missing_end = self.prepare("junit-missing-run-end")
        self.set_execution(missing_end)
        self.write_events(missing_end, game_test.FLOW_CASES, include_end=False)
        missing_end_report = game_test.build_report(missing_end)
        self.assertEqual(missing_end_report["status"], "fail")
        self.assertEqual(missing_end_report["layers"]["native"]["status"], "pending")
        missing_suite = ET.parse(
            missing_end / "receipts" / "junit.xml"
        ).getroot().find("testsuite")
        self.assertEqual(missing_suite.get("failures"), "1")
        self.assertEqual(missing_suite.get("tests"), str(len(game_test.FLOW_CASES) + 1))
        self.assertIn(
            "completed without the required native GameTest evidence",
            missing_suite.findall("testcase")[-1].find("failure").get("message"),
        )

    def test_junit_native_case_failure_keeps_existing_case_count(self):
        run_dir = self.prepare("junit-native-case-failure")
        self.set_execution(run_dir)
        self.write_events(
            run_dir,
            game_test.FLOW_CASES,
            include_end=True,
            end_status="fail",
        )
        events_path = Path(self.metadata(run_dir)["client"]["events"])
        events = [
            json.loads(line)
            for line in events_path.read_text(encoding="utf-8").splitlines()
        ]
        failed_case = next(event for event in events if event["event"] == "case_end")
        failed_case["status"] = "fail"
        failed_case["message"] = "fixture native case failure"
        events_path.write_text(
            "".join(json.dumps(event) + "\n" for event in events),
            encoding="utf-8",
        )

        report = game_test.build_report(run_dir)
        self.assertEqual(report["status"], "fail")
        suite = ET.parse(run_dir / "receipts" / "junit.xml").getroot().find("testsuite")
        self.assertEqual(suite.get("tests"), str(len(game_test.FLOW_CASES)))
        self.assertEqual(suite.get("failures"), "1")
        self.assertNotIn(
            "framework.infrastructure",
            [case.get("name") for case in suite.findall("testcase")],
        )

    def test_complete_real_flow_receipt_passes(self):
        run_dir = self.prepare("pass")
        self.set_execution(run_dir)
        self.write_events(run_dir, game_test.FLOW_CASES, include_end=True)
        report = game_test.build_report(run_dir)
        self.assertEqual(report["status"], "pass")
        self.assertEqual(report["layers"]["source"]["status"], "bound")
        self.assertTrue((run_dir / "receipts" / "report.json").is_file())
        self.assertTrue((run_dir / "receipts" / "junit.xml").is_file())
        suite = ET.parse(run_dir / "receipts" / "junit.xml").getroot().find("testsuite")
        self.assertEqual(suite.get("tests"), str(len(game_test.FLOW_CASES)))
        self.assertEqual(suite.get("failures"), "0")
        self.assertNotIn(
            "framework.infrastructure",
            [case.get("name") for case in suite.findall("testcase")],
        )

    def test_wrong_pid_image_malformed_and_premature_pass_fail(self):
        scenarios = ("pid", "image", "malformed", "premature")
        for scenario in scenarios:
            with self.subTest(scenario=scenario):
                run_dir = self.prepare(f"bad-{scenario}")
                self.set_execution(run_dir)
                metadata = self.metadata(run_dir)
                path = Path(metadata["client"]["events"])
                if scenario == "malformed":
                    path.write_bytes(b'{"run_id":')
                elif scenario == "premature":
                    self.write_events(run_dir, game_test.FLOW_CASES[:2], include_end=True)
                else:
                    event = self.event(metadata, "run_begin")
                    if scenario == "pid":
                        event["pid"] = 9999
                    else:
                        event["image_path"] = str(self.root / "foreign" / "GOnline.exe")
                    path.write_text(json.dumps(event) + "\n")
                report = game_test.build_report(run_dir)
                self.assertEqual(report["status"], "fail")

    def test_run_refuses_stale_events_execution_and_existing_lock(self):
        for kind in ("events", "execution", "lock"):
            with self.subTest(kind=kind):
                run_dir = self.prepare(f"stale-{kind}")
                metadata = self.metadata(run_dir)
                if kind == "events":
                    Path(metadata["client"]["events"]).write_bytes(b"")
                elif kind == "execution":
                    metadata["execution"] = {"status": "failed"}
                    game_test.atomic_write(run_dir / "run.json", json.dumps(metadata))
                else:
                    (run_dir / ".run.lock").write_text('{"token":"other","pid":47616}')
                with mock.patch.object(game_test.subprocess, "Popen") as popen:
                    with self.assertRaises(game_test.GameTestError):
                        game_test.run_prepared(run_dir, timeout=0, launch_timeout=0)
                popen.assert_not_called()
                if kind == "lock":
                    self.assertEqual(
                        json.loads((run_dir / ".run.lock").read_text())["token"],
                        "other",
                    )


if __name__ == "__main__":
    unittest.main()
