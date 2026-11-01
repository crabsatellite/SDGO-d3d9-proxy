"""Build the client registration for the collection capsule machine.

The ordinary capsule UI has six preview slots, but its reply handler consumes
the UnitID returned by the server packet.  The six slots created here are
therefore presentation anchors only.  All twelve card states use the same
placeholder art and never define the server prize pool.

The builder reads either the stock client archive or a previously compiled
capsule registry, preserves every unrelated machine row and generated asset,
and emits one complete registry for the shared ``capsule_catalog`` provider.
It never rewrites the game's GPackData archives.
"""
from __future__ import annotations

import configparser
import gzip
import hashlib
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import uuid
import zlib


BUILDER_VERSION = 1
MACHINE_PRICE_GP = 100_000
HOST_TIMESTAMP = 0x54BE1568
HOST_SIZE_OF_IMAGE = 0x00C97000
TABLE_BASES = {"0033": 155001, "0101": 135001, "0154": 3519001, "0155": 3520001}
TABLE_SIZES = {"0033": 1019, "0101": 114, "0154": 231, "0155": 18}

# Reserved by the capsule registry provider for generated resources.  The
# collection machine owns only these last two sprite IDs and one texture ID.
PLACEHOLDER_TEXTURE_ID = 65534
PLACEHOLDER_SHOWN_SPRITE_ID = 65534
PLACEHOLDER_HIDDEN_SPRITE_ID = 65535
PLACEHOLDER_TEXTURE = f"txrs\\{PLACEHOLDER_TEXTURE_ID}.txr"
PLACEHOLDER_SPRITES = (
    f"sprs\\{PLACEHOLDER_SHOWN_SPRITE_ID}.spr",
    f"sprs\\{PLACEHOLDER_HIDDEN_SPRITE_ID}.spr",
)

CAPSULE_UI_RESOURCE = "uirs/1161.uir"
SUPPORTED_UI_LENGTH = 36183
SUPPORTED_UI_SOURCE_SHA256 = (
    "01671cdc6dab9789935c47d3a23486117e302f68a6824d43eac769e1f53f4cd6"
)
SUPPORTED_UI_PATCHED_SHA256 = (
    "f3852c7bcc179c54ef0af8af7caea290fd388c9a0474c8ebcfc271f9c2ed489e"
)
PRICE_FIELDS = (
    (0, "PointStatic0", 0x7EC0, 0x7F3B, "10000"),
    (1, "PointStatic1", 0x7FA2, 0x801D, "1200"),
    (2, "PointStatic2", 0x8084, 0x80FF, "999"),
    (3, "PointStatic3", 0x8166, 0x81E1, "999"),
    (4, "PointStatic4", 0x8248, 0x82C3, "999"),
    (5, "PointStatic5", 0x84EE, 0x8569, "999"),
    (6, "PointStatic6", 0x832A, 0x83A5, "999"),
    (7, "PointStatic7", 0x840C, 0x8487, "999"),
)

NATIVE_CONTRACT = {
    "host_sha256": "e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92",
    "reply_dispatch_function_va": "0x0078f930",
    "ordinary_reply_branch_va": "0x00790062",
    "packet_unit_id_read_va": "0x00790144",
    "result_unit_id_store_va": "0x00790198",
    "finding": (
        "The ordinary capsule reply reads UnitID from packet offset +6, passes "
        "it to the native unit/result path and stores it in the result object. "
        "This branch does not read or compare the six 0033 preview UnitIDs."
    ),
    "scope": "static shipped-client evidence; manual game acceptance remains required",
}


class ClientRegistryError(ValueError):
    """The client, registry or generated resource failed a bounded check."""


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _json_bytes(value) -> bytes:
    return (
        json.dumps(
            value,
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        )
        + "\n"
    ).encode("utf-8")


def _pretty_json_bytes(value) -> bytes:
    return (
        json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + "\n"
    ).encode("utf-8")


def _absolute(path) -> Path:
    return Path(os.path.abspath(os.fspath(path)))


def _assert_existing_file(path: Path, label: str) -> Path:
    path = _absolute(path)
    if not path.is_file():
        raise ClientRegistryError(f"{label} is missing: {path}")
    _assert_no_linked_ancestors(path, label)
    return path


def _assert_existing_directory(path: Path, label: str) -> Path:
    path = _absolute(path)
    if not path.is_dir():
        raise ClientRegistryError(f"{label} is missing: {path}")
    _assert_no_linked_ancestors(path, label)
    return path


def _assert_no_linked_ancestors(path: Path, label: str) -> None:
    current = _absolute(path)
    while True:
        if current.exists():
            attributes = getattr(current.lstat(), "st_file_attributes", 0)
            if current.is_symlink() or attributes & getattr(
                stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400
            ):
                raise ClientRegistryError(f"{label} uses a linked path: {current}")
        parent = current.parent
        if parent == current:
            break
        current = parent


def _read_pe_host(path: Path) -> tuple[int, int]:
    raw = path.read_bytes()
    if len(raw) < 0x100:
        raise ClientRegistryError("GOnline.exe is truncated")
    nt = struct.unpack_from("<I", raw, 0x3C)[0]
    if nt < 0x40 or nt + 0x18 + 0x3C > len(raw) or raw[nt : nt + 4] != b"PE\0\0":
        raise ClientRegistryError("GOnline.exe has an invalid PE header")
    timestamp = struct.unpack_from("<I", raw, nt + 8)[0]
    size_of_image = struct.unpack_from("<I", raw, nt + 0x18 + 0x38)[0]
    return timestamp, size_of_image


class ArchiveReader:
    """Bounded ZDX/ZPK reader for the stock pack or a compiled overlay."""

    def __init__(self, zdx: Path, zpk: Path, *, loose_root: Path | None = None):
        self.zdx = _assert_existing_file(zdx, "archive index")
        self.zpk = _assert_existing_file(zpk, "archive payload")
        self.loose_root = _absolute(loose_root) if loose_root is not None else None
        raw = self.zdx.read_bytes()
        if len(raw) < 24 or len(raw) > 64 * 1024 * 1024:
            raise ClientRegistryError("Unsupported archive index size")
        version, self.chunk_size, chunks, _, count, _ = struct.unpack_from("<6I", raw)
        if (
            version != 1
            or not 0 < self.chunk_size <= 1024 * 1024
            or count > 1_000_000
        ):
            raise ClientRegistryError("Unsupported archive header")
        expected_zpk = chunks * (self.chunk_size + 8)
        if self.zpk.stat().st_size != expected_zpk:
            raise ClientRegistryError("Archive payload size differs from its chunk table")
        self.index_sha256 = _sha256(raw)
        self.entries: dict[str, tuple[int, int, int]] = {}
        cursor = 24
        for _ in range(count):
            chars = []
            for _ in range(4097):
                if cursor + 2 > len(raw):
                    raise ClientRegistryError("Archive path is truncated")
                value = struct.unpack_from("<H", raw, cursor)[0] ^ 0xAA
                cursor += 2
                if value == 0:
                    break
                if value > 0x7F:
                    raise ClientRegistryError("Archive path is not ASCII")
                chars.append(chr(value))
            else:
                raise ClientRegistryError("Archive path exceeds the supported bound")
            if cursor + 36 > len(raw):
                raise ClientRegistryError("Archive entry metadata is truncated")
            size, start, length = struct.unpack_from("<III", raw, cursor)
            cursor += 36
            name = "".join(chars).replace("\\", "/").lower()
            if not name or name in self.entries:
                raise ClientRegistryError("Archive contains an empty or duplicate entry")
            if size == 0 or length == 0 or start + length > chunks:
                raise ClientRegistryError("Archive entry has an invalid chunk range")
            self.entries[name] = (size, start, length)
        # The shipped index reserves an all-0xFF tail. Generated overlays do
        # not need one, but no other trailing bytes are accepted.
        if any(value != 0xFF for value in raw[cursor:]):
            raise ClientRegistryError("Archive index has unexpected trailing bytes")

    def has(self, name: str) -> bool:
        key = name.replace("\\", "/").lower()
        if self.loose_root is not None:
            loose = self.loose_root.joinpath(*key.split("/"))
            if loose.is_file():
                _assert_no_linked_ancestors(loose, f"loose resource {key}")
                return True
        return key in self.entries

    def read(self, name: str, *, limit: int = 32 * 1024 * 1024) -> bytes:
        key = name.replace("\\", "/").lower()
        if self.loose_root is not None:
            loose = self.loose_root.joinpath(*key.split("/"))
            if loose.is_file():
                _assert_no_linked_ancestors(loose, f"loose resource {key}")
                raw = loose.read_bytes()
                if len(raw) > limit:
                    raise ClientRegistryError(f"Loose resource exceeds the bound: {key}")
                return raw
        if key not in self.entries:
            raise ClientRegistryError(f"Archive resource is missing: {key}")
        size, current, length = self.entries[key]
        if size > limit or length > 100_000:
            raise ClientRegistryError(f"Archive resource exceeds the bound: {key}")
        data = bytearray()
        seen = set()
        with self.zpk.open("rb") as handle:
            for _ in range(length):
                if current in seen:
                    raise ClientRegistryError("Archive chunk chain is cyclic")
                seen.add(current)
                handle.seek(current * (self.chunk_size + 8))
                block = handle.read(self.chunk_size + 8)
                if len(block) != self.chunk_size + 8:
                    raise ClientRegistryError("Archive chunk is truncated")
                current = struct.unpack_from("<I", block)[0]
                data.extend(block[8:])
        raw = bytes(data[:size])
        if raw.startswith(b"ZOAGZIP1"):
            if len(raw) < 12:
                raise ClientRegistryError("Archive gzip header is truncated")
            expected = struct.unpack_from("<I", raw, 8)[0]
            if expected > limit:
                raise ClientRegistryError("Decoded archive resource exceeds the bound")
            stream = zlib.decompressobj(31)
            result = stream.decompress(raw[12:], expected + 1)
            if len(result) != expected or not stream.eof or stream.unused_data:
                raise ClientRegistryError("Archive gzip payload is invalid")
            return result
        return raw


def decode_gdt(data: bytes) -> tuple[int, list[bytes], int]:
    if data[:2] != b"\xf9\xf8" or len(data) < 14:
        raise ClientRegistryError("Unsupported GDT encoding")
    size, count, extra = struct.unpack_from("<III", data, 2)
    if not 0 < size <= 4096 or count > 100_000:
        raise ClientRegistryError("Unsupported GDT dimensions")
    key_size = (size + count + extra) % 65 + 192
    key = data[14 : 14 + key_size]
    if len(key) != key_size or len(set(key)) != key_size or max(key) >= key_size:
        raise ClientRegistryError("GDT permutation key is invalid")
    stride = math.ceil(size / key_size) * key_size
    if len(data) != 14 + key_size + stride * count:
        raise ClientRegistryError("GDT data is truncated or has trailing bytes")
    rows = []
    for index in range(count):
        start = 14 + key_size + index * stride
        decoded = bytearray()
        for offset in range(0, stride, key_size):
            block = data[start + offset : start + offset + key_size]
            decoded.extend(
                block[key[i]] ^ key[(768 + 3 * i) % key_size]
                for i in range(key_size)
            )
        rows.append(bytes(decoded[:size]))
    return size, rows, extra


def encode_gdt(size: int, rows: list[bytes], extra: int) -> bytes:
    if any(len(row) != size for row in rows):
        raise ClientRegistryError("GDT row size differs")
    count = len(rows)
    key_size = (size + count + extra) % 65 + 192
    key = bytes(range(key_size))
    result = bytearray(b"\xf9\xf8" + struct.pack("<III", size, count, extra) + key)
    stride = math.ceil(size / key_size) * key_size
    for row in rows:
        padded = row.ljust(stride, b"\0")
        for offset in range(0, stride, key_size):
            result.extend(
                padded[offset + i] ^ key[(768 + 3 * i) % key_size]
                for i in range(key_size)
            )
    encoded = bytes(result)
    if decode_gdt(encoded) != (size, rows, extra):
        raise ClientRegistryError("GDT encoder round-trip failed")
    return encoded


def _put_wide(row: bytearray, offset: int, capacity: int, text: str) -> None:
    raw = (text + "\0").encode("utf-16-le")
    if len(raw) > capacity:
        raise ClientRegistryError("Resource path exceeds the native GDT field")
    row[offset : offset + capacity] = raw.ljust(capacity, b"\0")


def _get_wide(row: bytes, offset: int, capacity: int) -> str:
    raw = row[offset : offset + capacity]
    terminator = next(
        (index for index in range(0, len(raw), 2) if raw[index : index + 2] == b"\0\0"),
        None,
    )
    if terminator is None:
        raise ClientRegistryError("GDT UTF-16 field is unterminated")
    try:
        return raw[:terminator].decode("utf-16-le")
    except UnicodeDecodeError as error:
        raise ClientRegistryError("GDT UTF-16 field is invalid") from error


def _machine_sprites(row: bytes) -> tuple[str, ...]:
    return tuple(_get_wide(row, 59 + index * 60, 60) for index in range(12))


def _owned_machine(row: bytes, machine_id: int) -> bool:
    if struct.unpack_from("<I", row, 5)[0] != machine_id:
        return False
    expected = tuple(PLACEHOLDER_SPRITES[index % 2] for index in range(12))
    return _machine_sprites(row) == expected


def _source_unit_id(value) -> int:
    if type(value) is int:
        result = value
    elif isinstance(value, dict):
        result = None
        for key in ("unit_id", "UnitNumber", "id"):
            if type(value.get(key)) is int:
                result = value[key]
                break
        if result is None:
            raise ClientRegistryError("Source unit row has no integer unit ID")
    else:
        raise ClientRegistryError("Source units must be integers or unit rows")
    if result <= 0 or result > 0x7FFFFFFF:
        raise ClientRegistryError("Source unit ID is outside the native SQL/client range")
    return result


def preview_unit_ids(source_units) -> tuple[int, ...]:
    """Select six stable, distinct presentation anchors from live source rows."""
    result = []
    seen = set()
    for item in source_units:
        unit_id = _source_unit_id(item)
        if unit_id not in seen:
            result.append(unit_id)
            seen.add(unit_id)
        if len(result) == 6:
            break
    if len(result) != 6:
        raise ClientRegistryError("Six distinct source units are required for native preview slots")
    return tuple(result)


def _card_texture(cover: Path) -> tuple[bytes, tuple[float, float, float, float], tuple[int, int]]:
    try:
        from PIL import Image
    except ImportError as error:
        raise ClientRegistryError("Pillow is required to compile the placeholder cover") from error
    with Image.open(cover) as source:
        source.load()
        if source.size != (182, 108):
            raise ClientRegistryError("Collection cover must be exactly 182x108 pixels")
        image = source.convert("RGBA")
    padding = 8
    width = 1 << (image.width + padding * 2 - 1).bit_length()
    height = 1 << (image.height + padding * 2 - 1).bit_length()
    texture = Image.new("RGBA", (width, height), (255, 0, 255, 0))
    texture.paste(image, (padding, padding))
    uv = (
        padding / width,
        padding / height,
        (padding + image.width) / width,
        (padding + image.height) / height,
    )
    stream = io.BytesIO()
    texture.save(stream, "PNG", optimize=False, compress_level=9)
    return stream.getvalue(), uv, image.size


def _standalone_sprite(
    texture_path: str,
    width: int,
    height: int,
    uv: tuple[float, float, float, float],
) -> bytes:
    raw_path = (texture_path + "\0").encode("utf-16-le")
    if len(raw_path) > 64:
        raise ClientRegistryError("Texture path exceeds the native sprite field")
    header = struct.pack("<IIIII4f2f", 10000, 1, 20, height, width, *uv, 0, 0)
    return header + raw_path.ljust(64, b"\0") + bytes(64) + struct.pack("<f", 1.0)


def patch_capsule_price_defaults(data: bytes) -> tuple[bytes, dict]:
    """Clear only the eight known design-time price defaults, fail closed."""
    if len(data) != SUPPORTED_UI_LENGTH:
        raise ClientRegistryError(f"Unsupported 1161.uir length: {len(data)}")
    states = []
    fields = []
    for slot, name, name_offset, text_offset, default in PRICE_FIELDS:
        encoded_name = name.encode("utf-16-le")
        occurrences = [m.start() for m in re.finditer(re.escape(encoded_name), data)]
        if occurrences != [name_offset]:
            raise ClientRegistryError(f"Unexpected {name} layout")
        if text_offset != name_offset + 0x7B:
            raise ClientRegistryError(f"Unexpected {name} price offset")
        raw = data[text_offset : text_offset + 64]
        if len(raw) != 64:
            raise ClientRegistryError(f"Truncated {name} price field")
        terminator = next(
            (index for index in range(0, 64, 2) if raw[index : index + 2] == b"\0\0"),
            None,
        )
        if terminator is None:
            raise ClientRegistryError(f"Unterminated {name} price field")
        text = raw[:terminator].decode("utf-16-le")
        expected = (default + "\0").encode("utf-16-le")
        if text == default:
            states.append("source")
        elif text == "" and raw[:2] == b"\0\0" and raw[2 : len(expected)] == expected[2:]:
            states.append("patched")
        else:
            raise ClientRegistryError(f"Unsupported {name} price default")
        fields.append(
            {
                "slot": slot,
                "name": name,
                "name_offset": name_offset,
                "text_offset": text_offset,
                "original_default": default,
            }
        )
    if len(set(states)) != 1:
        raise ClientRegistryError("Partially patched 1161.uir is unsupported")
    input_hash = _sha256(data)
    if states[0] == "source":
        if input_hash != SUPPORTED_UI_SOURCE_SHA256:
            raise ClientRegistryError("Unsupported source 1161.uir hash")
        result = bytearray(data)
        for field in fields:
            result[field["text_offset"] : field["text_offset"] + 2] = b"\0\0"
        output = bytes(result)
        changed = True
    else:
        if input_hash != SUPPORTED_UI_PATCHED_SHA256:
            raise ClientRegistryError("Unsupported patched 1161.uir hash")
        output = data
        changed = False
    if _sha256(output) != SUPPORTED_UI_PATCHED_SHA256:
        raise ClientRegistryError("1161.uir patch output differs from native evidence")
    return output, {
        "resource": CAPSULE_UI_RESOURCE,
        "input_sha256": input_hash,
        "output_sha256": _sha256(output),
        "changed": changed,
        "fields": fields,
    }


def build_archive(resources: dict[str, bytes]) -> tuple[bytes, bytes, dict]:
    """Encode an independent native-format overlay archive."""
    chunk_size = 512
    entries = []
    chunks = []
    for supplied_name, raw in sorted(resources.items()):
        name = supplied_name.replace("\\", "/").lower()
        generated = re.fullmatch(r"(sprs|txrs)/([0-9]{5})\.(spr|txr)", name)
        generated_ok = (
            generated is not None
            and 60000 <= int(generated.group(2)) <= 65535
            and (generated.group(1), generated.group(3))
            in {("sprs", "spr"), ("txrs", "txr")}
        )
        if not (
            re.fullmatch(r"gdrs/(?:0033|0101|0154|0155)\.gdt", name)
            or name == "uirs/1161.uir"
            or generated_ok
        ):
            raise ClientRegistryError(f"Resource is outside the provider allowlist: {name}")
        packed = b"ZOAGZIP1" + struct.pack("<I", len(raw)) + gzip.compress(raw, mtime=0)
        start = len(chunks)
        count = (len(packed) + chunk_size - 1) // chunk_size
        for index in range(count):
            cursor = len(chunks)
            chunks.append(
                struct.pack("<II", cursor + 1, max(0, cursor - 1))
                + packed[index * chunk_size : (index + 1) * chunk_size].ljust(
                    chunk_size, b"\0"
                )
            )
        entries.append(
            {
                "path": name,
                "zsize": len(packed),
                "start_chunk": start,
                "chunks": count,
                "sha256": _sha256(raw),
            }
        )
    zdx = bytearray(struct.pack("<6I", 1, chunk_size, len(chunks), 0, len(entries), 0))
    for entry in entries:
        for character in entry["path"].replace("/", "\\") + "\0":
            zdx.extend(struct.pack("<H", ord(character) ^ 0xAA))
        zdx.extend(
            struct.pack(
                "<III", entry["zsize"], entry["start_chunk"], entry["chunks"]
            )
            + bytes(24)
        )
    return bytes(zdx), b"".join(chunks), {
        "chunk_size": chunk_size,
        "entries": entries,
    }


def _read_runtime_registry(path: Path) -> ArchiveReader:
    runtime_path = _assert_existing_file(path / "runtime.ini", "current registry runtime.ini")
    parser = configparser.RawConfigParser(interpolation=None, strict=True)
    parser.optionxform = str.lower
    try:
        parser.read(runtime_path, encoding="ascii")
    except (UnicodeDecodeError, configparser.Error) as error:
        raise ClientRegistryError("Current registry runtime.ini is invalid") from error
    if parser.get("runtime", "schema", fallback="") != "1":
        raise ClientRegistryError("Current registry schema is unsupported")
    overlay = parser.get("runtime", "overlay", fallback="")
    if overlay != "overlay":
        raise ClientRegistryError("Current registry uses an unsupported overlay name")
    zdx = _assert_existing_file(path / "overlay.zdx", "current registry overlay.zdx")
    zpk = _assert_existing_file(path / "overlay.zpk", "current registry overlay.zpk")
    expected_zdx = parser.get("runtime", "overlay_zdx_sha256", fallback="").lower()
    expected_zpk = parser.get("runtime", "overlay_zpk_sha256", fallback="").lower()
    if _sha256(zdx.read_bytes()) != expected_zdx or _sha256(zpk.read_bytes()) != expected_zpk:
        raise ClientRegistryError("Current registry overlay hash differs from runtime.ini")
    reader = ArchiveReader(zdx, zpk)
    for table in TABLE_BASES:
        configured = parser.get("gdt", table, fallback="").replace("\\", "/").lower()
        expected = parser.get("gdt", f"{table}_sha256", fallback="").lower()
        if configured != f"data/{table}.gdt":
            raise ClientRegistryError(f"Current registry GDT path differs: {table}")
        if _sha256(reader.read(f"gdrs/{table}.gdt")) != expected:
            raise ClientRegistryError(f"Current registry GDT hash differs: {table}")
    configured_ui = parser.get("ui", "1161", fallback="").replace("\\", "/").lower()
    expected_ui = parser.get("ui", "1161_sha256", fallback="").lower()
    if configured_ui or expected_ui:
        if configured_ui != "ui/1161.uir" or not re.fullmatch(
            r"[0-9a-f]{64}", expected_ui
        ):
            raise ClientRegistryError("Current registry UI declaration differs: 1161")
        if not reader.has(CAPSULE_UI_RESOURCE):
            raise ClientRegistryError("Current registry UI resource is missing: 1161")
        if _sha256(reader.read(CAPSULE_UI_RESOURCE)) != expected_ui:
            raise ClientRegistryError("Current registry UI hash differs: 1161")
    elif reader.has(CAPSULE_UI_RESOURCE):
        raise ClientRegistryError("Current registry has an undeclared UI resource: 1161")
    return reader


def _stock_reader(client_root: Path) -> ArchiveReader:
    executable = _assert_existing_file(client_root / "GOnline.exe", "GOnline.exe")
    timestamp, image_size = _read_pe_host(executable)
    if timestamp != HOST_TIMESTAMP or image_size != HOST_SIZE_OF_IMAGE:
        raise ClientRegistryError(
            "Unsupported GOnline.exe host: "
            f"{timestamp:08X}/{image_size:08X}, expected "
            f"{HOST_TIMESTAMP:08X}/{HOST_SIZE_OF_IMAGE:08X}"
        )
    return ArchiveReader(
        client_root / "GPackData.zdx",
        client_root / "GPackData.zpk",
        loose_root=client_root,
    )


def _resource_is_generated(name: str) -> bool:
    match = re.fullmatch(r"(sprs|txrs)/([0-9]{5})\.(spr|txr)", name)
    return (
        match is not None
        and 60000 <= int(match.group(2)) <= 65535
        and (match.group(1), match.group(3))
        in {("sprs", "spr"), ("txrs", "txr")}
    )


def _make_machine_row(
    template: bytes,
    grid_id: int,
    machine_id: int,
    previews: tuple[int, ...],
) -> bytes:
    row = bytearray(template)
    struct.pack_into("<I", row, 0, grid_id)
    struct.pack_into("<I", row, 5, machine_id)
    struct.pack_into("<II", row, 11, 6, MACHINE_PRICE_GP)
    row[19:27] = bytes(8)
    row[27:59] = bytes(32)
    row[59:1019] = bytes(960)
    for index, unit_id in enumerate(previews):
        struct.pack_into("<I", row, 27 + index * 4, unit_id)
        _put_wide(row, 59 + index * 120, 60, PLACEHOLDER_SPRITES[0])
        _put_wide(row, 59 + index * 120 + 60, 60, PLACEHOLDER_SPRITES[1])
    return bytes(row)


def _validate_table(table: str, data: bytes) -> tuple[list[bytes], int]:
    size, rows, extra = decode_gdt(data)
    if size != TABLE_SIZES[table]:
        raise ClientRegistryError(
            f"Unexpected {table}.gdt row size: {size}, expected {TABLE_SIZES[table]}"
        )
    base = TABLE_BASES[table]
    for index, row in enumerate(rows):
        if struct.unpack_from("<I", row)[0] != base + index:
            raise ClientRegistryError(f"{table}.gdt grid IDs are not continuous")
    return rows, extra


def _write_tree(output: Path, files: dict[str, bytes]) -> None:
    output = _absolute(output)
    if output.exists():
        raise FileExistsError(f"Client registry output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    pending = output.with_name(f".{output.name}.{uuid.uuid4().hex}.pending")
    pending.mkdir()
    try:
        for relative, data in files.items():
            target = pending.joinpath(*relative.split("/"))
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            if target.read_bytes() != data:
                raise OSError(f"Client registry write readback failed: {relative}")
        pending.replace(output)
    except BaseException:
        shutil.rmtree(pending, ignore_errors=True)
        raise


def build_client_registry(
    *,
    client_root,
    current_registry=None,
    source_units=(),
    machine_id: int = 990001,
    cover,
    output,
    enabled: bool = True,
) -> dict:
    """Build a complete shared capsule registry.

    ``current_registry`` is an optional prior compiler output.  When supplied,
    all unrelated rows and generated 60000..65535 resources are retained.
    ``source_units`` supplies six valid presentation anchors and is ignored
    while disabling an already registered machine.
    """
    if type(machine_id) is not int or not 0 < machine_id <= 0x7FFFFFFF:
        raise ClientRegistryError("Machine ID must be a positive SQL/client INT")
    client_root = _assert_existing_directory(client_root, "client root")
    cover_path = _assert_existing_file(cover, "collection cover") if enabled else None
    stock = _stock_reader(client_root)
    current_path = (
        _assert_existing_directory(current_registry, "current capsule registry")
        if current_registry is not None
        else None
    )
    source = _read_runtime_registry(current_path) if current_path else stock

    raw_tables: dict[str, bytes] = {}
    decoded: dict[str, tuple[list[bytes], int]] = {}
    for table in TABLE_BASES:
        raw = source.read(f"gdrs/{table}.gdt")
        raw_tables[table] = raw
        decoded[table] = _validate_table(table, raw)

    fixed_rows, fixed_extra = decoded["0033"]
    if not fixed_rows:
        raise ClientRegistryError("Stock 0033.gdt has no row to use as a native template")
    matching = [
        index
        for index, row in enumerate(fixed_rows)
        if struct.unpack_from("<I", row, 5)[0] == machine_id
    ]
    if len(matching) > 1:
        raise ClientRegistryError("Machine ID appears more than once in 0033.gdt")
    owned_index = (
        matching[0]
        if matching and _owned_machine(fixed_rows[matching[0]], machine_id)
        else None
    )
    if matching and owned_index is None:
        raise ClientRegistryError("Machine ID is occupied by another capsule definition")
    previously_owned = owned_index is not None
    reserved_paths = {
        PLACEHOLDER_TEXTURE.replace("\\", "/"),
        *(sprite.replace("\\", "/") for sprite in PLACEHOLDER_SPRITES),
    }
    reserved_sprites = {
        sprite.replace("\\", "/").lower() for sprite in PLACEHOLDER_SPRITES
    }
    for index, row in enumerate(fixed_rows):
        if index == owned_index:
            continue
        referenced = {
            value.replace("\\", "/").lower()
            for value in _machine_sprites(row)
            if value
        }
        conflict = sorted(referenced & reserved_sprites)
        if conflict:
            raise ClientRegistryError(
                "Reserved collection sprite is referenced by another machine: "
                + ", ".join(conflict)
            )

    resources: dict[str, bytes] = {}
    if current_path:
        for name in source.entries:
            if _resource_is_generated(name):
                resources[name] = source.read(name)

    action = "unchanged"
    previews: tuple[int, ...] = ()
    if enabled:
        previews = preview_unit_ids(source_units)
        if not previously_owned:
            for path in sorted(reserved_paths):
                if path in resources or stock.has(path):
                    raise ClientRegistryError(
                        f"Reserved collection resource is occupied: {path}"
                    )
        if owned_index is None:
            index = len(fixed_rows)
            fixed_rows.append(
                _make_machine_row(
                    fixed_rows[0],
                    TABLE_BASES["0033"] + index,
                    machine_id,
                    previews,
                )
            )
            owned_index = index
            action = "added"
        else:
            grid_id = TABLE_BASES["0033"] + owned_index
            replacement = _make_machine_row(
                fixed_rows[0], grid_id, machine_id, previews
            )
            action = "unchanged" if fixed_rows[owned_index] == replacement else "updated"
            fixed_rows[owned_index] = replacement
        texture, uv, cover_size = _card_texture(cover_path)
        resources[PLACEHOLDER_TEXTURE.replace("\\", "/")] = texture
        sprite = _standalone_sprite(
            PLACEHOLDER_TEXTURE, cover_size[0], cover_size[1], uv
        )
        for path in PLACEHOLDER_SPRITES:
            resources[path.replace("\\", "/")] = sprite
    else:
        if owned_index is not None:
            fixed_rows.pop(owned_index)
            action = "removed"
            for path in (
                PLACEHOLDER_TEXTURE.replace("\\", "/"),
                *(sprite.replace("\\", "/") for sprite in PLACEHOLDER_SPRITES),
            ):
                resources.pop(path, None)
            for index, row in enumerate(fixed_rows):
                mutable = bytearray(row)
                struct.pack_into("<I", mutable, 0, TABLE_BASES["0033"] + index)
                fixed_rows[index] = bytes(mutable)

    raw_tables["0033"] = encode_gdt(TABLE_SIZES["0033"], fixed_rows, fixed_extra)
    for table, raw in raw_tables.items():
        resources[f"gdrs/{table}.gdt"] = raw

    ui_source = (
        source.read(CAPSULE_UI_RESOURCE)
        if source.has(CAPSULE_UI_RESOURCE)
        else stock.read(CAPSULE_UI_RESOURCE)
    )
    patched_ui, ui_receipt = patch_capsule_price_defaults(ui_source)
    resources[CAPSULE_UI_RESOURCE] = patched_ui

    counts = {
        "fixed": len(fixed_rows),
        "coin": len(decoded["0101"][0]),
        "custom": len(decoded["0154"][0]),
        "candidates": len(decoded["0155"][0]),
    }
    source_identity = {
        "client_index_sha256": stock.index_sha256,
        "current_registry_index_sha256": source.index_sha256 if current_path else None,
        "source_gdt_sha256": {
            table: _sha256(source.read(f"gdrs/{table}.gdt"))
            for table in TABLE_BASES
        },
    }
    semantic = {
        "builder_version": BUILDER_VERSION,
        "machine_id": machine_id,
        "price_gp": MACHINE_PRICE_GP,
        "enabled": enabled,
        "preview_unit_ids": list(previews),
        "resources": {
            name: _sha256(raw) for name, raw in sorted(resources.items())
        },
    }
    revision = _sha256(_json_bytes(semantic))
    zdx, zpk, overlay_index = build_archive(resources)

    files: dict[str, bytes] = {
        "overlay.zdx": zdx,
        "overlay.zpk": zpk,
        "ui/1161.uir": patched_ui,
    }
    for table, raw in raw_tables.items():
        files[f"data/{table}.gdt"] = raw
    if enabled:
        files[f"assets/{PLACEHOLDER_TEXTURE.replace(chr(92), '/')}"] = resources[
            PLACEHOLDER_TEXTURE.replace("\\", "/")
        ]
        for sprite_path in PLACEHOLDER_SPRITES:
            normalized = sprite_path.replace("\\", "/")
            files[f"assets/{normalized}"] = resources[normalized]
    file_hashes = {name: _sha256(data) for name, data in files.items()}
    runtime_lines = [
        "[runtime]",
        "schema=1",
        f"catalog_revision={revision}",
        "overlay=overlay",
        f"overlay_zdx_sha256={file_hashes['overlay.zdx']}",
        f"overlay_zpk_sha256={file_hashes['overlay.zpk']}",
        "",
        "[gdt]",
    ]
    for table in TABLE_BASES:
        runtime_lines += [
            f"{table}=data\\{table}.gdt",
            f"{table}_sha256={file_hashes[f'data/{table}.gdt']}",
        ]
    runtime_lines += [
        "",
        "[ui]",
        "1161=ui\\1161.uir",
        f"1161_sha256={file_hashes['ui/1161.uir']}",
        "",
        "[counts]",
        *(f"{key}={value}" for key, value in counts.items()),
        "",
    ]
    files["runtime.ini"] = "\n".join(runtime_lines).encode("ascii")

    if enabled:
        native_position = len(fixed_rows) - int(owned_index)
        placement = {
            "machine_id": machine_id,
            "grid_id": TABLE_BASES["0033"] + int(owned_index),
            "native_position": native_position,
            "page": (native_position - 1) // 8 + 1,
            "page_slot": (native_position - 1) % 8 + 1,
            "preview_only_unit_ids": list(previews),
        }
    else:
        placement = None
    manifest = {
        "schema_version": 1,
        "builder_version": BUILDER_VERSION,
        "catalog_revision": revision,
        "action": action,
        "machine": {
            "id": machine_id,
            "enabled": enabled,
            "price_gp": MACHINE_PRICE_GP,
            "dynamic_server_pool": True,
        },
        "placement": placement,
        "counts": counts,
        "files": {name: _sha256(data) for name, data in sorted(files.items())},
        "overlay": overlay_index,
        "source": source_identity,
        "ui_patch": ui_receipt,
        "native_contract": NATIVE_CONTRACT,
    }
    files["manifest.json"] = _pretty_json_bytes(manifest)
    _write_tree(output, files)
    return {
        "output": str(_absolute(output)),
        "catalog_revision": revision,
        "action": action,
        "machine_id": machine_id,
        "enabled": enabled,
        "counts": counts,
        "placement": placement,
        "manifest_sha256": _sha256(files["manifest.json"]),
    }
