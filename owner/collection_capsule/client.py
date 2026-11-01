"""Build the activity-page registration for the collection capsule machine.

The activity capsule page reads ``0099.gdt`` records in native grid order.
Each record contains the server machine ID, currency, price and one poster
sprite.  The server remains authoritative for the prize awarded by a draw.

The builder reads the stock client archive plus an optional previously
compiled capsule registry, preserves unrelated rows and generated assets, and
emits one complete registry for the shared ``capsule_catalog`` provider.  It
also removes the exact ordinary-page row produced by the older collection
builder.  It never rewrites the game's GPackData archives.
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


BUILDER_VERSION = 2
MACHINE_PRICE_GP = 100_000
HOST_TIMESTAMP = 0x54BE1568
HOST_SIZE_OF_IMAGE = 0x00C97000
TABLE_BASES = {
    "0033": 155001,
    "0101": 135001,
    "0154": 3519001,
    "0155": 3520001,
    "0099": 145001,
}
TABLE_SIZES = {
    "0031": 614,
    "0033": 1019,
    "0101": 114,
    "0154": 231,
    "0155": 18,
    "0099": 179,
}
LEGACY_TABLES = ("0033", "0101", "0154", "0155")
ACTIVITY_TABLE = "0099"
ITEM_CATEGORY_TABLE = "0031"
TABLE_NAMES = (*LEGACY_TABLES, ACTIVITY_TABLE, ITEM_CATEGORY_TABLE)
ACTIVITY_GRID_TYPE = 0x3C
ACTIVITY_SPRITE_OFFSET = 19
ACTIVITY_SPRITE_CAPACITY = 160
COVER_SIZE = (361, 322)
ITEM_CATEGORY_TEMPLATE_ID = 1140005
COLLECTION_CATEGORY_MIN = 1180000
COLLECTION_CATEGORY_MAX = 1199999

# Reserved by the capsule registry provider for generated resources.  The
# activity machine owns one poster sprite and one texture.  Sprite 65535 was
# used only by the superseded ordinary-page registration.
COLLECTION_TEXTURE_ID = 65534
COLLECTION_SPRITE_ID = 65534
LEGACY_HIDDEN_SPRITE_ID = 65535
COLLECTION_TEXTURE = f"txrs\\{COLLECTION_TEXTURE_ID}.txr"
COLLECTION_SPRITE = f"sprs\\{COLLECTION_SPRITE_ID}.spr"
LEGACY_ORDINARY_SPRITES = (
    f"sprs\\{COLLECTION_SPRITE_ID}.spr",
    f"sprs\\{LEGACY_HIDDEN_SPRITE_ID}.spr",
)

CAPSULE_UI_RESOURCE = "uirs/1161.uir"

NATIVE_CONTRACT = {
    "host_sha256": "e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92",
    "activity_loader_function_va": "0x007a69e0",
    "activity_grid_type": ACTIVITY_GRID_TYPE,
    "activity_first_grid_id": TABLE_BASES[ACTIVITY_TABLE],
    "activity_row_size": TABLE_SIZES[ACTIVITY_TABLE],
    "finding": (
        "The native activity capsule loader walks type 0x3c continuously from "
        "grid ID 145001, copies 179-byte records and lays valid posters out in "
        "eight slots per page."
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


def _owned_legacy_machine(row: bytes, machine_id: int) -> bool:
    if struct.unpack_from("<I", row, 5)[0] != machine_id:
        return False
    expected = tuple(LEGACY_ORDINARY_SPRITES[index % 2] for index in range(12))
    return _machine_sprites(row) == expected


def _activity_sprite(row: bytes) -> str:
    return _get_wide(row, ACTIVITY_SPRITE_OFFSET, ACTIVITY_SPRITE_CAPACITY)


def _owned_activity_machine(row: bytes, machine_id: int) -> bool:
    return (
        struct.unpack_from("<I", row, 5)[0] == machine_id
        and _activity_sprite(row).replace("/", "\\").lower()
        == COLLECTION_SPRITE.lower()
    )


def _card_texture(cover: Path) -> tuple[bytes, tuple[float, float, float, float], tuple[int, int]]:
    try:
        from PIL import Image
    except ImportError as error:
        raise ClientRegistryError("Pillow is required to compile the activity cover") from error
    with Image.open(cover) as source:
        source.load()
        if source.size != COVER_SIZE:
            raise ClientRegistryError(
                f"Collection cover must be exactly {COVER_SIZE[0]}x{COVER_SIZE[1]} pixels"
            )
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
    header = struct.pack(
        "<IIIII4f2f", 10000, 1, 20, height, width, *uv, 0, 0
    )
    return header + raw_path.ljust(64, b"\0") + bytes(64) + struct.pack("<f", 1.0)


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
            re.fullmatch(r"gdrs/(?:0031|0033|0099|0101|0154|0155)\.gdt", name)
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
    for table in LEGACY_TABLES:
        configured = parser.get("gdt", table, fallback="").replace("\\", "/").lower()
        expected = parser.get("gdt", f"{table}_sha256", fallback="").lower()
        if configured != f"data/{table}.gdt":
            raise ClientRegistryError(f"Current registry GDT path differs: {table}")
        if _sha256(reader.read(f"gdrs/{table}.gdt")) != expected:
            raise ClientRegistryError(f"Current registry GDT hash differs: {table}")
    for table in (ACTIVITY_TABLE, ITEM_CATEGORY_TABLE):
        configured = parser.get("gdt", table, fallback="").replace(
            "\\", "/"
        ).lower()
        expected = parser.get("gdt", f"{table}_sha256", fallback="").lower()
        if configured or expected:
            if configured != f"data/{table}.gdt" or not re.fullmatch(
                r"[0-9a-f]{64}", expected
            ):
                raise ClientRegistryError(
                    f"Current registry GDT declaration differs: {table}"
                )
            resource = f"gdrs/{table}.gdt"
            if not reader.has(resource):
                raise ClientRegistryError(
                    f"Current registry GDT resource is missing: {table}"
                )
            if _sha256(reader.read(resource)) != expected:
                raise ClientRegistryError(
                    f"Current registry GDT hash differs: {table}"
                )
        elif reader.has(f"gdrs/{table}.gdt"):
            raise ClientRegistryError(
                f"Current registry has an undeclared GDT resource: {table}"
            )
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


def _contains_utf16_ascii_path(data: bytes, expected: str) -> bool:
    """Find a normalized NUL-terminated ASCII path in an aligned UTF-16 field."""
    expected = expected.replace("/", "\\").lower()
    run = bytearray()
    for offset in range(0, len(data) - 1, 2):
        low, high = data[offset], data[offset + 1]
        if high == 0 and 0x20 <= low <= 0x7E:
            run.append(low)
            continue
        if low == 0 and high == 0 and run:
            if run.decode("ascii").replace("/", "\\").lower() == expected:
                return True
        run.clear()
    return False


def _make_activity_row(template: bytes, grid_id: int, machine_id: int) -> bytes:
    row = bytearray(template)
    row[:] = bytes(len(row))
    struct.pack_into("<I", row, 0, grid_id)
    struct.pack_into("<I", row, 5, machine_id)
    # Native activity rows store the currency as a little-endian u32 at +9.
    # Type 0 is GP; +13..+14 are observed reserved zero bytes.
    struct.pack_into("<I", row, 9, 0)
    struct.pack_into("<I", row, 15, MACHINE_PRICE_GP)
    _put_wide(
        row,
        ACTIVITY_SPRITE_OFFSET,
        ACTIVITY_SPRITE_CAPACITY,
        COLLECTION_SPRITE,
    )
    return bytes(row)


def _normalize_unit_items(unit_items) -> tuple[dict, ...]:
    normalized = []
    category_ids = set()
    unit_ids = set()
    for supplied in unit_items:
        if not isinstance(supplied, dict):
            raise ClientRegistryError("Unit item metadata must use objects")
        unit_id = supplied.get("unit_id")
        category_id = supplied.get("category_id")
        name = supplied.get("name")
        created = supplied.get("created_by_collection")
        if type(unit_id) is not int or not 0 < unit_id <= 0x7FFFFFFF:
            raise ClientRegistryError("Unit item has an invalid unit_id")
        if type(category_id) is not int or not 0 < category_id < 10_000_000:
            raise ClientRegistryError("Unit item has an invalid category_id")
        if type(created) is not bool:
            raise ClientRegistryError(
                "Unit item created_by_collection must be a boolean"
            )
        if created and not (
            COLLECTION_CATEGORY_MIN <= category_id <= COLLECTION_CATEGORY_MAX
        ):
            raise ClientRegistryError(
                "Collection-created category_id is outside 1180000..1199999"
            )
        if not isinstance(name, str) or not name or "\0" in name:
            raise ClientRegistryError("Unit item has an invalid name")
        encoded_name = (name + "\0").encode("utf-16-le")
        if len(encoded_name) > 100:
            raise ClientRegistryError(
                f"Unit item name exceeds the native field: {category_id}"
            )
        if category_id in category_ids:
            raise ClientRegistryError(
                f"Unit item category_id is duplicated: {category_id}"
            )
        if unit_id in unit_ids:
            raise ClientRegistryError(f"Unit item unit_id is duplicated: {unit_id}")
        category_ids.add(category_id)
        unit_ids.add(unit_id)
        normalized.append(
            {
                "unit_id": unit_id,
                "category_id": category_id,
                "name": name,
                "created_by_collection": created,
            }
        )
    return tuple(sorted(normalized, key=lambda value: value["category_id"]))


def _make_item_category_row(template: bytes, item: dict) -> bytes:
    row = bytearray(template)
    struct.pack_into("<I", row, 0, item["category_id"])
    row[4] = 0
    _put_wide(row, 5, 100, item["name"])
    struct.pack_into("<I", row, 105, item["unit_id"])
    row[109] = 0
    # The current client has one u32 field here.  Stock template 1140005
    # stores 1, while server Option2=2 is server-only and has no corresponding
    # bytes before the sprite path begins at +114.
    struct.pack_into("<I", row, 110, 1)
    return bytes(row)


def _validate_table(table: str, data: bytes) -> tuple[list[bytes], int]:
    size, rows, extra = decode_gdt(data)
    if size != TABLE_SIZES[table]:
        raise ClientRegistryError(
            f"Unexpected {table}.gdt row size: {size}, expected {TABLE_SIZES[table]}"
        )
    if table == ITEM_CATEGORY_TABLE:
        ids = [struct.unpack_from("<I", row)[0] for row in rows]
        if (
            any(not 0 < value < 10_000_000 for value in ids)
            or len(ids) != len(set(ids))
        ):
            raise ClientRegistryError("0031.gdt has invalid or duplicate records")
        return rows, extra
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
    unit_items=(),
    machine_id: int = 990001,
    cover,
    output,
    enabled: bool = True,
) -> dict:
    """Build a complete shared capsule registry.

    ``current_registry`` is an optional prior compiler output.  When supplied,
    all unrelated rows and generated 60000..65535 resources are retained.
    ``source_units`` remains in the public API for the owner tool, but activity
    machines have no client-side preview list and therefore ignore it.
    ``unit_items`` describes stable native gift-item categories.  Existing
    native rows are verified; collection-created rows are added from the
    shipped 1140005 permanent Lv1 unit-item template and are never implicitly
    removed.
    """
    if type(machine_id) is not int or not 0 < machine_id <= 0x7FFFFFFF:
        raise ClientRegistryError("Machine ID must be a positive SQL/client INT")
    normalized_items = _normalize_unit_items(unit_items)
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
    source_table_hashes: dict[str, str] = {}
    for table in TABLE_NAMES:
        resource = f"gdrs/{table}.gdt"
        table_reader = (
            source
            if source.has(resource)
            else stock
            if table in (ACTIVITY_TABLE, ITEM_CATEGORY_TABLE)
            else None
        )
        if table_reader is None:
            raise ClientRegistryError(f"Current registry GDT is missing: {table}")
        raw = table_reader.read(resource)
        raw_tables[table] = raw
        source_table_hashes[table] = _sha256(raw)
        decoded[table] = _validate_table(table, raw)

    fixed_rows, fixed_extra = decoded["0033"]
    activity_rows, activity_extra = decoded[ACTIVITY_TABLE]
    category_rows, category_extra = decoded[ITEM_CATEGORY_TABLE]
    if not fixed_rows:
        raise ClientRegistryError("Stock 0033.gdt has no row to use as a native template")
    if not activity_rows:
        raise ClientRegistryError("Stock 0099.gdt has no native activity row")

    categories_by_id = {
        struct.unpack_from("<I", row, 0)[0]: index
        for index, row in enumerate(category_rows)
    }
    stock_category_rows, _ = _validate_table(
        ITEM_CATEGORY_TABLE,
        stock.read(f"gdrs/{ITEM_CATEGORY_TABLE}.gdt"),
    )
    category_template = next(
        (
            row
            for row in stock_category_rows
            if struct.unpack_from("<I", row, 0)[0]
            == ITEM_CATEGORY_TEMPLATE_ID
        ),
        None,
    )
    if category_template is None:
        raise ClientRegistryError(
            f"Stock 0031.gdt has no {ITEM_CATEGORY_TEMPLATE_ID} unit-item template"
        )
    if (
        struct.unpack_from("<I", category_template, 105)[0] != 10001
        or category_template[109] != 0
        or struct.unpack_from("<I", category_template, 110)[0] != 1
    ):
        raise ClientRegistryError("0031.gdt unit-item template layout differs")
    created_categories = []
    verified_native_categories = []
    for item in normalized_items:
        existing_index = categories_by_id.get(item["category_id"])
        if existing_index is None:
            if not item["created_by_collection"]:
                raise ClientRegistryError(
                    "Native unit item category is missing from 0031.gdt: "
                    f"{item['category_id']}"
                )
            category_rows.append(_make_item_category_row(category_template, item))
            categories_by_id[item["category_id"]] = len(category_rows) - 1
            created_categories.append(item["category_id"])
            continue
        existing = category_rows[existing_index]
        existing_unit = struct.unpack_from("<I", existing, 105)[0]
        if (
            existing_unit != item["unit_id"]
            or existing[109] != 0
            or struct.unpack_from("<I", existing, 110)[0] != 1
        ):
            raise ClientRegistryError(
                "Unit item category conflicts with 0031.gdt: "
                f"{item['category_id']}"
            )
        if item["created_by_collection"]:
            category_rows[existing_index] = _make_item_category_row(
                category_template, item
            )
            created_categories.append(item["category_id"])
        else:
            verified_native_categories.append(item["category_id"])

    legacy_matching = [
        index
        for index, row in enumerate(fixed_rows)
        if struct.unpack_from("<I", row, 5)[0] == machine_id
    ]
    if len(legacy_matching) > 1:
        raise ClientRegistryError("Machine ID appears more than once in 0033.gdt")
    legacy_index = (
        legacy_matching[0]
        if legacy_matching
        and _owned_legacy_machine(fixed_rows[legacy_matching[0]], machine_id)
        else None
    )
    if legacy_matching and legacy_index is None:
        raise ClientRegistryError("Machine ID is occupied by another capsule definition")

    activity_matching = [
        index
        for index, row in enumerate(activity_rows)
        if struct.unpack_from("<I", row, 5)[0] == machine_id
    ]
    if len(activity_matching) > 1:
        raise ClientRegistryError("Machine ID appears more than once in 0099.gdt")
    owned_index = (
        activity_matching[0]
        if activity_matching
        and _owned_activity_machine(
            activity_rows[activity_matching[0]], machine_id
        )
        else None
    )
    if activity_matching and owned_index is None:
        raise ClientRegistryError(
            "Machine ID is occupied by another activity capsule definition"
        )
    previously_owned = owned_index is not None
    owned_resources = legacy_index is not None or previously_owned

    reserved_paths = {
        COLLECTION_TEXTURE.replace("\\", "/"),
        *(sprite.replace("\\", "/") for sprite in LEGACY_ORDINARY_SPRITES),
    }
    reserved_sprites = {
        sprite.replace("\\", "/").lower()
        for sprite in LEGACY_ORDINARY_SPRITES
    }
    for index, row in enumerate(fixed_rows):
        if index == legacy_index:
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
    for index, row in enumerate(activity_rows):
        if index == owned_index:
            continue
        referenced = _activity_sprite(row).replace("\\", "/").lower()
        if referenced in reserved_sprites:
            raise ClientRegistryError(
                "Reserved collection sprite is referenced by another activity "
                f"machine: {referenced}"
            )

    resources: dict[str, bytes] = {}
    if current_path:
        for name in source.entries:
            if _resource_is_generated(name):
                resources[name] = source.read(name)
        for name, raw in resources.items():
            if (
                name.endswith(".spr")
                and name.lower() not in reserved_sprites
                and _contains_utf16_ascii_path(raw, COLLECTION_TEXTURE)
            ):
                raise ClientRegistryError(
                    "Reserved collection texture is referenced by another "
                    f"generated sprite: {name}"
                )

    action = "unchanged"
    if legacy_index is not None:
        fixed_rows.pop(legacy_index)
        for index, row in enumerate(fixed_rows):
            mutable = bytearray(row)
            struct.pack_into("<I", mutable, 0, TABLE_BASES["0033"] + index)
            fixed_rows[index] = bytes(mutable)

    if enabled:
        if not owned_resources:
            for path in sorted(reserved_paths):
                if path in resources or stock.has(path):
                    raise ClientRegistryError(
                        f"Reserved collection resource is occupied: {path}"
                    )
        if owned_index is None:
            owned_index = len(activity_rows)
            activity_rows.append(
                _make_activity_row(
                    activity_rows[0],
                    TABLE_BASES[ACTIVITY_TABLE] + owned_index,
                    machine_id,
                )
            )
            action = "added"
        else:
            grid_id = TABLE_BASES[ACTIVITY_TABLE] + owned_index
            replacement = _make_activity_row(
                activity_rows[0], grid_id, machine_id
            )
            action = (
                "unchanged"
                if activity_rows[owned_index] == replacement
                and legacy_index is None
                else "updated"
            )
            activity_rows[owned_index] = replacement
        texture, uv, cover_size = _card_texture(cover_path)
        resources[COLLECTION_TEXTURE.replace("\\", "/")] = texture
        sprite = _standalone_sprite(
            COLLECTION_TEXTURE,
            cover_size[0],
            cover_size[1],
            uv,
        )
        resources[COLLECTION_SPRITE.replace("\\", "/")] = sprite
        if owned_resources:
            resources.pop(
                LEGACY_ORDINARY_SPRITES[1].replace("\\", "/"), None
            )
    else:
        if owned_index is not None:
            activity_rows.pop(owned_index)
            for index, row in enumerate(activity_rows):
                mutable = bytearray(row)
                struct.pack_into(
                    "<I",
                    mutable,
                    0,
                    TABLE_BASES[ACTIVITY_TABLE] + index,
                )
                activity_rows[index] = bytes(mutable)
        if owned_resources:
            action = "removed"
            for path in (
                COLLECTION_TEXTURE.replace("\\", "/"),
                *(
                    sprite.replace("\\", "/")
                    for sprite in LEGACY_ORDINARY_SPRITES
                ),
            ):
                resources.pop(path, None)

    raw_tables["0033"] = encode_gdt(TABLE_SIZES["0033"], fixed_rows, fixed_extra)
    raw_tables[ACTIVITY_TABLE] = encode_gdt(
        TABLE_SIZES[ACTIVITY_TABLE], activity_rows, activity_extra
    )
    raw_tables[ITEM_CATEGORY_TABLE] = encode_gdt(
        TABLE_SIZES[ITEM_CATEGORY_TABLE], category_rows, category_extra
    )
    for table, raw in raw_tables.items():
        resources[f"gdrs/{table}.gdt"] = raw

    ui_resource = None
    if current_path and source.has(CAPSULE_UI_RESOURCE):
        ui_resource = source.read(CAPSULE_UI_RESOURCE)
        resources[CAPSULE_UI_RESOURCE] = ui_resource
        ui_receipt = {
            "resource": CAPSULE_UI_RESOURCE,
            "action": "preserved",
            "sha256": _sha256(ui_resource),
        }
    else:
        ui_receipt = {
            "resource": CAPSULE_UI_RESOURCE,
            "action": "absent",
        }

    counts = {
        "fixed": len(fixed_rows),
        "coin": len(decoded["0101"][0]),
        "custom": len(decoded["0154"][0]),
        "candidates": len(decoded["0155"][0]),
        "activity": len(activity_rows),
        "item_categories": len(category_rows),
    }
    source_identity = {
        "client_index_sha256": stock.index_sha256,
        "current_registry_index_sha256": source.index_sha256 if current_path else None,
        "source_gdt_sha256": {
            table: source_table_hashes[table] for table in TABLE_NAMES
        },
    }
    semantic = {
        "builder_version": BUILDER_VERSION,
        "machine_id": machine_id,
        "price_gp": MACHINE_PRICE_GP,
        "enabled": enabled,
        "placement_kind": "activity",
        "unit_items": list(normalized_items),
        "resources": {
            name: _sha256(raw) for name, raw in sorted(resources.items())
        },
    }
    revision = _sha256(_json_bytes(semantic))
    zdx, zpk, overlay_index = build_archive(resources)

    files: dict[str, bytes] = {
        "overlay.zdx": zdx,
        "overlay.zpk": zpk,
    }
    if ui_resource is not None:
        files["ui/1161.uir"] = ui_resource
    for table, raw in raw_tables.items():
        files[f"data/{table}.gdt"] = raw
    if enabled:
        files[f"assets/{COLLECTION_TEXTURE.replace(chr(92), '/')}"] = resources[
            COLLECTION_TEXTURE.replace("\\", "/")
        ]
        normalized = COLLECTION_SPRITE.replace("\\", "/")
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
    for table in TABLE_NAMES:
        runtime_lines += [
            f"{table}=data\\{table}.gdt",
            f"{table}_sha256={file_hashes[f'data/{table}.gdt']}",
        ]
    runtime_lines += [
        "",
        "[ui]",
    ]
    if ui_resource is not None:
        runtime_lines += [
            "1161=ui\\1161.uir",
            f"1161_sha256={file_hashes['ui/1161.uir']}",
        ]
    runtime_lines += [
        "",
        "[counts]",
        *(f"{key}={value}" for key, value in counts.items()),
        "",
    ]
    files["runtime.ini"] = "\n".join(runtime_lines).encode("ascii")

    if enabled:
        native_position = int(owned_index) + 1
        placement = {
            "machine_id": machine_id,
            "placement_kind": "activity",
            "table": ACTIVITY_TABLE,
            "grid_type": ACTIVITY_GRID_TYPE,
            "grid_id": TABLE_BASES[ACTIVITY_TABLE] + int(owned_index),
            "native_position": native_position,
            "page": (native_position - 1) // 8 + 1,
            "page_slot": (native_position - 1) % 8 + 1,
            "preview_only_unit_ids": [],
        }
    else:
        placement = None
    manifest = {
        "schema_version": 1,
        "builder_version": BUILDER_VERSION,
        "catalog_revision": revision,
        "action": action,
        "placement_kind": "activity",
        "machine": {
            "id": machine_id,
            "enabled": enabled,
            "price_gp": MACHINE_PRICE_GP,
            "kind": "activity",
            "dynamic_server_pool": True,
        },
        "placement": placement,
        "counts": counts,
        "unit_items": {
            "items": list(normalized_items),
            "created_category_ids": created_categories,
            "verified_native_category_ids": verified_native_categories,
            "retention": "append_or_update_only",
        },
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
        "placement_kind": "activity",
        "counts": counts,
        "placement": placement,
        "manifest_sha256": _sha256(files["manifest.json"]),
    }
