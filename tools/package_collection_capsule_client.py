"""Build a self-contained collection capsule player package.

The package contains the current framework client loader/core, the shared
capsule registry provider, one compiled registry and a receipt-based scoped
uninstaller. It contains no server address, account, database or player
history.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import uuid


ROOT = Path(__file__).resolve().parents[1]
HOST_SYMBOL = "GOnline-54BE1568-00C97000.ini"
PACKAGE_ID = "collection-capsule-client"
SUPPORT_MARKER = b'SDGO_CAPSULE_REGISTRY_6TABLE_V2'
CAPSULE_SYMBOLS = {
    "capsule.main.open_file": {
        "kind": "code",
        "rva": "0x007E5600",
        "expect": "55 8B EC 6A FF 68 D3 3D E1 00 64 A1 00 00 00 00 50 83 EC 50 A1 10 71 FF",
    },
    "capsule.main.file_ctor": {
        "kind": "code",
        "rva": "0x007E95E0",
        "expect": "55 8B EC 6A FF 68 AB 40 E1 00 64 A1 00 00 00 00 50 83 EC 14 A1 10 71 FF",
    },
    "capsule.main.operator_new": {
        "kind": "code",
        "rva": "0x006F481C",
        "expect": "8B FF 55 8B EC 56 EB 13 A1 58 2A FE 00 85 C0 74 19 FF 75 08 FF D0 59 85",
    },
    "capsule.main.wfopen": {
        "kind": "code",
        "rva": "0x00887D78",
        "expect": "8B FF 55 8B EC 6A 40 FF 75 0C FF 75 08 E8 30 FF FF FF 83 C4 0C 5D C3",
    },
    "capsule.grid_verify_checksum": {
        "kind": "code",
        "rva": "0x00014980",
        "expect": "55 8B EC 6A FF 68 DE 77 D9 00 64 A1 00 00 00 00 50 81 EC 3C 06 00 00 A1 10 71 FF 00",
    },
    "capsule.grid_load_all": {
        "kind": "code",
        "rva": "0x00031E50",
        "expect": "56 8B F1 E8 48 0E 00 00 6A 01 68 B8 10 E3 00 8B CE E8 FA A8 FF FF 6A 04",
    },
}
GRID_SYMBOLS = {
    "grid.data": {
        "kind": "code",
        "rva": "0x00001D40",
        "expect": "55 8B EC 6A FF 68 6B 8B E1 00 64 A1 00 00 00 00 50 51 A1 10 71 FF 00 33",
    },
    "grid.find": {
        "kind": "code",
        "rva": "0x0019D2B0",
        "expect": "55 8B EC 51 56 8B F1 8D 45 0C 50 8D 4D FC 51 8B CE E8 4A E7 08 00 8B 45",
    },
}
HOST_VALUES = {
    "image": "GOnline.exe",
    "timestamp": "0x54BE1568",
    "size_of_image": "0x00C97000",
}


class ClientPackageError(ValueError):
    """The package input or output violates the standalone client contract."""


def _absolute(path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value, *, pretty=False) -> bytes:
    if pretty:
        text = json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)
    else:
        text = json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        )
    return (text + "\n").encode("utf-8")


def _assert_no_links(path: Path, label: str) -> None:
    current = _absolute(path)
    while True:
        if current.exists():
            attributes = getattr(current.lstat(), "st_file_attributes", 0)
            if current.is_symlink() or attributes & getattr(
                stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400
            ):
                raise ClientPackageError(f"{label} uses a linked path: {current}")
        parent = current.parent
        if parent == current:
            break
        current = parent


def _file(path, label: str) -> Path:
    result = _absolute(path)
    if not result.is_file():
        raise ClientPackageError(f"{label} is missing: {result}")
    _assert_no_links(result, label)
    return result


def _directory(path, label: str) -> Path:
    result = _absolute(path)
    if not result.is_dir():
        raise ClientPackageError(f"{label} is missing: {result}")
    _assert_no_links(result, label)
    return result


def _safe_join(root: Path, relative: str) -> Path:
    if (
        not relative
        or "\\" in relative
        or Path(relative).is_absolute()
        or any(part in ("", ".", "..") for part in relative.split("/"))
    ):
        raise ClientPackageError(f"Unsafe package path: {relative}")
    result = _absolute(root.joinpath(*relative.split("/")))
    try:
        result.relative_to(_absolute(root))
    except ValueError as error:
        raise ClientPackageError(f"Package path escapes its root: {relative}") from error
    return result


def _read_json(path: Path, label: str):
    try:
        value = json.loads(path.read_text(encoding="utf-8-sig"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ClientPackageError(f"{label} is invalid JSON") from error
    if not isinstance(value, dict):
        raise ClientPackageError(f"{label} must be a JSON object")
    return value


def _parse_sections(data: bytes, label: str) -> dict[str, dict[str, str]]:
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError as error:
        raise ClientPackageError(f"{label} must be UTF-8") from error
    result: dict[str, dict[str, str]] = {}
    current = None
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if not stripped or stripped.startswith((";", "#")):
            continue
        match = re.fullmatch(r"\[([^\]]+)\]", stripped)
        if match:
            current = match.group(1).strip().lower()
            if not current or current in result:
                raise ClientPackageError(f"{label} has a duplicate/empty section")
            result[current] = {}
            continue
        if current is None or "=" not in stripped:
            raise ClientPackageError(f"{label} has invalid content on line {number}")
        key, value = stripped.split("=", 1)
        key = key.strip().lower()
        if not key or key in result[current]:
            raise ClientPackageError(f"{label} has a duplicate/empty key on line {number}")
        result[current][key] = value.strip()
    return result


def _validate_section(
    sections: dict[str, dict[str, str]], name: str, expected: dict[str, str]
) -> None:
    actual = sections.get(name)
    if actual is None:
        raise ClientPackageError(f"Staged host symbols lack [{name}]")
    normalized = {key: value.lower() for key, value in actual.items()}
    wanted = {key: value.lower() for key, value in expected.items()}
    if normalized != wanted:
        raise ClientPackageError(f"Staged host symbol section differs: [{name}]")


def _render_symbols(sections: list[tuple[str, dict[str, str]]], preamble: str) -> bytes:
    blocks = [preamble.rstrip(), ""]
    for name, values in sections:
        blocks.append(f"[{name}]")
        blocks.extend(f"{key}={value}" for key, value in values.items())
        blocks.append("")
    return ("\n".join(blocks).rstrip() + "\n").encode("utf-8")


def _symbol_files(stage_symbol: Path) -> tuple[bytes, bytes]:
    sections = _parse_sections(stage_symbol.read_bytes(), "staged host symbols")
    _validate_section(sections, "host", HOST_VALUES)
    for name, expected in GRID_SYMBOLS.items():
        _validate_section(sections, name, expected)
    for name, expected in CAPSULE_SYMBOLS.items():
        _validate_section(sections, name, expected)
    full = [("host", HOST_VALUES)]
    full.extend(GRID_SYMBOLS.items())
    full.extend(CAPSULE_SYMBOLS.items())
    fragment = [("host", HOST_VALUES)]
    fragment.extend(CAPSULE_SYMBOLS.items())
    return (
        _render_symbols(
            full,
            "; Minimal host profile for the collection capsule player package.",
        ),
        _render_symbols(
            fragment,
            "; Capsule-owned sections merged into an existing client profile.",
        ),
    )


def _validated_registry(registry: Path) -> tuple[dict, dict[str, bytes]]:
    manifest_path = _file(registry / "manifest.json", "client registry manifest")
    manifest = _read_json(manifest_path, "client registry manifest")
    if (
        manifest.get("schema_version") != 1
        or type(manifest.get("machine", {}).get("id")) is not int
        or not 0 < manifest['machine']['id'] <= 2147483647
        or manifest.get("machine", {}).get("price_gp") != 100000
        or manifest.get("machine", {}).get("enabled") is not True
        or manifest.get('machine', {}).get('kind') != 'activity'
        or not re.fullmatch(
            r"[0-9a-f]{64}", str(manifest.get("catalog_revision", ""))
        )
    ):
        raise ClientPackageError("Client registry identity differs")
    declared = manifest.get("files")
    if not isinstance(declared, dict) or not declared:
        raise ClientPackageError("Client registry manifest has no files")
    payloads: dict[str, bytes] = {}
    for relative, expected in declared.items():
        if not isinstance(relative, str) or not re.fullmatch(r"[0-9a-f]{64}", str(expected)):
            raise ClientPackageError("Client registry manifest has an invalid file entry")
        path = _file(_safe_join(registry, relative), f"client registry file {relative}")
        raw = path.read_bytes()
        if _sha256(raw) != expected:
            raise ClientPackageError(f"Client registry file hash differs: {relative}")
        payloads[relative] = raw
    actual = {
        path.relative_to(registry).as_posix()
        for path in registry.rglob("*")
        if path.is_file()
    }
    if actual != set(payloads) | {"manifest.json"}:
        raise ClientPackageError("Client registry contains unmanifested files")
    payloads["manifest.json"] = manifest_path.read_bytes()
    return manifest, payloads


def package_client_registry(
    *,
    registry,
    framework_stage,
    support_dll,
    output,
) -> dict:
    """Create one no-Python player package from verified build artifacts."""
    registry = _directory(registry, "client registry")
    stage = _directory(framework_stage, "framework client stage")
    support_dll = _file(support_dll, "capsule registry support DLL")
    if SUPPORT_MARKER not in support_dll.read_bytes():
        raise ClientPackageError('Update the shared capsule provider: activity posters and retained unit gifts require the current six-table build')
    output = _absolute(output)
    if output.exists():
        raise FileExistsError(f"Client package output already exists: {output}")
    _assert_no_links(output.parent, "client package output parent")
    manifest, registry_files = _validated_registry(registry)

    stage_files = {
        "d3d9.dll": _file(stage / "d3d9.dll", "current framework loader"),
        "sdgo/core.dll": _file(stage / "sdgo/core.dll", "current framework core"),
        "sdgo/sdgo.ini": _file(stage / "sdgo/sdgo.ini", "current framework config"),
    }
    stage_symbol = _file(
        stage / "sdgo" / "symbols" / HOST_SYMBOL,
        "current framework client symbols",
    )
    package_symbols, fragment = _symbol_files(stage_symbol)
    mod_ini = _file(
        ROOT / "support" / "capsule_registry" / "runtime-mod.ini",
        "capsule registry support mod.ini",
    ).read_bytes()
    mod_sections = _parse_sections(mod_ini, "capsule registry support mod.ini")
    if (
        mod_sections.get("mod", {}).get("id") != "capsule_catalog"
        or mod_sections.get("mod", {}).get("enabled") != "1"
    ):
        raise ClientPackageError("Capsule registry support mod.ini identity differs")

    installer = _file(
        ROOT / "support" / "capsule_registry" / "Install-Client.ps1",
        "standalone client installer",
    ).read_bytes()
    uninstaller = _file(
        ROOT / "support" / "capsule_registry" / "Uninstall-Client.ps1",
        "standalone client uninstaller",
    ).read_bytes()
    install_cmd = (
        b"@echo off\r\n"
        b"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass "
        b"-File \"%~dp0Install-Client.ps1\" %*\r\n"
        b"if errorlevel 1 pause\r\n"
    )
    uninstall_cmd = (
        b"@echo off\r\n"
        b"powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass "
        b"-File \"%~dp0Uninstall-Client.ps1\" %*\r\n"
        b"if errorlevel 1 pause\r\n"
    )
    readme = (
        "Collection Capsule client package (Beta)\r\n"
        "\r\n"
        "Install: double-click Install-Client.cmd and select the folder containing GOnline.exe.\r\n"
        "Uninstall: close that client, double-click Uninstall-Client.cmd and select the same folder.\r\n"
        f"The server owner must install the matching activity capsule data (MachineID {manifest['machine']['id']}).\r\n"
        f"Find it under Activity capsule: page {manifest['placement']['page']}, slot {manifest['placement']['page_slot']}.\r\n"
        "Rewards arrive in the native prize box; claim the permanent level-1 unit there.\r\n"
        "C/B/A/S rank weights: 35/30/20/15. Exhausted ranks are removed and remaining weights renormalized.\r\n"
        "This player package contains no server address, account, database credential or player history.\r\n"
    ).encode("utf-8")

    payloads: dict[str, bytes] = {
        relative: path.read_bytes() for relative, path in stage_files.items()
    }
    payloads[f"sdgo/symbols/{HOST_SYMBOL}"] = package_symbols
    mod_prefix = "sdgo/mods/capsule_catalog"
    payloads[f"{mod_prefix}/capsule_catalog.dll"] = support_dll.read_bytes()
    payloads[f"{mod_prefix}/mod.ini"] = mod_ini
    payloads[f"{mod_prefix}/manager.json"] = _file(
        ROOT / "support/capsule_registry/manager.json", "capsule Manager contract").read_bytes()
    for relative, raw in registry_files.items():
        payloads[f"{mod_prefix}/{relative}"] = raw

    profile = {
        "schema_version": 1,
        "package_id": PACKAGE_ID,
        "catalog_revision": manifest["catalog_revision"],
        "framework": {
            relative: _sha256(raw)
            for relative, raw in sorted(payloads.items())
            if relative in stage_files or relative == f"sdgo/symbols/{HOST_SYMBOL}"
        },
        "support_dll_sha256": _sha256(support_dll.read_bytes()),
        "registry_manifest_sha256": _sha256(registry_files["manifest.json"]),
        "support_mod_ini_sha256": _sha256(mod_ini),
        "symbol_fragment_sha256": _sha256(fragment),
        "installer_sha256": _sha256(installer),
        "uninstaller_sha256": _sha256(uninstaller),
        "install_cmd_sha256": _sha256(install_cmd),
        "uninstall_cmd_sha256": _sha256(uninstall_cmd),
        "readme_sha256": _sha256(readme),
    }
    source_profile = _sha256(_json_bytes(profile))
    player_manifest = {
        "schema_version": 1,
        "package_id": PACKAGE_ID,
        "catalog_revision": manifest["catalog_revision"],
        "source_profile": source_profile,
        "symbol_fragment_sha256": _sha256(fragment),
        "files": {
            relative: _sha256(raw) for relative, raw in sorted(payloads.items())
        },
    }

    pending = output.with_name(f".{output.name}.{uuid.uuid4().hex}.pending")
    output.parent.mkdir(parents=True, exist_ok=True)
    pending.mkdir()
    try:
        root_files = {
            "Install-Client.ps1": installer,
            "Install-Client.cmd": install_cmd,
            "Uninstall-Client.ps1": uninstaller,
            "Uninstall-Client.cmd": uninstall_cmd,
            "README.txt": readme,
            "symbol-fragment.ini": fragment,
            "package-manifest.json": _json_bytes(player_manifest, pretty=True),
        }
        bundle_files = {
            **{
                name: _sha256(raw) for name, raw in sorted(root_files.items())
            },
            **{
                f"client/{name}": _sha256(raw)
                for name, raw in sorted(payloads.items())
            },
        }
        root_files["bundle-manifest.json"] = _json_bytes(
            {
                "schema_version": 1,
                "package_id": PACKAGE_ID,
                "catalog_revision": manifest["catalog_revision"],
                "source_profile": source_profile,
                "files": bundle_files,
            },
            pretty=True,
        )
        for relative, raw in {**root_files, **{
            f"client/{name}": data for name, data in payloads.items()
        }}.items():
            target = _safe_join(pending, relative)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
            if target.read_bytes() != raw:
                raise OSError(f"Client package write readback failed: {relative}")
        pending.replace(output)
    except BaseException:
        shutil.rmtree(pending, ignore_errors=True)
        raise

    return {
        "output": str(output),
        "package_id": PACKAGE_ID,
        "catalog_revision": manifest["catalog_revision"],
        "source_profile": source_profile,
        "files": len(payloads),
        "install": "Install-Client.cmd",
        "uninstall": "Uninstall-Client.cmd",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--framework-stage", type=Path, required=True)
    parser.add_argument("--support-dll", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    print(
        json.dumps(
            package_client_registry(
                registry=args.registry,
                framework_stage=args.framework_stage,
                support_dll=args.support_dll,
                output=args.out,
            ),
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    try:
        main()
    except (ClientPackageError, FileExistsError, OSError) as error:
        raise SystemExit(str(error)) from None
