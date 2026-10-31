#!/usr/bin/env python3
"""Build the standalone AI catalog addon package; no owner data or game files."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import zipfile

VERSION = "0.1.0"
ROOT = Path(__file__).resolve().parents[1]
SECTIONS = {"client": ("grid.find",), "server": ("catalog.find_ai_npc", "catalog.find_unit",
    "catalog.npc_manager", "catalog.unit_manager", "battle.build_ai_pilot")}
HOST = {"client": "GOnline-54BE1568-00C97000.ini", "server": "BattleServer-548A891D-0046C000.ini"}


def fragment(data, names):
    source = configparser.ConfigParser(interpolation=None)
    source.read_string(data.decode("utf-8-sig"))
    output = []
    for name in ("host", *names):
        output.append(f"[{name}]\n")
        output.extend(f"{key}={value}\n" for key, value in source[name].items())
        output.append("\n")
    return "".join(output).encode("ascii")


def package(stage, output):
    stage, output = Path(stage), Path(output)
    if output.exists(): raise FileExistsError("output already exists")
    files = {}
    for role in ("client", "server"):
        for name in ("mod.ini", "ai_catalog.dll"):
            relative = f"{role}/sdgo/mods/ai_catalog/{name}"
            files[relative] = (stage / relative).read_bytes()
        relative = f"{role}/sdgo/symbols/{HOST[role]}"
        files[relative] = fragment((stage / relative).read_bytes(), SECTIONS[role])
    for mod in ("coop_ai", "pvp_ai"):
        relative = f"server/sdgo/mods/{mod}/{mod}.dll"
        files[relative] = (stage / relative).read_bytes()
    for name in ("ai_catalog.py", "install_ai_catalog.py", "export_ai_catalog.py", "ai_catalog.rules.example.json", "README.zh-CN.md"):
        files["backend/" + name] = (ROOT / "backend" / name).read_bytes()
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        files[name] = (ROOT / name).read_bytes()
    files["README.zh-CN.md"] = (ROOT / "docs/ai-catalog.zh-CN.md").read_bytes()
    files["README.md"] = (ROOT / "docs/ai-catalog.md").read_bytes()
    files["docs/ai-catalog.zh-CN.md"] = files["README.zh-CN.md"]
    files["docs/ai-catalog.md"] = files["README.md"]
    files["README.zh-CN.md"] = files["README.zh-CN.md"].replace(b"(ai-catalog.md)", b"(docs/ai-catalog.md)")
    files["README.md"] = files["README.md"].replace(b"(ai-catalog.zh-CN.md)", b"(docs/ai-catalog.zh-CN.md)")
    files["licenses/MinHook-LICENSE.txt"] = (ROOT / "third_party/minhook/LICENSE.txt").read_bytes()
    files["manifest.json"] = (json.dumps({"version": VERSION, "addon": "ai_catalog",
        "files": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())}}, indent=2) + "\n").encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()): archive.writestr(name, data)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None or any(archive.read(name) != data for name, data in files.items()):
            raise OSError("addon archive readback failed")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, default=ROOT / "build/catalog/stage/Release")
    parser.add_argument("--out", type=Path, default=ROOT / f"dist/SDGO-AiCatalog-{VERSION}.zip")
    args = parser.parse_args()
    path = package(args.stage, args.out)
    print(path)
    print("SHA256 " + hashlib.sha256(path.read_bytes()).hexdigest())


if __name__ == "__main__": main()
