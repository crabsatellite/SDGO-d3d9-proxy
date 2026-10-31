"""Read-only contracts against the exact supported shipped GOnline binary.

These checks are independent of the C++ ABI fixtures: a fixture can otherwise
repeat an incorrect virtual-table index or callee return convention.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import pefile

EXPECTED = "e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92"

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("exe",type=Path)
    args=parser.parse_args()
    raw=args.exe.read_bytes()
    assert hashlib.sha256(raw).hexdigest()==EXPECTED,"unsupported native byte identity"
    pe=pefile.PE(data=raw)
    assert pe.OPTIONAL_HEADER.ImageBase==0x400000
    def bytes_at(va,n):return pe.get_data(va-0x400000,n)
    contracts={}
    for name,table,offset,expected in [
        ("plain_projectile_update",0xE6C804,0x18,0x6504A0),
        ("homing_projectile_update",0xE6C9BC,0x18,0x6521A0),
        ("split_projectile_update",0xE6CAFC,0x18,0x654910),
        ("npc_ranged_ray",0xE6CF74,0xB4,0x65E1F0),
    ]:
        value=struct.unpack("<I",bytes_at(table+offset,4))[0]
        assert value==expected,(name,hex(value),hex(expected))
        contracts[name]={"vtable_byte_offset":offset,"entry":hex(value)}
    for name,address,expected in [
        ("think_ret",0x65814C,b"\xc3"),
        ("dispatch_ret",0x65913B,b"\xc3"),
        ("ray_sret_ret4",0x65E6D4,b"\xc2\x04\x00"),
        ("shot_eax_ret12",0x6B193D,b"\xc2\x0c\x00"),
    ]:
        assert bytes_at(address,len(expected))==expected,name
        contracts[name]=hex(address)
    assert bytes_at(0x658141,2)==b"\xb0\x01","Think returns AL bool"
    # Native ray3 placement constructor: origin at 0..8, direction at C..14.
    assert bytes_at(0xBF8DFD,3)==b"\x89\x50\x0c"
    assert bytes_at(0xBF8E03,3)==b"\x89\x50\x10"
    print(json.dumps({"native_sha256":EXPECTED,"contracts":"pass","observations":contracts},indent=2))

if __name__=="__main__":main()
