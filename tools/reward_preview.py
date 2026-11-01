"""Reward calculator and standalone owner assistant for native runtime control."""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.rewards import model
from owner.rewards.runtime_control import RuntimeControl
from owner.rewards.workspace import Workspace
from assistant_modules.rewards import attach


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", type=Path)
    parser.add_argument("--runtime", type=Path, help="Native policy/status directory; defaults to WORKSPACE/runtime")
    parser.add_argument("--heartbeat-timeout", type=int, default=15)
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--scenario", type=Path, help="Offline JSON with context/stats; omit to serve the owner UI")
    args = parser.parse_args()
    if args.scenario:
        policy = Workspace(args.workspace).read() if args.workspace else model.defaults()
        value = json.loads(args.scenario.read_text(encoding="utf-8-sig"))
        print(json.dumps(model.preview(policy, value["context"], value["stats"]), ensure_ascii=False, indent=2))
        return
    if not args.workspace:
        parser.error("Owner UI requires a --workspace directory")

    class QuietHandler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

    runtime = RuntimeControl(args.workspace, args.runtime, args.heartbeat_timeout)
    workspace = Workspace(args.workspace, runtime)
    server = attach(ThreadingHTTPServer(("127.0.0.1", args.port), QuietHandler), workspace)
    server.daemon_threads = True
    state = workspace.public()
    connection = "native settlement connected" if state["delivery_connected"] else "waiting for native settlement heartbeat"
    print(f"SDGO reward assistant: http://127.0.0.1:{server.server_port}/rewards ({connection})", flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
