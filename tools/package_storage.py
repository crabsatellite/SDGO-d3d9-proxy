"""Build the allowlisted storage claim-all overlay for SDGO Mod Manager."""
import argparse
import configparser
import hashlib
import io
from pathlib import Path
import zipfile


def ini(path):
    result = configparser.ConfigParser(interpolation=None)
    result.read(path, encoding="utf-8-sig")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/storage"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    stage = args.build_dir.resolve() / "stage/Release/client"
    relative = Path("sdgo/mods/storage_claim")
    files = {"client/" + (relative / name).as_posix(): stage / relative / name
             for name in ("storage_claim.dll", "mod.ini", "manager.json")}
    for name, path in files.items():
        if not path.is_file():
            parser.error(f"Missing staged payload: {name}")
        if path.suffix != ".dll" and path.read_bytes() != (repo / "mods/storage_claim" / path.name).read_bytes():
            parser.error(f"Stale staged manifest: {name}; rebuild storage_claim")
    manifest = ini(stage / relative / "mod.ini")
    symbols = ini(stage / "sdgo/symbols/GOnline-54BE1568-00C97000.ini")
    source = ini(repo / "symbols/GOnline-54BE1568-00C97000.ini")
    fragment = configparser.ConfigParser(interpolation=None)
    for section in ["host"] + manifest["mod"]["requires"].split(","):
        if not symbols.has_section(section) or dict(symbols[section]) != dict(source[section]):
            parser.error(f"Stale/missing native symbol: {section}; rebuild sdgo_data")
        fragment[section] = dict(symbols[section])
    text = io.StringIO()
    fragment.write(text, space_around_delimiters=False)
    if args.output.exists():
        parser.error("Existing archives are preserved; choose a new output filename")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, path in sorted(files.items()):
            archive.write(path, name)
        archive.writestr("symbols.fragment.ini", text.getvalue().encode("ascii"))
        archive.write(repo / "docs/storage-claim.zh-CN.md", "README.zh-CN.md")
    with zipfile.ZipFile(args.output) as archive:
        if archive.testzip():
            raise RuntimeError("Archive CRC verification failed")
    print(args.output.resolve())
    print("SHA256 " + hashlib.sha256(args.output.read_bytes()).hexdigest())


if __name__ == "__main__":
    main()
