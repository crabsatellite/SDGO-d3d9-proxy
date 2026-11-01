"""Start the capsule editor from operator-owned, portable path settings."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import urllib.request
import webbrowser


PATH_OPTIONS = {
    "catalog": "file", "patterns": "file", "assets": "dir",
    "bundle_output": "output", "snapshot": "dir", "client": "dir",
    "lookup": "file", "install_client": "dir", "server_ini": "file",
    "framework_stage": "dir", "symbol_fragment": "file",
    "report_output": "output",
}


def command_from_settings(settings_path: Path):
    settings_path = settings_path.resolve(strict=True)
    settings = json.loads(settings_path.read_text(encoding="utf-8-sig"))
    allowed = {"schema_version", "port", "open_browser", *PATH_OPTIONS}
    if not isinstance(settings, dict) or set(settings) - allowed:
        raise ValueError("Settings contain an unsupported field")
    if settings.get("schema_version") != 1:
        raise ValueError("settings.json requires schema_version 1")
    port = settings.get("port", 0)
    if type(port) is not int or not 0 <= port <= 65535:
        raise ValueError("port must be an integer between 0 and 65535")
    if type(settings.get("open_browser", True)) is not bool:
        raise ValueError("open_browser must be true or false")
    paths = {}
    for key, kind in PATH_OPTIONS.items():
        value = settings.get(key, "")
        if not isinstance(value, str):
            raise ValueError(f"{key} must be a path string")
        if not value.strip():
            continue
        path = Path(value)
        if not path.is_absolute():
            path = settings_path.parent / path
        path = path.resolve()
        if kind == "file" and not path.is_file():
            raise ValueError(f"{key} file does not exist: {path}")
        if kind == "dir" and not path.is_dir():
            raise ValueError(f"{key} directory does not exist: {path}")
        paths[key] = path
    if "catalog" not in paths:
        raise ValueError("Set catalog in settings.json")
    if "client" in paths and not (paths["client"] / "GOnline.exe").is_file():
        raise ValueError("client must contain GOnline.exe")
    if bool(paths.get("client")) != bool(paths.get("lookup")):
        raise ValueError("Set both client and lookup to enable game compilation")
    if "install_client" in paths:
        if "client" not in paths or "framework_stage" not in paths:
            raise ValueError("Installation requires client, lookup and framework_stage")
        if not (paths["install_client"] / "GOnline.exe").is_file():
            raise ValueError("install_client must contain GOnline.exe")
    elif "server_ini" in paths:
        raise ValueError("Set install_client before configuring server_ini")
    script = Path(__file__).with_name("capsule_ui.py")
    command = [sys.executable, "-B", "-X", "utf8", str(script), "serve"]
    # A framework path alone is useful in a portable example; the UI enables
    # installation only when the operator explicitly sets install_client.
    for key, path in paths.items():
        if key == "framework_stage" and "install_client" not in paths:
            continue
        command.extend(["--" + key.replace("_", "-"), str(path)])
    command.extend(["--port", str(port)])
    if settings.get("open_browser", True):
        command.append("--open")
    return command, {
        "settings": str(settings_path), "valid": True,
        "bind": "127.0.0.1", "port": port,
        "compile": "client" in paths,
        "apply": "install_client" in paths,
        "server_apply": "server_ini" in paths,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--settings", type=Path,
                        default=Path(__file__).resolve().parents[1] / "settings.json")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    command, receipt = command_from_settings(args.settings)
    print(json.dumps(receipt, ensure_ascii=False), flush=True)
    if not args.check:
        if receipt["port"]:
            url = f"http://127.0.0.1:{receipt['port']}/"
            try:
                with urllib.request.urlopen(url + "api/bootstrap", timeout=2) as response:
                    bootstrap = json.load(response)
                catalog = command[command.index("--catalog") + 1]
                if Path(bootstrap.get("catalog_path", "")).resolve() == Path(catalog):
                    print(json.dumps({"already_running": True, "url": url}), flush=True)
                    if "--open" in command:
                        webbrowser.open(url)
                    return
            except (OSError, ValueError, KeyError, json.JSONDecodeError):
                pass
        raise SystemExit(subprocess.call(command))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, json.JSONDecodeError) as error:
        raise SystemExit(str(error)) from None
