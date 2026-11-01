"""Fail-closed Battle Rewards deployment for one supported BattleServer."""
from __future__ import annotations

import configparser
import ctypes
from dataclasses import dataclass, field
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile
import uuid

from . import boss_catalog
from .sql_repository import RewardWalletRepository
from .workspace import Workspace


SUPPORTED_HOSTS = {
    "BattleServer-548A891D-0046C000":
        "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4",
}
_SECTION = re.compile(r"^\s*\[([^\]\r\n]+)\]\s*$")
_SAFE_INSTANCE = re.compile(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}\Z")


class DeploymentError(RuntimeError):
    pass


class DeploymentApplyError(DeploymentError):
    """An apply failure with a durable receipt and honest write status."""

    def __init__(self, message: str, *, receipt: Path, writes_performed: bool):
        super().__init__(message)
        self.receipt = Path(receipt)
        self.writes_performed = bool(writes_performed)


@dataclass
class DeploymentPlan:
    action: str
    battle_dir: Path
    workspace: Path
    package_root: Path | None = None
    server_ini: Path | None = None
    host_key: str = ""
    host_sha256: str = ""
    install_script: Path | None = None
    package_server: Path | None = None
    package_mod: Path | None = None
    package_symbols: Path | None = None
    target_symbols: Path | None = None
    required_symbols: tuple[str, ...] = ()
    merged_symbols: str = ""
    source_hashes: dict[str, str] = field(default_factory=dict)
    package_hashes: dict[str, str] = field(default_factory=dict, repr=False)
    destination_hashes: dict[str, str | None] = field(default_factory=dict)
    server_ini_hash: str = ""
    package_version: str = ""
    file_versions: dict[str, str] = field(default_factory=dict)
    verified_package_files: int = 0
    database_sections: tuple[str, ...] = ()
    blockers: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    steps: list[str] = field(default_factory=list)

    def public(self) -> dict:
        return {
            "schema_version": 1,
            "mode": "plan",
            "action": self.action,
            "battle_directory": str(self.battle_dir),
            "workspace": str(self.workspace),
            "package_root": str(self.package_root) if self.package_root else None,
            "server_ini": str(self.server_ini) if self.server_ini else None,
            "host": {
                "key": self.host_key,
                "sha256": self.host_sha256,
                "supported": not any(
                    item.startswith("unsupported BattleServer")
                    for item in self.blockers
                ),
            },
            "required_symbols": list(self.required_symbols),
            "source_hashes": self.source_hashes,
            "destination_hashes": self.destination_hashes,
            "package_version": self.package_version or None,
            "file_versions": self.file_versions,
            "verified_package_files": self.verified_package_files,
            "database_sections": list(self.database_sections),
            "blockers": list(self.blockers),
            "warnings": list(self.warnings),
            "steps": list(self.steps),
            "writes_performed": False,
        }


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def optional_hash(path: Path) -> str | None:
    return sha256_file(path) if path.is_file() else None


def _tracked_paths(battle_dir: Path, host_key: str) -> dict[str, Path]:
    target_mod = battle_dir / "sdgo" / "mods" / "battle_rewards"
    return {
        "winmm.dll": battle_dir / "winmm.dll",
        "winmm_chain.dll": battle_dir / "winmm_chain.dll",
        "sdgo/core.dll": battle_dir / "sdgo" / "core.dll",
        "sdgo/sdgo.ini": battle_dir / "sdgo" / "sdgo.ini",
        "battle_rewards.dll": target_mod / "battle_rewards.dll",
        "battle_rewards/mod.ini": target_mod / "mod.ini",
        "battle_rewards/config.ini": target_mod / "config.ini",
        f"symbols/{host_key}.ini":
            battle_dir / "sdgo" / "symbols" / f"{host_key}.ini",
    }


def pe_host_key(path: Path) -> str:
    data = path.read_bytes()
    if len(data) < 0x40 or data[:2] != b"MZ":
        raise DeploymentError(f"{path} is not a Windows PE executable")
    nt = struct.unpack_from("<I", data, 0x3C)[0]
    if nt > len(data) - 0x18 - 0x3C or data[nt:nt + 4] != b"PE\0\0":
        raise DeploymentError(f"{path} has an invalid PE header")
    timestamp = struct.unpack_from("<I", data, nt + 8)[0]
    size_of_image = struct.unpack_from("<I", data, nt + 0x18 + 0x38)[0]
    if timestamp == 0 or size_of_image == 0:
        raise DeploymentError(f"{path} has invalid host metadata")
    return f"{path.stem}-{timestamp:08X}-{size_of_image:08X}"


def _ini_sections(text: str, *, source: str) -> tuple[dict[str, dict[str, str]], dict[str, str]]:
    values: dict[str, dict[str, str]] = {}
    blocks: dict[str, str] = {}
    matches = list(
        re.finditer(r"(?m)^[ \t]*\[([^\]\r\n]+)\][ \t]*$", text)
    )
    for index, match in enumerate(matches):
        name = match.group(1).strip()
        canonical = name.casefold()
        if not name or canonical in values:
            raise DeploymentError(f"duplicate symbol section [{name}] in {source}")
        end = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        block = text[match.start():end].rstrip()
        section: dict[str, str] = {}
        for raw in block.splitlines()[1:]:
            line = raw.strip()
            if not line or line.startswith((";", "#")):
                continue
            if "=" not in line:
                raise DeploymentError(
                    f"invalid symbol row in [{name}] from {source}"
                )
            key, value = (item.strip() for item in line.split("=", 1))
            folded = key.casefold()
            if not key or folded in section:
                raise DeploymentError(
                    f"duplicate symbol key {key!r} in [{name}] from {source}"
                )
            section[folded] = value
        values[canonical] = section
        blocks[canonical] = block
    if not values:
        raise DeploymentError(f"no INI sections in {source}")
    return values, blocks


def merge_required_symbols(
    existing_text: str | None,
    package_text: str,
    required: tuple[str, ...],
) -> str:
    package, package_blocks = _ini_sections(
        package_text, source="package symbol file"
    )
    selected = ("host", *required)
    missing_package = [name for name in selected if name.casefold() not in package]
    if missing_package:
        raise DeploymentError(
            "package symbol file lacks required definitions: "
            + ", ".join(missing_package)
        )
    if existing_text is None:
        return package_text if package_text.endswith("\n") else package_text + "\n"

    existing, _ = _ini_sections(existing_text, source="installed symbol file")
    additions: list[str] = []
    for name in selected:
        canonical = name.casefold()
        if canonical in existing:
            if existing[canonical] != package[canonical]:
                raise DeploymentError(
                    f"installed symbol [{name}] conflicts with the package"
                )
        else:
            additions.append(package_blocks[canonical])
    if not additions:
        return existing_text if existing_text.endswith("\n") else existing_text + "\n"
    return (
        existing_text.rstrip()
        + "\n\n; Added by the Battle Rewards scoped installer.\n"
        + "\n\n".join(additions)
        + "\n"
    )


def _find_section(parser: configparser.ConfigParser, normalized: str) -> str | None:
    return next(
        (
            name
            for name in parser.sections()
            if name.casefold().replace(" ", "") == normalized
        ),
        None,
    )


def database_sections(path: Path) -> tuple[str, str]:
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    try:
        parser.read_string(path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, configparser.Error) as error:
        raise DeploymentError(f"unable to read owner server INI: {error}") from error
    game = _find_section(parser, "dbgame")
    billing = _find_section(parser, "dbstorebilling")
    if not game or not billing:
        raise DeploymentError(
            "owner server INI must contain DB Game and DB StoreBilling sections"
        )
    return game, billing


def _safe_payload_path(value: str) -> Path:
    if (
        not isinstance(value, str)
        or not value
        or re.search(r"[\x00-\x1f:*?\"<>|]", value)
    ):
        raise DeploymentError(f"unsafe Battle Rewards payload path: {value!r}")
    normalized = value.replace("/", os.sep).replace("\\", os.sep)
    path = Path(normalized)
    if (
        path.is_absolute()
        or any(
            part in {"", ".", ".."} or part.endswith((" ", "."))
            for part in path.parts
        )
    ):
        raise DeploymentError(f"unsafe Battle Rewards payload path: {value!r}")
    return path


def _manifest(
    path: Path,
) -> tuple[tuple[str, ...], tuple[str, ...], tuple[str, ...]]:
    parser = configparser.ConfigParser(interpolation=None, strict=True)
    try:
        parser.read_string(path.read_text(encoding="utf-8-sig"))
        mod = parser["mod"]
    except (OSError, UnicodeError, configparser.Error, KeyError) as error:
        raise DeploymentError(f"invalid Battle Rewards manifest: {error}") from error
    if mod.get("id") != "battle_rewards" or mod.get("dll") != "battle_rewards.dll":
        raise DeploymentError("package contains the wrong Battle Rewards manifest")
    required = tuple(
        item.strip() for item in mod.get("requires", "").split(",") if item.strip()
    )
    files = tuple(
        str(_safe_payload_path(item.strip()))
        for item in mod.get("files", "").split(",")
        if item.strip()
    )
    hosts = tuple(
        item.strip() for item in mod.get("hosts", "").split(",") if item.strip()
    )
    if not required:
        raise DeploymentError("Battle Rewards manifest has no native requirements")
    if not hosts:
        raise DeploymentError("Battle Rewards manifest has no supported hosts")
    return required, files, hosts


def _disabled(value: str | None) -> bool:
    return value is not None and value.strip().casefold() in {
        "0", "false", "no", "off"
    }


def activation_warnings(sdgo_ini: Path) -> list[str]:
    if not sdgo_ini.is_file():
        return []
    parser = configparser.ConfigParser(interpolation=None, strict=False)
    try:
        parser.read(sdgo_ini, encoding="utf-8-sig")
    except (OSError, UnicodeError, configparser.Error):
        return ["installed sdgo.ini could not be read; verify gameplay gates"]
    warnings = []
    if parser.has_section("categories") and _disabled(
        parser.get("categories", "gameplay", fallback=None)
    ):
        warnings.append(
            "sdgo.ini disables category gameplay; Battle Rewards will stay inactive"
        )
    if parser.has_section("features") and _disabled(
        parser.get("features", "battle-rewards", fallback=None)
    ):
        warnings.append(
            "sdgo.ini disables feature battle-rewards; remove that explicit gate"
        )
    if parser.has_section("mods") and _disabled(
        parser.get("mods", "battle_rewards", fallback=None)
    ):
        warnings.append(
            "sdgo.ini disables mod battle_rewards; remove that explicit gate"
        )
    return warnings


def _process_probe() -> list[dict]:
    if os.name != "nt":
        return []
    script = (
        "$ErrorActionPreference='Stop';"
        "$p=@(Get-CimInstance Win32_Process -Filter \"Name='BattleServer.exe'\"|"
        "Select-Object ProcessId,ExecutablePath);"
        "$p|ConvertTo-Json -Compress"
    )
    completed = subprocess.run(
        ["powershell", "-NoProfile", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        timeout=20,
    )
    if completed.returncode:
        return [{"pid": None, "path": None, "probe_error": True}]
    text = completed.stdout.strip()
    if not text:
        return []
    value = json.loads(text)
    rows = value if isinstance(value, list) else [value]
    return [
        {
            "pid": row.get("ProcessId"),
            "path": row.get("ExecutablePath"),
        }
        for row in rows
    ]


def _locate_install_script(package_root: Path) -> Path:
    candidates = (
        package_root / "install_server.ps1",
        package_root / "tools" / "install_server.ps1",
    )
    return next((path for path in candidates if path.is_file()), candidates[0])


def _kit_root(package_root: Path) -> Path:
    direct = package_root / "server" / "sdgo" / "mods" / "battle_rewards"
    embedded_root = package_root / "rewards"
    embedded = (
        embedded_root / "server" / "sdgo" / "mods" / "battle_rewards"
    )
    if (
        embedded.is_dir()
        and (embedded_root / "VERSION").is_file()
        and (embedded_root / "SHA256.json").is_file()
    ):
        return embedded_root
    if (
        direct.is_dir()
        and (package_root / "VERSION").is_file()
        and (package_root / "SHA256.json").is_file()
    ):
        return package_root
    return package_root


def _safe_manifest_path(value: str) -> Path:
    if (
        not isinstance(value, str)
        or not value
        or "\\" in value
        or re.search(r"[\x00-\x1f:*?\"<>|]", value)
    ):
        raise DeploymentError(f"unsafe SHA256.json path: {value!r}")
    path = Path(value)
    if (
        path.is_absolute()
        or any(
            part in {"", ".", ".."} or part.endswith((" ", "."))
            for part in path.parts
        )
    ):
        raise DeploymentError(f"unsafe SHA256.json path: {value!r}")
    return path


def _mutable_kit_file(relative: str) -> bool:
    folded = relative.casefold()
    return (
        folded.startswith("docs/")
        or folded == "owner/rewards/policy.example.json"
        or folded.endswith("/config.example.ini")
    )


def verify_package_hashes(package_root: Path) -> dict[str, str]:
    manifest_path = package_root / "SHA256.json"
    try:
        value = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise DeploymentError(f"unable to read package SHA256.json: {error}") from error
    if not isinstance(value, dict) or not value:
        raise DeploymentError("SHA256.json must be a nonempty flat mapping")
    checked: dict[str, str] = {}
    for relative, expected in value.items():
        path = _safe_manifest_path(relative)
        if not isinstance(expected, str) or not re.fullmatch(
            r"[0-9A-Fa-f]{64}", expected
        ):
            raise DeploymentError(
                f"invalid SHA-256 for package file {relative!r}"
            )
        if _mutable_kit_file(relative):
            continue
        absolute = (package_root / path).resolve()
        if not absolute.is_relative_to(package_root) or not absolute.is_file():
            raise DeploymentError(
                f"checksummed package file is missing: {relative}"
            )
        actual = sha256_file(absolute)
        if actual.casefold() != expected.casefold():
            raise DeploymentError(
                f"package checksum mismatch: {relative}"
            )
        checked[relative] = actual
    required = {
        "VERSION",
        "install_server.ps1",
        "install_common.ps1",
        "server/winmm.dll",
        "server/sdgo/core.dll",
        "server/sdgo/sdgo.ini",
        "server/sdgo/mods/battle_rewards/battle_rewards.dll",
        "server/sdgo/mods/battle_rewards/mod.ini",
        "tools/install_battle_rewards.py",
        "owner/rewards/deployment.py",
        "owner/rewards/sql_repository.py",
        "owner/rewards/sql/battle_reward_wallet.sql",
    }
    immutable_files = {
        path.relative_to(package_root).as_posix()
        for path in package_root.rglob("*")
        if (
            path.is_file()
            and path.name != "SHA256.json"
            and "__pycache__" not in path.parts
            and path.suffix.casefold() != ".pyc"
            and not _mutable_kit_file(
                path.relative_to(package_root).as_posix()
            )
        )
    }
    unlisted = sorted(immutable_files - checked.keys())
    if unlisted:
        raise DeploymentError(
            "SHA256.json omits immutable package files: "
            + ", ".join(unlisted)
        )
    missing = sorted(required - checked.keys())
    if missing:
        raise DeploymentError(
            "SHA256.json omits immutable installer files: "
            + ", ".join(missing)
        )
    return checked


def _file_version(path: Path) -> str:
    if os.name != "nt":
        raise DeploymentError("PE file-version verification requires Windows")
    version = ctypes.windll.version
    version.GetFileVersionInfoSizeW.argtypes = [
        ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_uint32)
    ]
    version.GetFileVersionInfoSizeW.restype = ctypes.c_uint32
    ignored = ctypes.c_uint32()
    size = version.GetFileVersionInfoSizeW(str(path), ctypes.byref(ignored))
    if not size:
        raise DeploymentError(f"package DLL has no version resource: {path}")
    buffer = ctypes.create_string_buffer(size)
    version.GetFileVersionInfoW.argtypes = [
        ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p
    ]
    if not version.GetFileVersionInfoW(str(path), 0, size, buffer):
        raise DeploymentError(f"unable to read package DLL version: {path}")
    pointer = ctypes.c_void_p()
    length = ctypes.c_uint()
    version.VerQueryValueW.argtypes = [
        ctypes.c_void_p, ctypes.c_wchar_p,
        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_uint)
    ]
    if not version.VerQueryValueW(
        buffer, "\\", ctypes.byref(pointer), ctypes.byref(length)
    ) or length.value < 16:
        raise DeploymentError(f"package DLL version is malformed: {path}")
    fields = ctypes.cast(
        pointer, ctypes.POINTER(ctypes.c_uint32 * 13)
    ).contents
    signature = fields[0]
    if signature != 0xFEEF04BD:
        raise DeploymentError(f"package DLL version signature is invalid: {path}")
    file_ms, file_ls = fields[2], fields[3]
    return (
        f"{file_ms >> 16}.{file_ms & 0xFFFF}."
        f"{file_ls >> 16}"
    )


def build_plan(
    *,
    battle_dir: Path,
    workspace: Path,
    package_root: Path | None = None,
    server_ini: Path | None = None,
    uninstall: bool = False,
    supported_hosts: dict[str, str] | None = None,
    process_probe=None,
    version_probe=None,
) -> DeploymentPlan:
    battle_dir = Path(battle_dir).resolve(strict=True)
    workspace = Path(workspace).resolve()
    executable = battle_dir / "BattleServer.exe"
    if not executable.is_file():
        raise DeploymentError(
            f"battle directory has no BattleServer.exe: {battle_dir}"
        )
    host_key = pe_host_key(executable)
    host_sha = sha256_file(executable)
    plan = DeploymentPlan(
        action="uninstall" if uninstall else "install",
        battle_dir=battle_dir,
        workspace=workspace,
        host_key=host_key,
        host_sha256=host_sha,
        target_symbols=(
            battle_dir / "sdgo" / "symbols" / f"{host_key}.ini"
        ),
    )
    allowed = SUPPORTED_HOSTS if supported_hosts is None else supported_hosts
    expected_sha = allowed.get(host_key)
    if expected_sha is None or expected_sha.casefold() != host_sha.casefold():
        plan.blockers.append(
            f"unsupported BattleServer build {host_key} sha256={host_sha}"
        )
    probe = process_probe or _process_probe
    target_norm = os.path.normcase(str(executable.resolve()))
    for process in probe():
        path = process.get("path")
        if path and os.path.normcase(str(Path(path).resolve())) == target_norm:
            plan.blockers.append(
                f"BattleServer.exe is running from the target directory (PID {process.get('pid')})"
            )
        elif not path:
            plan.warnings.append(
                "a BattleServer.exe process has no readable path; apply will "
                "require exclusive file access"
            )
    plan.destination_hashes = {
        name: optional_hash(path)
        for name, path in _tracked_paths(battle_dir, host_key).items()
    }
    plan.warnings.extend(activation_warnings(battle_dir / "sdgo" / "sdgo.ini"))

    if uninstall:
        plan.steps = [
            "verify the target BattleServer is stopped without killing it",
            "create an owner deployment receipt with pre-change hashes",
            "move only sdgo/mods/battle_rewards into that receipt",
            "preserve framework, symbols, wallet ledger, jobs and balances",
        ]
        return plan

    if package_root is None or server_ini is None:
        raise DeploymentError(
            "install planning requires package_root and server_ini"
        )
    package_root = _kit_root(Path(package_root).resolve(strict=True))
    server_ini = Path(server_ini).resolve(strict=True)
    package_server = package_root / "server"
    package_mod = package_server / "sdgo" / "mods" / "battle_rewards"
    install_script = _locate_install_script(package_root)
    required, payload_files, hosts = _manifest(package_mod / "mod.ini")
    if host_key not in hosts:
        raise DeploymentError(
            f"Battle Rewards package manifest does not support {host_key}"
        )
    package_symbols = (
        package_server / "sdgo" / "symbols" / f"{host_key}.ini"
    )
    for path in (
        install_script,
        package_server / "winmm.dll",
        package_server / "sdgo" / "core.dll",
        package_server / "sdgo" / "sdgo.ini",
        package_mod / "battle_rewards.dll",
        package_mod / "mod.ini",
        package_symbols,
        *(package_mod / name for name in payload_files),
    ):
        if not path.is_file():
            raise DeploymentError(f"package is missing required file: {path}")

    plan.package_root = package_root
    plan.server_ini = server_ini
    plan.install_script = install_script
    plan.package_server = package_server
    plan.package_mod = package_mod
    plan.package_symbols = package_symbols
    plan.required_symbols = required
    plan.database_sections = database_sections(server_ini)
    plan.server_ini_hash = sha256_file(server_ini)
    _validate_ini_target(
        battle_dir / "sdgo" / "mods" / "battle_rewards" / "config.ini",
        "settings",
    )
    _validate_ini_target(
        battle_dir / "sdgo" / "mods" / "battle_rewards" / "mod.ini",
        "mod",
    )
    checked_hashes = verify_package_hashes(package_root)
    plan.package_hashes = checked_hashes
    symbol_manifest_key = f"server/sdgo/symbols/{host_key}.ini"
    if symbol_manifest_key not in checked_hashes:
        raise DeploymentError(
            f"SHA256.json omits immutable symbol file: {symbol_manifest_key}"
        )
    version_path = package_root / "VERSION"
    try:
        package_version = version_path.read_text(
            encoding="ascii"
        ).strip()
    except (OSError, UnicodeError) as error:
        raise DeploymentError(f"unable to read package VERSION: {error}") from error
    if not re.fullmatch(r"\d+\.\d+\.\d+", package_version):
        raise DeploymentError("package VERSION must be major.minor.patch")
    probe_version = version_probe or _file_version
    version_files = {
        "server/winmm.dll": package_server / "winmm.dll",
        "server/sdgo/core.dll": package_server / "sdgo" / "core.dll",
        "server/sdgo/mods/battle_rewards/battle_rewards.dll":
            package_mod / "battle_rewards.dll",
    }
    file_versions = {
        name: probe_version(path) for name, path in version_files.items()
    }
    wrong_versions = {
        name: value for name, value in file_versions.items()
        if value != package_version
    }
    if wrong_versions:
        raise DeploymentError(
            "package DLL/core versions disagree with VERSION "
            + package_version + ": " + json.dumps(wrong_versions, sort_keys=True)
        )
    plan.package_version = package_version
    plan.file_versions = file_versions
    plan.verified_package_files = len(checked_hashes)
    package_text = package_symbols.read_text(encoding="utf-8-sig")
    existing_text = (
        plan.target_symbols.read_text(encoding="utf-8-sig")
        if plan.target_symbols.is_file()
        else None
    )
    plan.merged_symbols = merge_required_symbols(
        existing_text, package_text, required
    )
    plan.source_hashes = {
        "winmm.dll": sha256_file(package_server / "winmm.dll"),
        "sdgo/core.dll": sha256_file(package_server / "sdgo" / "core.dll"),
        "battle_rewards.dll": sha256_file(
            package_mod / "battle_rewards.dll"
        ),
        f"sdgo/symbols/{host_key}.ini": hashlib.sha256(
            plan.merged_symbols.encode("utf-8")
        ).hexdigest(),
    }
    plan.steps = [
        "verify the exact supported BattleServer SHA-256 and PE host key",
        "verify the target is stopped; never kill or restart a process",
        "initialize the owner workspace and compile the current policy",
        "SELECT the native DB Game Boss identities into reward-bosses.txt",
        "install only versioned Battle Rewards ledger objects through the native MB procedure",
        "run install_server.ps1 with an allowlisted latest framework source",
        "merge required symbols without replacing unrelated definitions",
        "copy only the battle_rewards mod and preserve every other mod/config",
        "write UTF-16LE RuntimeDirectory/InstanceId config and explicitly enable the mod",
        "read back exact core/mod/symbol hashes and save an owner receipt",
    ]
    return plan


def _exclusive_read(path: Path) -> bool:
    if not path.exists() or os.name != "nt":
        return True
    kernel = ctypes.windll.kernel32
    kernel.CreateFileW.argtypes = [
        ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_uint32,
        ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p,
    ]
    kernel.CreateFileW.restype = ctypes.c_void_p
    kernel.CloseHandle.argtypes = [ctypes.c_void_p]
    handle = kernel.CreateFileW(
        str(path), 0x80000000, 0, None, 3, 0x80, None
    )
    if handle == ctypes.c_void_p(-1).value or handle is None:
        return False
    kernel.CloseHandle(handle)
    return True


def _assert_apply_ready(plan: DeploymentPlan, lock_probe=None) -> None:
    if plan.blockers:
        raise DeploymentError("; ".join(plan.blockers))
    probe = lock_probe or _exclusive_read
    paths = [
        plan.battle_dir / "BattleServer.exe",
        *_tracked_paths(plan.battle_dir, plan.host_key).values(),
    ]
    busy = [str(path) for path in paths if path.exists() and not probe(path)]
    if busy:
        raise DeploymentError(
            "target files are in use; stop the exact BattleServer first: "
            + ", ".join(busy)
        )


def _revalidate_apply(
    plan: DeploymentPlan,
    *,
    process_probe=None,
    version_probe=None,
) -> None:
    executable = plan.battle_dir / "BattleServer.exe"
    if (
        pe_host_key(executable) != plan.host_key
        or sha256_file(executable) != plan.host_sha256
    ):
        raise DeploymentError(
            "BattleServer.exe changed after planning; create a new plan"
        )
    target_norm = os.path.normcase(str(executable.resolve()))
    probe = process_probe or _process_probe
    for process in probe():
        path = process.get("path")
        if path and os.path.normcase(str(Path(path).resolve())) == target_norm:
            raise DeploymentError(
                "BattleServer.exe started after planning; stop the exact target "
                f"process (PID {process.get('pid')})"
            )
    current_destinations = {
        name: optional_hash(path)
        for name, path in _tracked_paths(
            plan.battle_dir, plan.host_key
        ).items()
    }
    if current_destinations != plan.destination_hashes:
        raise DeploymentError(
            "target files changed after planning; create a new plan"
        )
    if plan.action == "uninstall":
        return
    assert (
        plan.package_root and plan.package_server and plan.package_mod
        and plan.package_symbols and plan.target_symbols and plan.server_ini
    )
    if sha256_file(plan.server_ini) != plan.server_ini_hash:
        raise DeploymentError(
            "owner server INI changed after planning; create a new plan"
        )
    checked = verify_package_hashes(plan.package_root)
    if checked != plan.package_hashes:
        raise DeploymentError(
            "package changed after planning; create a new plan"
        )
    probe_version = version_probe or _file_version
    current_versions = {
        "server/winmm.dll":
            probe_version(plan.package_server / "winmm.dll"),
        "server/sdgo/core.dll":
            probe_version(plan.package_server / "sdgo" / "core.dll"),
        "server/sdgo/mods/battle_rewards/battle_rewards.dll":
            probe_version(plan.package_mod / "battle_rewards.dll"),
    }
    if current_versions != plan.file_versions:
        raise DeploymentError(
            "package DLL/core versions changed after planning"
        )
    required, _, hosts = _manifest(plan.package_mod / "mod.ini")
    if required != plan.required_symbols or plan.host_key not in hosts:
        raise DeploymentError(
            "Battle Rewards manifest changed after planning"
        )
    package_text = plan.package_symbols.read_text(encoding="utf-8-sig")
    existing_text = (
        plan.target_symbols.read_text(encoding="utf-8-sig")
        if plan.target_symbols.is_file()
        else None
    )
    merged = merge_required_symbols(existing_text, package_text, required)
    if merged != plan.merged_symbols:
        raise DeploymentError(
            "installed symbol definitions changed after planning"
        )
    current_sources = {
        "winmm.dll": sha256_file(plan.package_server / "winmm.dll"),
        "sdgo/core.dll": sha256_file(plan.package_server / "sdgo" / "core.dll"),
        "battle_rewards.dll":
            sha256_file(plan.package_mod / "battle_rewards.dll"),
        f"sdgo/symbols/{plan.host_key}.ini":
            hashlib.sha256(merged.encode("utf-8")).hexdigest(),
    }
    if current_sources != plan.source_hashes:
        raise DeploymentError(
            "install source changed after planning; create a new plan"
        )


def _connect_section(server_ini: Path, normalized: str, app: str):
    import pyodbc

    parser = configparser.ConfigParser(interpolation=None)
    parser.read(server_ini, encoding="utf-8-sig")
    section = _find_section(parser, normalized)
    if section is None:
        raise DeploymentError(f"owner INI lacks {normalized}")
    db = parser[section]
    drivers = [name for name in pyodbc.drivers() if "SQL Server" in name]
    if not drivers:
        raise DeploymentError("no SQL Server ODBC driver is installed")

    def escape(value):
        return "{" + str(value).replace("}", "}}") + "}"

    text = (
        f"DRIVER={escape(drivers[-1])};"
        f"SERVER={escape(db['DBServerName'])};"
        f"DATABASE={escape(db['DBName'])};"
        f"UID={escape(db['ID'])};PWD={escape(db['PWD'])};APP={app};"
    )
    try:
        result = pyodbc.connect(text, timeout=5, autocommit=False)
        result.timeout = 15
        return result
    except pyodbc.Error as error:
        state = str(error.args[0]) if error.args else "unknown"
        raise DeploymentError(
            f"owner database connection failed; SQLSTATE={state}"
        ) from None


def _connect_game(server_ini: Path):
    return _connect_section(
        server_ini, "dbgame", "SDGORewardInstallerBossRead"
    )


def _connect_billing(server_ini: Path):
    return _connect_section(
        server_ini, "dbstorebilling", "SDGORewardInstallerLedger"
    )


def _instance_id(battle_dir: Path) -> str:
    identity = os.path.normcase(str(battle_dir.resolve())).replace("\\", "/")
    result = "battle-" + hashlib.sha256(
        identity.encode("utf-8")
    ).hexdigest()[:16]
    if not _SAFE_INSTANCE.fullmatch(result):
        raise DeploymentError("unable to derive a safe Battle Rewards instance id")
    return result


def _ini_lines(path: Path) -> list[str]:
    if not path.is_file():
        return []
    try:
        raw = path.read_bytes()
        if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
            text = raw.decode("utf-16")
        else:
            text = raw.decode("utf-8-sig")
    except (OSError, UnicodeError):
        raise DeploymentError(f"unable to read editable INI {path}") from None
    return text.splitlines()


def _ini_section_span(
    path: Path, lines: list[str], section: str
) -> tuple[int | None, int]:
    header = f"[{section}]"
    matching_sections = [
        index for index, line in enumerate(lines)
        if line.strip().casefold() == header.casefold()
    ]
    if len(matching_sections) > 1:
        raise DeploymentError(
            f"{path} contains duplicate [{section}] sections"
        )
    start = matching_sections[0] if matching_sections else None
    if start is None:
        return None, len(lines)
    end = next(
        (
            index for index in range(start + 1, len(lines))
            if _SECTION.fullmatch(lines[index])
        ),
        len(lines),
    )
    seen: set[str] = set()
    for index in range(start + 1, end):
        stripped = lines[index].strip()
        if not stripped or stripped.startswith((";", "#")) or "=" not in stripped:
            continue
        key = stripped.split("=", 1)[0].strip().casefold()
        if key in seen:
            raise DeploymentError(
                f"{path} contains duplicate key {key!r} in [{section}]"
            )
        seen.add(key)
    return start, end


def _validate_ini_target(path: Path, section: str) -> None:
    _ini_section_span(path, _ini_lines(path), section)


def _set_ini_values(
    path: Path, section: str, updates: dict[str, str], *,
    encoding: str = "utf-8",
) -> None:
    lines = _ini_lines(path)
    header = f"[{section}]"
    start, end = _ini_section_span(path, lines, section)
    if start is None:
        if lines and lines[-1].strip():
            lines.append("")
        lines.append(header)
        start = len(lines) - 1
        end = len(lines)
    remaining = {key.casefold(): (key, value) for key, value in updates.items()}
    for index in range(start + 1, end):
        stripped = lines[index].strip()
        if not stripped or stripped.startswith((";", "#")) or "=" not in stripped:
            continue
        key = stripped.split("=", 1)[0].strip().casefold()
        if key in remaining:
            original, value = remaining.pop(key)
            lines[index] = f"{original}={value}"
    for key, value in remaining.values():
        lines.insert(end, f"{key}={value}")
        end += 1
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        rendered = "\n".join(lines).rstrip() + "\n"
        if encoding == "utf-16le-bom":
            data = b"\xff\xfe" + rendered.encode("utf-16-le")
        else:
            data = rendered.encode(encoding)
        with temporary.open("wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def _copy_allowlisted_source(plan: DeploymentPlan, destination: Path) -> None:
    assert plan.package_server and plan.package_mod and plan.target_symbols
    for relative in ("winmm.dll", "sdgo/core.dll", "sdgo/sdgo.ini"):
        source = plan.package_server / relative
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    symbol = destination / "sdgo" / "symbols" / plan.target_symbols.name
    symbol.parent.mkdir(parents=True, exist_ok=True)
    symbol.write_text(plan.merged_symbols, encoding="utf-8", newline="\n")

    _, payload_files, _ = _manifest(plan.package_mod / "mod.ini")
    target_mod = destination / "sdgo" / "mods" / "battle_rewards"
    target_mod.mkdir(parents=True, exist_ok=True)
    for name in ("battle_rewards.dll", "mod.ini", *payload_files):
        source = (plan.package_mod / name).resolve()
        if not source.is_relative_to(plan.package_mod.resolve()):
            raise DeploymentError(f"Battle Rewards payload escapes package: {name}")
        target = target_mod / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def _run_server_installer(
    install_script: Path, battle_dir: Path, source: Path
) -> str:
    completed = subprocess.run(
        [
            "powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
            "-File", str(install_script),
            "-ServerRoot", str(battle_dir),
            "-Source", str(source),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if completed.returncode:
        message = (completed.stderr or completed.stdout).strip()
        raise DeploymentError(
            "install_server.ps1 failed"
            + (": " + message[-1000:] if message else "")
        )
    return completed.stdout


def _receipt_directory(workspace: Path, action: str) -> Path:
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return (
        workspace / "deployment-receipts"
        / f"{stamp}-{action}-{uuid.uuid4().hex[:8]}"
    )


def _snapshot(paths: dict[str, Path]) -> dict[str, str | None]:
    return {name: optional_hash(path) for name, path in paths.items()}


def _safe_snapshot(paths: dict[str, Path]) -> dict[str, str | None]:
    result: dict[str, str | None] = {}
    for name, path in paths.items():
        try:
            result[name] = optional_hash(path)
        except OSError:
            result[name] = "unreadable"
    return result


def _write_json(path: Path, value: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = (
        json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True)
        + "\n"
    ).encode("utf-8")
    temporary = path.with_name("." + path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()


def apply_plan(
    plan: DeploymentPlan,
    *,
    lock_probe=None,
    process_probe=None,
    version_probe=None,
    game_connection_factory=None,
    billing_connection_factory=None,
    repository_factory=None,
    workspace_factory=None,
    server_runner=None,
) -> dict:
    _revalidate_apply(
        plan,
        process_probe=process_probe,
        version_probe=version_probe,
    )
    _assert_apply_ready(plan, lock_probe)
    receipt = _receipt_directory(plan.workspace, plan.action)
    target_mod = plan.battle_dir / "sdgo" / "mods" / "battle_rewards"
    tracked = _tracked_paths(plan.battle_dir, plan.host_key)
    before = _snapshot(tracked)
    started = {
        "schema_version": 1,
        "mode": "applying",
        "status": "started",
        "action": plan.action,
        "battle_directory": str(plan.battle_dir),
        "receipt": str(receipt),
        "before": before,
        "writes_performed": True,
    }
    try:
        receipt.mkdir(parents=True, exist_ok=False)
        _write_json(receipt / "receipt.json", started)
    except Exception:
        raise DeploymentApplyError(
            "unable to create the deployment receipt",
            receipt=receipt,
            writes_performed=True,
        ) from None
    phase = "backup"
    target_touched = False
    try:
        backup = receipt / "backup"
        for name, path in tracked.items():
            if path.is_file():
                destination = backup / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, destination)

        if plan.action == "uninstall":
            phase = "archive mod"
            moved = False
            if target_mod.exists():
                destination = receipt / "disabled" / "battle_rewards"
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(target_mod), str(destination))
                moved = True
            result = {
                "schema_version": 1,
                "mode": "applied",
                "status": "complete",
                "action": "uninstall",
                "battle_directory": str(plan.battle_dir),
                "receipt": str(receipt),
                "battle_rewards_moved": moved,
                "framework_preserved": True,
                "symbols_preserved": True,
                "wallet_ledger_preserved": True,
                "wallet_jobs_preserved": True,
                "wallet_balances_preserved": True,
                "before": before,
                "after": _snapshot(tracked),
                "writes_performed": True,
            }
            _write_json(receipt / "receipt.json", result)
            return result

        assert (
            plan.server_ini and plan.package_server and plan.package_mod
            and plan.install_script and plan.target_symbols
        )
        workspace_factory = workspace_factory or Workspace
        game_connection_factory = game_connection_factory or _connect_game
        billing_connection_factory = (
            billing_connection_factory or _connect_billing
        )
        repository_factory = repository_factory or RewardWalletRepository
        server_runner = server_runner or _run_server_installer

        phase = "workspace initialization"
        owner = workspace_factory(plan.workspace)
        runtime = owner.runtime.root

        phase = "Boss registry export"
        game = None
        try:
            game = game_connection_factory(plan.server_ini)
            rows = boss_catalog.read_native(game)
            boss_revision = boss_catalog.publish(
                runtime / "reward-bosses.txt", rows
            )
        except Exception as error:
            if isinstance(error, DeploymentError):
                raise
            raise DeploymentError(
                "native Boss registry export failed; verify DB Game schema "
                "and Boss identities"
            ) from None
        finally:
            if game is not None:
                try:
                    game.rollback()
                except Exception:
                    pass
                try:
                    game.close()
                except Exception:
                    pass

        phase = "wallet ledger installation"
        billing = None
        try:
            billing = billing_connection_factory(plan.server_ini)
            wallet_scope = repository_factory(billing).install()
        except Exception as error:
            if isinstance(error, DeploymentError):
                raise
            raise DeploymentError(
                "Battle Rewards MB ledger installation failed; native wallet "
                "procedures and balances were not intentionally modified"
            ) from None
        finally:
            if billing is not None:
                try:
                    billing.close()
                except Exception:
                    pass

        phase = "framework and mod installation"
        with tempfile.TemporaryDirectory(
            prefix="sdgo-battle-rewards-install-"
        ) as temporary:
            source = Path(temporary) / "server"
            _copy_allowlisted_source(plan, source)
            target_touched = True
            installer_output = server_runner(
                plan.install_script, plan.battle_dir, source
            )

        phase = "runtime configuration"
        config_path = target_mod / "config.ini"
        instance = _instance_id(plan.battle_dir)
        _set_ini_values(
            config_path,
            "settings",
            {
                "RuntimeDirectory": str(runtime.resolve()),
                "InstanceId": instance,
                "Trace": "0",
            },
            encoding="utf-16le-bom",
        )
        phase = "mod activation"
        _set_ini_values(target_mod / "mod.ini", "mod", {"enabled": "1"})

        phase = "installed file readback"
        expected = {
            "winmm.dll": plan.source_hashes["winmm.dll"],
            "sdgo/core.dll": plan.source_hashes["sdgo/core.dll"],
            "battle_rewards.dll": plan.source_hashes["battle_rewards.dll"],
            f"symbols/{plan.target_symbols.name}":
                plan.source_hashes[f"sdgo/symbols/{plan.host_key}.ini"],
        }
        after = _snapshot(tracked)
        mismatches = {
            name: {"expected": digest, "actual": after.get(name)}
            for name, digest in expected.items()
            if after.get(name) != digest
        }
        if mismatches:
            raise DeploymentError(
                "installed file readback mismatch: "
                + json.dumps(mismatches, sort_keys=True)
            )
        raw_config = config_path.read_bytes()
        if not raw_config.startswith(b"\xff\xfe"):
            raise DeploymentError(
                "Battle Rewards runtime config is not UTF-16LE with BOM"
            )
        parser = configparser.ConfigParser(interpolation=None, strict=True)
        parser.read(config_path, encoding="utf-16")
        if (
            parser.get("settings", "RuntimeDirectory", fallback="")
            != str(runtime.resolve())
            or parser.get("settings", "InstanceId", fallback="") != instance
            or parser.get("settings", "Trace", fallback="") != "0"
        ):
            raise DeploymentError(
                "Battle Rewards runtime config readback failed"
            )
        manifest = configparser.ConfigParser(
            interpolation=None, strict=True
        )
        manifest.read(target_mod / "mod.ini", encoding="utf-8-sig")
        if manifest.get("mod", "enabled", fallback="") != "1":
            raise DeploymentError("Battle Rewards was not explicitly enabled")

        result = {
            "schema_version": 1,
            "mode": "applied",
            "status": "complete",
            "action": "install",
            "battle_directory": str(plan.battle_dir),
            "runtime_directory": str(runtime.resolve()),
            "instance_id": instance,
            "receipt": str(receipt),
            "package_version": plan.package_version,
            "wallet_scope": str(wallet_scope),
            "boss_registry_revision": boss_revision,
            "boss_registry_pairs": len(rows),
            "installer_output": installer_output,
            "before": before,
            "after": after,
            "warnings": activation_warnings(
                plan.battle_dir / "sdgo" / "sdgo.ini"
            ),
            "native_wallet_procedure_replaced": False,
            "native_reward_tables_edited": False,
            "real_balance_changed_during_install": False,
            "writes_performed": True,
        }
        _write_json(receipt / "receipt.json", result)
        return result
    except Exception as error:
        if target_touched and (target_mod / "mod.ini").is_file():
            try:
                _set_ini_values(
                    target_mod / "mod.ini", "mod", {"enabled": "0"}
                )
            except Exception:
                pass
        if isinstance(error, DeploymentError):
            message = str(error)
        else:
            message = f"Battle Rewards deployment failed during {phase}"
        failed = {
            **started,
            "mode": "error",
            "status": "failed",
            "phase": phase,
            "error": message,
            "writes_performed": True,
            "target_touched": target_touched,
            "after": _safe_snapshot(tracked),
        }
        try:
            _write_json(receipt / "receipt.json", failed)
        except Exception:
            pass
        raise DeploymentApplyError(
            message,
            receipt=receipt,
            writes_performed=True,
        ) from None
