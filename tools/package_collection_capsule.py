"""Build the portable collection-capsule owner toolkit.

The output is an allowlisted source/tool/framework kit.  It never copies a
client installation, generated server model, database snapshot, player
history, private INI, test binary, or build log.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import uuid
import zipfile
import zlib

try:
    from package_frp import file_version, validate_markdown_links
except ModuleNotFoundError:
    from tools.package_frp import file_version, validate_markdown_links


CLIENT_SYMBOL = "GOnline-54BE1568-00C97000.ini"
SERVER_SYMBOL = "WorldServer-548A86E3-0010C000.ini"
STAGE_FILES = {
    "framework/client/d3d9.dll": "client/d3d9.dll",
    "framework/client/sdgo/core.dll": "client/sdgo/core.dll",
    "framework/client/sdgo/sdgo.ini": "client/sdgo/sdgo.ini",
    f"framework/client/sdgo/symbols/{CLIENT_SYMBOL}":
        f"client/sdgo/symbols/{CLIENT_SYMBOL}",
    "framework/server/winmm.dll": "server/winmm.dll",
    "framework/server/sdgo/core.dll": "server/sdgo/core.dll",
    "framework/server/sdgo/sdgo.ini": "server/sdgo/sdgo.ini",
    f"framework/server/sdgo/symbols/{SERVER_SYMBOL}":
        f"server/sdgo/symbols/{SERVER_SYMBOL}",
    "framework/server/sdgo/mods/collection_capsule/collection_capsule.dll":
        "server/sdgo/mods/collection_capsule/collection_capsule.dll",
    "framework/server/sdgo/mods/collection_capsule/mod.ini":
        "server/sdgo/mods/collection_capsule/mod.ini",
    "framework/server/sdgo/mods/collection_capsule/manager.json":
        "server/sdgo/mods/collection_capsule/manager.json",
    "framework/server/sdgo/mods/collection_capsule/config.example.ini":
        "server/sdgo/mods/collection_capsule/config.example.ini",
    "framework/support/capsule-registry/capsule_catalog.dll":
        "support/capsule-registry/capsule_catalog.dll",
}
TOOL_FILES = (
    "collection_capsule.py",
    "package_collection_capsule_client.py",
    "install_server.ps1",
    "install_common.ps1",
)
SUPPORT_FILES = (
    "manager.json",
    "Install-Client.ps1",
    "Uninstall-Client.ps1",
    "runtime-mod.ini",
    "PROVENANCE.md",
)
ASSET_FILES = ("cover.png", "cover.svg")
DOCUMENTS = ("collection-capsule.md", "collection-capsule.zh-CN.md")
ZIP_TIMESTAMP = (2020, 1, 1, 0, 0, 0)


class OwnerPackageError(ValueError):
    """The portable owner kit failed an allowlist or artifact check."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value) -> bytes:
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


def _file(path: Path, label: str) -> Path:
    if not path.is_file() or path.is_symlink():
        raise OwnerPackageError(f"{label} is missing or linked: {path}")
    return path


def _version(repo: Path) -> tuple[str, tuple[int, int, int, int]]:
    text = _file(repo / "VERSION", "repository VERSION").read_text(
        encoding="ascii"
    ).strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+", text):
        raise OwnerPackageError("Repository VERSION must contain three integers")
    return text, tuple(int(part) for part in text.split(".")) + (0,)


def _validate_x86_dll(path: Path, expected_version: tuple[int, int, int, int]) -> bytes:
    raw = _file(path, "staged DLL").read_bytes()
    if len(raw) < 0x40 or raw[:2] != b"MZ":
        raise OwnerPackageError(f"Staged binary is not a PE DLL: {path.name}")
    pe = struct.unpack_from("<I", raw, 0x3C)[0]
    if (
        pe < 0x40
        or pe + 24 > len(raw)
        or raw[pe : pe + 4] != b"PE\0\0"
        or struct.unpack_from("<H", raw, pe + 4)[0] != 0x14C
        or not struct.unpack_from("<H", raw, pe + 22)[0] & 0x2000
    ):
        raise OwnerPackageError(f"Staged binary is not an x86 DLL: {path.name}")
    if file_version(path) != expected_version:
        raise OwnerPackageError(
            f"Staged DLL version differs from repository VERSION: {path.name}"
        )
    return raw


def _git_commit(repo: Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--verify", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    value = result.stdout.strip().lower()
    return value if re.fullmatch(r"[0-9a-f]{40,64}", value) else None


def contents(repo: Path, stage: Path) -> dict[str, bytes]:
    repo = Path(repo)
    stage = Path(stage)
    if not repo.is_dir() or repo.is_symlink():
        raise OwnerPackageError("Repository root is missing or linked")
    if not stage.is_dir() or stage.is_symlink():
        raise OwnerPackageError("Framework stage is missing or linked")
    version_text, expected_version = _version(repo)
    files: dict[str, bytes] = {}

    def add(destination: str, source: Path) -> None:
        if (
            not destination
            or "\\" in destination
            or destination.startswith("/")
            or any(part in ("", ".", "..") for part in destination.split("/"))
            or destination in files
        ):
            raise OwnerPackageError(f"Unsafe or duplicate package path: {destination}")
        source = _file(source, f"package source {destination}")
        files[destination] = (
            _validate_x86_dll(source, expected_version)
            if destination.lower().endswith(".dll")
            else source.read_bytes()
        )

    for destination, relative in STAGE_FILES.items():
        add(destination, stage.joinpath(*relative.split("/")))
    if b'SDGO_CAPSULE_REGISTRY_6TABLE_V2' not in files['framework/support/capsule-registry/capsule_catalog.dll']:
        raise OwnerPackageError('The owner toolkit needs the current six-table capsule registry provider')

    owner_root = repo / "owner" / "collection_capsule"
    owner_python = sorted(owner_root.glob("*.py"), key=lambda path: path.name)
    owner_sql = sorted((owner_root / "sql").glob("*.sql"), key=lambda path: path.name)
    if not owner_python or not owner_sql:
        raise OwnerPackageError("Collection owner sources are incomplete")
    for source in owner_python:
        add(f"owner/collection_capsule/{source.name}", source)
    for source in owner_sql:
        add(f"owner/collection_capsule/sql/{source.name}", source)
    for name in TOOL_FILES:
        add(f"tools/{name}", repo / "tools" / name)
    for name in SUPPORT_FILES:
        add(
            f"support/capsule_registry/{name}",
            repo / "support" / "capsule_registry" / name,
        )
    for name in ASSET_FILES:
        add(
            f"mods/collection_capsule/assets/{name}",
            repo / "mods" / "collection_capsule" / "assets" / name,
        )
    for name in DOCUMENTS:
        add(f"docs/{name}", repo / "docs" / name)
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md", "VERSION"):
        add(name, repo / name)
    add(
        "licenses/MinHook-LICENSE.txt",
        repo / "third_party" / "minhook" / "LICENSE.txt",
    )
    files["framework/VERSION"] = files["VERSION"]
    files["requirements-collection-capsule.txt"] = (
        b"Pillow>=10,<13\npyodbc>=5,<6\n"
    )
    files["README.md"] = (
        "# Collection capsule owner toolkit (Beta)\n\n"
        "Portable tools and matched framework files for building and installing "
        "the collection capsule. It contains no game client, credentials, "
        "database snapshot, player history, or generated server package.\n\n"
        "Install owner dependencies with "
        "`python -m pip install -r requirements-collection-capsule.txt`, then "
        "read [the owner tutorial](docs/collection-capsule.md).\n"
    ).encode("utf-8")
    files["README.zh-CN.md"] = (
        "# 收藏扭蛋服主工具包（Beta）\n\n"
        "本包提供收藏扭蛋的服主工具和配套 framework，不包含游戏客户端、凭据、"
        "数据库快照、玩家历史或已经生成的服务器包。\n\n"
        "先运行 `python -m pip install -r requirements-collection-capsule.txt`，"
        "再阅读[服主教程](docs/collection-capsule.zh-CN.md)。\n"
    ).encode("utf-8")

    source_inventory = {
        name: _sha256(data)
        for name, data in sorted(files.items())
        if not name.startswith("framework/")
    }
    provenance = {
        "schema_version": 1,
        "feature": "collection-capsule",
        "version": version_text,
        "source_tree_sha256": _sha256(_json_bytes(source_inventory)),
        "source_files": len(source_inventory),
    }
    commit = _git_commit(repo)
    if commit is not None:
        provenance["git_commit"] = commit
    files["SOURCE_PROVENANCE.json"] = _json_bytes(provenance)
    validate_markdown_links(files)
    return files


def _write_directory(destination: Path, files: dict[str, bytes]) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    pending = destination.with_name(
        f".{destination.name}.{uuid.uuid4().hex}.pending"
    )
    pending.mkdir()
    try:
        for name, data in sorted(files.items()):
            target = pending.joinpath(*name.split("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            if target.read_bytes() != data:
                raise OSError(f"Owner package readback failed: {name}")
        pending.replace(destination)
    except BaseException:
        shutil.rmtree(pending, ignore_errors=True)
        raise


def _zip_info(name: str) -> zipfile.ZipInfo:
    info = zipfile.ZipInfo(name, ZIP_TIMESTAMP)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    return info


def _write_zip(path: Path, files: dict[str, bytes]) -> None:
    with zipfile.ZipFile(
        path, "x", compression=zipfile.ZIP_DEFLATED, compresslevel=9
    ) as archive:
        for name, data in sorted(files.items()):
            archive.writestr(_zip_info(name), data, compresslevel=9)
    with zipfile.ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise OwnerPackageError("Owner package ZIP CRC verification failed")
        if archive.namelist() != sorted(files):
            raise OwnerPackageError("Owner package ZIP order differs")
        for info in archive.infolist():
            data = files[info.filename]
            if info.CRC != zlib.crc32(data) & 0xFFFFFFFF:
                raise OwnerPackageError(
                    f"Owner package ZIP CRC differs: {info.filename}"
                )
            if archive.read(info.filename) != data:
                raise OwnerPackageError(
                    f"Owner package ZIP readback differs: {info.filename}"
                )


def package(
    repo: Path,
    stage: Path,
    out: Path,
    directory_only: bool = False,
) -> Path:
    """Create the allowlisted toolkit directory and optional deterministic ZIP."""
    out = Path(out)
    archive = out.with_name(out.name + ".zip")
    if out.exists() or archive.exists():
        raise FileExistsError("Refusing to replace an existing owner toolkit")
    files = contents(Path(repo), Path(stage))
    inventory = {name: _sha256(data) for name, data in sorted(files.items())}
    files["SHA256.json"] = _json_bytes(inventory)
    _write_directory(out, files)
    if directory_only:
        return out
    try:
        _write_zip(archive, files)
    except BaseException:
        if archive.exists():
            archive.unlink()
        raise
    return archive


def main() -> None:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument(
        "--stage",
        type=Path,
        default=root / "build" / "stage" / "Release",
    )
    parser.add_argument("--directory-only", action="store_true")
    args = parser.parse_args()
    result = package(root, args.stage, args.out, args.directory_only)
    payload = {
        "output": str(result.resolve()),
        "directory_only": args.directory_only,
    }
    if result.is_file():
        payload.update(bytes=result.stat().st_size, sha256=_sha256(result.read_bytes()))
    print(json.dumps(payload, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
