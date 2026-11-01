"""Allowlisted owner kit for native contribution rewards; excludes game data and private runtime state."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile
from package_frp import file_version, validate_markdown_links

OWNER_FILES = (
    "__init__.py", "model.py", "policy.example.json", "workspace.py", "runtime_control.py",
    "boss_catalog.py", "sql_repository.py", "worker.py", "deployment.py", "sql/battle_reward_wallet.sql",
    "examples/pvp-mvp.json", "examples/mission-clear.json",
)
TOOLS = ("reward_assistant.py", "reward_preview.py", "reward_worker.py", "install_battle_rewards.py",
         "Start-Battle-Rewards.ps1", "Stop-Battle-Rewards.ps1")


def contents(repo, binaries):
    version = tuple(int(n) for n in (repo / "VERSION").read_text().strip().split(".")) + (0,)
    files = {}
    def add(name, source):
        raw = source.read_bytes()
        if name.endswith(".dll"):
            pe = struct.unpack_from("<I", raw, 0x3C)[0]
            if raw[pe:pe+4] != b"PE\0\0" or struct.unpack_from("<H", raw, pe+4)[0] != 0x14C or file_version(source) != version:
                raise ValueError("Stale or non-x86 reward binary: " + source.name)
        files[name] = raw
    for name in OWNER_FILES:
        add("owner/rewards/" + name, repo / "owner/rewards" / name)
    for name in TOOLS:
        add("tools/" + name, repo / "tools" / name)
    for path in sorted((repo / "tools/assistant_modules/rewards").iterdir()):
        if path.name in ("__init__.py", "ui.html"):
            add("tools/assistant_modules/rewards/" + path.name, path)
    for name in ("install_server.ps1", "install_common.ps1"):
        add(name, repo / "tools" / name)
    for name in ("battle-rewards.md", "battle-rewards.zh-CN.md", "battle-reward-design.md", "battle-reward-design.zh-CN.md"):
        add("docs/" + name, repo / "docs" / name)
    files["README.md"] = (
        "# Battle contribution rewards (Beta)\n\n"
        "Owner toolkit for the native BattleServer Mod. No game files, credentials or private runtime configuration are included.\n\n"
        "Read [deployment and acceptance](docs/battle-rewards.md) before applying.\n"
        "Python is required on the owner computer; players do not run this wallet worker.\n"
    ).encode()
    add("server/winmm.dll", binaries / "winmm.dll")
    add("server/sdgo/core.dll", binaries / "core.dll")
    add("server/sdgo/sdgo.ini", repo / "config/sdgo.ini")
    add("server/sdgo/symbols/BattleServer-548A891D-0046C000.ini", repo / "symbols/BattleServer-548A891D-0046C000.ini")
    for name in ("mod.ini", "config.example.ini"):
        add("server/sdgo/mods/battle_rewards/" + name, repo / "mods/battle_rewards" / name)
    add("server/sdgo/mods/battle_rewards/battle_rewards.dll", binaries / "battle_rewards.dll")
    add("VERSION", repo / "VERSION")
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        add(name, repo / name)
    add("licenses/MinHook-LICENSE.txt", repo / "third_party/minhook/LICENSE.txt")
    files["requirements-rewards.txt"] = b"pyodbc>=5,<6\n"
    validate_markdown_links(files)
    return files


def package(repo, binaries, out, directory_only=False):
    files = contents(repo, binaries)
    archive = out.with_name(out.name + ".zip")
    if out.exists() or archive.exists():
        raise FileExistsError("Reward package output already exists")
    files["SHA256.json"] = (json.dumps({k: hashlib.sha256(v).hexdigest() for k, v in files.items()}, indent=2) + "\n").encode()
    out.mkdir(parents=True)
    for name, data in files.items():
        destination = out / name; destination.parent.mkdir(parents=True, exist_ok=True); destination.write_bytes(data)
    if directory_only:
        return out
    with zipfile.ZipFile(archive, "x", compression=zipfile.ZIP_DEFLATED) as writer:
        for name, data in files.items(): writer.writestr(name, data)
    with zipfile.ZipFile(archive) as reader:
        if reader.testzip() is not None: raise ValueError("Reward archive CRC failed")
    return archive


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--binaries", type=Path)
    parser.add_argument("--directory-only", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = package(root, args.binaries or root / "build/Release", args.out, args.directory_only)
    print(json.dumps({"output": str(output.resolve()), "directory_only": args.directory_only}))
