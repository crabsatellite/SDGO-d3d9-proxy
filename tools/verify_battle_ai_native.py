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
        ("swing_entry_ret4",0x65BA4E,b"\xc2\x04\x00"),
        ("swing_update_ret4",0x65D4E7,b"\xc2\x04\x00"),
        ("fire_receipts_ret4",0x6B444C,b"\xc2\x04\x00"),
        ("queue_swing_ret0",0x68CE9A,b"\xc3"),
        ("target_facing_ret0",0x65C152,b"\xc3"),
        ("relative_yaw_ret8",0x65ED9F,b"\xc2\x08\x00"),
        ("dispatch_message_ret12",0x49AA90,b"\xc2\x0c\x00"),
        ("actual_state_promoted_to_plus4",0xBE38C0,b"\x89\x46\x04"),
        ("pending_state_cleared_plus8",0xBE38C3,b"\xc7\x46\x08\x00\x00\x00\x00"),
        ("queue_writes_only_pending",0xBE3926,b"\x89\x41\x08"),
        ("unit_movement_fsm_pointer",0x6851E6,b"\x8b\x8b\xc8\x09\x00\x00"),
        ("player_sp_gate_ret0",0x68D1E6,b"\xc3"),
        ("shared_attack_admission_ret0",0x6B9299,b"\xc3"),
        ("room_title_decoder_assign",0x72DDD9,bytes.fromhex("8b ce e8 10 49 cd ff")),
        ("room_title_start_assign",0xA63212,bytes.fromhex("8b ce e8 d7 f4 99 ff")),
        ("room_title_inline_capacity_and_length",0x6E90A0,bytes.fromhex("8b c1 33 c9 c7 40 14 07 00 00 00 89 48 10")),
    ]:
        assert bytes_at(address,len(expected))==expected,name
        contracts[name]=hex(address)
    # Strict contact caller identity, rather than all general hurt deliveries.
    call=bytes_at(0x6B27E6,5)
    assert call[0]==0xE8 and 0x6B27EB+struct.unpack('<i',call[1:])[0]==0x49AA40
    contracts['melee_contact_return_anchor']='0x6B27EB'
    for call_at in (0x6B42E3,0x6B4317,0x6B43DB):
        instruction=bytes_at(call_at,5)
        assert instruction[0]==0xE8 and call_at+5+struct.unpack('<i',instruction[1:])[0]==0x6B1270
    contracts['primary_vs_chained_receipts']=['kind1@6B42E8','kind7@6B431C','kind8@6B43E0']
    assert bytes_at(0x658141,2)==b"\xb0\x01","Think returns AL bool"
    # Native ray3 placement constructor: origin at 0..8, direction at C..14.
    assert bytes_at(0xBF8DFD,3)==b"\x89\x50\x0c"
    assert bytes_at(0xBF8E03,3)==b"\x89\x50\x10"
    print(json.dumps({"native_sha256":EXPECTED,"contracts":"pass","observations":contracts},indent=2))

if __name__=="__main__":main()
