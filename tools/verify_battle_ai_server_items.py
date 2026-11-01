#!/usr/bin/env python3
"""Verify the pinned BattleServer item/damage ABI used by pvp_ai.

This is a static check of one shipped BattleServer.exe. It does not start the
server and does not claim that a live client observed the resulting effects.
"""

from __future__ import annotations

import argparse
import configparser
import hashlib
from pathlib import Path
import sys

import pefile
from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86_const import X86_OP_IMM


SERVER_SHA256 = "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4"
TIMESTAMP = 0x548A891D
SIZE_OF_IMAGE = 0x0046C000
PREFERRED_BASE = 0x00400000

SPANS = {
    "waitRoomProcTick": (0x004B2960, 1107, "767671accdb66776d780ca8b6b4bc9a2d25aa0f041a725bab30edde782869d49"),
    "roomLoopLocks": (0x004B2A97, 777, "c6ea52d450334ffec9e771e076f868e3b721bc55b220ab783ef0cc391c34855f"),
    "scheduler": (0x0042FA80, 37, "43fff4b681294ed12e0f972d110a63a3dfdf724ad69fbfa7a860af400d750db9"),
    "damageWrapper": (0x004A0300, 2177, "fb7d0e999629530ce813d8e44c86e467e6369b50bcdef69260e18c30cde2f52b"),
    "directHp": (0x0047C240, 5509, "faa515dcd265c1654c718e6b9833d19cf02a8c4bedf7d0b6bc39a8a5ea2070ed"),
    "useItem": (0x004BFC30, 6444, "837a275cec9850aa89a1397b44b30d218063dac0269758f5fd06b5559c477ad9"),
    "setItem": (0x004D7D70, 156, "541f2b3c7c8bc07f075c32b08819f1d53dc84706e40b4c0ce099ff312ebd0fe9"),
    "eraseHelper": (0x004ADAF0, 86, "4805b145d8267f7aadc3333670991a0d62ad9bd6538b495696d91eda1e83d7e5"),
    "mapInsertLoop": (0x004AF269, 196, "fcf5876f114cc303f43f28d20edabef8aec4faff2e6779c8207aef307b6012c8"),
}

SYMBOLS = {
    "items.waitroom_tick": (0x000B2960, "code"),
    "items.accepted_damage": (0x000A0300, "code"),
    "items.pilot_map_find": (0x00322020, "code"),
    "items.pilot_map_insert": (0x00322180, "code"),
    "items.pilot_map_erase": (0x00322300, "code"),
    "items.lock_room": (0x000AE8B0, "code"),
    "items.unlock_room": (0x000AE9C0, "code"),
    "items.item_table": (0x0042B338, "data"),
    "items.item_lookup": (0x0000F781, "code"),
    "items.set_item": (0x000D7D70, "code"),
    "items.use_item": (0x000BFC30, "code"),
}


class VerificationError(RuntimeError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


class Image:
    def __init__(self, path: Path):
        self.path = path
        self.raw = path.read_bytes()
        self.pe = pefile.PE(data=self.raw, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.disassembler = Cs(CS_ARCH_X86, CS_MODE_32)
        self.disassembler.detail = True

    def read_va(self, va: int, size: int) -> bytes:
        offset = self.pe.get_offset_from_rva(va - self.base)
        data = self.raw[offset : offset + size]
        require(len(data) == size, f"short read at VA 0x{va:08X}")
        return data

    def instructions(self, va: int, size: int):
        return list(self.disassembler.disasm(self.read_va(va, size), va))


def parse_expect(text: str) -> bytes:
    return bytes(int(part, 16) for part in text.split())


def verify_identity(image: Image) -> None:
    digest = hashlib.sha256(image.raw).hexdigest()
    require(digest == SERVER_SHA256, f"server SHA-256 mismatch: {digest}")
    require(image.pe.FILE_HEADER.TimeDateStamp == TIMESTAMP, "PE timestamp mismatch")
    require(image.pe.OPTIONAL_HEADER.SizeOfImage == SIZE_OF_IMAGE, "SizeOfImage mismatch")
    require(image.base == PREFERRED_BASE, "preferred image base mismatch")
    require(image.pe.FILE_HEADER.Machine == 0x14C, "server is not x86 PE32")


def verify_spans(image: Image) -> None:
    for name, (va, size, expected) in SPANS.items():
        actual = hashlib.sha256(image.read_va(va, size)).hexdigest()
        require(actual == expected, f"{name} span hash mismatch: {actual}")


def verify_symbol_file(image: Image, path: Path) -> None:
    parser = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=())
    parser.optionxform = str
    parser.read(path, encoding="utf-8")
    for section, (rva, kind) in SYMBOLS.items():
        require(parser.has_section(section), f"missing [{section}]")
        require(parser.get(section, "kind") == kind, f"[{section}] kind mismatch")
        actual_rva = int(parser.get(section, "rva"), 0)
        require(actual_rva == rva, f"[{section}] RVA mismatch")
        if parser.has_option(section, "expect"):
            expected = parse_expect(parser.get(section, "expect"))
            actual = image.read_va(image.base + rva, len(expected))
            require(actual == expected, f"[{section}] expected bytes mismatch")


def instruction_text(image: Image, va: int, size: int) -> list[str]:
    return [f"{instruction.mnemonic} {instruction.op_str}".strip()
            for instruction in image.instructions(va, size)]


def has_immediate(image: Image, va: int, size: int, value: int) -> bool:
    for instruction in image.instructions(va, size):
        for operand in instruction.operands:
            if operand.type == X86_OP_IMM and (operand.imm & 0xFFFFFFFF) == value:
                return True
    return False


def verify_contracts(image: Image) -> None:
    # Hook ABIs: current function tails are outside the hashed body by up to two
    # bytes, so pin their complete return instructions separately.
    require(image.read_va(0x004A0B80, 3) == b"\xC2\x30\x00",
            "accepted-damage wrapper is no longer ret 0x30")
    require(image.read_va(0x004B2DB2, 1) == b"\xC3",
            "CWaitRoomProc::Tick is no longer ret 0")
    require(image.read_va(0x004C155B, 3) == b"\xC2\x08\x00",
            "use-item handler is no longer ret 8")
    require(image.read_va(0x004D7E09, 3) == b"\xC2\x08\x00",
            "set-item function is no longer ret 8")
    require(image.read_va(0x007220CD, 3) == b"\xC2\x08\x00",
            "map Find is no longer ret 8")
    require(image.read_va(0x007222F5, 3) == b"\xC2\x08\x00",
            "map Insert is no longer ret 8")
    require(image.read_va(0x00722440, 3) == b"\xC2\x04\x00",
            "map Erase is no longer ret 4")
    require(image.read_va(0x004AE9B5, 3) == b"\xC2\x08\x00",
            "LockRoom is no longer ret 8")
    require(image.read_va(0x004AEAB3, 3) == b"\xC2\x04\x00",
            "UnlockRoom is no longer ret 4")

    set_text = instruction_text(image, 0x004D7D70, 156)
    for fragment in (
        "mov ecx, dword ptr [eax + 0xa]",
        "mov dword ptr [edx + 0x64], ecx",
        "movsx eax, word ptr [edx + 8]",
        "mov dword ptr [ecx + 0x68], eax",
        "movsx eax, word ptr [edx + 0xe]",
        "mov dword ptr [ecx + 0x6c], eax",
        "mov dword ptr [edx + 0x70], 0",
        "movsx ecx, word ptr [eax + 0x6c]",
    ):
        require(any(fragment in text for text in set_text),
                f"set-item contract missing: {fragment}")

    use_text = instruction_text(image, 0x004BFC30, 6447)
    for offset in ("0x6e", "0x6f", "0x70", "0x73", "0x67"):
        require(any(f"+ {offset}]" in text for text in use_text),
                f"use-item handler no longer reads item +{offset}")
    for error in range(0x1393, 0x13A0):
        if error in (0x1394, 0x1397):
            continue
        require(has_immediate(image, 0x004BFC30, 6447, error),
                f"use-item handler missing native error 0x{error:X}")
    require(has_immediate(image, 0x004BFC30, 6447, 7),
            "use-item handler missing Feature 7 branch")

    damage_text = instruction_text(image, 0x0047C240, 5512)
    require(any("byte ptr [ecx + 0x38]" in text for text in damage_text),
            "direct HP path no longer consumes compact weapon byte +0x38")
    require(any("byte ptr [eax + 0x39]" in text for text in damage_text),
            "direct HP path no longer consumes compact weapon byte +0x39")


def main() -> int:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser()
    parser.add_argument("server", type=Path, help="path to the pinned BattleServer.exe")
    parser.add_argument(
        "--symbols",
        type=Path,
        default=root / "symbols" / "BattleServer-548A891D-0046C000.ini",
    )
    args = parser.parse_args()

    try:
        image = Image(args.server.resolve())
        verify_identity(image)
        verify_spans(image)
        verify_symbol_file(image, args.symbols.resolve())
        verify_contracts(image)
    except (OSError, pefile.PEFormatError, VerificationError, ValueError) as exc:
        print(f"BATTLE AI SERVER ITEM VERIFY FAILED: {exc}", file=sys.stderr)
        return 1

    print("PASS pinned BattleServer identity")
    print(f"PASS {len(SPANS)} native function/body hashes")
    print(f"PASS {len(SYMBOLS)} items.* symbol RVAs and expected bytes")
    print("PASS damage, map, room-lock, item-set and item-use ABI contracts")
    print("BATTLE AI SERVER ITEM VERIFY PASSED (static/offline)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
