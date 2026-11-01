"""Exercise real shop ZIPs/installers with inert PE fixtures; never runs a game."""
import argparse
import configparser
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def ini(path):
    b = path.read_bytes()
    p = configparser.ConfigParser(interpolation=None)
    p.read_string(b.decode("utf-16" if b.startswith(b"\xff\xfe") else "utf-8-sig"))
    return p


def digest_tree(path):
    return {p.relative_to(path).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in path.rglob("*") if p.is_file()}


def fixture(path, role, symbols=None):
    # One raw section per verified symbol. These bytes describe an inert file,
    # never an executable process or substitute for native symbol verification.
    entries = [(int(symbols[s]["rva"], 0), bytes.fromhex(symbols[s]["expect"]))
               for s in symbols.sections() if s != "host"] if symbols else []
    b = bytearray(0x400 + max(1, len(entries)) * 0x100)
    b[:2] = b"MZ"
    struct.pack_into("<I", b, 0x3c, 0x80)
    b[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHI", b, 0x84, 0x14c, len(entries), 0x54BE1568 if role == "Labels" else 0x548A86E3)
    struct.pack_into("<H", b, 0x94, 0xE0)
    struct.pack_into("<I", b, 0xD0, 0xC97000 if role == "Labels" else 0x10C000)
    for i, (rva, code) in enumerate(entries):
        at = 0x178 + i * 40
        struct.pack_into("<IIII", b, at + 8, 0x100, rva, 0x100, 0x400 + i * 0x100)
        b[0x400 + i * 0x100:0x400 + i * 0x100 + len(code)] = code
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", type=Path, default=ROOT / "build")
    p.add_argument("--powershell", default="powershell")
    args = p.parse_args()
    version = "test-" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%d%H%M%S") + "-" + str(os.getpid())
    work = args.build.resolve() / "shop-install-tests" / version
    work.mkdir(parents=True, exist_ok=False)
    built = subprocess.run([sys.executable, "-B", str(ROOT / "tools/package_shop.py"), "--build", str(args.build.resolve()),
                            "--output", str(work / "packages"), "--version", version], capture_output=True, text=True)
    if built.returncode: raise AssertionError(built.stdout + built.stderr)
    packages = {}
    for zpath in (work / "packages").glob("*.zip"):
        with zipfile.ZipFile(zpath) as z:
            assert z.testzip() is None
            manifest = json.loads(z.read("package.json"))
            for name, digest in manifest["files"].items(): assert hashlib.sha256(z.read(name)).hexdigest() == digest
            assert set(z.namelist()) == set(manifest["files"]) | {"package.json"}
            dest = work / manifest["role"]
            z.extractall(dest)
            packages[manifest["role"]] = dest
    symbols = ini(packages["Labels"] / "symbols.ini")
    checks = []

    def check(ok, name):
        if not ok: raise AssertionError(name)
        checks.append(name)
        print("PASS", name, flush=True)

    def run(role, target, *extra, expected=True, busy=False, package=None):
        package = package or packages[role]
        command = [args.powershell, "-NoProfile", "-ExecutionPolicy", "Bypass"]
        env = os.environ.copy()
        if busy:
            wrapper = work / "busy-fixture.ps1"
            wrapper.write_text("function Get-CimInstance { [pscustomobject]@{ ExecutablePath=$env:SHOP_TEST_BUSY_EXE } }\n& $env:SHOP_TEST_INSTALL -Target $env:SHOP_TEST_TARGET @args\n", encoding="utf-8")
            env.update(SHOP_TEST_BUSY_EXE=str(target / ("GOnline.exe" if role == "Labels" else "WorldServer.exe")), SHOP_TEST_TARGET=str(target),
                       SHOP_TEST_INSTALL=str(package / "Install.ps1"))
            command += ["-File", str(wrapper), *extra]
        else:
            command += ["-File", str(package / "Install.ps1"), "-Target", str(target), *extra]
        result = subprocess.run(command, env=env, capture_output=True, timeout=40)
        if (result.returncode == 0) != expected:
            raise AssertionError(result.stdout.decode(errors="replace") + result.stderr.decode(errors="replace"))
        return result

    for role, module, executable, loader, side in [("Labels", "shop_labels", "GOnline.exe", "d3d9.dll", "client"),
                                                 ("Catalog", "shop_catalog", "IndependentWorld.exe", "winmm.dll", "server")]:
        target = work / (role + " 中文 空格")
        fixture(target / executable, role, symbols if role == "Labels" else None)
        extra = ["-Executable", executable] if role == "Catalog" else []
        run(role, target, *extra)
        mod = target / "sdgo/mods" / module
        check((target / loader).is_file() and (target / "sdgo/core.dll").is_file() and (mod / (module + ".dll")).is_file(), role + "_fresh_bootstrap")
        if role == "Catalog": check(ini(mod / "mod.ini")["mod"]["enabled"] == "0", "catalog_disabled_without_host_profile")
        # Existing shared bytes and unrelated mod/symbol content must survive an update.
        (target / "sdgo/mods/other").mkdir()
        (target / "sdgo/mods/other/keep.txt").write_bytes(b"unrelated\0mod\xff")
        global_ini = target / "sdgo/sdgo.ini"
        global_ini.write_bytes(b"; keep comments\r\n[core]\r\nlog_level=0\r\n")
        original_symbols = b"; unrelated symbol comments\r\n[host]\r\ntimestamp=0x54BE1568\r\nsize_of_image=0x00C97000\r\n[other.symbol]\r\nkind=data\r\nrva=0x1234\r\n"
        symbol_path = target / "sdgo/symbols/GOnline-54BE1568-00C97000.ini"
        if role == "Labels": symbol_path.write_bytes(original_symbols)
        settings = (mod / "mod.ini").read_text(encoding="utf-16")
        (mod / "mod.ini").write_text(settings + "\nUserSetting=42\n", encoding="utf-16")
        preserved = {f: f.read_bytes() for f in [target / loader, target / "sdgo/core.dll", global_ini, target / "sdgo/mods/other/keep.txt"]}
        profile = work / (role + "-profile.ini")
        profile.write_text("[shop]\nRevision=test\n" if role == "Labels" else "[shop]\nSchema=1\nCatalogId=install-test\nRevision=test\n", encoding="utf-16")
        run(role, target, *extra, "-ConfigFile", str(profile))
        check(all(f.read_bytes() == data for f, data in preserved.items()), role + "_update_preserves_shared_and_other_mods")
        check(ini(mod / "mod.ini")["settings"]["UserSetting"] == "42", role + "_update_preserves_host_settings")
        check((mod / ("display.ini" if role == "Labels" else "catalog.ini")).read_text(encoding="utf-16") == profile.read_text(encoding="utf-16"), role + "_Unicode_profile_import")
        if role == "Catalog":
            check(ini(mod / "mod.ini")["mod"]["enabled"] == "1", "catalog_enabled_after_explicit_profile")
            # No database is configured here; preserve the mod if retirement fails.
            before = digest_tree(target)
            run(role, target, *extra, "-Uninstall", expected=False)
            check(digest_tree(target) == before, "catalog_retirement_failure_preserves_install")
            # This empty catalog was never applied. Remove only our fixture profile
            # so the no-profile uninstall path can be tested without SQL access.
            (mod / "catalog.ini").unlink()
        run(role, target, *extra, "-Uninstall")
        check(not mod.exists() and all(f.read_bytes() == data for f, data in preserved.items()), role + "_uninstall_preserves_shared_bytes")
        if role == "Labels": check(symbol_path.read_bytes() == original_symbols, "uninstall_removes_only_owned_symbol_bytes")
        check(any((target / "sdgo/shop-backups").iterdir()), role + "_uninstall_retains_own_configuration")

    for case in ["signature", "hash", "busy", "core", "chain", "symbol-conflict", "host-mismatch"]:
        target = work / ("reject-" + case)
        fixture(target / "GOnline.exe", "Labels", symbols)
        package = packages["Labels"]
        if case == "signature":
            b = bytearray((target / "GOnline.exe").read_bytes()); b[0x400] ^= 1
            (target / "GOnline.exe").write_bytes(b)
        if case == "hash":
            package = work / "bad-hash-package"
            shutil.copytree(packages["Labels"], package)
            (package / "payload/shop_labels/shop_labels.dll").write_bytes(b"bad hash")
        if case == "core":
            (target / "sdgo").mkdir(); (target / "sdgo/core.dll").write_bytes(b"foreign core")
        if case == "chain":
            (target / "d3d9.dll").write_bytes(b"third-party wrapper")
            (target / "d3d9_chain.dll").write_bytes(b"occupied")
        if case in ["symbol-conflict", "host-mismatch"]:
            at = target / "sdgo/symbols/GOnline-54BE1568-00C97000.ini"
            at.parent.mkdir(parents=True)
            header = "[host]\ntimestamp=0x54BE1568\nsize_of_image=0x00C97000\n"
            at.write_text(header + "[shop.tooltip.option]\nrva=0x123\n" if case == "symbol-conflict" else header.replace("0x54BE1568", "0x12345678"), encoding="ascii")
        before = digest_tree(target)
        run("Labels", target, expected=False, busy=case == "busy", package=package)
        check(digest_tree(target) == before, case + "_refused_before_installation_changes")

    target = work / "chain-success"
    fixture(target / "GOnline.exe", "Labels", symbols)
    wrapper = b"third-party-wrapper\x00"
    (target / "d3d9.dll").write_bytes(wrapper)
    run("Labels", target)
    check((target / "d3d9_chain.dll").read_bytes() == wrapper and ini(target / "sdgo/sdgo.ini")["loader"]["chain_d3d9"] == "d3d9_chain.dll", "third_party_wrapper_is_chained")
    target = work / "deferred-host"
    fixture(target / "WorldServer.exe", "Catalog")
    before = digest_tree(target)
    run("Catalog", target, busy=True, expected=False)
    check(digest_tree(target) == before, "running_host_requires_explicit_deferred_first_install")
    run("Catalog", target, "-DeferActivation", busy=True)
    mod = target / "sdgo/mods/shop_catalog"
    check(json.loads((mod / "shop-install.json").read_text(encoding="utf-8-sig"))["startup_activation"] == "deferred", "first_host_install_can_defer_startup_without_stopping_process")
    before = digest_tree(target)
    run("Catalog", target, "-DeferActivation", busy=True, expected=False)
    check(digest_tree(target) == before, "running_host_catalog_update_is_still_refused")
    (work / "result.json").write_text(json.dumps({"result": "PASS", "checks": checks, "scope": "real packages and installers; inert PE files only"}, indent=2), encoding="utf-8")
    print("SHOP INSTALLER CHECKS PASSED", work, flush=True)


if __name__ == "__main__": main()
