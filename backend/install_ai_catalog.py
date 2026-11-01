#!/usr/bin/env python3
"""Install/toggle/remove only the optional AI catalog overlay in a stopped host.

Owner-side Python only. Players can receive the prepared client folder and use
their existing launcher. No SQL, game archive, loader or shared core is changed.
"""
from __future__ import annotations
import argparse
import base64
import configparser
import ctypes
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import tempfile


STATE = "sdgo/install-state/ai_catalog.json"
PENDING = "sdgo/install-state/ai_catalog.pending.json"
OWN = "sdgo/mods/ai_catalog/"
HOSTS = {"client": ("GOnline.exe", 0x54BE1568, 0x00C97000),
         "server": ("BattleServer.exe", 0x548A891D, 0x0046C000)}
SECTIONS = {"client": ("grid.data", "grid.find", "grid.load_npc_table", "grid.make_node", "grid.insert_node",
             "grid.register_record", "grid.map_find", "crt.operator_new", "crt.operator_delete",
             "mode.base_create_npc", "mode.create_npc", "mode.create_remote_npc"), "server": ("catalog.find_ai_npc", "catalog.find_unit",
             "catalog.npc_manager", "catalog.unit_manager", "battle.build_ai_pilot")}


def digest(data):
    return None if data is None else hashlib.sha256(data).hexdigest()


def encode(data):
    return None if data is None else base64.b64encode(data).decode("ascii")


def decode(data):
    return None if data is None else base64.b64decode(data, validate=True)


def ini(data):
    result = configparser.ConfigParser(interpolation=None)
    result.optionxform = str.lower
    result.read_string(data.decode("utf-8-sig"))
    return result


def host_info(path, role):
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise ValueError("host is not a PE executable")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if pe + 84 > len(data) or data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("invalid PE header")
    machine, stamp = struct.unpack_from("<H", data, pe + 4)[0], struct.unpack_from("<I", data, pe + 8)[0]
    size = struct.unpack_from("<I", data, pe + 24 + 56)[0]
    if machine != 0x14C or (stamp, size) != HOSTS[role][1:]:
        raise ValueError("unsupported host build")
    return f"{path.stem}-{stamp:08X}-{size:08X}", stamp, size


def running(path):
    if sys.platform != "win32":
        return False
    from ctypes import wintypes as w
    class Process(ctypes.Structure):
        _fields_ = [("dwSize", w.DWORD), ("cntUsage", w.DWORD), ("pid", w.DWORD),
                    ("heap", ctypes.c_size_t), ("module", w.DWORD), ("threads", w.DWORD),
                    ("parent", w.DWORD), ("priority", w.LONG), ("flags", w.DWORD), ("exe", w.WCHAR * 260)]
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateToolhelp32Snapshot.argtypes = [w.DWORD, w.DWORD]
    kernel.CreateToolhelp32Snapshot.restype = w.HANDLE
    kernel.Process32FirstW.argtypes = [w.HANDLE, ctypes.POINTER(Process)]
    kernel.Process32NextW.argtypes = [w.HANDLE, ctypes.POINTER(Process)]
    kernel.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    kernel.OpenProcess.restype = w.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [w.HANDLE, w.DWORD, w.LPWSTR, ctypes.POINTER(w.DWORD)]
    kernel.CloseHandle.argtypes = [w.HANDLE]
    snapshot = kernel.CreateToolhelp32Snapshot(2, 0)
    if snapshot == ctypes.c_void_p(-1).value:
        raise OSError("cannot enumerate running hosts")
    entry = Process(); entry.dwSize = ctypes.sizeof(entry)
    try:
        valid = kernel.Process32FirstW(snapshot, ctypes.byref(entry))
        while valid:
            if entry.exe.casefold() == path.name.casefold():
                handle = kernel.OpenProcess(0x1000, False, entry.pid)
                if not handle:
                    raise OSError("cannot verify a running namesake host")
                try:
                    buffer = ctypes.create_unicode_buffer(32768); count = w.DWORD(len(buffer))
                    if not kernel.QueryFullProcessImageNameW(handle, 0, buffer, ctypes.byref(count)):
                        raise OSError("cannot resolve a running namesake host")
                    if Path(buffer.value).resolve() == path.resolve():
                        return True
                finally:
                    kernel.CloseHandle(handle)
            valid = kernel.Process32NextW(snapshot, ctypes.byref(entry))
    finally:
        kernel.CloseHandle(snapshot)
    return False


def file_version(path):
    if sys.platform != "win32" or not path.is_file(): return None
    from ctypes import wintypes as w
    api = ctypes.WinDLL("version", use_last_error=True)
    api.GetFileVersionInfoSizeW.argtypes = [w.LPCWSTR, ctypes.POINTER(w.DWORD)]
    api.GetFileVersionInfoW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, ctypes.c_void_p]
    api.VerQueryValueW.argtypes = [ctypes.c_void_p, w.LPCWSTR, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(w.UINT)]
    size = api.GetFileVersionInfoSizeW(str(path), None)
    if not size: return None
    data = ctypes.create_string_buffer(size)
    if not api.GetFileVersionInfoW(str(path), 0, size, data): return None
    pointer, length = ctypes.c_void_p(), w.UINT()
    if not api.VerQueryValueW(data, "\\", ctypes.byref(pointer), ctypes.byref(length)) or length.value < 16: return None
    values = ctypes.cast(pointer, ctypes.POINTER(w.DWORD))
    return (values[2] >> 16, values[2] & 0xFFFF, values[3] >> 16, values[3] & 0xFFFF)


class Overlay:
    def __init__(self, target):
        self.root = Path(target).resolve()
        if not self.root.is_dir():
            raise ValueError("target directory does not exist")

    def path(self, relative):
        if relative not in (STATE, PENDING) and relative not in {OWN + name for name in (
            "ai_catalog.dll", "mod.ini", "catalog.bin")} and not re.fullmatch(
                r"sdgo/(?:mods/(?:coop_ai|pvp_ai)/(?:coop_ai|pvp_ai)\.dll|symbols/[^/\\]+\.ini)", relative):
            raise ValueError("installation record contains an unmanaged path")
        path = self.root / relative
        if not path.resolve().is_relative_to(self.root):
            raise ValueError("installation path leaves the selected target")
        for parent in (path, *path.parents):
            if parent == self.root:
                break
            if parent.is_symlink() or (hasattr(parent, "is_junction") and parent.is_junction()):
                raise ValueError("installation path contains a link or junction")
        return path

    def read(self, relative):
        path = self.path(relative)
        return path.read_bytes() if path.is_file() else None

    def write(self, relative, data):
        path = self.path(relative)
        if data is None:
            if path.exists(): path.unlink()
            return
        path.parent.mkdir(parents=True, exist_ok=True)
        descriptor, name = tempfile.mkstemp(prefix=".ai-catalog-", dir=path.parent)
        try:
            with os.fdopen(descriptor, "wb") as stream:
                stream.write(data); stream.flush(); os.fsync(stream.fileno())
            os.replace(name, path)
        finally:
            if os.path.exists(name): os.unlink(name)

    def recover(self):
        pending = self.read(PENDING)
        if pending is None: return False
        value = json.loads(pending)
        for relative, change in value["changes"].items():
            before = decode(change["before"])
            if digest(self.read(relative)) not in (digest(before), change["after"]):
                raise ValueError("interrupted installation conflicts with another change; no automatic recovery")
        for relative, change in value["changes"].items():
            self.write(relative, decode(change["before"]))
        self.write(STATE, decode(value["previous_state"]))
        self.write(PENDING, None)
        return True

    def transact(self, changes, state):
        old_state = self.read(STATE)
        pending = {"schema_version": 1, "previous_state": encode(old_state), "changes": {
            relative: {"before": encode(self.read(relative)), "after": digest(data)} for relative, data in changes.items()}}
        self.write(PENDING, (json.dumps(pending) + "\n").encode())
        try:
            for relative, data in changes.items():
                self.write(relative, data)
                if self.read(relative) != data: raise OSError("installation readback failed")
            self.write(STATE, None if state is None else (json.dumps(state, indent=2) + "\n").encode())
            self.write(PENDING, None)
        except BaseException:
            self.recover()
            raise

    def state(self):
        data = self.read(STATE)
        if data is None: return None
        value = json.loads(data)
        if value.get("schema_version") != 1 or value.get("target") != str(self.root):
            raise ValueError("installation receipt does not describe this target")
        for relative, item in value["files"].items():
            if digest(self.read(relative)) != item["after"]:
                raise ValueError(f"managed file changed since installation: {relative}; disable the mod without restoring other files")
        return value


def manifest_enabled(data, enabled):
    text = data.decode("utf-8-sig")
    parsed = ini(data)
    if parsed.get("mod", "id", fallback="") != "ai_catalog":
        raise ValueError("unexpected mod manifest")
    match = re.search(r"(?ims)^\[mod\]\s*\r?\n(.*?)(?=^\[|\Z)", text)
    if not match: raise ValueError("mod section missing")
    section = match.group(1)
    updated, count = re.subn(r"(?im)^enabled\s*=.*$", "enabled=" + str(int(enabled)), section)
    if not count: updated = "enabled=" + str(int(enabled)) + "\n" + section
    return (text[:match.start(1)] + updated + text[match.end(1):]).encode("utf-8")


def merge_symbols(existing, source, role, stamp, size):
    donor = ini(source)
    if int(donor["host"]["timestamp"], 0) != stamp or int(donor["host"]["size_of_image"], 0) != size:
        raise ValueError("package symbol file is for a different host")
    current = ini(existing) if existing is not None else None
    if current is not None and (int(current["host"]["timestamp"], 0), int(current["host"]["size_of_image"], 0)) != (stamp, size):
        raise ValueError("installed symbol file describes another host")
    text = existing.decode("utf-8-sig") if existing is not None else (
        f"[host]\ntimestamp=0x{stamp:08X}\nsize_of_image=0x{size:08X}\n")
    for section in SECTIONS[role]:
        if section not in donor: raise ValueError("package is missing a required catalog symbol")
        if current is not None and section in current:
            if dict(current[section]) != dict(donor[section]):
                raise ValueError(f"installed symbol conflicts with package: {section}")
        else:
            text += "\n[" + section + "]\n" + "".join(key + "=" + value + "\n" for key, value in donor[section].items())
    return text.encode("utf-8")


def install(overlay, role, source, catalog, host_exe=None):
    from ai_catalog import HEADER, MAGIC, RECORD, fnv
    executable = overlay.root / (host_exe or HOSTS[role][0])
    if executable.parent != overlay.root or executable.suffix.lower() != ".exe":
        raise ValueError("host-exe must be an executable basename")
    key, stamp, size = host_info(executable, role)
    if running(executable): raise ValueError("selected host is running; close that copy before installing")
    if not (overlay.root / "sdgo/core.dll").is_file():
        raise ValueError("install the mod framework in this target first")
    binary = Path(catalog).read_bytes()
    if len(binary) < HEADER.size: raise ValueError("catalog header missing")
    magic, version, row_size, count, checksum = HEADER.unpack_from(binary)
    if magic != MAGIC or version != 1 or row_size != RECORD.size or not 0 < count <= 3000 or (
            len(binary) != HEADER.size + count * row_size or fnv(binary[HEADER.size:], 32) != checksum):
        raise ValueError("invalid catalog length or checksum")
    source = Path(source).resolve()
    previous = overlay.state()
    if previous is not None and (previous["role"] != role or previous["host_key"] != key):
        raise ValueError("receipt role or host changed")
    role_source = source / role
    own_source = role_source / "sdgo/mods/ai_catalog"
    enabled = True if previous is None else ini(overlay.read(OWN + "mod.ini")).getint("mod", "enabled", fallback=1) != 0
    manifest = manifest_enabled((own_source / "mod.ini").read_bytes(), enabled)
    # Renamed local copies get their own exact host key, without changing source defaults.
    manifest = re.sub(rb"(?m)^hosts=.*$", b"hosts=" + key.encode("ascii"), manifest)
    symbol_key = f"{Path(HOSTS[role][0]).stem}-{stamp:08X}-{size:08X}.ini"
    relative_symbol = f"sdgo/symbols/{key}.ini"
    changes = {OWN + "ai_catalog.dll": (own_source / "ai_catalog.dll").read_bytes(),
               OWN + "mod.ini": manifest, OWN + "catalog.bin": binary,
               relative_symbol: merge_symbols(overlay.read(relative_symbol),
                    (role_source / "sdgo/symbols" / symbol_key).read_bytes(), role, stamp, size)}
    if role == "server":
        bridges = 0
        for mod in ("coop_ai", "pvp_ai"):
            if (overlay.root / f"sdgo/mods/{mod}/mod.ini").is_file():
                relative = f"sdgo/mods/{mod}/{mod}.dll"
                installed_version = file_version(overlay.root / relative)
                package_version = file_version(role_source / relative)
                if installed_version is not None and package_version is not None and installed_version > package_version:
                    raise ValueError(f"refusing to downgrade a newer {mod} DLL")
                changes[relative] = (role_source / relative).read_bytes(); bridges += 1
        if not bridges: raise ValueError("install coop_ai or pvp_ai in this battle server first")
    if previous is None and any(overlay.read(OWN + filename) is not None for filename in ("mod.ini", "ai_catalog.dll", "catalog.bin")):
        stock = overlay.read(OWN + "mod.ini")
        if (stock is None or ini(stock).get("mod", "id", fallback="") != "ai_catalog" or
            ini(stock).getint("mod", "enabled", fallback=1) != 0 or overlay.read(OWN + "catalog.bin") is not None or
            overlay.read(OWN + "ai_catalog.dll") != changes[OWN + "ai_catalog.dll"]):
            raise ValueError("an unmanaged catalog installation already exists")
    files = dict(previous["files"]) if previous else {}
    for relative, data in changes.items():
        before = files.get(relative, {}).get("before", encode(overlay.read(relative)))
        files[relative] = {"before": before, "after": digest(data)}
    state = {"schema_version": 1, "target": str(overlay.root), "role": role, "host_key": key,
             "host_exe": executable.name, "catalog_sha256": digest(binary), "files": files}
    if previous == state and all(overlay.read(relative) == data for relative, data in changes.items()): return "unchanged"
    overlay.transact(changes, state)
    return "installed"


def toggle(overlay, enabled):
    # Disable remains available even if another updater changed a bridge/symbol.
    raw = overlay.read(STATE)
    if raw is None: raise ValueError("no managed installation exists")
    state = json.loads(raw)
    if state.get("target") != str(overlay.root) or state.get("schema_version") != 1:
        raise ValueError("receipt target mismatch")
    if running(overlay.root / state["host_exe"]): raise ValueError("selected host is running; restart is required for toggles")
    if enabled:
        overlay.state()
        global_config = overlay.root / "sdgo/sdgo.ini"
        if global_config.exists():
            config = ini(global_config.read_bytes())
            for section, key in (("mods", "ai_catalog"), ("features", "ai-catalog"), ("categories", "gameplay")):
                if config.getint(section, key, fallback=1) == 0:
                    raise ValueError(f"sdgo.ini has [{section}] {key}=0; remove that override before enabling")
    relative = OWN + "mod.ini"
    current = overlay.read(relative)
    if digest(current) != state["files"][relative]["after"]:
        raise ValueError("catalog manifest changed outside the installer")
    changed = manifest_enabled(current, enabled)
    if changed == current: return "unchanged"
    state["files"][relative]["after"] = digest(changed)
    overlay.transact({relative: changed}, state)
    return "enabled" if enabled else "disabled"


def uninstall(overlay):
    state = overlay.state()
    if state is None: return "unchanged"
    if running(overlay.root / state["host_exe"]): raise ValueError("selected host is running; close that copy before uninstalling")
    changes = {relative: decode(item["before"]) for relative, item in state["files"].items()}
    overlay.transact(changes, None)
    # Empty owned directories only; user-created files are preserved.
    for relative in ("sdgo/mods/ai_catalog", "sdgo/install-state"):
        path = overlay.root / relative
        try: path.rmdir()
        except OSError: pass
    return "uninstalled"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("install", "enable", "disable", "uninstall", "recover"))
    parser.add_argument("--target", required=True, type=Path)
    parser.add_argument("--role", choices=tuple(HOSTS))
    parser.add_argument("--source", type=Path, help="release package root or build/stage/<Config>")
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--host-exe", help="optional renamed host executable basename")
    args = parser.parse_args(argv)
    try:
        overlay = Overlay(args.target)
        if args.action == "recover":
            result = "recovered" if overlay.recover() else "unchanged"
        else:
            if overlay.read(PENDING) is not None:
                raise ValueError("an interrupted operation needs the recover action first")
            if args.action == "install":
                if not args.role or args.source is None or args.catalog is None:
                    raise ValueError("install needs --role, --source and --catalog")
                result = install(overlay, args.role, args.source, args.catalog, args.host_exe)
            elif args.action == "uninstall": result = uninstall(overlay)
            else: result = toggle(overlay, args.action == "enable")
    except (OSError, ValueError, KeyError, configparser.Error, struct.error) as error:
        print(f"AI catalog install: {error}", file=sys.stderr); return 1
    print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
