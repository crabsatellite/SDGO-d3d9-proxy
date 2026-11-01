# Native hangar-capacity contract

Static artifact analysis on 2026-11-01. These observations do not establish
visual or multiplayer acceptance. No game/server entry point was executed.

## Supported first-version boundary

The original capacity model is **24 base slots + ExpandSlotCount**. The native
expansion procedures limit added slots to `6 * 196 = 1176`, giving 1200 total.
Allocation also requires the corresponding numbers in
`dbo.tbl_UserUnitSlotNumberList`; changing only the account field is insufficient.

Client `GOnline.exe` SHA-256:
`e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.

- VA `0x6178DE..0x6178F3`: initial count handler reads the reply's unsigned
  16-bit field at `+6`, adds `0x18`, and passes the total to `CreateGroup`.
- Initial creation only happens while the current total is zero. Repeating
  the initial-count reply does not resize an already populated client.
- Pages contain 36 slots; 1200 slots give 34 pages with 12 slots on the last.
- VA `0x5EF3C3..0x5EF3D4`: the grid resource changes after six-slot group 200,
  matching the original 1200-slot expansion boundary. The precise visual
  meaning of the two resources has not been established.
- Slot identities use 16-bit arithmetic/storage. This is a representation
  bound, not evidence that capacities above 1200 are usable.

The first version therefore prepares capacity before initial allocation and
does not hook the client's live `AddSlot` path or create capacity above 1200.

## WorldServer adapter

Host: `WorldServer-548A86E3-0010C000`, SHA-256:
`82d3c4cf09679a431b9bfa639795b148f932642d8774ff66021785115dc53d46`.

| Witness | RVA | Contract |
|---|---:|---|
| `hangar.capacity.get_count` | `0x12BE0` | Original `sp_GetCountUserUnits` wrapper; returns with `ret 8` |
| `hangar.capacity.connection_command` | `0x850F8` | Native connection holds `ICommandText*` at `+8`; unavailable flag at `+0x240` |
| `hangar.capacity.args_layout` | `0x12C68` | Bound argument buffer is 10 bytes |
| `hangar.capacity.args_bindings` | `0x12CA8` | INT input / SMALLINT output / INT output, offsets 0 / 4 / 6 |
| `hangar.capacity.request_args` | `0x4F134` | Native request copies account and passes its leased connection to the wrapper |

The wrapper is `int __thiscall(self, connection, CountArgs*)`; its detour is
`int __fastcall(self, unused_edx, connection, CountArgs*)`. `CountArgs` is packed:
32-bit account, 16-bit raw extra, 32-bit procedure result. The only executable
direct call to its thunk is at VA `0x44F14D`, in handler `0x44F100`.

Success sends the native 8-byte `0x081F` reply, account at `+2`, raw extra at
`+6`. Nonzero return sends native 8-byte `0x0911`, failure flag 1, without
reading the capacity field. Assurance failure can therefore return canonical
database error 1 before calling the original or writing its argument buffer.
Successful assurance calls the original exactly once and retains its result.

The PE supports ASLR. `HIGHLOW` relocations at RVA `0x12BEA` and `0x4F149`
require four-byte operand wildcards; surrounding instructions remain exact.
Layout witnesses are instruction sites, not pointers to numeric constants.

Verify all shared World symbols without launching the server:

```powershell
python tools/verify_symbols.py "D:/Server/WorldServer/WorldServer.exe" symbols/WorldServer-548A86E3-0010C000.ini
```

The Mod creates its own command on the borrowed session, retaining the native
command and bindings. It assumes that this native callback exclusively leases
the session. Ambient COM/SQL transactions and implicit transactions are rejected.
The hook is separate from the rank and new-account hook sites.
