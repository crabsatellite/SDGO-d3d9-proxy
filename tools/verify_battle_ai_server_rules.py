"""Independent contracts for native SP branches and server bot loadout layout."""
import argparse
import hashlib
import json
from pathlib import Path
import pefile
import capstone as cs

EXPECTED = "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("exe", type=Path)
    parser.add_argument("--agent-exe", type=Path)
    args = parser.parse_args()
    raw = args.exe.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == EXPECTED, "unsupported server identity"
    pe = pefile.PE(data=raw)
    assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
    md = cs.Cs(cs.CS_ARCH_X86, cs.CS_MODE_32)
    md.detail = True

    def instructions(at, length):
        return list(md.disasm(pe.get_data(at - 0x400000, length), at))

    observations = {}
    sites = [
        ("request_gate", 0x4C3ADD, 0x4C3AEA, 0x4C3B2F, 0x328D8, 0x328DC, 0x3C, 0),
        ("accepted_cost", 0x4C3BDD, 0x4C3BEA, 0x4C3C02, 0x328D8, 0x328DC, 0x3C, -10000),
        ("melee_end", 0x4C4661, 0x4C466E, 0x4C4686, 0x32950, 0x32994, 0x20, -20000),
        ("ranged_end", 0x4C8C3A, 0x4C8C48, 0x4C8C60, 0x14F8, 0x1510, 0x20, -20000),
        ("map_clear", 0x4CEFAA, 0x4CEFB7, 0x4CEFCF, 0x33CD0, 0x33CD4, 0x18, -20000),
        ("rush_clear", 0x4E624A, 0x4E6257, 0x4E626F, 0x33CD0, 0x33CD4, 0x18, -20000),
    ]
    for name, site, player, common, handle, pilot, room, cost in sites:
        head = instructions(site, player - site)
        assert len(head) == 3 and [x.mnemonic for x in head] == ["mov", "and", "jne"], name
        assert head[0].operands[1].mem.base == cs.x86.X86_REG_EBP and head[0].operands[1].mem.disp == -handle
        assert head[1].operands[1].imm & 0xFFFFFFFF == 0x80000000
        assert head[2].operands[0].imm == common
        if cost:
            block = instructions(player, common - player)
            assert [x.mnemonic for x in block] == ["push", "push", "mov", "push", "push", "mov", "call"], name
            assert block[0].operands[0].imm == block[1].operands[0].imm == 0
            assert block[2].operands[1].mem.disp == -pilot and block[5].operands[1].mem.disp == -room
            signed_cost=block[4].operands[0].imm & 0xFFFFFFFF
            if signed_cost & 0x80000000:
                signed_cost -= 0x100000000
            assert signed_cost == cost and block[6].operands[0].imm == 0x402AE0
        else:
            block = instructions(player, 15)
            assert block[0].operands[1].mem.disp == -pilot
            assert block[1].mnemonic == "cmp" and block[1].operands[0].mem.disp == 5 and block[1].operands[1].imm == 30000
        observations[name] = {"site": hex(site), "player_block": hex(player), "native_cost": cost}

    # These original terminal paths guard state 3 before reaching the cost;
    # map/rush replay protection is explicitly supplied by the mod decision.
    for at in (0x4C4625, 0x4C8BFE):
        compare = instructions(at, 4)[0]
        assert compare.mnemonic == "cmp" and compare.operands[0].mem.disp == 9 and compare.operands[1].imm == 3
    for at in (0x4858BE, 0x48595D):
        assert instructions(at, 40), "missing original BuildAiPilot data flow"
    # Exact write of source NPC level -> authoritative BattleUnit level +0C.
    assert pe.get_data(0x4858C7 - 0x400000, 12) == bytes.fromhex("8b 45 0c 8b 4d f4 8a 51 19 88 50 0c")
    observations["server_level_write"] = "NPC+19 -> BattleUnit+0C"
    assert pe.get_data(0x4C542C-0x400000,23) == bytes.fromhex("0f b6 85 2f ff ff ff 85 c0 7c 0c 0f b6 8d 2f ff ff ff 83 f9 09 7e 19")
    observations["sp_only_state"] = "0779, native subtype 0..9; FF+zero-victim subtype reserved by this mod"
    if args.agent_exe:
        agent_raw=args.agent_exe.read_bytes()
        assert hashlib.sha256(agent_raw).hexdigest()=="7becced6510c078c48dfff2df1dbd73f6433d312056a862bd161fb4521d2df45", "unsupported relay identity"
        agent=pefile.PE(data=agent_raw)
        assert agent.get_data(0x420875-0x400000,7)==bytes.fromhex("6a 01 68 f0 00 00 00"), "relay must forward complete native0779 size"
        observations["relay_state_forward"]="0779 fixed size F0 preserved; no Agent mod required"
    print(json.dumps({"native_sha256": EXPECTED, "contracts": "pass", "observations": observations}, indent=2))


if __name__ == "__main__":
    main()
