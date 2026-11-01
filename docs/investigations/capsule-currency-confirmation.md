# Activity currency: list and confirmation are separate consumers

Artifact: `GOnline-54BE1568-00C97000`, SHA-256
`e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.
These are static native-code observations, not a runtime trace or purchase test.

| Function | Observed role |
| --- | --- |
| `0x007A69E0` | Builds reversed, padded activity display records from GridData type `0x3C`; vector at `this+0x1A4/1A8/1AC`, page count `+0x1DC`, stride `0xB3`. |
| `0x0079F540` | Reads currency at display record `+9`, price at `+15`. |
| `0x007A6FB0` | Takes only MachineID (`+5`) from that display copy and requests machine type 2. |
| `0x0079D180`, `0x00790680`, `0x0078C870` | Build the type-2 purchase object by MachineID; display currency and record pointer are not forwarded. |
| `0x00795BF0` | Purchase object's Init slot (vtable `0x00E8C67C`); independently enumerates GridData type `0x3C` and matches MachineID. Reads that registered row's `+9` for currency and `+15` for price. Type 2 uses the token wallet (`player+0x590`); Type 0 uses the GP wallet (`player+0x63C`). |

Private local decompilation: `decompiled/gonline/chunk_019.c`, including confirmation
record selection near lines 7949–7976 and the two currency branches thereafter.
The original display-vector-only fix could not change the confirmation branch.

Read-only configuration comparison found one Type/Price mismatch among 94 deployed
activity rows: Grid 145093 / Machine 9301 has client Type 0 / Price 99, while the
local server configuration has Type 2 / Price 99. Other 93 rows match. No account
state or SQL was written. Private decoded rows and SQL results are not distributed.

The revised hook validates the whole display vector and the unique 9301/99 row,
then resolves Grid 145093 with verified native `grid.data`/`grid.find` APIs for type
`0x3C`. All 179 bytes must equal the display copy. Only then are the currency bytes
in both client-memory records changed from 0 to 2. No global API is hooked, disk
resource replaced, wallet value assigned, or server charging path changed.

Automatic evidence: actual DLL detour execution with the deployed decoded 94 rows
and mocked native lookup/list objects, byte-difference checks, idempotency, foreign
lookup rejection and unexpected-price rejection. This does not execute the game's
confirmation controls; visual acceptance and any real purchase remain manual.

Local installation readback (2026-11-01): the stopped primary client received the
1.0.1 script-free package through Manager's standard import API. The Fix archive
reports enabled/registered/version 1.0.1. All installed Mod file hashes match the
package, all 192 existing symbol sections retain their values, and only
`capsule.activity.load_items` was added. Framework, global settings, shared capsule
runtime/overlay/table and the localization DLL retained their pre-import hashes.
No new client session or visual result is claimed by this installation receipt.
