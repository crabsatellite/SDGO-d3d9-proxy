"""Prove copied Agent forwards 0x064B from a real BATTLE EXTEND socket.

The test uses one copied Agent process, isolated 127.88.0.1 ports and one
synthetic ephemeral user. It never contacts authentication, databases, live
servers, or a real game client.
"""
from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time


AGENT_SHA256 = "7becced6510c078c48dfff2df1dbd73f6433d312056a862bd161fb4521d2df45"
ACCOUNT = 4_100_643
CURRENT_MB = 0x10203040
PACKET = struct.pack("<HII", 0x064B, ACCOUNT, CURRENT_MB)
FRAME_MASK = 0x03F00000


def bound_listener(ip: str) -> socket.socket:
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    listener.bind((ip, 0))
    listener.listen(1)
    listener.settimeout(12)
    return listener


def free_port(ip: str, socket_type: int) -> int:
    reserved = socket.socket(socket.AF_INET, socket_type)
    reserved.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    reserved.bind((ip, 0))
    port = reserved.getsockname()[1]
    reserved.close()
    return port


def accept_peer(listener: socket.socket) -> socket.socket:
    connection, _ = listener.accept()
    connection.settimeout(0.2)
    return connection


def wait_json(path: Path, seconds: float) -> dict | None:
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if path.exists():
            try:
                return json.loads(path.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError):
                pass
        time.sleep(0.025)
    return None


def receive_packet(connection: socket.socket, expected: bytes, seconds: float) -> tuple[bool, list[str]]:
    queued = bytearray()
    frames: list[str] = []
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        while len(queued) >= 4:
            header = struct.unpack_from("<I", queued)[0]
            if header & 0xFFF00000 != FRAME_MASK:
                return False, frames
            size = header & 0x000FFFFF
            if size > 65536:
                return False, frames
            if len(queued) < 4 + size:
                break
            payload = bytes(queued[4:4 + size])
            del queued[:4 + size]
            frames.append(payload.hex())
            if payload == expected:
                return True, frames
        try:
            chunk = connection.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            break
        queued.extend(chunk)
    return False, frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--winmm", type=Path, required=True)
    parser.add_argument("--core", type=Path, required=True)
    args = parser.parse_args()

    repo = Path(__file__).resolve().parents[1]
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    runtime = repo / "build" / "runtime" / f"agent-cash-{stamp}"
    runtime.mkdir(parents=True, exist_ok=False)
    mod_dir = runtime / "sdgo" / "mods" / "native_agent_cash_probe"
    symbol_dir = runtime / "sdgo" / "symbols"
    mod_dir.mkdir(parents=True)
    symbol_dir.mkdir(parents=True)

    source_agent = args.source / "AgentServer.exe"
    actual_hash = hashlib.sha256(source_agent.read_bytes()).hexdigest()
    if actual_hash != AGENT_SHA256:
        raise RuntimeError(f"unexpected AgentServer.exe SHA256 {actual_hash}")
    for name in ("sip.txt", "csauth2.cfg", "ggauth60.dll", "libcomservice2.dll", "dbghelp.dll"):
        shutil.copy2(args.source / name, runtime / name)
    copied_agent = runtime / "AgentCashProbe.exe"
    shutil.copy2(source_agent, copied_agent)
    shutil.copy2(args.winmm, runtime / "winmm.dll")
    shutil.copy2(args.core, runtime / "sdgo" / "core.dll")
    shutil.copy2(args.fixture, mod_dir / "native_agent_cash_probe_mod.dll")

    symbol_text = (repo / "symbols" / "AgentServer-548A86BC-000D7000.ini").read_text(encoding="utf-8")
    symbol_text += "\n" + (repo / "tests" / "native_agent_cash_probe_symbols.ini").read_text(encoding="utf-8")
    (symbol_dir / "AgentCashProbe-548A86BC-000D7000.ini").write_text(symbol_text, encoding="ascii")
    (mod_dir / "mod.ini").write_text(
        "[mod]\n"
        "id=native_agent_cash_probe\n"
        "name=Native isolated Agent cash forwarding probe\n"
        "api=1\n"
        "dll=native_agent_cash_probe_mod.dll\n"
        "order=100\n"
        "hosts=AgentCashProbe-548A86BC-000D7000\n"
        "requires=agent.frame_proc,agent.user_manager,agent.user_by_id,"
        "agent.close_client_tcp,agent.add_user,agent.remove_user,"
        "agent.connect_client\n",
        encoding="ascii",
    )

    ip = "127.88.0.1"
    battle_listener = bound_listener(ip)
    client_listener = bound_listener(ip)
    battle_port = battle_listener.getsockname()[1]
    client_port = client_listener.getsockname()[1]
    agent_port = free_port(ip, socket.SOCK_STREAM)
    frontend_tcp_port = free_port(ip, socket.SOCK_STREAM)
    frontend_udp_port = free_port(ip, socket.SOCK_DGRAM)
    (runtime / "sip.txt").write_text(
        f"LISTEN {ip}:{agent_port} AGENT\n"
        f"EXTEND {ip}:{battle_port} BATTLE\n"
        f"LISTEN_FOR {ip}:{frontend_tcp_port} TCP\n"
        f"LISTEN_FOR {ip}:{frontend_udp_port} UDP\n"
        f"LISTEN_VIRTUAL {ip}\n"
        "SKIP_ALIVE_SIGNAL\n",
        encoding="ascii",
    )

    result = {
        "scope": (
            "copied Agent native BATTLE ingress to native client egress with "
            "one synthetic ephemeral user; no authentication, database, "
            "real client, or gameplay"
        ),
        "agent_sha256": actual_hash,
        "agent_copy": str(copied_agent),
        "isolated_ip": ip,
        "ports": {
            "battle_extend": battle_port,
            "synthetic_client_peer": client_port,
            "agent_listen": agent_port,
            "frontend_tcp": frontend_tcp_port,
            "frontend_udp": frontend_udp_port,
        },
        "input_packet_hex": PACKET.hex(),
        "expected_packet_size": len(PACKET),
    }
    process: subprocess.Popen[bytes] | None = None
    battle_connection: socket.socket | None = None
    client_connection: socket.socket | None = None
    try:
        environment = dict(os.environ)
        environment["__COMPAT_LAYER"] = "RunAsInvoker"
        environment["SDGO_CASH_PROBE_CLIENT_PORT"] = str(client_port)
        startup = subprocess.STARTUPINFO()
        startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
        startup.wShowWindow = 0
        with (runtime / "stdout.log").open("wb") as output:
            process = subprocess.Popen(
                [str(copied_agent)],
                cwd=runtime,
                env=environment,
                startupinfo=startup,
                creationflags=subprocess.CREATE_NO_WINDOW,
                stdin=subprocess.DEVNULL,
                stdout=output,
                stderr=output,
            )
            result["owned_pid"] = process.pid
            battle_connection = accept_peer(battle_listener)
            result["battle_extend_connected"] = True
            client_connection = accept_peer(client_listener)
            result["synthetic_client_connected"] = True
            ready = wait_json(mod_dir / "ready.json", 5)
            result["fixture_ready"] = ready
            if not ready or ready.get("account") != ACCOUNT:
                raise RuntimeError("fixture did not publish the expected synthetic user")

            battle_connection.sendall(struct.pack("<I", FRAME_MASK | len(PACKET)) + PACKET)
            forwarded, frames = receive_packet(client_connection, PACKET, 5)
            result["client_frames_hex"] = frames
            result["forwarded_exact_10_bytes"] = forwarded
            peer_result = {
                "scope": "loopback peer byte comparison",
                "pass": forwarded,
                "expected_hex": PACKET.hex(),
                "frames_hex": frames,
            }
            (mod_dir / "peer-result.json").write_text(
                json.dumps(peer_result, indent=2) + "\n", encoding="utf-8"
            )
            fixture_result = wait_json(mod_dir / "fixture-result.json", 5)
            result["fixture_result"] = fixture_result
            if fixture_result is None:
                raise RuntimeError("fixture cleanup receipt was not produced")
    except (OSError, RuntimeError) as error:
        result["error"] = str(error)
    finally:
        for connection in (client_connection, battle_connection):
            if connection is not None:
                connection.close()
        battle_listener.close()
        client_listener.close()
        result["exited_before_supervisor_stop"] = (
            process is not None and process.poll() is not None
        )
        result["owned_process_terminated"] = False
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
            result["owned_process_terminated"] = True
        result["exit_code"] = process.returncode if process is not None else None

    log_path = runtime / "sdgo" / "logs" / "sdgo.log"
    log = log_path.read_text(encoding="utf-8", errors="replace") if log_path.exists() else ""
    result["mod_loaded"] = (
        "native_agent_cash_probe" in log and
        "native Agent 064B forwarding probe" in log
    )
    fixture = result.get("fixture_result") or {}
    result["pass"] = bool(
        result.get("battle_extend_connected")
        and result.get("synthetic_client_connected")
        and result.get("forwarded_exact_10_bytes")
        and fixture.get("native_user_cleanup")
        and fixture.get("pass")
        and not result.get("exited_before_supervisor_stop")
        and result.get("owned_process_terminated")
        and not result.get("error")
    )
    result["limitation"] = (
        "This proves shipped Agent transport and dispatch behavior with a "
        "synthetic account mapping. It does not prove client HUD rendering "
        "or authenticated gameplay."
    )
    (runtime / "result.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, indent=2))
    return 0 if result["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
