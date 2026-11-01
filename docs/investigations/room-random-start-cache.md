# Random loadout start-cache synchronization

The reported mission and PvP failures were not command-parsing failures. A live
mission readback showed the selected model in the server room's full unit record,
constructed battle pilot and native start-packet model source. The user still
controlled the previously selected model on the stock client.

The original `room_random` only changed the server's temporary records. In the
supported client, `0x06C3` enters `CServiceWaitRoomMgr::OnPacket` at `0x00A3F329`.
For ordinary type-2 rooms, its cache updates at `0x00A3F452` cover team/colour,
not the complete selected `_UnitData`. Native model replacement under the type-3
branch does not cover ordinary mission/PvP rooms.

The repair sends the stock full-record notifications after all human constructors
succeed, before the original start announcement:

| Opcode | Bytes | Payload at +0x12 | Client copy |
|---|---:|---|---|
| `0x0861` | `0xAB` | Complete `0x99` unit record | `0x00A3F9E2` |
| `0x098B` | `0x2CE` | Complete `0x2BC` ship record for modes 6/7 | `0x00A40845` |

Both carry subject pilot at +6, slot at +10 and native room index at +14. The
existing room broadcaster rewrites recipient routing at +2, skips AI clients and
retains the native Agent delivery path. The server's existing builders at
`0x004B45F2` and `0x004D8A0C` establish these packet layouts. No selection request,
database write, private opcode or additional client Mod is used.

The full ship leader and four inventory UIDs match the chosen unit records.
Partial notification or later start failure restores original records; native
pilot release restores them at teardown. Notices are queued only once per start,
and native rebuilds/respawns outside that start do not redraw or re-notify.
Type-3's alternate announcement field is also captured, updated and restored.

The regression fixture checks all fourteen mode values, cache contents before
start completes, unrelated bytes, exact decks, UID widths, no-command behavior,
partial send failures, late failures and restoration. With explicitly supplied
native files it runs the actual server model-selection branch and client memory
copy instructions in private fixture memory. It omits the handlers' unrelated
logging, UI callbacks and stack cleanup; it does not run entry points, imports,
sockets or authenticated game flows. Full gameplay remains manual acceptance.

Supported native files:

- BattleServer SHA-256: `f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4`.
- GOnline SHA-256: `e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.

The client copy spans and server branch have independent byte hashes in the test.
These addresses and copied instructions are observations of the supplied binaries,
not original source or a claim that every client build is compatible.
