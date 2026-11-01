"""Read-only fixture for the exact encrypted 0099.gdt used by this client build."""

from __future__ import annotations

import argparse
import hashlib
import io
import math
from pathlib import Path
import struct


EXPECTED_SHA256 = "380686339eeb3fffca11b3bc49258cabcb97f19ff519325a387d1ef1a62ca4ab"
RECORD_SIZE = 0xB3
RECORD_COUNT = 93


def derive_key_length(record_size: int, record_count: int, extra_value: int, mode: int) -> int:
    combined = extra_value + record_count + record_size
    if mode == 2:
        return combined % 0x41 + 0xC0
    masked = combined & 0x8000000F
    if masked & 0x80000000:
        masked = ((masked - 1) | 0xFFFFFFF0) + 1
    return (masked & 0xFF) + 0x10


def decrypt_chunk(chunk: bytes, key: bytes, mode: int, output_length: int) -> bytes:
    result = bytearray(output_length)
    if mode == 1:
        for index in range(output_length):
            result[index] = chunk[key[index]] ^ key[0]
        return bytes(result)
    xor_counter = 0x300
    for index in range(output_length):
        result[index] = chunk[key[index]] ^ key[xor_counter % len(key)]
        xor_counter += 3
    return bytes(result)


def decrypt_records(data: bytes) -> tuple[int, int, list[bytes]]:
    if len(data) < 14 or data[:2] not in (b"\xf9\xf8", b"\xf8\xf9"):
        raise ValueError("fixture requires an encrypted SDGO GDT")
    mode = 2 if data[:2] == b"\xf9\xf8" else 1
    record_size, record_count, extra_value = struct.unpack_from("<iii", data, 2)
    key_length = derive_key_length(record_size, record_count, extra_value, mode)
    key = data[14:14 + key_length]
    if len(key) != key_length or max(key) >= key_length:
        raise ValueError("invalid GDT permutation key")

    chunks_per_record = math.ceil(record_size / key_length)
    payload = io.BytesIO(data[14 + key_length:])
    records: list[bytes] = []
    for record_index in range(record_count):
        output = bytearray(record_size)
        output_offset = 0
        for _ in range(chunks_per_record):
            chunk = payload.read(key_length)
            if len(chunk) != key_length:
                raise ValueError(f"truncated record {record_index}")
            remaining = min(key_length, record_size - output_offset)
            output[output_offset:output_offset + remaining] = decrypt_chunk(
                chunk, key, mode, remaining
            )
            output_offset += remaining
        records.append(bytes(output))
    return record_size, record_count, records


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("gdt", type=Path, help="path to the extracted encrypted 0099.gdt")
    args = parser.parse_args()

    data = args.gdt.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if digest != EXPECTED_SHA256:
        raise SystemExit(f"0099.gdt identity mismatch: {digest}")

    record_size, record_count, records = decrypt_records(data)
    if (record_size, record_count) != (RECORD_SIZE, RECORD_COUNT):
        raise SystemExit(f"unexpected table shape: {record_size} x {record_count}")

    decoded = []
    for index, record in enumerate(records, 1):
        decoded.append(
            (
                index,
                struct.unpack_from("<I", record, 0)[0],
                struct.unpack_from("<I", record, 5)[0],
                record[9],
                struct.unpack_from("<I", record, 15)[0],
            )
        )

    target = [row for row in decoded if row[2] == 9301]
    if target != [(93, 145093, 9301, 0, 99)]:
        raise SystemExit(f"unexpected 9301 source record: {target!r}")
    if decoded[-3:] != [
        (91, 145091, 9101, 2, 3),
        (92, 145092, 9201, 0, 10000),
        (93, 145093, 9301, 0, 99),
    ]:
        raise SystemExit(f"unexpected source tail: {decoded[-3:]!r}")

    padded_slots = math.ceil(record_count / 8) * 8
    if padded_slots != 96:
        raise SystemExit(f"unexpected UI slot shape: {padded_slots}")
    print(
        "source fixture passed: 93 records -> 12 pages/96 slots; "
        "9301 is unique Type0 Price99"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
