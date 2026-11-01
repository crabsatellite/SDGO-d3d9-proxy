"""Build two separate, whitelisted shop packages; never includes game/private data."""
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import shutil
import struct
import zipfile

ROOT = Path(__file__).resolve().parents[1]
SECTIONS = ["shop.buy_options.populate", "shop.gift_options.populate", "shop.tooltip.option",
            "text.ansi_to_wide", "shop_labels.grid_data", "shop_labels.grid_find"]


def pe32(path):
    b = path.read_bytes()
    start = struct.unpack_from("<I", b, 0x3C)[0]
    if b[:2] != b"MZ" or b[start:start+4] != b"PE\0\0" or struct.unpack_from("<H", b, start+4)[0] != 0x14C:
        raise ValueError("Expected a native x86 artifact: " + path.name)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build", type=Path, default=ROOT / "build")
    p.add_argument("--output", type=Path, default=ROOT / "dist")
    p.add_argument("--staging-root", type=Path, help="New staging parent (default: build/shop-packages)")
    p.add_argument("--version", default="0.1.0")
    args = p.parse_args()
    if not all(c.isalnum() or c in ".-" for c in args.version): raise ValueError("Invalid version")
    stage = args.build / "stage/Release"
    symbol = ROOT / "symbols/GOnline-54BE1568-00C97000.ini"
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(symbol, encoding="utf-8-sig")
    args.output.mkdir(parents=True, exist_ok=True)
    receipts = []
    for role, module, name, side, loader in [("Labels", "shop_labels", "SDGO-ShopLabels-Patch", "client", "d3d9.dll"),
                                            ("Catalog", "shop_catalog", "SDGO-ShopCatalog-Mod", "server", "winmm.dll")]:
        target = (args.staging_root or args.build / "shop-packages") / (name + "-" + args.version)
        target.mkdir(parents=True, exist_ok=False)
        def copy(source, relative):
            dest = target / relative
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, dest)
        for relative in [loader, "sdgo/core.dll", "sdgo/sdgo.ini"]:
            copy(stage/side/relative, Path("framework")/side/relative)
        base_parser = configparser.ConfigParser(interpolation=None)
        base_parser.read(symbol, encoding="utf-8-sig")
        for section in SECTIONS: base_parser.remove_section(section)
        if role == "Labels":
            dest = target / "framework/client/sdgo/symbols/GOnline-54BE1568-00C97000.ini"
            dest.parent.mkdir(parents=True, exist_ok=True)
            with dest.open("w", encoding="ascii") as f: base_parser.write(f)
            fragment = configparser.ConfigParser(interpolation=None)
            for section in ["host"] + SECTIONS: fragment[section] = dict(parser[section])
            with (target/"symbols.ini").open("w", encoding="ascii") as f: fragment.write(f)
        for file in [module+".dll", "mod.ini", "display.example.ini" if role == "Labels" else "catalog.example.ini"]:
            source = stage/side/"sdgo/mods"/module/file
            if source.suffix == ".dll": pe32(source)
            copy(source, Path("payload")/module/file)
        if role == "Catalog":
            pe32(args.build/"Release/shop_catalog_apply.exe")
            copy(args.build/"Release/shop_catalog_apply.exe", "payload/shop_catalog/shop_catalog_apply.exe")
            copy(ROOT/"tools/shop_catalog.py", "host-tools/shop_catalog.py")
            gui=args.build/"shop-ui/SdgoShopManager.exe"
            if not gui.is_file(): raise ValueError("Build the owner UI first: powershell -File tools/build_shop_ui.ps1")
            copy(gui, "SdgoShopManager.exe")
        copy(ROOT/"tools/install_shop.ps1", "Install.ps1")
        copy(ROOT/("docs/shop-labels.zh-CN.md" if role == "Labels" else "docs/shop-catalog.zh-CN.md"), "README.zh-CN.md")
        copy(ROOT/"docs/shop-validation.zh-CN.md", "shop-validation.zh-CN.md")
        copy(ROOT/"docs/shop-testing.zh-CN.md", "shop-testing.zh-CN.md")
        copy(ROOT/"LICENSE", "LICENSE")
        copy(ROOT/"THIRD_PARTY_NOTICES.md", "THIRD_PARTY_NOTICES.md")
        copy(ROOT/"third_party/minhook/LICENSE.txt", "licenses/MinHook-LICENSE.txt")
        files = {p.relative_to(target).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in sorted(target.rglob("*")) if p.is_file()}
        manifest = {"schema_version": 1, "role": role, "version": args.version,
                    "module": module, "framework_api": 1, "files": files}
        (target/"package.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
        artifact = args.output / f"{name}-{args.version}.zip"
        if artifact.exists(): raise ValueError("Package already exists; choose an explicit new output/version")
        with zipfile.ZipFile(artifact, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as z:
            for file in sorted(target.rglob("*")):
                if file.is_file(): z.write(file, file.relative_to(target).as_posix())
        with zipfile.ZipFile(artifact) as z:
            if z.testzip() is not None: raise ValueError("ZIP CRC validation failed")
            if any("private" in n.lower() or "test-server" in n.lower() or "GPackData" in n or n.endswith("GOnline.exe") for n in z.namelist()):
                raise ValueError("Unexpected private/game file in package")
        receipt = {"path": str(artifact.resolve()), "bytes": artifact.stat().st_size,
                   "sha256": hashlib.sha256(artifact.read_bytes()).hexdigest(), "files": len(files)+1}
        receipts.append(receipt)
        print(json.dumps(receipt))
    (args.output/"shop-package-receipt.json").write_text(json.dumps(receipts, indent=2), encoding="utf-8")


if __name__ == "__main__": main()
