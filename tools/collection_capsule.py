"""Owner CLI for collection capsule server data and deployment.

Player packages never contain host INI paths, credentials, or player history.
"""
from __future__ import annotations

import argparse
import configparser
import contextlib
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import re
import tempfile
from datetime import datetime, timezone
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.collection_capsule.deployment import connection, import_history, register_unit_family, export_configuration
from owner.collection_capsule.metadata import imported_roots, registered_imports
from owner.collection_capsule.pool import build_pool, load_units
from owner.collection_capsule.sql import installation, disable, pool_hash

HOST_SHA256 = "82d3c4cf09679a431b9bfa639795b148f932642d8774ff66021785115dc53d46"
HOST_SYMBOLS = "WorldServer-548A86E3-0010C000.ini"
SERVER_FILES = {"winmm.dll", "sdgo/core.dll", "sdgo/sdgo.ini", f"sdgo/symbols/{HOST_SYMBOLS}",
                "sdgo/mods/collection_capsule/collection_capsule.dll", "sdgo/mods/collection_capsule/mod.ini",
                "sdgo/mods/collection_capsule/config.example.ini", "sdgo/mods/collection_capsule/config.ini"}


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def build(snapshot, output, machine_id, families, framework_stage=None, *, client_root=None, client_gdrs=None):
    from owner.collection_capsule.native_catalog import resolve_native_roots
    rows = load_units(snapshot)
    declarations = [read_json(path) for path in families]
    explicit = imported_roots(rows, declarations)
    existing, claimed = registered_imports(rows, snapshot, ignore_packages={m["package_id"] for m in declarations})
    for manifest in declarations:
        claimed.update(form["id"] for form in manifest["family"]["forms"])
    native = resolve_native_roots(rows, client_root=client_root, client_gdrs=client_gdrs)
    imported_root_ids = set(existing) | set(explicit)
    missing_native = [item for item in native["audit"].get("unresolved", [])
                      if not (set(item["forms"]) <= claimed
                              and len(set(item["forms"]) & imported_root_ids) == 1)]
    if missing_native:
        raise ValueError("Initial-form metadata is missing or inconsistent for "
                         + str(len(missing_native)) + " native/imported families")
    roots = {uid: proof for uid, proof in native["roots"].items() if uid not in claimed}
    roots.update(existing)
    roots.update(explicit)
    pool = build_pool(rows, verified_bases=roots)
    unresolved = [entry for entry in pool["excluded"] if entry["reason"] == "ambiguous_transform_cycle"]
    if unresolved:
        raise ValueError("Native/import form metadata is required for all ambiguous families; "
                         + str(len(unresolved)) + " form records remain unresolved")
    output.mkdir(parents=True, exist_ok=False)
    model = {"schema_version": 1, "feature": "collection-capsule", "machine_id": machine_id,
             "price_gp": 100000, "source_units": rows, "pool": pool,
             "native_root_revision": native["revision"], "native_root_audit": native["audit"]}
    write_json(output / "model.json", model)
    (output / "install.sql").write_text(installation(pool, rows, machine_id), encoding="utf-8")
    (output / "disable.sql").write_text(disable(machine_id), encoding="utf-8")
    if framework_stage:
        server = output / "server"
        required = ["winmm.dll", "sdgo/core.dll", "sdgo/sdgo.ini", f"sdgo/symbols/{HOST_SYMBOLS}",
                    "sdgo/mods/collection_capsule/collection_capsule.dll",
                    "sdgo/mods/collection_capsule/mod.ini", "sdgo/mods/collection_capsule/config.example.ini"]
        for relative in required:
            source = framework_stage / relative
            if not source.is_file():
                raise ValueError(f"Current framework server stage is missing {relative}")
            target = server / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        (server / "sdgo/mods/collection_capsule/config.ini").write_text(
            f"[settings]\nMachineID={machine_id}\n", encoding="ascii")
    files = {path.relative_to(output).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
             for path in output.rglob("*") if path.is_file()}
    write_json(output / "manifest.json", {"schema_version": 1, "feature": "collection-capsule",
               "machine_id": machine_id, "price_gp": 100000, "pool_revision": pool_hash(pool),
               "files": files})
    return {"machine_id": machine_id, "price_gp": 100000, "resolved_roots": len(pool["units"]),
            "pool_revision": pool_hash(pool), "output": str(output)}


def checked_model(package):
    manifest, model = read_json(package / "manifest.json"), read_json(package / "model.json")
    if (manifest.get("schema_version") != 1 or manifest.get("feature") != "collection-capsule"
            or model.get("schema_version") != 1 or model.get("feature") != "collection-capsule"
            or manifest.get("machine_id") != model.get("machine_id")
            or model.get("price_gp") != 100000 or manifest.get("price_gp") != 100000
            or manifest.get("pool_revision") != pool_hash(model["pool"])):
        raise ValueError("Collection server package metadata differs")
    for relative, expected in manifest.get("files", {}).items():
        path = (package / relative).resolve()
        if not path.is_relative_to(package.resolve()) or not path.is_file():
            raise ValueError("Server package contains an invalid file path")
        if hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError("Server package file hash differs")
    actual = {p.relative_to(package).as_posix() for p in package.rglob("*") if p.is_file()}
    if actual != set(manifest.get("files", {})) | {"manifest.json"}:
        raise ValueError("Server package contains undeclared or missing files")
    server_files = {name.removeprefix("server/") for name in actual if name.startswith("server/")}
    if server_files and server_files != SERVER_FILES:
        raise ValueError("Server package attempts to modify files outside this Mod/framework payload")
    expected_install = installation(model["pool"], model["source_units"], model["machine_id"])
    if (package / "install.sql").read_text(encoding="utf-8") != expected_install:
        raise ValueError("Server SQL differs from this feature's trusted generator")
    return model, expected_install


def merged_world_symbols(existing: bytes | None, incoming: bytes) -> bytes:
    """Preserve unrelated symbol blocks; append only this feature's two entries."""
    def text(raw):
        if raw.startswith(b"\xff\xfe"):
            return raw[2:].decode("utf-16-le"), "utf-16-le"
        return raw.decode("utf-8-sig"), "utf-8"
    source, _ = text(incoming)
    wanted = configparser.ConfigParser(interpolation=None)
    wanted.read_string(source)
    current, encoding = text(existing) if existing else ("", "utf-8")
    present = configparser.ConfigParser(interpolation=None)
    if current:
        present.read_string(current)
        for key in ("timestamp", "size_of_image"):
            if int(present["host"][key], 0) != int(wanted["host"][key], 0):
                raise ValueError("Existing World symbol host differs from the supported executable")
    additions = []
    names = ["collection.pick", "collection.connection_command"]
    if not current:
        names.insert(0, "host")
    for name in names:
        if name in present:
            def normalized(section):
                return {k: (" ".join(v.lower().split()) if k == "expect" else v.lower()) for k, v in section.items()}
            if normalized(present[name]) != normalized(wanted[name]):
                raise ValueError("A collection World symbol conflicts with an existing definition")
            continue
        match = re.search(r"(?ms)^\[" + re.escape(name) + r"\]\s*\n.*?(?=^\[|\Z)", source)
        if not match:
            raise ValueError("Package lacks a required collection symbol")
        additions.append(match.group(0).rstrip())
    suffix = ("\r\n\r\n" + "\r\n\r\n".join(additions) + "\r\n") if additions else ""
    return (existing or b"") + suffix.encode(encoding)


def _world_is_running(target: Path) -> bool:
    script = "@(Get-CimInstance Win32_Process -Filter \"Name='WorldServer.exe'\" | Where-Object { $_.ExecutablePath } | Select-Object -ExpandProperty ExecutablePath) | ConvertTo-Json -Compress"
    result = subprocess.run(["powershell", "-NoProfile", "-Command", script], capture_output=True, text=True, timeout=20)
    if result.returncode:
        raise RuntimeError("Cannot verify whether the selected World host is stopped")
    paths = json.loads(result.stdout) if result.stdout.strip() else []
    if isinstance(paths, str):
        paths = [paths]
    expected = str(target / "WorldServer.exe").casefold()
    return any(str(Path(value).resolve()).casefold() == expected for value in paths)


class ServerFiles:
    """Scoped reversible payload update; never copies the host's private INI."""
    def __init__(self, target, source):
        self.target, self.source = target.resolve(), source.resolve()
        self.backup = target / "sdgo-install-backups" / ("collection-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8])
        self.before = {}
        self.expected = {}
        self.after = {}
        self.committed = False

    def capture(self):
        relatives = {p.relative_to(self.source).as_posix() for p in self.source.rglob("*") if p.is_file()} | {"winmm_chain.dll"}
        for relative in sorted(relatives):
            path = self.target / relative
            current = path
            while current != self.target:
                if current.is_symlink() or (hasattr(current, "is_junction") and current.is_junction()):
                    raise ValueError("World payload must not traverse linked directories")
                current = current.parent
            if not path.resolve().is_relative_to(self.target):
                raise ValueError("World payload escapes selected target")
            if path.is_file():
                saved = self.backup / relative
                saved.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, saved)
                self.before[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
            else:
                self.before[relative] = None
            incoming = self.source / relative
            self.expected[relative] = hashlib.sha256(incoming.read_bytes()).hexdigest() if incoming.is_file() else None
        self.backup.mkdir(parents=True, exist_ok=True)
        write_json(self.backup / "before.json", self.before)

    def observe(self):
        self.after = {relative: hashlib.sha256((self.target / relative).read_bytes()).hexdigest()
                      if (self.target / relative).is_file() else None for relative in self.before}

    def restore(self):
        conflicts = []
        for relative, old in self.before.items():
            path = self.target / relative
            current = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
            allowed = {old, self.expected[relative], self.after.get(relative)}
            if relative == "winmm_chain.dll" and not old:
                allowed.add(self.before.get("winmm.dll"))
            if current not in allowed:
                conflicts.append(relative)
                continue
            if old:
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(self.backup / relative, path)
            elif path.is_file():
                path.unlink()
        if conflicts:
            raise RuntimeError("Concurrent World file edits were retained; restore from " + str(self.backup))


def install_server(package, server_ini, *, check_only=False):
    model, sql = checked_model(package)
    target = server_ini.resolve().parent
    executable = target / "WorldServer.exe"
    if not executable.is_file() or hashlib.sha256(executable.read_bytes()).hexdigest() != HOST_SHA256:
        raise ValueError("World executable is not the verified native build")
    if not (package / "server/winmm.dll").is_file():
        raise ValueError("Build the package with the current framework server stage first")
    if not check_only and _world_is_running(target):
        raise RuntimeError("Stop the selected World host before installing this gameplay Mod")
    settings = target / "sdgo/mods/collection_capsule/config.ini"
    if not settings.is_file():
        settings = target / "sdgo/mods/collection_capsule/mod.ini"
    if settings.is_file():
        config = configparser.ConfigParser(interpolation=None)
        raw = settings.read_bytes()
        config.read_string(raw.decode("utf-16") if raw.startswith(b"\xff\xfe") else raw.decode("utf-8-sig"))
        if config.getint("settings", "MachineID", fallback=990001) != model["machine_id"]:
            raise ValueError("Build with the existing owner MachineID; updates preserve owner configuration")
    symbol = target / "sdgo/symbols" / HOST_SYMBOLS
    merged = merged_world_symbols(symbol.read_bytes() if symbol.is_file() else None,
                                 (package / "server/sdgo/symbols" / HOST_SYMBOLS).read_bytes())
    with contextlib.closing(connection(server_ini)) as db:
        actual = db.cursor().execute("SELECT DB_NAME()").fetchone()[0]
        if check_only:
            return {"checked": True, "database": actual, "machine_id": model["machine_id"], "applied": False}
        with tempfile.TemporaryDirectory(prefix="sdgo-collection-server-") as temporary:
            payload = Path(temporary) / "server"
            shutil.copytree(package / "server", payload)
            (payload / "sdgo/symbols" / HOST_SYMBOLS).write_bytes(merged)
            files = ServerFiles(target, payload)
            files.capture()
            cursor = db.cursor()
            try:
                # SQL's inner transaction joins this outer transaction. Publish only
                # after the complete file update and state readback have succeeded.
                cursor.execute("SET XACT_ABORT ON;BEGIN TRANSACTION;")
                cursor.execute(sql)
                row = cursor.execute("SELECT MachineID,PriceGP,Enabled,PoolHash FROM sdgo_collection_capsule.State WHERE Singleton=1").fetchone()
                if not row or tuple(row) != (model["machine_id"], 100000, True, pool_hash(model["pool"])):
                    raise RuntimeError("Installed collection state readback differs")
                result = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                    str(ROOT / "tools/install_server.ps1"), "-ServerRoot", str(target), "-Role", "World",
                    "-Source", str(payload)], capture_output=True, text=True)
                if result.returncode:
                    raise RuntimeError("World file install failed; backup retained at " + str(files.backup))
                files.observe()
                cursor.execute("COMMIT TRANSACTION;")
                files.committed = True
            except Exception:
                with contextlib.suppress(Exception):
                    cursor.execute("IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;")
                files.restore()
                raise
            finally:
                cursor.close()
    return {"applied": True, "machine_id": model["machine_id"], "price_gp": 100000,
            "restart_world_required": True, "backup": str(files.backup)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("inspect")
    p.add_argument("--snapshot", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p = commands.add_parser("build-server")
    p.add_argument("--snapshot", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--machine-id", type=int, default=990001)
    p.add_argument("--unit-family", type=Path, action="append", default=[])
    p.add_argument("--framework-stage", type=Path)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument("--client", type=Path)
    source.add_argument("--client-gdrs", type=Path)
    p = commands.add_parser("install-server")
    p.add_argument("--package", type=Path, required=True)
    p.add_argument("--server-ini", type=Path, required=True)
    p.add_argument("--check-only", action="store_true")
    p = commands.add_parser("disable")
    p.add_argument("--server-ini", type=Path, required=True)
    p.add_argument("--machine-id", type=int, default=990001)
    p = commands.add_parser("import-history")
    p.add_argument("--server-ini", type=Path, required=True)
    p.add_argument("--history", type=Path, required=True)
    p = commands.add_parser("export")
    p.add_argument("--server-ini", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p = commands.add_parser("register-family")
    p.add_argument("--server-ini", type=Path, required=True)
    p.add_argument("--unit-family", type=Path, required=True)
    p = commands.add_parser("build-kit")
    p.add_argument("--snapshot", type=Path, required=True)
    p.add_argument("--client", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--machine-id", type=int, default=990001)
    p.add_argument("--unit-family", type=Path, action="append", default=[])
    p.add_argument("--current-registry", type=Path)
    p.add_argument("--cover", type=Path, default=ROOT / "mods/collection_capsule/assets/cover.png")
    p.add_argument("--framework-stage", type=Path)
    p.add_argument("--support-dll", type=Path)
    args = parser.parse_args()
    if args.command == "inspect":
        result = build_pool(load_units(args.snapshot))
        args.out.parent.mkdir(parents=True, exist_ok=True)
        write_json(args.out, result)
        print(json.dumps({"roots": len(result["units"]), "excluded": len(result["excluded"]), "output": str(args.out)}))
    elif args.command == "build-server":
        print(json.dumps(build(args.snapshot, args.out, args.machine_id, args.unit_family, args.framework_stage,
                               client_root=args.client, client_gdrs=args.client_gdrs)))
    elif args.command == "install-server":
        print(json.dumps(install_server(args.package, args.server_ini, check_only=args.check_only)))
    elif args.command == "disable":
        with contextlib.closing(connection(args.server_ini)) as db:
            db.cursor().execute(disable(args.machine_id))
        print(json.dumps({"disabled": True, "history_preserved": True, "restart_world_required": True}))
    elif args.command == "import-history":
        with contextlib.closing(connection(args.server_ini)) as db:
            print(json.dumps(import_history(db, read_json(args.history))))
    elif args.command == "export":
        with contextlib.closing(connection(args.server_ini)) as db:
            print(json.dumps(export_configuration(db, args.out)))
    elif args.command == "register-family":
        with contextlib.closing(connection(args.server_ini)) as db:
            print(json.dumps(register_unit_family(db, read_json(args.unit_family))))
    elif args.command == "build-kit":
        from owner.collection_capsule.client import build_client_registry
        # Plain module import uses this script's tools/ directory, including in
        # the portable owner kit; it never imports code from a game folder.
        sys.path.insert(0, str(ROOT / "tools"))
        from package_collection_capsule_client import package_client_registry
        stage = args.framework_stage
        if stage is None:
            stage = ROOT / "framework" if (ROOT / "framework").is_dir() else ROOT / "build/stage/Release"
        support = args.support_dll or stage / "support/capsule-registry/capsule_catalog.dll"
        current = args.current_registry
        if current is None and (args.client / "sdgo/mods/capsule_catalog/runtime.ini").is_file():
            current = args.client / "sdgo/mods/capsule_catalog"
        args.out.mkdir(parents=True, exist_ok=False)
        server = build(args.snapshot, args.out / "server-package", args.machine_id,
                       args.unit_family, stage / "server", client_root=args.client)
        model = read_json(args.out / "server-package/model.json")
        registry = build_client_registry(client_root=args.client, current_registry=current,
            source_units=model["pool"]["units"], machine_id=args.machine_id, cover=args.cover,
            output=args.out / "client-registry")
        player = package_client_registry(registry=args.out / "client-registry", framework_stage=stage / "client",
                                         support_dll=support, output=args.out / "player")
        receipt = {"schema_version": 1, "machine_id": args.machine_id, "price_gp": 100000,
                   "server": server, "registry": registry, "player": player,
                   "distribution": "Give friends only the player directory; server package stays with owner"}
        write_json(args.out / "build-receipt.json", receipt)
        print(json.dumps(receipt, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError, RuntimeError, KeyError) as error:
        raise SystemExit(str(error)) from None
