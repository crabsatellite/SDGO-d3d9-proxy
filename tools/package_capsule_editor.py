"""Package the authoring UI and runtime, without local environment state."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil

from capsule_catalog import digest, read_json, validate
from capsule_deployment import _build_package_symbols


TOOL_FILES = (
    "start_capsule_editor.py", "capsule_ui.py", "capsule_catalog.py",
    "capsule_assets.py", "capsule_portraits.py", "capsule_client_compile.py", "capsule_server_runtime.py",
    "capsule_ui_layout.py",
    "capsule_deployment.py", "init_capsules.py", "export_capsule_snapshot.py",
    "install_capsule_client.ps1",
)
HOST_SYMBOL = "GOnline-54BE1568-00C97000.ini"


def build(args):
    root = Path(__file__).resolve().parents[1]
    output = args.output.resolve()
    if output.exists():
        raise ValueError("Use a new output directory; existing packages are preserved")
    catalog = read_json(args.catalog)
    validate(catalog)
    capsule_lookup = args.capsule_lookup or args.lookup.with_name("capsule_spr_lookup.json")
    if not capsule_lookup.is_file():
        raise ValueError("The native capsule sprite lookup is required for authentic cards")
    # Package only explicitly selected public configuration and resource inputs.
    for path in (args.catalog, args.patterns, args.lookup, capsule_lookup):
        text = path.read_text(encoding="utf-8-sig")
        import re
        if re.search(r"[A-Za-z]:[\\/]", text):
            raise ValueError(f"Input contains a local absolute path: {path.name}")
    output.mkdir(parents=True)
    def copy(source, relative):
        source = Path(source)
        if source.is_symlink():
            raise ValueError("Linked input is not supported")
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    for name in TOOL_FILES:
        copy(root / "tools" / name, "tools/" + name)
    for source in (root / "ui" / "capsules").iterdir():
        if source.is_file() and source.suffix in {".html", ".js", ".css"}:
            copy(source, "ui/capsules/" + source.name)
    copy(args.catalog, "config/catalog.json")
    copy(args.patterns, "config/patterns.json")
    copy(args.lookup, "data/unit_spr_lookup.json")
    copy(capsule_lookup, "data/capsule_spr_lookup.json")
    # Only PNG previews, never logs or source-side metadata.
    for source in (args.assets / "units").glob("*.png"):
        if not source.stem.isdecimal():
            raise ValueError("Unexpected unit preview filename")
        copy(source, "assets/units/" + source.name)
    for relative in ("d3d9.dll", "sdgo/core.dll", "sdgo/sdgo.ini",
                     "sdgo/mods/capsule_catalog/capsule_catalog.dll",
                     "sdgo/mods/capsule_catalog/mod.ini"):
        copy(args.framework_stage / relative, "framework-client/" + relative)
    fragment = root / "mods" / "capsule_catalog" / "symbol-proposal.ini"
    copy(fragment, "mods/capsule_catalog/symbol-proposal.ini")
    symbols = _build_package_symbols(
        args.framework_stage / "sdgo/symbols" / HOST_SYMBOL, fragment)
    target_symbol = output / "framework-client/sdgo/symbols" / HOST_SYMBOL
    target_symbol.parent.mkdir(parents=True, exist_ok=True)
    target_symbol.write_bytes(symbols)
    copy(root / "docs" / "capsule-authoring.zh-CN.md", "docs/capsule-authoring.zh-CN.md")
    copy(root / "docs" / "capsule-display-fix.zh-CN.md", "docs/capsule-display-fix.zh-CN.md")
    settings = {
        "schema_version": 1, "catalog": "config/catalog.json",
        "patterns": "config/patterns.json", "assets": "assets",
        "bundle_output": "output", "snapshot": "", "client": "", "lookup": "",
        "install_client": "", "server_ini": "", "framework_stage": "framework-client",
        "symbol_fragment": "mods/capsule_catalog/symbol-proposal.ini",
        "port": 0, "open_browser": True,
    }
    (output / "settings.json").write_text(
        json.dumps(settings, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    (output / "Start-Editor.cmd").write_bytes(
        b'@echo off\r\ncd /d "%~dp0"\r\npython -B -X utf8 "%~dp0tools\\start_capsule_editor.py" %*\r\nif errorlevel 1 pause\r\n')
    (output / "requirements.txt").write_text("Pillow>=10\npyodbc>=5\n", encoding="ascii")
    (output / "README.zh-CN.txt").write_text(
        "扭蛋机编辑器与运行时 Mod\n\n"
        "服主安装 Python 3.10+，依赖见 requirements.txt。玩家安装器不需要 Python。\n"
        "双击 Start-Editor.cmd 可先浏览与编辑配置；服务仅绑定本机127.0.0.1。\n"
        "在 settings.json 填写自己的 client 游戏目录；lookup 填 data/unit_spr_lookup.json。\n"
        "需要实际导入时，install_client 填目标游戏目录，server_ini 填自己的服务端INI路径。\n"
        "口令保留在服务端INI，不要填写在本文件或发送给玩家。可先执行 --check 检查设置。\n"
        "编辑完成后点击导入到游戏；按提示重启自己的服务器。将返回路径中的整个 player 目录分发给玩家。\n"
        "player/Install-Client.cmd 选择游戏目录后安装；保留其他Mod和原配置。\n"
        "要重新分析自己的服务器，先用 export_capsule_snapshot.py 导出快照并设置 snapshot。\n"
        "当前 catalog 是本次初始化示例；不同服务器应从自己的快照生成配置。\n"
        "详细说明见 docs/capsule-authoring.zh-CN.md。真实账号抽奖需服主人工验收。\n",
        encoding="utf-8-sig")
    files = {path.relative_to(output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
             for path in sorted(output.rglob("*")) if path.is_file()}
    manifest = {"schema_version": 1, "package_id": "capsule-editor",
                "catalog_revision": digest(catalog), "files": files,
                "scope": "authoring tools and runtime; no credentials or private environment"}
    (output / "package-manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(output), "files": len(files),
                      "catalog_revision": manifest["catalog_revision"]}, ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("catalog", "patterns", "assets", "lookup", "framework-stage", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--capsule-lookup", type=Path)
    build(parser.parse_args())


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as error:
        raise SystemExit(str(error)) from None
