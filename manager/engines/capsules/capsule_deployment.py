"""Build and apply a bounded capsule-catalog deployment.

The browser-facing UI supplies only a catalog revision.  All filesystem and
database targets are fixed when :class:`DeploymentTarget` is constructed.
This module therefore never accepts a target path from an apply request.

The generated package is safe to hand to another operator: it contains the
framework client files needed by the capsule Mod, compiled catalog resources,
and generated server SQL.  It does not contain database credentials, player
data, local test helpers, or machine-specific absolute paths.
"""
from __future__ import annotations

import configparser
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import time
import uuid


PRODUCT_NAME = "SDGO mod framework"
PACKAGE_BUILDER_VERSION = 1
HOST_TIMESTAMP = 0x54BE1568
HOST_SIZE_OF_IMAGE = 0x00C97000
HOST_SYMBOL_NAME = "GOnline-54BE1568-00C97000.ini"
SERVER_PERMISSIONS = (
    "CanCreateTable",
    "CanCreateProcedure",
    "CanAlterSchema",
    "CanSelectSchema",
    "CanInsertSchema",
    "CanUpdateSchema",
    "CanDeleteSchema",
    "CanExecuteSchema",
    "CanViewDefinition",
)
CAPSULE_SYMBOLS = (
    "capsule.main.open_file",
    "capsule.main.file_ctor",
    "capsule.main.operator_new",
    "capsule.main.wfopen",
    "capsule.grid_verify_checksum",
    "capsule.grid_load_all",
)
SUPPLEMENTAL_COMPILED_FILES = (
    "runtime.ini",
    "placements.json",
    "overlay-index.json",
    "manifest.json",
)
FORBIDDEN_SQL_TOKENS = (
    "tbl_user",
    "tbl_userunits",
    "tbl_usercustomcapsulerecord",
)


class ClientRunningError(RuntimeError):
    """The configured client target is currently in use."""


class ServerDeploymentError(RuntimeError):
    """A sanitized server-side deployment failure."""


def _absolute(path):
    return Path(os.path.abspath(os.fspath(path)))


def _sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _json_bytes(value):
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            indent=2,
            allow_nan=False,
        )
        + "\n"
    ).encode("utf-8")


def _read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def _is_reparse(path):
    path = Path(path)
    if path.is_symlink():
        return True
    try:
        attributes = os.lstat(path).st_file_attributes
    except (AttributeError, FileNotFoundError):
        return False
    return bool(attributes & 0x400)


def _assert_existing_components_no_reparse(path):
    path = _absolute(path)
    parts = path.parts
    if not parts:
        raise ValueError("Empty filesystem path")
    current = Path(parts[0])
    for part in parts[1:]:
        current /= part
        if current.exists() or current.is_symlink():
            if _is_reparse(current):
                raise ValueError(f"Refusing reparse-point path: {current}")
        else:
            break
    return path


def _assert_tree_no_reparse(root):
    root = _assert_existing_components_no_reparse(root)
    if not root.is_dir():
        raise ValueError(f"Expected directory: {root}")
    stack = [root]
    while stack:
        directory = stack.pop()
        with os.scandir(directory) as entries:
            for entry in entries:
                entry_path = Path(entry.path)
                if entry.is_symlink() or _is_reparse(entry_path):
                    raise ValueError(f"Refusing reparse point in deployment tree: {entry_path}")
                if entry.is_dir(follow_symlinks=False):
                    stack.append(entry_path)


def _safe_relative(value):
    raw = str(value).replace("\\", "/")
    if not raw or raw.startswith("/") or re.match(r"^[A-Za-z]:", raw):
        raise ValueError(f"Unsafe relative path: {value}")
    parts = raw.split("/")
    if any(part in ("", ".", "..") for part in parts):
        raise ValueError(f"Unsafe relative path: {value}")
    return Path(*parts)


def _safe_join(root, relative):
    root = _absolute(root)
    relative = _safe_relative(relative)
    path = root / relative
    try:
        common = os.path.commonpath((str(root), str(path)))
    except ValueError as error:
        raise ValueError(f"Unsafe relative path: {relative}") from error
    if os.path.normcase(common) != os.path.normcase(str(root)):
        raise ValueError(f"Path escapes deployment root: {relative}")
    _assert_existing_components_no_reparse(path)
    return path


def _atomic_write(path, data):
    path = _absolute(path)
    _assert_existing_components_no_reparse(path.parent)
    path.parent.mkdir(parents=True, exist_ok=True)
    _assert_existing_components_no_reparse(path.parent)
    pending = path.with_name(f"{path.name}.{uuid.uuid4().hex}.pending")
    with pending.open("xb") as handle:
        handle.write(data)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(pending, path)


def _rename_directory(source, destination):
    source = Path(source)
    destination = Path(destination)
    last_error = None
    for attempt in range(20):
        try:
            source.rename(destination)
            return True
        except FileExistsError:
            return False
        except PermissionError as error:
            if destination.exists():
                return False
            last_error = error
            time.sleep(0.05 * (attempt + 1))
    raise last_error


def _copy_tree_verified(source, destination):
    source = _absolute(source)
    destination = _absolute(destination)
    _assert_tree_no_reparse(source)
    if destination.exists() or destination.is_symlink():
        raise ValueError(f"Deployment destination already exists: {destination}")
    destination.mkdir(parents=True)
    for path in sorted(source.rglob("*"), key=lambda item: item.as_posix()):
        if _is_reparse(path):
            raise ValueError(f"Refusing reparse point: {path}")
        relative = path.relative_to(source)
        target = destination / relative
        if path.is_dir():
            target.mkdir()
        elif path.is_file():
            _atomic_write(target, path.read_bytes())
        else:
            raise ValueError(f"Unsupported deployment entry: {path}")


def _tree_hashes(root):
    root = _absolute(root)
    _assert_tree_no_reparse(root)
    return {
        path.relative_to(root).as_posix(): _sha256_file(path)
        for path in sorted(root.rglob("*"), key=lambda item: item.as_posix())
        if path.is_file()
    }


def _parse_pe(path):
    path = Path(path)
    data = path.read_bytes()
    if len(data) < 0x100 or data[:2] != b"MZ":
        raise ValueError("GOnline.exe is not a valid PE image")
    pe_offset = struct.unpack_from("<I", data, 0x3C)[0]
    if pe_offset < 0x40 or pe_offset + 0x60 > len(data):
        raise ValueError("GOnline.exe has an invalid PE header offset")
    if data[pe_offset : pe_offset + 4] != b"PE\0\0":
        raise ValueError("GOnline.exe has no PE signature")
    machine = struct.unpack_from("<H", data, pe_offset + 4)[0]
    timestamp = struct.unpack_from("<I", data, pe_offset + 8)[0]
    optional_size = struct.unpack_from("<H", data, pe_offset + 20)[0]
    optional = pe_offset + 24
    if machine != 0x014C or optional_size < 0x3C:
        raise ValueError("GOnline.exe must be a 32-bit x86 PE image")
    if struct.unpack_from("<H", data, optional)[0] != 0x010B:
        raise ValueError("GOnline.exe must use the PE32 optional header")
    size_of_image = struct.unpack_from("<I", data, optional + 0x38)[0]
    if timestamp != HOST_TIMESTAMP or size_of_image != HOST_SIZE_OF_IMAGE:
        raise ValueError(
            "Unsupported GOnline.exe build; expected timestamp "
            f"0x{HOST_TIMESTAMP:08X} and SizeOfImage 0x{HOST_SIZE_OF_IMAGE:08X}"
        )
    return {
        "machine": machine,
        "timestamp": timestamp,
        "size_of_image": size_of_image,
        "sha256": _sha256_bytes(data),
    }


def _product_name(path):
    """Read Windows VersionInfo without loading the DLL into this process."""
    if os.name != "nt":
        return None
    script = (
        "& { param([string]$FilePath) "
        "$ErrorActionPreference='Stop'; "
        "$value=(Get-Item -LiteralPath $FilePath).VersionInfo.ProductName; "
        "if ($null -ne $value) { [Console]::Out.Write($value) } }"
    )
    result = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            script,
            str(_absolute(path)),
        ],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=20,
    )
    if result.returncode:
        raise OSError(f"Unable to read DLL version information: {Path(path).name}")
    return result.stdout.strip() or None


def _running_gonline_paths():
    """Return exact executable paths for running GOnline.exe processes."""
    if os.name != "nt":
        return []
    script = (
        "$ErrorActionPreference='Stop'; "
        "@(Get-CimInstance Win32_Process -Filter \"Name='GOnline.exe'\" | "
        "Where-Object { $_.ExecutablePath } | "
        "ForEach-Object { $_.ExecutablePath }) | ConvertTo-Json -Compress"
    )
    result = subprocess.run(
        [
            "powershell.exe",
            "-NoLogo",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            script,
        ],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=20,
    )
    if result.returncode:
        raise OSError("Unable to verify whether the configured GOnline.exe is running")
    text = result.stdout.strip()
    if not text:
        return []
    value = json.loads(text)
    if isinstance(value, str):
        return [value]
    if value is None:
        return []
    if not isinstance(value, list) or not all(isinstance(item, str) for item in value):
        raise OSError("Unexpected process inventory result")
    return value


def _assert_client_image_unmapped(executable):
    """Probe the exact image without writing bytes or needing process access."""
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [
        wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
        wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE,
    ]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    # An image mapped by an elevated process may disappear from the process
    # inventory. Windows still rejects a writable handle to that exact image.
    handle = kernel.CreateFileW(str(executable), 0x40000000, 7, None, 3, 128, None)
    if handle == ctypes.c_void_p(-1).value:
        error = ctypes.get_last_error()
        if error in (32, 33):
            raise ClientRunningError(
                f"GOnline.exe in {Path(executable).parent} is running; "
                "close that client before applying"
            )
        raise OSError(error, "Unable to verify that the target client image is closed")
    kernel.CloseHandle(handle)


def _assert_client_stopped(target):
    expected = os.path.normcase(os.path.abspath(str(Path(target) / "GOnline.exe")))
    running = {
        os.path.normcase(os.path.abspath(value))
        for value in _running_gonline_paths()
        if value
    }
    if expected in running:
        raise ClientRunningError(
            f"GOnline.exe in {target} is running; close that client before applying"
        )
    _assert_client_image_unmapped(Path(target) / "GOnline.exe")
    mod_image = Path(target) / "sdgo/mods/capsule_catalog/capsule_catalog.dll"
    if mod_image.exists():
        try:
            _assert_client_image_unmapped(mod_image)
        except ClientRunningError:
            raise ClientRunningError(
                f"The capsule Mod in {target} is running; close that client before applying"
            ) from None


def _decode_text(raw, label):
    bom = raw.startswith(b"\xef\xbb\xbf")
    try:
        return raw.decode("utf-8-sig"), bom
    except UnicodeDecodeError as error:
        raise ValueError(f"{label} must be UTF-8 text") from error


def _encode_text(text, bom):
    data = text.encode("utf-8")
    return (b"\xef\xbb\xbf" + data) if bom else data


def _ini_values(raw, label):
    text, _ = _decode_text(raw, label)
    sections = {}
    current = None
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith((";", "#")):
            continue
        if stripped.startswith("[") and stripped.endswith("]"):
            name = stripped[1:-1].strip().lower()
            if not name or name in sections:
                raise ValueError(f"{label} has a duplicate or empty section")
            sections[name] = {}
            current = name
            continue
        if "=" in stripped and current is not None:
            key, value = stripped.split("=", 1)
            key = key.strip().lower()
            if not key or key in sections[current]:
                raise ValueError(f"{label} has a duplicate or empty key on line {number}")
            sections[current][key] = value.strip()
    return sections


def _set_ini_value(raw, section, key, value, label):
    if any(char in section + key + value for char in "\r\n[]"):
        raise ValueError("Unsafe INI value")
    text, bom = _decode_text(raw, label)
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines(keepends=True)
    section_key = section.lower()
    key_key = key.lower()
    section_indices = []
    for index, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            if stripped[1:-1].strip().lower() == section_key:
                section_indices.append(index)
    if len(section_indices) > 1:
        raise ValueError(f"{label} has duplicate [{section}] sections")
    if not section_indices:
        if text and not text.endswith(("\n", "\r")):
            lines[-1] += newline
        if lines and lines[-1].strip():
            lines.append(newline)
        lines.extend((f"[{section}]{newline}", f"{key}={value}{newline}"))
        return _encode_text("".join(lines), bom)

    start = section_indices[0]
    end = len(lines)
    for index in range(start + 1, len(lines)):
        stripped = lines[index].strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            end = index
            break
    matches = []
    for index in range(start + 1, end):
        stripped = lines[index].strip()
        if not stripped or stripped.startswith((";", "#")) or "=" not in stripped:
            continue
        candidate = stripped.split("=", 1)[0].strip().lower()
        if candidate == key_key:
            matches.append(index)
    if len(matches) > 1:
        raise ValueError(f"{label} has duplicate [{section}] {key} keys")
    if matches:
        terminator = "\r\n" if lines[matches[0]].endswith("\r\n") else (
            "\n" if lines[matches[0]].endswith("\n") else newline
        )
        lines[matches[0]] = f"{key}={value}{terminator}"
    else:
        lines.insert(end, f"{key}={value}{newline}")
    return _encode_text("".join(lines), bom)


def _merge_mod_ini(package_raw, existing_raw):
    if existing_raw is None:
        return package_raw
    old = _ini_values(existing_raw, "existing capsule mod.ini")
    merged = package_raw
    enabled = old.get("mod", {}).get("enabled")
    if enabled is not None:
        merged = _set_ini_value(
            merged, "mod", "enabled", enabled, "package capsule mod.ini"
        )
    for key, value in old.get("settings", {}).items():
        merged = _set_ini_value(
            merged, "settings", key, value, "package capsule mod.ini"
        )
    return merged


def _parse_symbol_sections(raw, label):
    text, _ = _decode_text(raw, label)
    pattern = re.compile(r"(?m)^[ \t]*\[([^\]\r\n]+)\][ \t]*(?:\r?\n|$)")
    matches = list(pattern.finditer(text))
    if not matches:
        raise ValueError(f"{label} has no INI sections")
    sections = {}
    order = []
    for index, match in enumerate(matches):
        name = match.group(1).strip()
        key = name.lower()
        if not name or key in sections:
            raise ValueError(f"{label} has a duplicate or empty section")
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        block = text[match.start() : end]
        values = {}
        entries = []
        for line in block.splitlines()[1:]:
            stripped = line.strip()
            if not stripped or stripped.startswith((";", "#")):
                continue
            if "=" not in stripped:
                raise ValueError(f"{label} section [{name}] has invalid content")
            item_key, item_value = stripped.split("=", 1)
            item_key = item_key.strip()
            normalized = item_key.lower()
            if not item_key or normalized in values:
                raise ValueError(f"{label} section [{name}] has a duplicate key")
            item_value = item_value.strip()
            values[normalized] = item_value
            entries.append((item_key, item_value))
        sections[key] = {
            "name": name,
            "start": match.start(),
            "end": end,
            "block": block,
            "values": values,
            "entries": entries,
        }
        order.append(key)
    return {
        "text": text,
        "preamble": text[: matches[0].start()],
        "sections": sections,
        "order": order,
    }


def _render_symbol_section(section):
    lines = [f"[{section['name']}]"]
    lines.extend(f"{key}={value}" for key, value in section["entries"])
    return "\n".join(lines) + "\n"


def _build_package_symbols(stage_symbol, fragment):
    stage = _parse_symbol_sections(
        Path(stage_symbol).read_bytes(), "staged host symbol file"
    )
    additions = _parse_symbol_sections(
        Path(fragment).read_bytes(), "capsule symbol fragment"
    )
    required = ("host", "grid.data", "grid.find")
    missing = [name for name in required if name not in stage["sections"]]
    if missing:
        raise ValueError(f"Staged symbol file lacks required sections: {', '.join(missing)}")
    if set(additions["sections"]) != set(CAPSULE_SYMBOLS):
        raise ValueError("Capsule symbol fragment does not contain the exact runtime symbols")
    selected = [stage["sections"][name] for name in required]
    selected.extend(additions["sections"][name] for name in CAPSULE_SYMBOLS)
    text = (
        "; Minimal symbols for the capsule catalog runtime.\n"
        "; Existing unrelated symbol sections are preserved during installation.\n\n"
        + "\n".join(_render_symbol_section(section).rstrip() for section in selected)
        + "\n"
    )
    return text.encode("utf-8")


def _build_player_symbol_fragment(stage_symbol, fragment):
    stage = _parse_symbol_sections(
        Path(stage_symbol).read_bytes(), "staged host symbol file"
    )
    additions = _parse_symbol_sections(
        Path(fragment).read_bytes(), "capsule symbol fragment"
    )
    if "host" not in stage["sections"]:
        raise ValueError("Staged symbol file lacks the [host] section")
    if set(additions["sections"]) != set(CAPSULE_SYMBOLS):
        raise ValueError("Capsule symbol fragment does not contain the exact runtime symbols")
    selected = [stage["sections"]["host"]]
    selected.extend(additions["sections"][name] for name in CAPSULE_SYMBOLS)
    return (
        "; Capsule-owned symbol update for the standalone client installer.\n\n"
        + "\n".join(_render_symbol_section(section).rstrip() for section in selected)
        + "\n"
    ).encode("utf-8")


def _merge_symbols(existing_raw, package_raw):
    if existing_raw is None:
        return package_raw
    existing = _parse_symbol_sections(existing_raw, "existing host symbol file")
    package = _parse_symbol_sections(package_raw, "package host symbol file")
    replacements = {}
    missing = []
    for name in package["order"]:
        incoming = package["sections"][name]
        current = existing["sections"].get(name)
        if current is None:
            missing.append(incoming)
        elif name.startswith("capsule."):
            replacements[name] = _render_symbol_section(incoming)
        elif current["values"] != incoming["values"]:
            raise ValueError(f"Existing non-capsule symbol section conflicts: [{incoming['name']}]")

    pieces = [existing["preamble"]]
    for name in existing["order"]:
        section = existing["sections"][name]
        pieces.append(replacements.get(name, section["block"]))
    result = "".join(pieces)
    if missing:
        if result and not result.endswith(("\n", "\r")):
            result += "\n"
        if result and not result.endswith("\n\n"):
            result += "\n"
        result += "\n".join(
            _render_symbol_section(section).rstrip() for section in missing
        )
        result += "\n"
    return result.encode("utf-8")


def _validate_semantic_bundle(bundle_root, catalog, revision):
    from .capsule_catalog import read_json

    client_path = Path(bundle_root) / "client.json"
    server_path = Path(bundle_root) / "server.json"
    manifest_path = Path(bundle_root) / "manifest.json"
    client = read_json(client_path)
    server = read_json(server_path)
    manifest = read_json(manifest_path)
    if client.get("role") != "client" or server.get("role") != "server":
        raise ValueError("Semantic bundle roles are invalid")
    if any(
        value.get("catalog_revision") != revision
        for value in (client, server, manifest)
    ):
        raise ValueError("Semantic bundle catalog revisions differ")
    client_body = dict(client)
    server_body = dict(server)
    client_body.pop("role", None)
    server_body.pop("role", None)
    if client_body != server_body:
        raise ValueError("Client and server semantic payloads differ")
    for name, path in (("client.json", client_path), ("server.json", server_path)):
        if manifest.get("files", {}).get(name) != _sha256_file(path):
            raise ValueError(f"Semantic bundle hash mismatch: {name}")
    if client_body.get("fixed_machines") != catalog.get("fixed_machines"):
        raise ValueError("Semantic bundle differs from the requested catalog")
    return client_path, server_path, manifest_path


def _parse_runtime_ini(path):
    parser = configparser.RawConfigParser(interpolation=None, strict=True)
    parser.optionxform = str.lower
    try:
        with Path(path).open("r", encoding="ascii", newline="") as handle:
            parser.read_file(handle)
    except (UnicodeDecodeError, configparser.Error) as error:
        raise ValueError("Compiled runtime.ini is invalid") from error
    if not parser.has_section("runtime") or parser.get("runtime", "schema", fallback="") != "1":
        raise ValueError("Compiled runtime.ini has an unsupported schema")
    return parser


def _validate_compiled(runtime_output, revision):
    from .capsule_client_compile import COMPILER_VERSION

    runtime_output = _absolute(runtime_output)
    _assert_tree_no_reparse(runtime_output)
    manifest_path = _safe_join(runtime_output, "manifest.json")
    placements_path = _safe_join(runtime_output, "placements.json")
    runtime_ini_path = _safe_join(runtime_output, "runtime.ini")
    overlay_index_path = _safe_join(runtime_output, "overlay-index.json")
    for path in (manifest_path, placements_path, runtime_ini_path, overlay_index_path):
        if not path.is_file():
            raise ValueError(f"Compiled runtime is missing {path.name}")
    manifest = _read_json(manifest_path)
    if manifest.get("compiler_version") != COMPILER_VERSION:
        raise ValueError(
            f"Recompile the client runtime with compiler version {COMPILER_VERSION}"
        )
    placements = _read_json(placements_path)
    runtime = _parse_runtime_ini(runtime_ini_path)
    if manifest.get("catalog_revision") != revision:
        raise ValueError("Compiled manifest catalog revision differs")
    if placements.get("catalog_revision") != revision:
        raise ValueError("Compiled placements catalog revision differs")
    if runtime.get("runtime", "catalog_revision", fallback="") != revision:
        raise ValueError("Compiled runtime.ini catalog revision differs")
    if not isinstance(placements.get("machines"), list):
        raise ValueError("Compiled placements are missing")
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        raise ValueError("Compiled manifest has no resource hashes")
    required = {
        "overlay.zdx",
        "overlay.zpk",
        "data/0033.gdt",
        "data/0101.gdt",
        "data/0154.gdt",
        "data/0155.gdt",
        "ui/1161.uir",
    }
    if not required <= set(files):
        raise ValueError("Compiled manifest lacks required capsule resources")
    hashes = {}
    for relative, expected in files.items():
        if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
            raise ValueError(f"Compiled manifest has an invalid hash: {relative}")
        path = _safe_join(runtime_output, relative)
        if not path.is_file() or _sha256_file(path) != expected:
            raise ValueError(f"Compiled runtime hash mismatch: {relative}")
        hashes[Path(relative).as_posix()] = expected
    overlay_zdx = runtime.get("runtime", "overlay_zdx_sha256", fallback="")
    overlay_zpk = runtime.get("runtime", "overlay_zpk_sha256", fallback="")
    if overlay_zdx != hashes["overlay.zdx"] or overlay_zpk != hashes["overlay.zpk"]:
        raise ValueError("Compiled runtime.ini overlay hashes differ")
    for table in ("0033", "0101", "0154", "0155"):
        expected_path = f"data/{table}.gdt"
        configured = runtime.get("gdt", table, fallback="").replace("\\", "/")
        configured_hash = runtime.get("gdt", f"{table}_sha256", fallback="")
        if configured != expected_path or configured_hash != hashes[expected_path]:
            raise ValueError(f"Compiled runtime.ini GDT entry differs: {table}")
    if (runtime.get("ui", "1161", fallback="").replace("\\", "/") != "ui/1161.uir"
        or runtime.get("ui", "1161_sha256", fallback="") != hashes["ui/1161.uir"]):
        raise ValueError("Compiled runtime.ini capsule UI entry differs")
    for relative in SUPPLEMENTAL_COMPILED_FILES:
        path = _safe_join(runtime_output, relative)
        hashes[relative] = _sha256_file(path)
    return manifest, placements, hashes


def _validate_server_runtime(server_output, server_bundle, revision):
    server_output = _absolute(server_output)
    _assert_tree_no_reparse(server_output)
    manifest = _read_json(_safe_join(server_output, "manifest.json"))
    if manifest.get("catalog_revision") != revision:
        raise ValueError("Generated server runtime catalog revision differs")
    if manifest.get("source_bundle_sha256") != _sha256_file(server_bundle):
        raise ValueError("Generated server runtime source bundle hash differs")
    files = manifest.get("files")
    if set(files or {}) != {"install.sql", "verify.sql", "rollback.sql"}:
        raise ValueError("Generated server runtime file set is invalid")
    for relative, expected in files.items():
        path = _safe_join(server_output, relative)
        if not path.is_file() or _sha256_file(path) != expected:
            raise ValueError(f"Generated server runtime hash mismatch: {relative}")
        text = path.read_text(encoding="utf-8")
        lowered = text.lower()
        if any(token in lowered for token in FORBIDDEN_SQL_TOKENS):
            raise ValueError(f"Generated SQL exceeds capsule configuration scope: {relative}")
        if re.search(r"(?im)^\s*go\s*(?:--.*)?$", text):
            raise ValueError(f"Generated SQL unexpectedly contains a GO batch: {relative}")
    return manifest


def _validate_package(package_root, revision):
    package_root = _absolute(package_root)
    _assert_tree_no_reparse(package_root)
    manifest_path = _safe_join(package_root, "manifest.json")
    manifest = _read_json(manifest_path)
    if manifest.get("catalog_revision") != revision:
        raise ValueError("Deployment package catalog revision differs")
    if not re.fullmatch(r"[0-9a-f]{64}", str(manifest.get("source_profile", ""))):
        raise ValueError("Deployment package source profile is invalid")
    files = manifest.get("files")
    if not isinstance(files, dict) or not files:
        raise ValueError("Deployment package manifest has no files")
    for relative, expected in files.items():
        path = _safe_join(package_root, relative)
        if not path.is_file() or _sha256_file(path) != expected:
            raise ValueError(f"Deployment package hash mismatch: {relative}")
    actual = {
        path.relative_to(package_root).as_posix()
        for path in package_root.rglob("*")
        if path.is_file() and path != manifest_path
    }
    if actual != set(files):
        raise ValueError("Deployment package contains unmanifested files")
    forbidden = ("password", "pwd", "account", "capsule_local_test_env")
    for relative in actual:
        lowered = relative.lower()
        if any(token in lowered for token in forbidden):
            raise ValueError(f"Unsafe file in distributable package: {relative}")
    player_manifest_path = _safe_join(
        package_root, "player/package-manifest.json"
    )
    player_fragment_path = _safe_join(
        package_root, "player/symbol-fragment.ini"
    )
    player_manifest = _read_json(player_manifest_path)
    if (
        player_manifest.get("schema_version") != 1
        or player_manifest.get("package_id") != "capsule-catalog"
        or player_manifest.get("catalog_revision") != revision
        or player_manifest.get("source_profile") != manifest["source_profile"]
    ):
        raise ValueError("Standalone player manifest identity differs")
    if (
        player_manifest.get("symbol_fragment_sha256")
        != _sha256_file(player_fragment_path)
    ):
        raise ValueError("Standalone player symbol fragment hash differs")
    player_files = player_manifest.get("files")
    expected_player_files = {
        relative.removeprefix("player/client/"): expected
        for relative, expected in files.items()
        if relative.startswith("player/client/")
    }
    if player_files != expected_player_files:
        raise ValueError("Standalone player manifest file hashes differ")
    return manifest


def _write_package_file(root, relative, data, hashes):
    path = _safe_join(root, relative)
    _atomic_write(path, data)
    hashes[Path(relative).as_posix()] = _sha256_bytes(data)


def _build_package(
    bundle_root,
    revision,
    client_bundle,
    server_bundle,
    runtime_output,
    compiled_hashes,
    server_output,
    framework_stage,
    symbol_fragment,
):
    stage_files = {
        "d3d9.dll": "player/client/d3d9.dll",
        "sdgo/core.dll": "player/client/sdgo/core.dll",
        "sdgo/sdgo.ini": "player/client/sdgo/sdgo.ini",
        "sdgo/mods/capsule_catalog/capsule_catalog.dll": (
            "player/client/sdgo/mods/capsule_catalog/capsule_catalog.dll"
        ),
        "sdgo/mods/capsule_catalog/mod.ini": (
            "player/client/sdgo/mods/capsule_catalog/mod.ini"
        ),
    }
    installer = Path(__file__).with_name("install_capsule_client.ps1")
    installer = _assert_existing_components_no_reparse(installer)
    if not installer.is_file():
        raise ValueError("Standalone capsule client installer is missing")
    installer_data = installer.read_bytes()
    command_wrapper = Path(__file__).with_name("install_capsule_client.cmd")
    if command_wrapper.is_file():
        wrapper = command_wrapper.read_bytes()
    else:
        wrapper = (
            b"@echo off\r\n"
            b"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass "
            b"-File \"%~dp0Install-Client.ps1\" %*\r\n"
            b"if errorlevel 1 pause\r\n"
        )
    stage_hashes = {}
    for source_relative in stage_files:
        source = _safe_join(framework_stage, source_relative)
        if not source.is_file():
            raise ValueError(f"Framework stage is missing {source_relative}")
        stage_hashes[source_relative] = _sha256_file(source)
    stage_symbol = _safe_join(
        framework_stage, f"sdgo/symbols/{HOST_SYMBOL_NAME}"
    )
    if not stage_symbol.is_file():
        raise ValueError("Framework stage is missing the supported host symbols")
    profile_value = {
        "builder_version": PACKAGE_BUILDER_VERSION,
        "semantic": {
            "client.json": _sha256_file(client_bundle),
            "server.json": _sha256_file(server_bundle),
        },
        "compiled": dict(sorted(compiled_hashes.items())),
        "server": {
            relative: _sha256_file(_safe_join(server_output, relative))
            for relative in ("install.sql", "verify.sql", "rollback.sql", "manifest.json")
        },
        "stage": dict(sorted(stage_hashes.items())),
        "stage_symbols": _sha256_file(stage_symbol),
        "symbol_fragment": _sha256_file(symbol_fragment),
        "installer": _sha256_bytes(installer_data),
        "command_wrapper": _sha256_bytes(wrapper),
    }
    source_profile = _sha256_bytes(_json_bytes(profile_value))

    packages = _safe_join(bundle_root, f"deployment/{revision}")
    packages.mkdir(parents=True, exist_ok=True)
    _assert_existing_components_no_reparse(packages)
    final = _safe_join(packages, source_profile)
    if final.exists():
        existing = _validate_package(final, revision)
        if existing.get("source_profile") != source_profile:
            raise ValueError("Existing deployment package source profile differs")
        return final
    pending = _safe_join(packages, f".{source_profile}.{uuid.uuid4().hex}.pending")
    pending.mkdir()
    hashes = {}
    try:
        _write_package_file(
            pending, "semantic/client.json", Path(client_bundle).read_bytes(), hashes
        )
        _write_package_file(
            pending, "semantic/server.json", Path(server_bundle).read_bytes(), hashes
        )
        for source_relative, package_relative in stage_files.items():
            source = _safe_join(framework_stage, source_relative)
            data = source.read_bytes()
            if source_relative == "sdgo/mods/capsule_catalog/mod.ini":
                data = _set_ini_value(
                    data,
                    "mod",
                    "enabled",
                    "1",
                    "staged capsule mod.ini",
                )
            _write_package_file(pending, package_relative, data, hashes)
        for relative in ("d3d9.dll", "sdgo/core.dll"):
            source = _safe_join(framework_stage, relative)
            if _product_name(source) != PRODUCT_NAME:
                raise ValueError(f"Staged {Path(relative).name} is not the SDGO mod framework")

        package_symbols = _build_package_symbols(stage_symbol, symbol_fragment)
        player_fragment = _build_player_symbol_fragment(
            stage_symbol, symbol_fragment
        )
        _write_package_file(
            pending,
            f"player/client/sdgo/symbols/{HOST_SYMBOL_NAME}",
            package_symbols,
            hashes,
        )
        _write_package_file(
            pending,
            "player/symbol-fragment.ini",
            player_fragment,
            hashes,
        )

        mod_prefix = "player/client/sdgo/mods/capsule_catalog"
        for relative in sorted(compiled_hashes):
            source = _safe_join(runtime_output, relative)
            _write_package_file(
                pending, f"{mod_prefix}/{Path(relative).as_posix()}", source.read_bytes(), hashes
            )

        player_files = {
            relative.removeprefix("player/client/"): expected
            for relative, expected in hashes.items()
            if relative.startswith("player/client/")
        }
        player_manifest = {
            "schema_version": 1,
            "package_id": "capsule-catalog",
            "catalog_revision": revision,
            "source_profile": source_profile,
            "symbol_fragment_sha256": _sha256_bytes(player_fragment),
            "files": dict(sorted(player_files.items())),
        }
        _write_package_file(
            pending,
            "player/package-manifest.json",
            _json_bytes(player_manifest),
            hashes,
        )
        _write_package_file(
            pending,
            "player/Install-Client.ps1",
            installer_data,
            hashes,
        )
        _write_package_file(
            pending,
            "player/Install-Client.cmd",
            wrapper,
            hashes,
        )

        for relative in ("install.sql", "verify.sql", "rollback.sql", "manifest.json"):
            source = _safe_join(server_output, relative)
            _write_package_file(
                pending, f"server/{relative}", source.read_bytes(), hashes
            )
        manifest = {
            "schema_version": 1,
            "catalog_revision": revision,
            "source_profile": source_profile,
            "roles": {
                "player": "client files only; no database credentials or test helpers",
                "server": "generated configuration SQL; explicit operator application required",
            },
            "files": dict(sorted(hashes.items())),
        }
        _atomic_write(pending / "manifest.json", _json_bytes(manifest))
        _validate_package(pending, revision)
        if not _rename_directory(pending, final):
            _validate_package(final, revision)
        return final
    except Exception:
        # The pending directory is deliberately retained.  It can be inspected
        # and sent to the Recycle Bin without any ambiguity about ownership.
        raise


def _read_server_settings(path):
    path = _assert_existing_components_no_reparse(path)
    if not path.is_file():
        raise ValueError("Configured server INI does not exist")
    parser = configparser.RawConfigParser(interpolation=None, strict=True)
    try:
        with path.open("r", encoding="utf-8-sig") as handle:
            parser.read_file(handle)
    except (OSError, UnicodeDecodeError, configparser.Error) as error:
        raise ValueError("Configured server INI could not be read safely") from error
    if not parser.has_section("DB Game"):
        raise ValueError("Configured server INI has no [DB Game] section")
    section = parser["DB Game"]
    values = {}
    for key in ("DBServerName", "DBName", "ID", "PWD"):
        value = section.get(key, "").strip()
        if not value:
            raise ValueError(f"Configured server INI is missing [DB Game] {key}")
        values[key] = value
    return values


def _odbc_escape(value):
    return "{" + str(value).replace("}", "}}") + "}"


def _sql_connect(settings):
    try:
        import pyodbc
    except ImportError as error:
        raise ServerDeploymentError(
            "Server deployment requires the host pyodbc package"
        ) from error
    drivers = list(pyodbc.drivers())
    driver = next(
        (
            name
            for name in (
                "ODBC Driver 18 for SQL Server",
                "ODBC Driver 17 for SQL Server",
                "SQL Server",
            )
            if name in drivers
        ),
        None,
    )
    if driver is None:
        raise ServerDeploymentError("No supported SQL Server ODBC driver is installed")
    connection_string = ";".join(
        (
            f"DRIVER={_odbc_escape(driver)}",
            f"SERVER={_odbc_escape(settings['DBServerName'])}",
            f"DATABASE={_odbc_escape(settings['DBName'])}",
            f"UID={_odbc_escape(settings['ID'])}",
            f"PWD={_odbc_escape(settings['PWD'])}",
            "Encrypt=no",
            "TrustServerCertificate=yes",
        )
    )
    return pyodbc.connect(connection_string, autocommit=False, timeout=15)


def _row_dict(description, row):
    if description is None or row is None:
        return None
    return {
        str(column[0]): row[index]
        for index, column in enumerate(description)
    }


def _execute_last_result(cursor, sql):
    cursor.execute(sql)
    result = None
    while True:
        if cursor.description is not None:
            rows = cursor.fetchall()
            if rows:
                result = _row_dict(cursor.description, rows[-1])
        if not cursor.nextset():
            break
    return result


def _server_preflight(connection, expected_database):
    sql = """
SELECT DB_NAME() AS DatabaseName,
       HAS_PERMS_BY_NAME(DB_NAME(), 'DATABASE', 'CREATE TABLE') AS CanCreateTable,
       HAS_PERMS_BY_NAME(DB_NAME(), 'DATABASE', 'CREATE PROCEDURE') AS CanCreateProcedure,
       HAS_PERMS_BY_NAME('dbo', 'SCHEMA', 'ALTER') AS CanAlterSchema,
       HAS_PERMS_BY_NAME('dbo', 'SCHEMA', 'SELECT') AS CanSelectSchema,
       HAS_PERMS_BY_NAME('dbo', 'SCHEMA', 'INSERT') AS CanInsertSchema,
       HAS_PERMS_BY_NAME('dbo', 'SCHEMA', 'UPDATE') AS CanUpdateSchema,
       HAS_PERMS_BY_NAME('dbo', 'SCHEMA', 'DELETE') AS CanDeleteSchema,
       HAS_PERMS_BY_NAME('dbo', 'SCHEMA', 'EXECUTE') AS CanExecuteSchema,
       HAS_PERMS_BY_NAME(DB_NAME(), 'DATABASE', 'VIEW DEFINITION') AS CanViewDefinition;
"""
    row = _execute_last_result(connection.cursor(), sql)
    if not row or str(row.get("DatabaseName", "")).lower() != expected_database.lower():
        raise ServerDeploymentError("Server connection opened the wrong database")
    missing = [name for name in SERVER_PERMISSIONS if not bool(row.get(name))]
    if missing:
        raise ServerDeploymentError(
            "Server account lacks required capsule schema/procedure permissions: "
            + ", ".join(missing)
        )
    connection.rollback()


def _validate_server_result(row, revision, verify=False):
    expected = (
        {
            "CatalogRevision",
            "WeightedMachines",
            "WeightedSlots",
            "EnabledCustomMachines",
            "EnabledCustomCandidates",
            "Verified",
        }
        if verify
        else {
            "CatalogRevision",
            "InstalledUtc",
            "WeightedMachines",
            "CustomMachines",
        }
    )
    if not row or set(row) != expected:
        raise ServerDeploymentError("Server SQL returned an unexpected result shape")
    if str(row.get("CatalogRevision", "")) != revision:
        raise ServerDeploymentError("Server SQL returned a different catalog revision")
    if verify and not bool(row.get("Verified")):
        raise ServerDeploymentError("Server verification did not pass")


def _apply_server(connection, server_dir, revision):
    install = _safe_join(server_dir, "install.sql").read_text(encoding="utf-8")
    verify = _safe_join(server_dir, "verify.sql").read_text(encoding="utf-8")
    cursor = connection.cursor()
    try:
        # ODBC manual-commit mode already owns the outer transaction. The
        # generated installer nests its own BEGIN/COMMIT within that scope.
        # Adding another BEGIN here leaves one level open after commit(),
        # which SQL Server rolls back when this connection closes.
        installed = _execute_last_result(cursor, install)
        _validate_server_result(installed, revision)
        verified = _execute_last_result(cursor, verify)
        _validate_server_result(verified, revision, verify=True)
        connection.commit()
        state = _execute_last_result(
            cursor, "SELECT @@TRANCOUNT AS OpenTransactions;"
        )
        if not state or state.get("OpenTransactions") != 0:
            raise ServerDeploymentError(
                "Server transaction did not finish committing"
            )
        return verified
    except Exception:
        try:
            connection.rollback()
        except Exception:
            pass
        raise


def _backup_file(source, backup_root, relative):
    if not source.exists():
        return None
    if not source.is_file() or _is_reparse(source):
        raise ValueError(f"Refusing unsafe existing client file: {source}")
    destination = _safe_join(backup_root, f"previous/{relative}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    _atomic_write(destination, source.read_bytes())
    return destination


def _move_failed_new(path, backup_root, relative):
    if not path.exists():
        return
    destination = _safe_join(backup_root, f"failed-new/{relative}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        raise OSError(f"Rollback destination already exists: {destination}")
    path.rename(destination)


class _ClientTransaction:
    def __init__(self, target, package_client, revision):
        self.target = _absolute(target)
        self.package_client = _absolute(package_client)
        self.revision = revision
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        self.backup_root = _safe_join(
            self.target,
            f"sdgo/backups/capsule_catalog/{stamp}-{revision[:12]}-{uuid.uuid4().hex[:8]}",
        )
        self.touched = {}
        self.old_mod = None
        self.prepared = {}
        self.prepared_hashes = {}
        self.installed_files = []
        self.foreign_loader = False

    def _target_file(self, relative):
        return _safe_join(self.target, relative)

    def _package_file(self, relative):
        path = _safe_join(self.package_client, relative)
        if not path.is_file():
            raise ValueError(f"Player package is missing {relative}")
        return path

    def prepare(self):
        _assert_existing_components_no_reparse(self.target)
        if not self.target.is_dir():
            raise ValueError("Configured client target is not a directory")
        _assert_tree_no_reparse(self.package_client)
        executable = self._target_file("GOnline.exe")
        if not executable.is_file():
            raise ValueError("Configured client target does not contain GOnline.exe")
        _parse_pe(executable)
        _assert_client_stopped(self.target)

        package_loader = self._package_file("d3d9.dll")
        package_core = self._package_file("sdgo/core.dll")
        if _product_name(package_loader) != PRODUCT_NAME:
            raise ValueError("Packaged d3d9.dll is not the SDGO mod framework")
        if _product_name(package_core) != PRODUCT_NAME:
            raise ValueError("Packaged core.dll is not the SDGO mod framework")

        loader = self._target_file("d3d9.dll")
        chain = self._target_file("d3d9_chain.dll")
        core = self._target_file("sdgo/core.dll")
        if loader.exists():
            if not loader.is_file() or _is_reparse(loader):
                raise ValueError("Existing d3d9.dll is unsafe")
            self.foreign_loader = _product_name(loader) != PRODUCT_NAME
            if self.foreign_loader and chain.exists():
                raise ValueError(
                    "Client has both a foreign d3d9.dll and d3d9_chain.dll; "
                    "resolve the existing wrapper chain first"
                )
        if core.exists():
            if not core.is_file() or _is_reparse(core):
                raise ValueError("Existing sdgo/core.dll is unsafe")
            if _product_name(core) != PRODUCT_NAME:
                raise ValueError("Existing sdgo/core.dll belongs to another product")

        package_ini = self._package_file("sdgo/sdgo.ini").read_bytes()
        target_ini = self._target_file("sdgo/sdgo.ini")
        if target_ini.exists():
            if not target_ini.is_file() or _is_reparse(target_ini):
                raise ValueError("Existing sdgo.ini is unsafe")
            ini_raw = target_ini.read_bytes()
        else:
            ini_raw = package_ini
        if self.foreign_loader:
            values = _ini_values(ini_raw, "existing sdgo.ini")
            configured = values.get("loader", {}).get("chain_d3d9", "")
            if configured and configured.lower() != "d3d9_chain.dll":
                raise ValueError(
                    "Existing sdgo.ini already chains a different d3d9 wrapper"
                )
            ini_raw = _set_ini_value(
                ini_raw,
                "loader",
                "chain_d3d9",
                "d3d9_chain.dll",
                "existing sdgo.ini",
            )

        package_symbol = self._package_file(
            f"sdgo/symbols/{HOST_SYMBOL_NAME}"
        ).read_bytes()
        target_symbol = self._target_file(f"sdgo/symbols/{HOST_SYMBOL_NAME}")
        if target_symbol.exists() and (
            not target_symbol.is_file() or _is_reparse(target_symbol)
        ):
            raise ValueError("Existing host symbol file is unsafe")
        symbol_raw = _merge_symbols(
            target_symbol.read_bytes() if target_symbol.exists() else None,
            package_symbol,
        )

        package_mod = _safe_join(
            self.package_client, "sdgo/mods/capsule_catalog"
        )
        if not package_mod.is_dir():
            raise ValueError("Player package is missing the capsule Mod directory")
        _assert_tree_no_reparse(package_mod)
        target_mod = self._target_file("sdgo/mods/capsule_catalog")
        if target_mod.exists():
            _assert_tree_no_reparse(target_mod)
        old_mod_ini = target_mod / "mod.ini"
        old_mod_raw = old_mod_ini.read_bytes() if old_mod_ini.is_file() else None

        self.prepared = {
            "d3d9.dll": package_loader.read_bytes(),
            "sdgo/core.dll": package_core.read_bytes(),
            "sdgo/sdgo.ini": ini_raw,
            f"sdgo/symbols/{HOST_SYMBOL_NAME}": symbol_raw,
        }
        if self.foreign_loader:
            self.prepared["d3d9_chain.dll"] = loader.read_bytes()
        self.mod_files = {}
        for source in sorted(package_mod.rglob("*"), key=lambda item: item.as_posix()):
            if source.is_file():
                relative = source.relative_to(package_mod).as_posix()
                data = source.read_bytes()
                if relative.lower() == "mod.ini":
                    data = _merge_mod_ini(data, old_mod_raw)
                self.mod_files[relative] = data
        self.prepared_hashes = {
            relative: _sha256_bytes(data) for relative, data in self.prepared.items()
        }
        self.prepared_hashes.update(
            {
                f"sdgo/mods/capsule_catalog/{relative}": _sha256_bytes(data)
                for relative, data in self.mod_files.items()
            }
        )
        runtime = configparser.RawConfigParser(interpolation=None)
        runtime.read_string(self.mod_files["runtime.ini"].decode("ascii"))
        if runtime.get("runtime", "catalog_revision", fallback="") != self.revision:
            raise ValueError("Player package runtime revision differs")
        manifest = json.loads(self.mod_files["manifest.json"].decode("utf-8-sig"))
        if manifest.get("catalog_revision") != self.revision:
            raise ValueError("Player package manifest revision differs")
        return self

    def install(self):
        _assert_client_stopped(self.target)
        self.backup_root.mkdir(parents=True)
        _assert_existing_components_no_reparse(self.backup_root)
        for relative in self.prepared:
            target = self._target_file(relative)
            self.touched[relative] = {
                "existed": target.exists(),
                "backup": _backup_file(target, self.backup_root, relative),
            }

        target_mod = self._target_file("sdgo/mods/capsule_catalog")
        pending_mod = target_mod.with_name(
            f".capsule_catalog.{uuid.uuid4().hex}.pending"
        )
        if pending_mod.exists():
            raise ValueError("Unexpected capsule Mod pending directory")
        pending_mod.mkdir(parents=True)
        for relative, data in self.mod_files.items():
            _atomic_write(_safe_join(pending_mod, relative), data)
        for relative, expected in self.mod_files.items():
            path = _safe_join(pending_mod, relative)
            if _sha256_file(path) != _sha256_bytes(expected):
                raise OSError(f"Prepared capsule Mod hash mismatch: {relative}")

        try:
            if target_mod.exists():
                self.old_mod = _safe_join(
                    self.backup_root, "previous/sdgo/mods/capsule_catalog"
                )
                self.old_mod.parent.mkdir(parents=True, exist_ok=True)
                target_mod.rename(self.old_mod)
            pending_mod.rename(target_mod)
            self.installed_files.extend(
                f"sdgo/mods/capsule_catalog/{relative}"
                for relative in sorted(self.mod_files)
            )
            for relative, data in self.prepared.items():
                _atomic_write(self._target_file(relative), data)
                self.installed_files.append(relative)
            self.verify()
            receipt = {
                "schema_version": 1,
                "catalog_revision": self.revision,
                "installed_utc": datetime.now(timezone.utc)
                .replace(microsecond=0)
                .isoformat()
                .replace("+00:00", "Z"),
                "prepared_sha256": dict(sorted(self.prepared_hashes.items())),
                "previous_files": {
                    relative: {
                        "existed": state["existed"],
                        "sha256": (
                            _sha256_file(state["backup"])
                            if state["backup"] is not None
                            else None
                        ),
                    }
                    for relative, state in sorted(self.touched.items())
                },
                "retained_previous_mod": self.old_mod is not None,
            }
            _atomic_write(self.backup_root / "receipt.json", _json_bytes(receipt))
            return {
                "backup_path": str(self.backup_root),
                "installed_files": sorted(self.installed_files),
            }
        except Exception:
            self.rollback()
            raise

    def verify(self):
        for relative, expected in self.prepared_hashes.items():
            path = self._target_file(relative)
            if not path.is_file() or _sha256_file(path) != expected:
                raise OSError(f"Installed client hash mismatch: {relative}")
        _parse_pe(self._target_file("GOnline.exe"))
        return True

    def rollback(self):
        target_mod = self._target_file("sdgo/mods/capsule_catalog")
        if target_mod.exists():
            _move_failed_new(
                target_mod,
                self.backup_root,
                "sdgo/mods/capsule_catalog",
            )
        if self.old_mod is not None and self.old_mod.exists():
            target_mod.parent.mkdir(parents=True, exist_ok=True)
            self.old_mod.rename(target_mod)
        for relative, state in reversed(list(self.touched.items())):
            target = self._target_file(relative)
            if state["existed"]:
                backup = state["backup"]
                if backup is None or not backup.is_file():
                    raise OSError(f"Client rollback backup missing: {relative}")
                _atomic_write(target, backup.read_bytes())
            elif target.exists():
                _move_failed_new(target, self.backup_root, relative)


@dataclass
class DeploymentTarget:
    client_target: Path
    server_ini: Path | None
    framework_stage: Path
    symbol_fragment: Path

    def __post_init__(self):
        self.client_target = _absolute(self.client_target)
        self.server_ini = (
            _absolute(self.server_ini) if self.server_ini is not None else None
        )
        self.framework_stage = _absolute(self.framework_stage)
        self.symbol_fragment = _absolute(self.symbol_fragment)

    def apply(self, catalog, runtime_output: Path, bundle_root: Path) -> dict:
        """Build a revision-aligned package and apply configured targets."""
        from .capsule_catalog import digest, export_bundle
        from .capsule_server_runtime import build as build_server

        revision = digest(catalog)
        runtime_output = _assert_existing_components_no_reparse(runtime_output)
        bundle_root = _assert_existing_components_no_reparse(bundle_root)
        framework_stage = _assert_existing_components_no_reparse(self.framework_stage)
        symbol_fragment = _assert_existing_components_no_reparse(self.symbol_fragment)
        if not runtime_output.is_dir():
            raise ValueError("Compiled client runtime output does not exist")
        if not framework_stage.is_dir():
            raise ValueError("Framework stage does not exist")
        if not symbol_fragment.is_file():
            raise ValueError("Capsule symbol fragment does not exist")
        bundle_root.mkdir(parents=True, exist_ok=True)

        export_bundle(catalog, bundle_root)
        client_bundle, server_bundle, _ = _validate_semantic_bundle(
            bundle_root, catalog, revision
        )
        _, _, compiled_hashes = _validate_compiled(runtime_output, revision)

        server_output = _safe_join(
            bundle_root, f"runtime/{revision}/server-v1"
        )
        server_output.mkdir(parents=True, exist_ok=True)
        server_manifest = build_server(server_bundle, server_output)
        if server_manifest.get("catalog_revision") != revision:
            raise ValueError("Server generator returned a different catalog revision")
        _validate_server_runtime(server_output, server_bundle, revision)

        package_root = _build_package(
            bundle_root,
            revision,
            client_bundle,
            server_bundle,
            runtime_output,
            compiled_hashes,
            server_output,
            framework_stage,
            symbol_fragment,
        )
        package_manifest = _validate_package(package_root, revision)
        package_client = _safe_join(package_root, "player/client")

        try:
            transaction = _ClientTransaction(
                self.client_target, package_client, revision
            ).prepare()
        except ClientRunningError as error:
            return {
                "applied": False,
                "message": str(error),
                "catalog_revision": revision,
                "client_target": str(self.client_target),
                "server_applied": False,
                "needs_server_restart": False,
                "package_path": str(package_root),
                "package_manifest": str(package_root / "manifest.json"),
            }

        connection = None
        if self.server_ini is not None:
            settings = _read_server_settings(self.server_ini)
            try:
                connection = _sql_connect(settings)
                _server_preflight(connection, settings["DBName"])
            except ServerDeploymentError:
                if connection is not None:
                    try:
                        connection.close()
                    except Exception:
                        pass
                raise
            except Exception:
                if connection is not None:
                    try:
                        connection.close()
                    except Exception:
                        pass
                raise ServerDeploymentError(
                    "Server connection or permission preflight failed"
                ) from None

        try:
            client_result = transaction.install()
            server_applied = False
            if connection is not None:
                try:
                    _apply_server(
                        connection,
                        _safe_join(package_root, "server"),
                        revision,
                    )
                    server_applied = True
                except Exception:
                    transaction.rollback()
                    raise ServerDeploymentError(
                        "Server deployment failed; its transaction was rolled back "
                        "and the client was restored"
                    ) from None
            message = (
                "Capsule catalog client and server configuration applied. "
                "Restart the native server manually before players connect."
                if server_applied
                else "Capsule catalog client applied. Server configuration was not "
                "changed because no server INI was configured."
            )
            return {
                "applied": True,
                "message": message,
                "catalog_revision": revision,
                "client_target": str(self.client_target),
                "server_ini": str(self.server_ini) if self.server_ini else None,
                "server_applied": server_applied,
                "needs_server_restart": server_applied,
                "package_path": str(package_root),
                "package_manifest": str(package_root / "manifest.json"),
                "package_files": len(package_manifest["files"]),
                **client_result,
            }
        finally:
            if connection is not None:
                try:
                    connection.close()
                except Exception:
                    pass
