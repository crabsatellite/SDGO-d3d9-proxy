"""Check a symbol file against a host executable without running the game.

Usage:
    python tools/verify_symbols.py <GOnline.exe> [symbols/<host_key>.ini]

Applies the same rules as the core: [host] must describe the executable, every
code/site entry must match its expected bytes, every slot must point at its target.
Exits non-zero if anything is rejected.
"""
import configparser
import os
import struct
import sys


def read_pe(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    timestamp = struct.unpack_from("<I", data, pe + 8)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<I", data, opt + 28)[0]
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    sections = []
    for i in range(nsec):
        s = pe + 24 + opt_size + 40 * i
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, s + 8)
        sections.append((vaddr, max(vsize, rawsize), rawptr, rawsize))
    return data, timestamp, image_base, size_of_image, sections


def read_rva(pe, rva, count):
    data, _, _, _, sections = pe
    for vaddr, vsize, rawptr, rawsize in sections:
        if vaddr <= rva < vaddr + vsize:
            off = rva - vaddr
            if off + count > rawsize:
                return None
            return data[rawptr + off: rawptr + off + count]
    return None


def host_key(path, pe):
    stem = os.path.splitext(os.path.basename(path))[0]
    return "%s-%08X-%08X" % (stem, pe[1], pe[3])


def main():
    exe = sys.argv[1]
    pe = read_pe(exe)
    key = host_key(exe, pe)
    ini_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "symbols", key + ".ini")
    cfg = configparser.ConfigParser(interpolation=None, inline_comment_prefixes=None, comment_prefixes=(";", "#"))
    cfg.optionxform = str
    if not cfg.read(ini_path, encoding="ascii"):
        print("no symbol file for", key)
        return 1

    failures = 0
    host = cfg["host"]
    if int(host["timestamp"], 0) != pe[1] or int(host["size_of_image"], 0) != pe[3]:
        print("[host] does not describe", key)
        return 1

    base = pe[2]
    for name in cfg.sections():
        if name == "host":
            continue
        sec = cfg[name]
        kind, rva = sec.get("kind"), int(sec["rva"], 0)
        problem = None
        if kind in ("code", "site", "data") and sec.get("expect"):
            expect = sec["expect"].split()
            got = read_rva(pe, rva, len(expect))
            if got is None:
                problem = "not readable from file"
            else:
                for i, tok in enumerate(expect):
                    if tok != "??" and got[i] != int(tok, 16):
                        problem = "byte +%d is %02X, expected %s" % (i, got[i], tok)
                        break
            if kind in ("code", "site") and len(expect) < 5:
                problem = "needs at least 5 expected bytes"
        elif kind == "slot":
            got = read_rva(pe, rva, 4)
            target = base + int(sec["target_rva"], 0)
            value = struct.unpack("<I", got)[0] if got else None
            if value != target:
                problem = "slot holds %s, expected %08X" % ("?" if value is None else "%08X" % value, target)
        elif kind != "data":
            problem = "unknown kind %r" % kind
        status = "ok " if problem is None else "BAD"
        failures += problem is not None
        print("%s %-28s %-4s VA %08X %s" % (status, name, kind, base + rva, problem or ""))
    print("%d symbols, %d rejected (%s)" % (len(cfg.sections()) - 1, failures, key))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
