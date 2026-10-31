"""Build a universal incremental FRP Mod kit, never a copy of a game or local host."""
from __future__ import annotations
import argparse
import configparser
import ctypes
from ctypes import wintypes
import hashlib
import io
import json
import posixpath
from pathlib import Path
import struct
import re
from urllib.parse import unquote
import zipfile


def file_version(path: Path) -> tuple[int, int, int, int]:
    """Read PE version resources without loading/executing the DLL (Windows build host)."""
    version = ctypes.windll.version
    version.GetFileVersionInfoSizeW.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(wintypes.DWORD)]
    version.GetFileVersionInfoW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID]
    version.VerQueryValueW.argtypes = [wintypes.LPCVOID, wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(wintypes.UINT)]
    size = version.GetFileVersionInfoSizeW(str(path.resolve()), None)
    buffer = ctypes.create_string_buffer(size)
    pointer, length = ctypes.c_void_p(), wintypes.UINT()
    if not size or not version.GetFileVersionInfoW(str(path.resolve()), 0, size, buffer) or not version.VerQueryValueW(buffer, "\\", ctypes.byref(pointer), ctypes.byref(length)):
        raise ValueError(f"Missing DLL version resource: {path}")
    fields = (ctypes.c_uint32 * 13).from_address(pointer.value)
    return fields[2] >> 16, fields[2] & 65535, fields[3] >> 16, fields[3] & 65535


def symbol_fragment(path: Path, names: tuple[str, ...]) -> bytes:
    source = configparser.ConfigParser(interpolation=None)
    source.read(path, encoding="utf-8-sig")
    target = configparser.ConfigParser(interpolation=None)
    for name in ("host",) + names:
        target[name] = dict(source[name])
    output = io.StringIO()
    target.write(output, space_around_delimiters=False)
    return output.getvalue().encode("ascii")


def validate_markdown_links(files: dict[str, bytes]) -> None:
    for name, data in files.items():
        if not name.endswith('.md'):
            continue
        for target in re.findall(r'!?\[[^\]]*\]\(([^)]+)\)', data.decode('utf-8-sig')):
            if target.startswith(('http:', 'https:', 'mailto:', '#')):
                continue
            relative = unquote(target.split('#', 1)[0].strip('<>'))
            resolved = posixpath.normpath(posixpath.join(posixpath.dirname(name), relative))
            if relative and resolved not in files:
                raise ValueError(f"Missing packaged documentation link: {name} -> {target}")


def contents(repo: Path, binaries: Path) -> dict[str, bytes]:
    files: dict[str, bytes] = {}
    expected_version = tuple(int(n) for n in (repo/"VERSION").read_text().strip().split(".")) + (0,)
    def add(name: str, source: Path) -> None:
        data = source.read_bytes()
        if name.endswith(".dll"):
            pe = struct.unpack_from("<I", data, 0x3C)[0]
            if data[pe:pe+4] != b"PE\0\0" or struct.unpack_from("<H", data, pe+4)[0] != 0x14C:
                raise ValueError(f"Not a Win32 DLL: {source}")
            if file_version(source) != expected_version:
                raise ValueError(f"Stale DLL version: {source}; rebuild the latest framework first")
        files[name] = data
    for side, mods in (("client", ("net_route",)), ("agent", ("agent_udp_pool", "agent_relay_log_fix"))):
        for mod in mods:
            add(f"payload/{side}/mods/{mod}/{mod}.dll", binaries/f"{mod}.dll")
            add(f"payload/{side}/mods/{mod}/mod.ini", repo/f"mods/{mod}/mod.ini")
            template = repo/f"mods/{mod}/config.example.ini"
            if template.exists():
                add(f"payload/{side}/mods/{mod}/config.example.ini", template)
        loader = "d3d9.dll" if side == "client" else "winmm.dll"
        add(f"framework/{side}/{loader}", binaries/loader)
        add(f"framework/{side}/sdgo/core.dll", binaries/"core.dll")
        add(f"framework/{side}/sdgo/sdgo.ini", repo/"config/sdgo.ini")
        files[f"framework/{side}/sdgo/mods/.keep"] = b""
        files[f"framework/{side}/sdgo/symbols/.keep"] = b""
    for side, key, names in (
        ("client", "GOnline-54BE1568-00C97000", ("net.login_argv_call",)),
        ("agent", "AgentServer-548A86BC-000D7000", ("agent.udp_bind_call", "agent.bind_import", "agent.relay_log_format")),
    ):
        files[f"payload/{side}/symbols/{key}.ini"] = symbol_fragment(repo/f"symbols/{key}.ini", names)
    for name in ("install_frp.ps1", "install_client.ps1", "install_server.ps1"):
        add(name, repo/"tools"/name)
    add("owner-tools/make_frp_profile.py", repo/"tools/make_frp_profile.py")
    add("owner-tools/owner-profile.example.json", repo/"docs/examples/frp-profile.example.json")
    add("README.zh-CN.md", repo/"docs/frp-distribution.zh-CN.md")
    add("framework/VERSION", repo/"VERSION")
    for document in ("net-route.md", "net-route.zh-CN.md", "sakura-frp-setup.zh-CN.md", "frp-distribution.zh-CN.md", "frp-testing.md"):
        add(f"docs/{document}", repo/"docs"/document)
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        add(name, repo/name)
    add("licenses/MinHook-LICENSE.txt", repo/"third_party/minhook/LICENSE.txt")
    validate_markdown_links(files)
    return files


def package(repo: Path, destination: Path, binaries: Path | None = None, directory_only: bool = False) -> Path:
    files = contents(repo, binaries or repo/"build/Release")
    archive = destination.with_name(destination.name+".zip")
    if destination.exists() or archive.exists():
        raise FileExistsError("Refusing to replace an existing kit or archive")
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in files.items()}
    files["SHA256.json"] = (json.dumps(hashes, indent=2)+"\n").encode("ascii")
    destination.mkdir(parents=True)
    for name, data in files.items():
        path = destination/name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    if directory_only:
        return destination
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as output:
        for name, data in files.items():
            output.writestr(name, data)
    with zipfile.ZipFile(archive) as output:
        if output.testzip() is not None:
            raise ValueError("Archive CRC failed")
        for name, digest in hashes.items():
            if hashlib.sha256(output.read(name)).hexdigest() != digest:
                raise ValueError("Archive checksum failed")
    return archive


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--binaries", type=Path, help="Built DLL directory; defaults to build/Release")
    parser.add_argument("--directory-only", action="store_true", help="Embed the kit in a full framework package")
    args = parser.parse_args()
    result = package(Path(__file__).resolve().parents[1], args.out, args.binaries, args.directory_only)
    if args.directory_only:
        print(json.dumps({"directory": str(result.resolve())}))
    else:
        print(json.dumps({"archive": str(result.resolve()), "bytes": result.stat().st_size,
                          "sha256": hashlib.sha256(result.read_bytes()).hexdigest()}, indent=2))


if __name__ == "__main__":
    main()
