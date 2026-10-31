"""Generate friend/client and host/Agent mod configurations from tunnel endpoints.

Offline only: no Sakura API, tokens, tunnel changes, game installation or overwrite.
"""
from __future__ import annotations

import argparse
import ipaddress
import json
from pathlib import Path
import re
import zipfile


def port(value: object) -> int:
    if type(value) is not int or not 1 <= value <= 65535:
        raise ValueError("Ports must be integers in 1..65535")
    return value


def host(value: object) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9.-]+", value):
        raise ValueError("External hosts must be DNS names or IPv4, without scheme/path/credentials")
    return value


def address(value: object) -> str:
    if not isinstance(value, str):
        raise ValueError("Logical IPv4 addresses must be strings")
    result = ipaddress.IPv4Address(value)
    if result.is_unspecified or int(result) == 0xFFFFFFFF or result.is_multicast:
        raise ValueError("Use the exact unicast IPv4 advertised by the service")
    return str(result)


def render(profile: dict) -> tuple[str, str, list[dict]]:
    if type(profile.get("schema_version")) is not int or profile.get("schema_version") not in (1, 2):
        raise ValueError("Need schema_version=1 or 2")
    bind_ip = address(profile["udp_bind_ip"])
    login = profile["login"]
    login_ip = address(login["logical_ip"])
    login_local, login_external = port(login["logical_port"]), port(login["external_port"])
    login_host = host(login["external_host"])
    pool = profile["udp"]
    if not isinstance(pool, list) or not 1 <= len(pool) <= 31:
        raise ValueError("Need 1..31 UDP pool endpoints")
    local_ports = [port(entry["local_port"]) for entry in pool]
    first = min(local_ports)
    if first < 1024 or sorted(local_ports) != list(range(first, first + len(pool))):
        raise ValueError("UDP local ports must form a unique contiguous pool starting at 1024 or above")
    rules = [f"tcp,{login_ip},{login_local},{login_host},{login_external}"]
    inventory = [{"protocol": "tcp", "local_ip": login_ip, "local_port": login_local,
                  "external_host": login_host, "external_port": login_external}]
    logical_tcp = {(login_ip, login_local)}
    aliases = login.get("aliases", [])
    if not isinstance(aliases, list):
        raise ValueError("login.aliases must be an array of logical_ip/logical_port entries")
    for alias in aliases:
        alias_ip, alias_port = address(alias["logical_ip"]), port(alias["logical_port"])
        if (alias_ip, alias_port) in logical_tcp:
            raise ValueError("Duplicate logical TCP endpoint")
        logical_tcp.add((alias_ip, alias_port))
        rules.append(f"tcp,{alias_ip},{alias_port},{login_host},{login_external}")
        inventory.append({"protocol": "tcp", "local_ip": alias_ip, "local_port": alias_port,
                          "external_host": login_host, "external_port": login_external})
    extra_tcp = profile.get("tcp", [])
    if not isinstance(extra_tcp, list):
        raise ValueError("tcp must be an array of logical and external endpoint entries")
    for entry in extra_tcp:
        logical_ip, logical_port = address(entry["logical_ip"]), port(entry["logical_port"])
        if (logical_ip, logical_port) in logical_tcp:
            raise ValueError("Duplicate logical TCP endpoint")
        logical_tcp.add((logical_ip, logical_port))
        external_host = host(entry["external_host"])
        external_port = port(entry["external_port"])
        rules.append(f"tcp,{logical_ip},{logical_port},{external_host},{external_port}")
        inventory.append({"protocol": "tcp", "local_ip": logical_ip, "local_port": logical_port,
                          "external_host": external_host, "external_port": external_port})
    if len(rules) + len(pool) > 32:
        raise ValueError("TCP routes plus UDP pool must fit the client's 32-route limit")
    seen = set()
    for entry in sorted(pool, key=lambda item: item["local_port"]):
        public_host, public_port = host(entry["external_host"]), port(entry["external_port"])
        endpoint = (public_host.lower(), public_port)
        if endpoint in seen:
            raise ValueError("UDP external endpoints must be unique (DNS aliases are also checked by the mod at startup)")
        seen.add(endpoint)
        local = port(entry["local_port"])
        rules.append(f"udp,{bind_ip},{local},{public_host},{public_port}")
        inventory.append({"protocol": "udp", "local_ip": bind_ip, "local_port": local,
                          "external_host": public_host, "external_port": public_port})
    client = (
        "; Generated client profile. Contains connection endpoints, no Sakura credentials.\n"
        "; Native relay and real external multiplayer still require acceptance.\n"
        "[mod]\nid=net_route\nname=Network endpoint routes\nversion=0.2.0\napi=1\n"
        "dll=net_route.dll\norder=10\nenabled=1\nhosts=GOnline-54BE1568-00C97000\n"
        f"\n[settings]\nLoginIPv4={login_ip}\nLoginPort={login_local}\nRouteCount={len(rules)}\nTrace=0\n"
        + "".join(f"Route{i}={rule}\n" for i, rule in enumerate(rules, 1))
    )
    agent = (
        "; Host Agent profile. All Agents must use the same UDP bind IP and this pool.\n"
        "[mod]\nid=agent_udp_pool\nname=Agent native UDP port pool\nversion=0.2.0\napi=1\n"
        "dll=agent_udp_pool.dll\norder=10\nenabled=1\nhosts=AgentServer-548A86BC-000D7000\n"
        "requires=agent.udp_bind_call,agent.bind_import,agent.relay_log_format\n"
        f"\n[settings]\nFirstPort={first}\nPortCount={len(pool)}\n"
    )
    return client, agent, inventory


def configurations(profile: dict) -> tuple[str, str, list[dict]]:
    """Compile allowlisted settings; never copy the owner's source JSON."""
    client, agent, inventory = render(profile)
    return ("; Player connection settings. Start with your existing launcher.\n[settings]" +
            client.split("[settings]", 1)[1],
            "; Owner-only Agent UDP pool settings.\n[settings]" + agent.split("[settings]", 1)[1], inventory)


def export_profile(profile: dict, destination: Path) -> Path:
    client, agent, inventory = configurations(profile)
    destination.mkdir(parents=True, exist_ok=False)
    for relative, content in (
        ("client/sdgo/mods/net_route/config.ini", client),
        ("agent/sdgo/mods/agent_udp_pool/config.ini", agent),
        ("owner-tunnel-endpoints.json", json.dumps(inventory, indent=2) + "\n"),
    ):
        path = destination / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="ascii")
    archive = destination / "player-profile.zip"
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as output:
        output.writestr("sdgo/mods/net_route/config.ini", client)
        output.writestr("README.txt", "Install the universal FRP mod once, then put this sdgo folder in your game folder.\n"
                        "Use your existing launcher. Restart the game after changing connection settings.\n"
                        "No Sakura account or special launch script is required on players' computers.\n")
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="New output directory; existing paths are refused")
    args = parser.parse_args()
    archive = export_profile(json.loads(args.profile.read_text(encoding="utf-8-sig")), args.out)
    print(f"Wrote owner settings and portable player profile: {archive.resolve()}")


if __name__ == "__main__":
    main()
