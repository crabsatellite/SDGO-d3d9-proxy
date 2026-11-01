import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "tools" / "reward_assistant.py"


FIXTURE_ASSISTANT = r'''
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
from assistant_modules.shared import PACKAGE_MARKER


class Workspace:
    def __init__(self, root, client, snapshot, module_binary=None, server_ini=None, label="unset"):
        self.root = Path(root).resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.state = {
            "root": str(self.root),
            "client": str(Path(client).resolve()),
            "snapshot": str(Path(snapshot).resolve()),
            "module_binary": str(Path(module_binary).resolve()) if module_binary else None,
            "server_ini": str(Path(server_ini).resolve()) if server_ini else None,
            "label": label,
            "package_marker": PACKAGE_MARKER,
        }
        (self.root / "base-state.json").write_text(json.dumps(self.state), encoding="utf-8")


def make_server(workspace, port=0):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def json_response(self, status, value):
            body = json.dumps(value).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.headers.get("Host") not in {
                f"127.0.0.1:{self.server.server_port}",
                f"localhost:{self.server.server_port}",
            }:
                return self.json_response(403, {"error": "bad host"})
            if self.path == "/api/modules":
                return self.json_response(200, {"modules": [
                    {"id": "existing", "name": "Existing", "url": "/base"},
                    {"id": "rewards", "name": "obsolete", "url": "/obsolete"},
                ]})
            if self.path == "/base-state":
                return self.json_response(200, workspace.state)
            return self.json_response(404, {"error": "missing"})

        def do_POST(self):
            return self.json_response(200, {"base_post": True, "path": self.path})

    server = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    server.daemon_threads = True
    return server
'''


class RunningAssistant:
    def __init__(self, process, workspace):
        self.process = process
        self.workspace = workspace
        self.session = workspace / "reward-assistant-session.json"
        self.url = None

    def wait(self):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if self.process.poll() is not None:
                stdout, stderr = self.process.communicate()
                raise AssertionError(
                    f"assistant exited {self.process.returncode}\nstdout={stdout}\nstderr={stderr}"
                )
            if self.session.is_file():
                try:
                    value = json.loads(self.session.read_text(encoding="utf-8"))
                    with urlopen(value["url"].rsplit("/", 1)[0] + "/api/rewards", timeout=1):
                        self.url = value["url"].rsplit("/", 1)[0]
                        return value
                except (OSError, ValueError, KeyError):
                    pass
            time.sleep(0.05)
        raise AssertionError("assistant did not publish a reachable session")

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        self.process.communicate()


def launch(arguments, workspace):
    environment = dict(os.environ)
    environment["PYTHONUTF8"] = "1"
    environment["PYTHONDONTWRITEBYTECODE"] = "1"
    process = subprocess.Popen(
        [sys.executable, "-B", "-X", "utf8", str(LAUNCHER), *map(str, arguments)],
        cwd=ROOT,
        env=environment,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
    )
    running = RunningAssistant(process, workspace)
    running.wait()
    return running


class RewardAssistantLauncherTests(unittest.TestCase):
    def read(self, origin, path):
        with urlopen(origin + path, timeout=3) as response:
            return json.load(response)

    def test_installed_factory_shares_port_and_preserves_routes_and_state(self):
        with tempfile.TemporaryDirectory(prefix="sdgo reward launcher ") as directory:
            root = Path(directory)
            assistant_root = root / "installed assistant package"
            tools = assistant_root / "tools"
            package = tools / "assistant_modules"
            package.mkdir(parents=True)
            (package / "__init__.py").write_text('"""fixture regular package"""\n', encoding="utf-8")
            (package / "shared.py").write_text('PACKAGE_MARKER = "installed-package"\n', encoding="utf-8")
            (tools / "mod_assistant.py").write_text(FIXTURE_ASSISTANT, encoding="utf-8")

            reward_workspace = root / "reward owner workspace"
            runtime = root / "native runtime status"
            base_workspace = root / "existing assistant state"
            client = root / "client with spaces"
            snapshot = root / "snapshot with spaces"
            module_binary = root / "module binary.dll"
            server_ini = root / "server config.ini"
            for path in (client, snapshot):
                path.mkdir()
            module_binary.write_bytes(b"fixture")
            server_ini.write_text("[server]\n", encoding="ascii")

            running = launch([
                "--workspace", reward_workspace,
                "--runtime", runtime,
                "--assistant-root", assistant_root,
                "--assistant-workspace", base_workspace,
                "--assistant-client", client,
                "--assistant-snapshot", snapshot,
                "--assistant-module-binary", module_binary,
                "--assistant-server-ini", server_ini,
                "--assistant-option", "label=fixture-state",
                "--port", "0",
            ], reward_workspace)
            try:
                session = json.loads(running.session.read_text(encoding="utf-8"))
                self.assertEqual(session["mode"], "shared")
                self.assertEqual(session["status"], "waiting_for_native")
                self.assertFalse(session["delivery_connected"])

                modules = self.read(running.url, "/api/modules")["modules"]
                self.assertEqual([item["id"] for item in modules], ["existing", "rewards"])
                base = self.read(running.url, "/base-state")
                self.assertEqual(base["root"], str(base_workspace.resolve()))
                self.assertEqual(base["client"], str(client.resolve()))
                self.assertEqual(base["snapshot"], str(snapshot.resolve()))
                self.assertEqual(base["module_binary"], str(module_binary.resolve()))
                self.assertEqual(base["server_ini"], str(server_ini.resolve()))
                self.assertEqual(base["label"], "fixture-state")
                self.assertEqual(base["package_marker"], "installed-package")

                with urlopen(Request(running.url + "/base-post", data=b"{}"), timeout=3) as response:
                    self.assertEqual(json.load(response), {"base_post": True, "path": "/base-post"})

                state = self.read(running.url, "/api/rewards")
                self.assertEqual(state["status"], "waiting_for_native")
                self.assertFalse(state["delivery_connected"])
                payload = json.dumps({"base_multiplier": "7"}).encode("utf-8")
                headers = {
                    "Content-Type": "application/json",
                    "Origin": running.url,
                    "X-Assistant-Token": state["session_token"],
                    "X-Reward-Revision": state["revision"],
                }
                with urlopen(Request(
                    running.url + "/api/rewards/multiplier",
                    data=payload,
                    headers=headers,
                ), timeout=3) as response:
                    saved = json.load(response)
                self.assertEqual(saved["policy"]["base_multiplier"], "7")
                self.assertIn(b"Base=7\r\n", (runtime / "reward-policy.ini").read_bytes())

                headers["Origin"] = "https://unrelated.invalid"
                with self.assertRaises(HTTPError) as rejected:
                    urlopen(Request(
                        running.url + "/api/rewards/multiplier",
                        data=payload,
                        headers=headers,
                    ), timeout=3)
                self.assertEqual(rejected.exception.code, 403)
                rejected.exception.close()
            finally:
                running.close()

    def test_standalone_mode_exposes_reward_module_on_loopback(self):
        with tempfile.TemporaryDirectory(prefix="sdgo standalone reward ") as directory:
            workspace = Path(directory) / "owner state with spaces"
            running = launch(["--workspace", workspace, "--port", "0"], workspace)
            try:
                session = json.loads(running.session.read_text(encoding="utf-8"))
                self.assertEqual(session["mode"], "standalone")
                self.assertEqual(
                    [item["id"] for item in self.read(running.url, "/api/modules")["modules"]],
                    ["rewards"],
                )
                with urlopen(running.url + "/rewards", timeout=3) as response:
                    self.assertIn("战斗奖励", response.read().decode("utf-8"))
                self.assertTrue((workspace / "runtime" / "reward-policy.ini").is_file())
            finally:
                running.close()

    def test_occupied_fixed_port_fails_before_creating_workspaces(self):
        with tempfile.TemporaryDirectory(prefix="sdgo occupied reward ") as directory:
            root = Path(directory)
            workspace = root / "owner"
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            port = listener.getsockname()[1]
            try:
                process = subprocess.run(
                    [
                        sys.executable,
                        "-B",
                        "-X",
                        "utf8",
                        str(LAUNCHER),
                        "--workspace",
                        str(workspace),
                        "--port",
                        str(port),
                    ],
                    cwd=ROOT,
                    capture_output=True,
                    text=True,
                    encoding="utf-8",
                    timeout=10,
                )
            finally:
                listener.close()
            self.assertEqual(process.returncode, 2)
            self.assertIn(f"本机端口 {port} 已被占用", process.stderr)
            self.assertFalse(workspace.exists())

    def test_missing_installed_workspace_arguments_fail_before_binding(self):
        with tempfile.TemporaryDirectory(prefix="sdgo missing assistant args ") as directory:
            root = Path(directory)
            assistant_root = root / "installed"
            tools = assistant_root / "tools"
            package = tools / "assistant_modules"
            package.mkdir(parents=True)
            (package / "__init__.py").write_text("", encoding="utf-8")
            (package / "shared.py").write_text('PACKAGE_MARKER = "fixture"\n', encoding="utf-8")
            (tools / "mod_assistant.py").write_text(FIXTURE_ASSISTANT, encoding="utf-8")
            reward_workspace = root / "reward owner"
            base_workspace = root / "base owner"
            process = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    "-X",
                    "utf8",
                    str(LAUNCHER),
                    "--workspace",
                    str(reward_workspace),
                    "--assistant-root",
                    str(assistant_root),
                    "--assistant-workspace",
                    str(base_workspace),
                ],
                cwd=ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                timeout=10,
            )
            self.assertEqual(process.returncode, 2)
            self.assertIn("Workspace 工厂缺少参数：client, snapshot", process.stderr)
            self.assertFalse(reward_workspace.exists())
            self.assertFalse(base_workspace.exists())


if __name__ == "__main__":
    unittest.main()
