"""Read the bundled mod manifests; classify, validate and select complete features.

Publisher/build tool only. The core and player installers do not need Python.
"""
from __future__ import annotations

import argparse
import configparser
import json
from pathlib import Path, PurePosixPath
import re
import sys

CATEGORIES = {
    "fixes": {"en": "Core fixes", "zh-CN": "基础修复"},
    "gameplay": {"en": "Gameplay extensions", "zh-CN": "玩法扩展"},
    "network": {"en": "Networking and hosting", "zh-CN": "联机与服主工具"},
    "development": {"en": "Developer tools", "zh-CN": "开发诊断"},
}
CHANNELS = {"stable", "beta", "experimental"}
ACTIVATIONS = {"automatic", "room-title", "manual"}
ID = re.compile(r"[a-z][a-z0-9_-]*\Z")
CONFIG_EXTENSIONS = {".ini", ".json", ".toml", ".yaml", ".yml", ".cfg", ".txt"}


def safe_file(name: str) -> str:
    name = name.replace("\\", "/")
    parts = name.split("/")
    if (not name or name.startswith("/") or any(p in {"", ".", ".."} for p in parts)
            or any(c in name for c in ':*?"<>|') or any(ord(c) < 32 for c in name)
            or any(p.endswith((" ", ".")) for p in parts)):
        raise ValueError(f"Unsafe mod-relative file: {name!r}")
    return str(PurePosixPath(name))


def list_value(text: str) -> list[str]:
    return [item.strip() for item in text.split(",") if item.strip()]


def read_catalog(root: Path, *, check_build: bool = True) -> dict:
    root = root.resolve()
    mods = []
    for path in sorted((root / "mods").glob("*/mod.ini")):
        parser = configparser.ConfigParser(interpolation=None)
        parser.read_string(path.read_text(encoding="utf-8-sig"))
        cfg = parser["mod"]
        mod_id = cfg["id"]
        feature = cfg["feature"]
        category = cfg["category"]
        channel = cfg["channel"]
        activation = cfg["activation"]
        toolkit = cfg.get("toolkit", "")
        if not ID.fullmatch(mod_id) or mod_id != path.parent.name or not ID.fullmatch(feature):
            raise ValueError(f"Invalid mod/feature id in {path}")
        if category not in CATEGORIES or channel not in CHANNELS or activation not in ACTIVATIONS:
            raise ValueError(f"Invalid category/channel/activation in {path}")
        if toolkit not in {"", "frp"}:
            raise ValueError(f"Unknown package toolkit in {path}")
        dll = cfg.get("dll", f"{mod_id}.dll")
        if dll != f"{mod_id}.dll":
            raise ValueError(f"Bundled mod DLL must be {mod_id}.dll")
        targets = set()
        for host in list_value(cfg.get("hosts", "")):
            stem = host.split("-", 1)[0]
            if stem not in {"GOnline", "BattleServer", "AgentServer"}:
                raise ValueError(f"Unknown install target in {path}: {host}")
            targets.add({"GOnline": "client", "BattleServer": "server", "AgentServer": "server"}[stem])
        if len(targets) != 1:
            raise ValueError(f"A bundled mod needs exactly one install target: {path}")
        files = [safe_file(name) for name in list_value(cfg.get("files", ""))]
        configs = [safe_file(name) for name in list_value(cfg.get("config_files", ""))]
        if len(set(files)) != len(files) or any(Path(name).name.lower() in {"mod.ini", dll.lower()} for name in files):
            raise ValueError(f"Duplicate/reserved payload files in {path}")
        for name in files:
            payload = (path.parent / name).resolve()
            if not payload.is_relative_to(path.parent.resolve()) or not payload.is_file():
                raise ValueError(f"Missing or external mod payload: {name}")
        for name in configs:
            if Path(name).suffix.lower() not in CONFIG_EXTENSIONS or Path(name).name.lower() == "mod.ini":
                raise ValueError(f"Not a user config file: {name}")
        enabled = cfg.getint("enabled", fallback=1)
        if enabled not in {0, 1}:
            raise ValueError(f"Invalid enabled default in {path}")
        if category == "development" and enabled:
            raise ValueError(f"Developer diagnostics must start disabled: {mod_id}")
        guide = root / "docs" / f"{feature}.md"
        if not guide.is_file():
            raise ValueError(f"Missing feature guide: {guide.name}")
        mods.append({"id": mod_id, "feature": feature, "category": category,
                     "channel": channel, "activation": activation,
                     "toolkit": toolkit,
                     "hosts": list_value(cfg.get("hosts", "")), "version": cfg.get("version", "0"),
                     "target": next(iter(targets)), "enabled_by_default": bool(enabled),
                     "files": files, "config_files": configs,
                     "guide": f"docs/{feature}.md"})
    if not mods:
        raise ValueError("No bundled mod manifests found")
    category_order = {key: i for i, key in enumerate(CATEGORIES)}
    mods.sort(key=lambda item: (category_order[item["category"]], item["feature"], item["id"]))
    if check_build:
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        built = set(re.findall(r"\bsdgo_add_mod\(([a-z][a-z0-9_]*)\b", cmake))
        declared = {mod["id"] for mod in mods}
        if built != declared:
            raise ValueError(f"Build/catalog mismatch: built-only={sorted(built-declared)}, manifest-only={sorted(declared-built)}")
    features = {}
    for mod in mods:
        old = features.setdefault(mod["feature"], {"category": mod["category"], "channel": mod["channel"],
                                                   "activation": mod["activation"], "toolkit": mod["toolkit"], "mods": [], "guide": mod["guide"]})
        if any(old[key] != mod[key] for key in ("category", "channel", "activation", "toolkit")):
            raise ValueError(f"Feature metadata disagrees: {mod['feature']}")
        old["mods"].append(mod["id"])
    catalog = {"schema_version": 1, "categories": CATEGORIES, "features": features, "mods": mods}
    validate_readmes(root, catalog)
    return catalog


def validate_readmes(root: Path, catalog: dict) -> None:
    for filename in ("README.md", "README.zh-CN.md"):
        text = (root / filename).read_text(encoding="utf-8")
        sections = {}
        for category in CATEGORIES:
            match = re.search(rf"<!-- mod-category:{category} -->(.*?)<!-- /mod-category:{category} -->", text, re.S)
            if not match:
                raise ValueError(f"Missing README category {category}: {filename}")
            sections[category] = match.group(1)
        for feature, info in catalog["features"].items():
            guides = {f"docs/{feature}.md", f"docs/{feature}.zh-CN.md"}
            rows = [line for line in sections[info["category"]].splitlines()
                    if any(f"]({guide})" in line for guide in guides)]
            if len(rows) != 1 or any(f"`{mod}`" not in rows[0] for mod in info["mods"]):
                raise ValueError(f"README feature/category/mods mismatch: {feature} in {filename}")
            if info["channel"] != "stable" and info["channel"] not in rows[0].lower():
                raise ValueError(f"Missing README release channel for {feature}: {filename}")


def select_catalog(catalog: dict, profile: str = "full", features: list[str] | None = None) -> dict:
    if profile not in {"full", "base"}:
        raise ValueError(f"Unknown profile: {profile}")
    requested = set(features or [])
    unknown = requested - catalog["features"].keys()
    if unknown:
        raise ValueError(f"Unknown features: {', '.join(sorted(unknown))}")
    mods = [mod for mod in catalog["mods"] if (
        (profile == "full" and not requested) or mod["category"] == "fixes" or mod["feature"] in requested)]
    selected_features = {mod["feature"] for mod in mods}
    return {**catalog, "profile": "custom" if requested else profile,
            "features": {key: value for key, value in catalog["features"].items() if key in selected_features}, "mods": mods}


def validate_stage(root: Path, stage: Path, catalog: dict) -> None:
    """Check publisher payloads, including their new manifest/config contract."""
    for target, loader in (("client", "d3d9.dll"), ("server", "winmm.dll")):
        for name in (loader, "sdgo/core.dll", "sdgo/sdgo.ini"):
            if not (stage / target / name).is_file():
                raise ValueError(f"Missing staged framework file: {target}/{name}")
    for mod in catalog["mods"]:
        folder = stage / mod["target"] / "sdgo/mods" / mod["id"]
        for name in (f"{mod['id']}.dll", "mod.ini", *mod["files"]):
            path = folder / name
            if not path.is_file():
                raise ValueError(f"Missing staged mod payload: {mod['id']}/{name}")
            if not name.endswith(".dll") and path.read_bytes() != (root / "mods" / mod["id"] / name).read_bytes():
                raise ValueError(f"Stale staged mod data: {mod['id']}/{name}")


def write_catalog(catalog: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(catalog, ensure_ascii=False, indent=2) + "\n"
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--profile", choices=("full", "base"), default="full")
    parser.add_argument("--features", default="", help="Comma-separated optional feature ids; base fixes are always included")
    parser.add_argument("--stage", type=Path)
    parser.add_argument("--output", type=Path, action="append", default=[])
    args = parser.parse_args()
    try:
        catalog = select_catalog(read_catalog(args.root), args.profile, list_value(args.features))
        if args.stage:
            validate_stage(args.root, args.stage, catalog)
        for path in args.output:
            write_catalog(catalog, path)
        if not args.output:
            # Windows PowerShell 5.1 decodes native stdout using the console
            # code page. ASCII JSON survives that boundary without losing names.
            print(json.dumps(catalog, ensure_ascii=True, indent=2))
        return 0
    except (ValueError, KeyError, configparser.Error, OSError) as error:
        print(f"mod catalog: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
