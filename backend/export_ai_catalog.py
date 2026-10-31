#!/usr/bin/env python3
"""Export an owner's client catalog overlay without backend/private settings."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile
from ai_catalog import HEADER, MAGIC, RECORD, fnv
from install_ai_catalog import manifest_enabled


def export(source, catalog, output):
    source, output = Path(source), Path(output)
    if output.exists(): raise FileExistsError("output already exists")
    binary = Path(catalog).read_bytes()
    if len(binary) < HEADER.size: raise ValueError("catalog header missing")
    magic, version, size, count, checksum = HEADER.unpack_from(binary)
    if (magic != MAGIC or version != 1 or size != RECORD.size or not 0 < count <= 3000 or
        len(binary) != HEADER.size + count * size or fnv(binary[HEADER.size:], 32) != checksum):
        raise ValueError("invalid catalog length or checksum")
    directory = source / "client/sdgo/mods/ai_catalog"
    files = {
        "sdgo/mods/ai_catalog/ai_catalog.dll": (directory / "ai_catalog.dll").read_bytes(),
        "sdgo/mods/ai_catalog/mod.ini": manifest_enabled((directory / "mod.ini").read_bytes(), True),
        "sdgo/mods/ai_catalog/catalog.bin": binary,
        "README.txt": ("Install the matching SDGO mod framework and AI gameplay mods first.\n"
                       "Close the game, extract this overlay into the GOnline.exe folder, then use your normal launcher.\n"
                       "Every participant and the battle server need the same catalog.bin.\n"
                       "To disable: close the game and set sdgo/mods/ai_catalog/mod.ini [mod] enabled=0 on both sides.\n"
                       "No Sakura account, SQL connection, Python or special launcher is needed by players.\n").encode(),
    }
    files["manifest.json"] = (json.dumps({"schema_version": 1, "catalog_sha256": hashlib.sha256(binary).hexdigest(),
        "files": {name: hashlib.sha256(data).hexdigest() for name, data in sorted(files.items())}}, indent=2) + "\n").encode()
    output.parent.mkdir(parents=True, exist_ok=True)
    # Exclusive creation refuses replacement even if another export races this one.
    with zipfile.ZipFile(output, "x", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in sorted(files.items()): archive.writestr(name, data)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None or any(archive.read(name) != data for name, data in files.items()):
            raise OSError("export readback failed")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--catalog", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    try: export(args.source, args.catalog, args.out)
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        print(f"AI catalog export: {error}", file=sys.stderr); return 1
    print(str(args.out)); return 0


if __name__ == "__main__": raise SystemExit(main())
