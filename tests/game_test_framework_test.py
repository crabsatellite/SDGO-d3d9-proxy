"""OFFLINE controller fixture for the native GameTest ABI.

This executes runner.h in a small synthetic Windows process. It is not game,
client, server, room, asset, rendering, or gameplay evidence.
"""
from __future__ import annotations

import argparse
import configparser
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import uuid


HOST_KEY = "BattleServer-548A891D-0046C000"
HOST_EXECUTABLE: Path | None = None
SCENARIOS = {
    "valid": {
        "exit": 0,
        "case_status": "pass",
        "assert_status": "pass",
        "assert_message": "WAIT ticks preserved state before PASS",
        "run_status": "pass",
    },
    "pass_no_assert": {
        "case_status": "fail",
        "assert_status": "fail",
        "assert_message": "case returned PASS without assertions",
        "run_status": "fail",
    },
    "failed_assert_pass": {
        "case_status": "fail",
        "assert_status": "fail",
        "assert_message": "fixture failed assertion survives PASS",
        "run_status": "fail",
    },
    "timeout_wait": {
        "case_status": "fail",
        "assert_status": "fail",
        "assert_message": "native case timed out",
        "run_status": "fail",
    },
    "skip": {
        "case_status": "skip",
        "run_status": "fail",
    },
    "seh": {
        "case_status": "fail",
        "assert_status": "fail",
        "assert_message": "native exception 0xE0421001",
        "run_status": "fail",
    },
}

DISCOVERY_FAILURE_SCENARIOS = {
    "missing_required": "required native case is not registered: missing.case",
    "duplicate_case": "invalid or duplicate native case",
}


def read_events(path: Path) -> list[dict]:
    events = []
    for line_number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        try:
            event = json.loads(raw)
        except json.JSONDecodeError as error:
            raise AssertionError(f"invalid JSONL line {line_number}: {error}") from error
        if not isinstance(event, dict):
            raise AssertionError(f"event line {line_number} is not an object")
        events.append(event)
    return events


class NativeControllerAbiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if HOST_EXECUTABLE is None:
            raise unittest.SkipTest("pass --host with the built game_test_framework_host executable")
        cls.host = HOST_EXECUTABLE.resolve()
        if not cls.host.is_file():
            raise AssertionError(f"native controller fixture host not found: {cls.host}")

    def run_scenario(
        self,
        scenario: str,
        *,
        required_cases: str | None = None,
    ) -> tuple[subprocess.CompletedProcess, str, list[dict]]:
        with tempfile.TemporaryDirectory(prefix=f"sdgo-gametest-{scenario}-") as temporary:
            root = Path(temporary)
            host = root / self.host.name
            shutil.copy2(self.host, host)
            game_test_dir = root / "sdgo" / "gametest"
            game_test_dir.mkdir(parents=True)
            run_id = str(uuid.uuid4())
            config = configparser.ConfigParser(interpolation=None)
            config["run"] = {"id": run_id, "suite": "pvp-ai", "role": "server"}
            if required_cases is not None:
                config["run"]["required_cases"] = required_cases
            with (game_test_dir / "run.ini").open("w", encoding="utf-8", newline="\n") as stream:
                config.write(stream, space_around_delimiters=False)
            environment = os.environ.copy()
            environment.update(
                {
                    "SDGO_GAME_TEST_RUN": run_id,
                    "SDGO_GAME_TEST_ROLE": "server",
                    "SDGO_GAME_TEST_FIXTURE_SCENARIO": scenario,
                    "__COMPAT_LAYER": "RunAsInvoker",
                }
            )
            completed = subprocess.run(
                [str(host)],
                cwd=root,
                env=environment,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                timeout=10,
                check=False,
            )
            events_path = game_test_dir / "events.jsonl"
            self.assertTrue(events_path.is_file(), f"{scenario}: controller created no event receipt")
            return completed, run_id, read_events(events_path)

    def test_controller_contract_scenarios(self):
        for scenario, expected in SCENARIOS.items():
            with self.subTest(scenario=scenario):
                completed, run_id, events = self.run_scenario(scenario)
                if expected.get("exit") == 0:
                    self.assertEqual(completed.returncode, 0)
                else:
                    self.assertNotEqual(completed.returncode, 0)
                self.assertGreaterEqual(len(events), 4)
                self.assertTrue(all(event["run_id"] == run_id for event in events))
                self.assertTrue(all(event["role"] == "server" for event in events))
                self.assertTrue(all(event["host_key"] == HOST_KEY for event in events))
                self.assertEqual(len({event["pid"] for event in events}), 1)
                self.assertEqual(events[0]["event"], "run_begin")
                self.assertEqual(events[-1]["event"], "run_end")
                self.assertEqual(events[-1]["status"], expected["run_status"])
                self.assertTrue(
                    any(
                        event["event"] == "observation"
                        and "OFFLINE controller fixture" in event["message"]
                        for event in events
                    )
                )
                case_end = [event for event in events if event["event"] == "case_end"]
                self.assertEqual(len(case_end), 1)
                self.assertEqual(case_end[0]["case"], "framework.bootstrap")
                self.assertEqual(case_end[0]["status"], expected["case_status"])
                if "assert_status" in expected:
                    assertions = [event for event in events if event["event"] == "assert"]
                    self.assertTrue(
                        any(
                            event["status"] == expected["assert_status"]
                            and expected["assert_message"] in event["message"]
                            for event in assertions
                        )
                    )
                pass_ends = [
                    event for event in events if event["event"] == "run_end" and event["status"] == "pass"
                ]
                self.assertEqual(bool(pass_ends), scenario == "valid")

    def test_required_and_duplicate_cases_fail_discovery(self):
        for scenario, message in DISCOVERY_FAILURE_SCENARIOS.items():
            with self.subTest(scenario=scenario):
                required = "framework.bootstrap,missing.case" if scenario == "missing_required" else None
                completed, run_id, events = self.run_scenario(
                    scenario,
                    required_cases=required,
                )
                self.assertNotEqual(completed.returncode, 0)
                self.assertTrue(all(event["run_id"] == run_id for event in events))
                self.assertEqual(events[0]["event"], "run_begin")
                self.assertEqual(events[-1]["event"], "run_end")
                self.assertEqual(events[-1]["status"], "fail")
                self.assertFalse(any(event["event"] == "case_begin" for event in events))
                self.assertFalse(any(event["event"] == "case_end" for event in events))
                self.assertTrue(
                    any(
                        event["event"] == "assert"
                        and event["status"] == "fail"
                        and message in event["message"]
                        for event in events
                    )
                )

    def test_failed_dependency_stops_later_cases(self):
        completed, _, events = self.run_scenario("dependency_short_circuit")
        self.assertNotEqual(completed.returncode, 0)
        begun = [event["case"] for event in events if event["event"] == "case_begin"]
        ended = [event for event in events if event["event"] == "case_end"]
        self.assertEqual(begun, ["framework.bootstrap"])
        self.assertEqual(len(ended), 1)
        self.assertEqual(ended[0]["case"], "framework.bootstrap")
        self.assertEqual(ended[0]["status"], "fail")
        self.assertFalse(
            any("DEPENDENT CASE EXECUTED" in event["message"] for event in events)
        )
        self.assertEqual(events[-1]["event"], "run_end")
        self.assertEqual(events[-1]["status"], "fail")

    def test_failed_assertion_during_wait_survives_later_pass(self):
        completed, _, events = self.run_scenario("failed_assert_wait_pass")
        self.assertNotEqual(completed.returncode, 0)
        assertions = [event for event in events if event["event"] == "assert"]
        self.assertTrue(
            any(
                event["status"] == "fail"
                and event["message"] == "fixture failed assertion during WAIT"
                for event in assertions
            )
        )
        self.assertTrue(
            any(
                event["status"] == "pass"
                and event["message"] == "later PASS cannot erase the WAIT assertion failure"
                for event in assertions
            )
        )
        case_end = [event for event in events if event["event"] == "case_end"]
        self.assertEqual(len(case_end), 1)
        self.assertEqual(case_end[0]["status"], "fail")
        self.assertEqual(events[-1]["event"], "run_end")
        self.assertEqual(events[-1]["status"], "fail")

    def test_ordinary_environment_does_not_run(self):
        with tempfile.TemporaryDirectory(prefix="sdgo-gametest-disabled-") as temporary:
            root = Path(temporary)
            host = root / self.host.name
            shutil.copy2(self.host, host)
            environment = os.environ.copy()
            for name in (
                "SDGO_GAME_TEST_RUN",
                "SDGO_GAME_TEST_ROLE",
                "SDGO_GAME_TEST_FIXTURE_SCENARIO",
            ):
                environment.pop(name, None)
            environment["__COMPAT_LAYER"] = "RunAsInvoker"
            completed = subprocess.run(
                [str(host)],
                cwd=root,
                env=environment,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                timeout=10,
                check=False,
            )
            self.assertEqual(completed.returncode, 4)
            self.assertFalse((root / "sdgo" / "gametest" / "events.jsonl").exists())


def parse_cli(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--host", type=Path)
    return parser.parse_known_args(argv)


if __name__ == "__main__":
    options, unittest_args = parse_cli(sys.argv[1:])
    HOST_EXECUTABLE = options.host
    unittest.main(argv=[sys.argv[0], *unittest_args])
