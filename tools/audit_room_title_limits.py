"""Read-only audit of native room-title bounds in the supported SDGO binaries.

No game execution, process attach, patching, or third-party Python packages.
The pinned hashes make this evidence version-specific, not a patch recipe.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct


TARGETS = {
    "client": {
        "sha256": "e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92",
        "anchors": [
            ("mission_setting_title_bound", 0x0084C626, "80 f9 14 76 0d", "cmp cl,20; jbe"),
            ("mission_setting_title_clamp", 0x0084C62B, "c6 85 3f ff ff ff 14", "set byte length to 20"),
            ("mission_setting_packet_size", 0x0084C666, "6a 3e", "push 62-byte packet size"),
            ("pvp_make_room_title_copy", 0x008D43FE, "6a 14 50", "push 20; push source"),
            ("pvp_make_room_packet_size", 0x008D471E, "6a 53", "push 83-byte packet size"),
            ("mission_make_room_title_bound", 0x009EE32B, "80 f9 14 76 02 b1 14", "cmp cl,20; jbe; mov cl,20"),
            ("mission_make_room_title_copy", 0x009EE389, "6a 14", "push 20"),
            ("mission_make_room_packet_size", 0x009EE7AA, "6a 53", "push 83-byte packet size"),
        ],
    },
    "battle": {
        "sha256": "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4",
        "anchors": [
            ("lobby_create_title_length_field", 0x004FDED4, "88 50 53", "mov byte ptr [eax+0x53],dl"),
            ("lobby_create_title_copy", 0x004FDEE6, "6a 14", "push 20"),
            ("battle_rename_title_length_field", 0x004DCCE3, "88 41 79", "mov byte ptr [ecx+0x79],al"),
            ("battle_rename_title_copy", 0x004DCCE6, "6a 14", "push 20"),
            ("battle_setting_title_copy", 0x004BF190, "6a 14", "push 20"),
            ("battle_room_info_title_copy", 0x004F611D, "6a 14", "push 20"),
        ],
    },
}


def va_to_offset(data: bytes, va: int, size: int) -> int:
    """Map a file-backed VA through the PE section table; reject virtual padding."""
    if data[:2] != b"MZ":
        raise ValueError("not an MZ/PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe : pe + 4] != b"PE\0\0":
        raise ValueError("PE signature missing")
    machine, sections = struct.unpack_from("<HH", data, pe + 4)
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    if machine != 0x14C or struct.unpack_from("<H", data, optional)[0] != 0x10B:
        raise ValueError("expected x86 PE32")
    base = struct.unpack_from("<I", data, optional + 28)[0]
    rva = va - base
    table = optional + optional_size
    for index in range(sections):
        entry = table + 40 * index
        virtual_size, virtual_address, raw_size, raw_offset = struct.unpack_from("<IIII", data, entry + 8)
        if virtual_address <= rva < virtual_address + max(virtual_size, raw_size):
            delta = rva - virtual_address
            offset = raw_offset + delta
            if delta + size > raw_size or offset + size > len(data):
                raise ValueError("anchor is not fully file-backed")
            return offset
    raise ValueError(f"unmapped VA {va:#x}")


def audit(path: Path, role: str) -> dict:
    data = path.read_bytes()
    definition = TARGETS[role]
    actual_hash = hashlib.sha256(data).hexdigest()
    result = {
        "role": role,
        "file_name": path.name,
        "sha256": actual_hash,
        "expected_sha256": definition["sha256"],
        "identity_matches": actual_hash == definition["sha256"],
        "anchors": [],
    }
    if not result["identity_matches"]:
        result["status"] = "unsupported_binary"
        return result
    for name, va, expected_hex, interpretation in definition["anchors"]:
        expected = bytes.fromhex(expected_hex)
        offset = va_to_offset(data, va, len(expected))
        actual = data[offset : offset + len(expected)]
        result["anchors"].append({
            "name": name,
            "va": f"0x{va:08X}",
            "file_offset": offset,
            "expected_hex": expected.hex(" "),
            "actual_hex": actual.hex(" "),
            "matches": actual == expected,
            "interpretation": interpretation,
        })
    result["status"] = "passed" if all(a["matches"] for a in result["anchors"]) else "failed"
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--client", type=Path, required=True, help="GOnline.exe to read")
    parser.add_argument("--battle", type=Path, required=True, help="BattleServer.exe to read")
    parser.add_argument("--out", type=Path, help="new JSON report (existing files are refused)")
    args = parser.parse_args()
    report = {
        "scope": "static file bytes only; no runtime or gameplay acceptance",
        "targets": [audit(args.client, "client"), audit(args.battle, "battle")],
    }
    report["status"] = "passed" if all(t["status"] == "passed" for t in report["targets"]) else "failed"
    output = json.dumps(report, ensure_ascii=False, indent=2) + "\n"
    if args.out:
        with args.out.open("x", encoding="utf-8", newline="\n") as stream:
            stream.write(output)
        print(f"{report['status']}: {sum(len(t['anchors']) for t in report['targets'])} native anchors; {args.out}")
    else:
        print(output, end="")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
