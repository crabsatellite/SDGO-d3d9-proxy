"""Compile a capsule catalog into a complete, versioned native client overlay."""
from __future__ import annotations
import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import struct

from capsule_catalog import GRADE_SETS, digest, gdt_records, read_json, validate, write_json
from capsule_assets import Assets, standalone_sprite
from capsule_ui_layout import patch_capsule_price_defaults


TABLE_BASES = {"0033": 155001, "0101": 135001, "0154": 3519001, "0155": 3520001}
COMPILER_VERSION = 7


def encode_gdt(size, records, extra=11):
    if any(len(row) != size for row in records):
        raise ValueError("Native GDT row size differs")
    count = len(records)
    key_size = (size + count + extra) % 65 + 192
    key = bytes(range(key_size))
    result = bytearray(b"\xf9\xf8" + struct.pack("<III", size, count, extra) + key)
    for row in records:
        padded = row.ljust((len(row) + key_size - 1) // key_size * key_size, b"\0")
        for pos in range(0, len(padded), key_size):
            result.extend(padded[pos + i] ^ key[(768 + 3 * i) % key_size] for i in range(key_size))
    encoded = bytes(result)
    if gdt_records(encoded) != (size, list(records)):
        raise ValueError("GDT encoder round-trip failed")
    return encoded


def put_wide(row, offset, capacity, text):
    raw = (text + "\0").encode("utf-16-le")
    if len(raw) > capacity:
        raise ValueError(f"Text exceeds native {capacity // 2 - 1}-character field: {text}")
    row[offset:offset + capacity] = raw.ljust(capacity, b"\0")


def card_texture(image, padding=8):
    """Keep native card pixels on a power-of-two texture with exact sub-UVs."""
    from PIL import Image
    if not (181 <= image.width <= 183 and 107 <= image.height <= 109):
        raise ValueError("Generated capsule art must keep native card dimensions")
    width = 1 << (image.width + padding * 2 - 1).bit_length()
    height = 1 << (image.height + padding * 2 - 1).bit_length()
    texture = Image.new("RGBA", (width, height), (255, 0, 255, 0))
    texture.paste(image.convert("RGBA"), (padding, padding))
    uv = (padding / width, padding / height,
          (padding + image.width) / width, (padding + image.height) / height)
    return texture, uv


def build_archive(resources, output):
    """Independent native-format pack; never rewrites the game's archive."""
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    chunk_size = 512
    entries, chunks = [], []
    for name, raw in sorted(resources.items()):
        packed = b"ZOAGZIP1" + struct.pack("<I", len(raw)) + gzip.compress(raw, mtime=0)
        start = len(chunks)
        count = (len(packed) + chunk_size - 1) // chunk_size
        for i in range(count):
            cursor = len(chunks)
            # The engine carries an explicit chunk count; the original pack
            # writer uses monotonically increasing links even at a file's end.
            chunks.append(struct.pack("<II", cursor + 1, max(0, cursor - 1)) +
                          packed[i * chunk_size:(i + 1) * chunk_size].ljust(chunk_size, b"\0"))
        entries.append({"path": name.replace("/", "\\").lower(), "zsize": len(packed),
                        "start_chunk": start, "chunks": count})
    zdx = bytearray(struct.pack("<6I", 1, chunk_size, len(chunks), 0, len(entries), 0))
    for entry in entries:
        for char in entry["path"] + "\0":
            zdx.extend(struct.pack("<H", ord(char) ^ 0xAA))
        zdx.extend(struct.pack("<III", entry["zsize"], entry["start_chunk"], entry["chunks"]) + bytes(24))
    output.with_suffix(".zdx").write_bytes(zdx)
    output.with_suffix(".zpk").write_bytes(b"".join(chunks))
    return {"chunk_size": chunk_size, "entries": entries}


def compile_catalog(catalog, client, lookup, output):
    validate(catalog)
    out = Path(output)
    out.mkdir(parents=True, exist_ok=False)
    data_dir = out / "data"
    data_dir.mkdir()
    assets = Assets(client, lookup, catalog)
    reader = assets.reader
    original = {}
    original_extras = {}
    for table in TABLE_BASES:
        source_data = reader.read_resource(f"gdrs/{table}.gdt")
        size, rows = gdt_records(source_data)
        original_extras[table] = struct.unpack_from("<I", source_data, 10)[0]
        original[table] = (size, rows)
    units = {u["id"]: u for u in catalog["units"]}
    revision = digest(catalog)
    resources, files, placement = {}, {}, []
    occupied_sprite_ids = {int(Path(name).stem) for name in reader.entries
                           if name.startswith("sprs/") and Path(name).stem.isdecimal()}
    occupied_texture_ids = {int(Path(name).stem) for name in reader.entries
                            if name.startswith("txrs/") and Path(name).stem.isdecimal()}
    next_sprite = 60000
    cards = {}
    native_card_references = []
    generated_cards = []

    def card_paths(uid):
        nonlocal next_sprite
        if uid in cards:
            return cards[uid]
        # Keep the game's authored head crop, rank banner and rare silhouette.
        # Re-rendering its SPR into a new PNG loses native frame properties;
        # full-body unit previews are a different resource type altogether.
        has_override = getattr(assets, "has_portrait_override", lambda *_: False)
        if units[uid].get("capsule_sprites") and not has_override(uid):
            paths = list(assets.card_sprite_paths(uid))
            native_card_references.extend(
                {"unit_id": uid, "variant": variant, "sprite": path}
                for variant, path in enumerate(paths)
            )
        else:
            paths = []
            for variant in range(2):
                if units[uid].get("capsule_sprites") and not has_override(uid, variant):
                    path = assets.card_sprite_paths(uid)[variant]
                    paths.append(path)
                    native_card_references.append({"unit_id": uid, "variant": variant, "sprite": path})
                    continue
                while next_sprite in occupied_sprite_ids or next_sprite in occupied_texture_ids:
                    next_sprite += 1
                if next_sprite > 65535:
                    raise ValueError("No unoccupied capsule sprite IDs in the reserved range")
                number = next_sprite
                occupied_sprite_ids.add(number)
                occupied_texture_ids.add(number)
                next_sprite += 1
                image = assets.card(uid, variant)
                texture, uv = card_texture(image)
                stream = io.BytesIO()
                texture.save(stream, "PNG")
                texture_path = f"txrs/{number}.txr"
                sprite_path = f"sprs/{number}.spr"
                resources[texture_path] = stream.getvalue()
                resources[sprite_path] = standalone_sprite(
                    texture_path.replace("/", "\\"), image.width, image.height, uv=uv)
                for relative in (texture_path, sprite_path):
                    target = out / "assets" / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    target.write_bytes(resources[relative])
                    files[target.relative_to(out).as_posix()] = hashlib.sha256(resources[relative]).hexdigest()
                provenance = {}
                if has_override(uid, variant):
                    from capsule_portraits import get_override
                    uploaded = get_override(catalog, uid, variant)
                    provenance = {"uploaded_png_sha256": uploaded["sha256"], "crop": uploaded["crop"]}
                generated_cards.append({"unit_id": uid, "variant": variant,
                    "sprite": sprite_path, "png_sha256": hashlib.sha256(stream.getvalue()).hexdigest(),
                    "texture_size": list(texture.size), "uv": list(uv),
                    "source": "native_frame_and_uploaded_png" if provenance else "native_frame_and_cropped_unit_sprite",
                    **provenance})
                paths.append(sprite_path.replace("/", "\\"))
        cards[uid] = paths
        return paths

    fixed_source = {struct.unpack_from("<I", r, 5)[0]: r for r in original["0033"][1]}
    enabled_fixed = [m for m in catalog["fixed_machines"] if m["enabled"] and not m.get("retired")]
    fixed_rows = []
    for index, machine in enumerate(enabled_fixed):
        if machine["currency"] != "GP":
            raise ValueError("New fixed-machine adapter currently supports GP prices")
        model = fixed_source.get(machine["id"], fixed_source.get(machine.get("template_id")))
        if model is None:
            model = original["0033"][1][0]
        row = bytearray(model)
        struct.pack_into("<I", row, 0, TABLE_BASES["0033"] + index)
        struct.pack_into("<I", row, 5, machine["id"])
        struct.pack_into("<II", row, 11, 6, machine["price"])
        original_row = fixed_source.get(machine["id"])
        unchanged_native_pool = (
            machine.get("owner") == "native" and original_row is not None
            and [slot["unit_id"] for slot in machine["slots"]]
                == list(struct.unpack_from("<6I", original_row, 27))
        )
        if unchanged_native_pool:
            # Keep per-machine native frame choices and unused record fields
            # exactly; still verify and inventory the referenced unit art.
            for slot in machine["slots"]:
                card_paths(slot["unit_id"])
        else:
            row[19:27] = bytes(8)
            row[27:59] = bytes(32)
            row[59:1019] = bytes(960)
            for i, slot in enumerate(machine["slots"]):
                uid = slot["unit_id"]
                row[19 + i] = int(units[uid]["subgrade"] == 4)
                struct.pack_into("<I", row, 27 + i * 4, uid)
                for j, sprite in enumerate(card_paths(uid)):
                    put_wide(row, 59 + (2 * i + j) * 60, 60, sprite)
        fixed_rows.append(bytes(row))
        native_position = len(enabled_fixed) - index
        placement.append({"kind": "fixed_machines", "id": machine["id"], "name": machine["name"],
                          "management_position": index + 1,
                          "position": native_position, "page": (native_position - 1) // 8 + 1,
                          "page_slot": (native_position - 1) % 8 + 1,
                          "grid_id": TABLE_BASES["0033"] + index, "price": machine["price"]})
    # Preserve existing coin variants, filter retired physical machines and
    # follow the user's current fixed-machine order. Do not invent new coins.
    coin_source = {struct.unpack_from("<I", r, 5)[0]: r for r in original["0101"][1]}
    coin_rows = []
    for machine in enabled_fixed:
        source = coin_source.get(machine["id"] + 1)
        if source is not None:
            row = bytearray(source)
            struct.pack_into("<I", row, 0, TABLE_BASES["0101"] + len(coin_rows))
            coin_rows.append(bytes(row))
    custom_source = {struct.unpack_from("<I", r, 5)[0]: r for r in original["0154"][1]}
    old_candidates = {(struct.unpack_from("<I", r, 5)[0], struct.unpack_from("<I", r, 9)[0]): r
                      for r in original["0155"][1]}
    custom_rows, candidate_rows = [], []
    for machine in catalog["custom_machines"]:
        if not machine["enabled"] or machine.get("retired"):
            continue
        candidates = [row for row in custom_source.values()
                      if struct.unpack_from("<H", row, 9)[0] == machine["currency_type"]]
        model = custom_source.get(machine["id"], candidates[0] if candidates else original["0154"][1][0])
        row = bytearray(model)
        struct.pack_into("<I", row, 0, TABLE_BASES["0154"] + len(custom_rows))
        struct.pack_into("<IHI", row, 5, machine["id"], machine["currency_type"], machine["price"])
        put_wide(row, 15, 100, machine["name"])
        for i, rule in enumerate(machine["slot_rules"]):
            grade_key = next((j for j, values in enumerate(GRADE_SETS) if set(values) == set(rule["subgrades"])), None)
            if grade_key is None or not 1 <= rule["rank_max"] <= 4:
                raise ValueError("Custom slot rule is not supported by the native format")
            row[115 + i * 2:117 + i * 2] = bytes((rule["rank_max"], grade_key))
        custom_rows.append(bytes(row))
        placement.append({"kind": "custom_machines", "id": machine["id"], "name": machine["name"],
                          "position": len(custom_rows), "page": (len(custom_rows) - 1) // 8 + 1,
                          "page_slot": (len(custom_rows) - 1) % 8 + 1,
                          "grid_id": TABLE_BASES["0154"] + len(custom_rows) - 1})
        for item in machine["candidates"]:
            uid = item["unit_id"]
            source = old_candidates.get((machine["id"], uid))
            if source is None:
                # Preserve the remaining native record shape. The final byte
                # controls the featured filter; new items default to unfeatured.
                same = [r for (_, old_uid), r in old_candidates.items()
                        if units[old_uid]["grade"] == units[uid]["grade"]]
                same = same or [r for (_, old_uid), r in old_candidates.items()
                                if units[old_uid]["rank"] == units[uid]["rank"]]
                source = same[0] if same else original["0155"][1][0]
            candidate = bytearray(source)
            struct.pack_into("<I", candidate, 0, TABLE_BASES["0155"] + len(candidate_rows))
            struct.pack_into("<III", candidate, 5, machine["id"], uid, item["extra_price"])
            if (machine["id"], uid) not in old_candidates or "featured" in item:
                candidate[17] = int(bool(item.get("featured", False)))
            candidate_rows.append(bytes(candidate))
    compiled = {"0033": fixed_rows, "0101": coin_rows, "0154": custom_rows, "0155": candidate_rows}
    for table, rows in compiled.items():
        data = encode_gdt(original[table][0], rows, extra=original_extras[table])
        target = data_dir / f"{table}.gdt"
        target.write_bytes(data)
        resources[f"gdrs/{table}.gdt"] = data
        files[f"data/{table}.gdt"] = hashlib.sha256(data).hexdigest()
    ui_data, ui_receipt = patch_capsule_price_defaults(reader.read_resource("uirs/1161.uir"))
    ui_path = out / "ui" / "1161.uir"
    ui_path.parent.mkdir()
    ui_path.write_bytes(ui_data)
    resources["uirs/1161.uir"] = ui_data
    files["ui/1161.uir"] = hashlib.sha256(ui_data).hexdigest()
    archive = build_archive(resources, out / "overlay")
    for name in ("overlay.zdx", "overlay.zpk"):
        files[name] = hashlib.sha256((out / name).read_bytes()).hexdigest()
    lines = ["[runtime]", "schema=1", f"catalog_revision={revision}", "asset_root=assets",
             "overlay=overlay", f"overlay_zdx_sha256={files['overlay.zdx']}",
             f"overlay_zpk_sha256={files['overlay.zpk']}", "", "[gdt]"]
    for table in TABLE_BASES:
        lines += [f"{table}=data\\{table}.gdt", f"{table}_sha256={files[f'data/{table}.gdt']}"]
    lines += ["", "[ui]", "1161=ui\\1161.uir",
              f"1161_sha256={files['ui/1161.uir']}"]
    counts = {"fixed": len(fixed_rows), "coin": len(coin_rows), "custom": len(custom_rows), "candidates": len(candidate_rows)}
    lines += ["", "[counts]"] + [f"{key}={value}" for key, value in counts.items()]
    (out / "runtime.ini").write_text("\n".join(lines) + "\n", encoding="ascii")
    write_json(out / "overlay-index.json", archive)
    write_json(out / "placements.json", {"catalog_revision": revision, "machines": placement})
    write_json(out / "manifest.json", {"schema_version": 1, "compiler_version": COMPILER_VERSION,
        "catalog_revision": revision,
        "counts": counts, "files": files, "generated_cards": generated_cards,
        "native_card_references": native_card_references,
        "ui_patches": [ui_receipt],
        "source_gdt_extras": original_extras,
        "source_index_sha256": reader.index_hash})
    return {"catalog_revision": revision, "counts": counts, "generated_cards": len(generated_cards),
            "native_card_units": len({row["unit_id"] for row in native_card_references}),
            "output": str(out)}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("catalog", "client", "lookup", "output"):
        p.add_argument("--" + name, type=Path, required=True)
    args = p.parse_args()
    print(json.dumps(compile_catalog(read_json(args.catalog), args.client, args.lookup, args.output)))


if __name__ == "__main__":
    main()
