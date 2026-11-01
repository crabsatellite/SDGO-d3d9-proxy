import configparser
import ctypes
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


ROOT = Path(__file__).resolve().parents[1]
START = ROOT / "tools" / "Start-Battle-Rewards.ps1"
STOP = ROOT / "tools" / "Stop-Battle-Rewards.ps1"
POWERSHELL = shutil.which("pwsh") or shutil.which("powershell")


FAKE_WORKER = r'''
import argparse
import json
import os
from pathlib import Path
import sys
import time

sys.path.insert(0, __REPOSITORY_ROOT__)
from owner.rewards.runtime_control import _atomic_write

parser = argparse.ArgumentParser()
parser.add_argument("--server-ini", required=True)
parser.add_argument("--runtime", required=True)
args = parser.parse_args()
runtime = Path(args.runtime)
runtime.mkdir(parents=True, exist_ok=True)
with (runtime / "worker-starts.txt").open("a", encoding="ascii") as stream:
    stream.write(str(os.getpid()) + "\n")
(runtime / "worker-args.json").write_text(json.dumps({
    "server_ini": str(Path(args.server_ini).resolve()),
    "runtime": str(runtime.resolve()),
    "argv": list(__import__("sys").argv),
}), encoding="utf-8")
scope = "12345678-1234-4abc-8def-123456789abc"
while True:
    if not (runtime / "pause-heartbeat").exists():
        rows = [
            "[wallet]",
            "SchemaVersion=1",
            "Healthy=1",
            "HeartbeatUnix=" + str(int(time.time())),
            "ScopeId=" + scope,
            "Revision=sdgo-native-gc-ledger-v1",
            "LastError=",
            "Processed=0",
            "Replays=0",
        ]
        target = runtime / "wallet-status.ini"
        _atomic_write(target, ("\r\n".join(rows) + "\r\n").encode("ascii"))
    time.sleep(0.1)
'''


FAKE_ASSISTANT = r'''
import argparse
import json
from pathlib import Path
import time

parser = argparse.ArgumentParser()
parser.add_argument("--workspace", required=True)
parser.add_argument("--runtime", required=True)
parser.add_argument("--heartbeat-timeout", required=True)
parser.add_argument("--port", required=True, type=int)
parser.add_argument("--assistant-root")
parser.add_argument("--assistant-entry")
parser.add_argument("--assistant-factory")
parser.add_argument("--assistant-workspace-factory")
parser.add_argument("--assistant-workspace")
parser.add_argument("--assistant-client")
parser.add_argument("--assistant-snapshot")
parser.add_argument("--assistant-module-binary")
parser.add_argument("--assistant-server-ini")
parser.add_argument("--assistant-option", action="append", default=[])
args, unknown = parser.parse_known_args()
workspace = Path(args.workspace)
runtime = Path(args.runtime)
workspace.mkdir(parents=True, exist_ok=True)
(workspace / ("assistant-" + str(args.port) + ".json")).write_text(json.dumps({
    **vars(args),
    "unknown": unknown,
    "policy_exists": (runtime / "reward-policy.ini").is_file(),
}), encoding="utf-8")
if args.port == 42001:
    while not (workspace / "assistant-stop").exists():
        time.sleep(0.1)
'''


def process_exists(pid):
    handle = ctypes.windll.kernel32.OpenProcess(0x1000, False, int(pid))
    if not handle:
        return False
    try:
        code = ctypes.c_ulong()
        if not ctypes.windll.kernel32.GetExitCodeProcess(
            handle, ctypes.byref(code)
        ):
            return False
        return code.value == 259
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def terminate_pid(pid):
    handle = ctypes.windll.kernel32.OpenProcess(0x00100001, False, int(pid))
    if not handle:
        return
    try:
        ctypes.windll.kernel32.TerminateProcess(handle, 1)
        ctypes.windll.kernel32.WaitForSingleObject(handle, 5000)
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def wait_for(path, process=None, timeout=20):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process is not None and process.poll() is not None:
            stdout, stderr = process.communicate()
            raise AssertionError(
                f"process exited {process.returncode}\nstdout={stdout}\nstderr={stderr}"
            )
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {path}")


def run_script(script, arguments, **kwargs):
    command = [
        POWERSHELL,
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        str(script),
        *map(str, arguments),
    ]
    environment = dict(os.environ)
    environment["PYTHONUTF8"] = "1"
    return subprocess.run(
        command,
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=kwargs.get("timeout", 40),
    )


def run_stop_with_partial_cim(root, arguments, pid):
    wrapper = root / "stop-with-partial-cim.ps1"
    wrapper.write_text(
        r'''param(
    [Parameter(Mandatory = $true)][string]$StopScript,
    [Parameter(Mandatory = $true)][int]$StubPid,
    [Parameter(Mandatory = $true)][string]$Workspace,
    [Parameter(Mandatory = $true)][string]$Runtime,
    [Parameter(Mandatory = $true)][string]$ToolRoot
)
$ErrorActionPreference = 'Stop'
function Get-CimInstance {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)][string]$ClassName,
        [string]$Filter
    )
    [pscustomobject]@{
        ProcessId = $StubPid
        ExecutablePath = $null
        CommandLine = $null
        CreationDate = $null
    }
}
& $StopScript -Workspace $Workspace -Runtime $Runtime -ToolRoot $ToolRoot
''',
        encoding="utf-8-sig",
    )
    environment = dict(os.environ)
    environment["PYTHONUTF8"] = "1"
    return subprocess.run(
        [
            POWERSHELL,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(wrapper),
            "-StopScript",
            str(STOP),
            "-StubPid",
            str(pid),
            *map(str, arguments),
        ],
        cwd=ROOT,
        env=environment,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=40,
    )


def start_process(arguments):
    environment = dict(os.environ)
    environment["PYTHONUTF8"] = "1"
    return subprocess.Popen(
        [
            POWERSHELL,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(START),
            *map(str, arguments),
        ],
        cwd=ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def status_values(path):
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    parser.optionxform = str
    parser.read(path, encoding="ascii")
    return dict(parser["wallet"])


@unittest.skipUnless(os.name == "nt" and POWERSHELL, "Windows PowerShell required")
class RewardOwnerLauncherTests(unittest.TestCase):
    def fixture(self, root):
        tools = root / "fixture tools"
        tools.mkdir()
        worker = FAKE_WORKER.replace(
            "__REPOSITORY_ROOT__", repr(str(ROOT))
        )
        (tools / "reward_worker.py").write_text(worker, encoding="utf-8")
        (tools / "reward_assistant.py").write_text(FAKE_ASSISTANT, encoding="utf-8")
        return tools

    def common_arguments(self, root, tools, port):
        workspace = root / "owner workspace"
        runtime = root / "native runtime"
        server_ini = root / "Store Server.ini"
        server_ini.write_text("[DB StoreBilling]\n", encoding="ascii")
        assistant_root = root / "installed assistant"
        assistant_workspace = root / "assistant state"
        client = root / "client files"
        snapshot = root / "assistant snapshot"
        module = root / "operator module.dll"
        for path in (assistant_root, assistant_workspace, client, snapshot):
            path.mkdir(exist_ok=True)
        module.write_bytes(b"fixture")
        return workspace, runtime, server_ini, [
            "-Workspace", workspace,
            "-Runtime", runtime,
            "-StoreServerIni", server_ini,
            "-Port", str(port),
            "-HeartbeatTimeout", "15",
            "-AssistantRoot", assistant_root,
            "-AssistantWorkspace", assistant_workspace,
            "-AssistantEntry", "tools/custom_assistant.py",
            "-AssistantFactory", "fixture_server",
            "-AssistantWorkspaceFactory", "FixtureWorkspace",
            "-AssistantClient", client,
            "-AssistantSnapshot", snapshot,
            "-AssistantModuleBinary", module,
            "-AssistantServerIni", server_ini,
            "-AssistantOption", "label=value with spaces",
            "-Python", sys.executable,
            "-ToolRoot", tools,
        ]

    def test_start_reuse_stale_guard_and_exact_stop_leave_unrelated_process(self):
        with tempfile.TemporaryDirectory(prefix="sdgo owner launcher ") as directory:
            root = Path(directory)
            tools = self.fixture(root)
            workspace, runtime, server_ini, first_arguments = self.common_arguments(
                root, tools, 42001
            )
            first = start_process(first_arguments)
            unrelated = None
            worker_pid = None
            try:
                receipt_path = workspace / "battle-reward-wallet-worker.json"
                first_call = workspace / "assistant-42001.json"
                wait_for(receipt_path, first)
                wait_for(first_call, first)
                receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
                worker_pid = receipt["pid"]
                self.assertTrue(process_exists(worker_pid))
                self.assertEqual(
                    (runtime / "worker-starts.txt").read_text(encoding="ascii").splitlines(),
                    [str(worker_pid)],
                )
                worker_args = json.loads(
                    (runtime / "worker-args.json").read_text(encoding="utf-8")
                )
                self.assertEqual(worker_args["server_ini"], str(server_ini.resolve()))
                self.assertEqual(worker_args["runtime"], str(runtime.resolve()))
                self.assertNotIn("--install-ledger", worker_args["argv"])

                first_state = json.loads(first_call.read_text(encoding="utf-8"))
                self.assertTrue(first_state["policy_exists"])
                self.assertEqual(first_state["assistant_option"], ["label=value with spaces"])
                self.assertEqual(first_state["assistant_factory"], "fixture_server")
                self.assertEqual(
                    first_state["assistant_workspace_factory"], "FixtureWorkspace"
                )
                self.assertEqual(first_state["unknown"], [])

                pause = runtime / "pause-heartbeat"
                pause.write_text("", encoding="ascii")
                time.sleep(0.3)
                stale = status_values(runtime / "wallet-status.ini")
                stale["HeartbeatUnix"] = str(int(time.time()) - 16)
                rows = ["[wallet]", *(f"{key}={value}" for key, value in stale.items())]
                (runtime / "wallet-status.ini").write_bytes(
                    ("\r\n".join(rows) + "\r\n").encode("ascii")
                )
                _, _, _, stale_arguments = self.common_arguments(root, tools, 42002)
                rejected = run_script(START, stale_arguments)
                self.assertNotEqual(rejected.returncode, 0)
                self.assertTrue(process_exists(worker_pid))
                self.assertEqual(
                    len((runtime / "worker-starts.txt").read_text().splitlines()), 1
                )
                self.assertFalse((workspace / "assistant-42002.json").exists())

                pause.unlink()
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    current = status_values(runtime / "wallet-status.ini")
                    if int(current["HeartbeatUnix"]) >= int(time.time()) - 1:
                        break
                    time.sleep(0.05)
                else:
                    self.fail("worker heartbeat did not recover")

                reused = run_script(START, stale_arguments)
                self.assertEqual(
                    reused.returncode,
                    0,
                    f"stdout={reused.stdout}\nstderr={reused.stderr}",
                )
                second_state = json.loads(
                    (workspace / "assistant-42002.json").read_text(encoding="utf-8")
                )
                self.assertTrue(second_state["policy_exists"])
                self.assertEqual(
                    len((runtime / "worker-starts.txt").read_text().splitlines()), 1
                )
                self.assertEqual(
                    json.loads(receipt_path.read_text(encoding="utf-8"))["pid"],
                    worker_pid,
                )

                unrelated = subprocess.Popen(
                    [sys.executable, "-c", "import time; time.sleep(60)"]
                )
                stopped = run_script(STOP, [
                    "-Workspace", workspace,
                    "-Runtime", runtime,
                    "-ToolRoot", tools,
                ])
                self.assertEqual(
                    stopped.returncode,
                    0,
                    f"stdout={stopped.stdout}\nstderr={stopped.stderr}",
                )
                deadline = time.monotonic() + 5
                while process_exists(worker_pid) and time.monotonic() < deadline:
                    time.sleep(0.05)
                self.assertFalse(process_exists(worker_pid))
                self.assertIsNone(unrelated.poll())
                self.assertFalse(receipt_path.exists())
                self.assertEqual(
                    status_values(runtime / "wallet-status.ini")["Healthy"], "0"
                )

                (workspace / "assistant-stop").write_text("", encoding="ascii")
                stdout, stderr = first.communicate(timeout=15)
                self.assertEqual(
                    first.returncode,
                    0,
                    f"stdout={stdout}\nstderr={stderr}",
                )
            finally:
                try:
                    run_script(STOP, [
                        "-Workspace", workspace,
                        "-Runtime", runtime,
                        "-ToolRoot", tools,
                    ], timeout=10)
                except Exception:
                    pass
                (workspace / "assistant-stop").parent.mkdir(
                    parents=True, exist_ok=True
                )
                (workspace / "assistant-stop").touch()
                if first.poll() is None:
                    try:
                        first.communicate(timeout=10)
                    except subprocess.TimeoutExpired:
                        first.terminate()
                        first.wait(timeout=5)
                if worker_pid is not None and process_exists(worker_pid):
                    terminate_pid(worker_pid)
                if unrelated is not None and unrelated.poll() is None:
                    unrelated.terminate()
                    unrelated.wait(timeout=5)

    def test_forged_receipt_never_stops_unrelated_pid(self):
        with tempfile.TemporaryDirectory(prefix="sdgo forged reward receipt ") as directory:
            root = Path(directory)
            tools = self.fixture(root)
            workspace = root / "owner"
            runtime = root / "runtime"
            workspace.mkdir()
            runtime.mkdir()
            unrelated = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(60)"]
            )
            try:
                receipt = {
                    "schema_version": 1,
                    "component": "battle_reward_wallet_worker",
                    "pid": unrelated.pid,
                    "creation_filetime_utc": 1,
                    "executable_path": str(Path(sys.executable).resolve()),
                    "command_line": "forged command line",
                    "worker_script": str(
                        (tools / "reward_worker.py").resolve()
                    ),
                    "runtime_root": str(runtime.resolve()),
                    "server_ini": str((root / "StoreServer.ini").resolve()),
                    "scope_id": "12345678-1234-4abc-8def-123456789abc",
                }
                receipt_path = workspace / "battle-reward-wallet-worker.json"
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")
                stopped = run_script(STOP, [
                    "-Workspace", workspace,
                    "-Runtime", runtime,
                    "-ToolRoot", tools,
                ])
                self.assertNotEqual(stopped.returncode, 0)
                self.assertIsNone(unrelated.poll())
                self.assertTrue(receipt_path.exists())
            finally:
                if unrelated.poll() is None:
                    unrelated.terminate()
                    unrelated.wait(timeout=5)

    def test_partial_cim_identity_only_cleans_an_already_exited_pid(self):
        with tempfile.TemporaryDirectory(prefix="sdgo partial reward identity ") as directory:
            root = Path(directory)
            tools = self.fixture(root)
            workspace = root / "owner"
            runtime = root / "runtime"
            workspace.mkdir()
            runtime.mkdir()
            process = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(60)"]
            )
            pid = process.pid
            process.terminate()
            process.wait(timeout=5)
            receipt = {
                "schema_version": 1,
                "component": "battle_reward_wallet_worker",
                "pid": pid,
                "creation_filetime_utc": 1,
                "executable_path": str(Path(sys.executable).resolve()),
                "command_line": "exited worker command line",
                "worker_script": str((tools / "reward_worker.py").resolve()),
                "runtime_root": str(runtime.resolve()),
                "server_ini": str((root / "StoreServer.ini").resolve()),
                "scope_id": "12345678-1234-4abc-8def-123456789abc",
            }
            receipt_path = workspace / "battle-reward-wallet-worker.json"
            receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

            stopped = run_stop_with_partial_cim(root, [
                "-Workspace", workspace,
                "-Runtime", runtime,
                "-ToolRoot", tools,
            ], pid)

            self.assertEqual(
                stopped.returncode,
                0,
                f"stdout={stopped.stdout}\nstderr={stopped.stderr}",
            )
            self.assertFalse(receipt_path.exists())
            self.assertEqual(
                status_values(runtime / "wallet-status.ini")["Healthy"], "0"
            )

    def test_partial_cim_identity_never_stops_a_live_pid(self):
        with tempfile.TemporaryDirectory(prefix="sdgo live partial identity ") as directory:
            root = Path(directory)
            tools = self.fixture(root)
            workspace = root / "owner"
            runtime = root / "runtime"
            workspace.mkdir()
            runtime.mkdir()
            process = subprocess.Popen(
                [sys.executable, "-c", "import time; time.sleep(60)"]
            )
            try:
                receipt = {
                    "schema_version": 1,
                    "component": "battle_reward_wallet_worker",
                    "pid": process.pid,
                    "creation_filetime_utc": 1,
                    "executable_path": str(Path(sys.executable).resolve()),
                    "command_line": "unreadable live worker command line",
                    "worker_script": str((tools / "reward_worker.py").resolve()),
                    "runtime_root": str(runtime.resolve()),
                    "server_ini": str((root / "StoreServer.ini").resolve()),
                    "scope_id": "12345678-1234-4abc-8def-123456789abc",
                }
                receipt_path = workspace / "battle-reward-wallet-worker.json"
                receipt_path.write_text(json.dumps(receipt), encoding="utf-8")

                stopped = run_stop_with_partial_cim(root, [
                    "-Workspace", workspace,
                    "-Runtime", runtime,
                    "-ToolRoot", tools,
                ], process.pid)

                self.assertNotEqual(stopped.returncode, 0)
                self.assertIsNone(process.poll())
                self.assertTrue(receipt_path.exists())
            finally:
                if process.poll() is None:
                    process.terminate()
                    process.wait(timeout=5)

    def test_worker_exit_before_heartbeat_cleans_owned_receipt(self):
        with tempfile.TemporaryDirectory(prefix="sdgo failed reward worker ") as directory:
            root = Path(directory)
            tools = self.fixture(root)
            (tools / "reward_worker.py").write_text(
                "raise SystemExit(3)\n", encoding="utf-8"
            )
            workspace, runtime, _, arguments = self.common_arguments(
                root, tools, 42002
            )
            failed = run_script(START, arguments)
            self.assertNotEqual(failed.returncode, 0)
            self.assertFalse(
                (workspace / "battle-reward-wallet-worker.json").exists()
            )
            self.assertFalse((workspace / "assistant-42002.json").exists())
            self.assertFalse(
                list(runtime.glob(".wallet-status.*.tmp"))
            )


if __name__ == "__main__":
    unittest.main()
