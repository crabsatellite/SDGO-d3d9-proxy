# Server persistence, duplicate counting and panel overflow (0.3.0)

The user reported no visible sorting even after a new login, and a GB preview
of zero. The 0.2.4 display repair does not close either report. Legacy local
`*_Sorting.sql` and `sp_Daily_SortAndDedup.sql` sort the server's
`tbl_UserUnits.UserUnitSlotNumber`. Some legacy scripts also clear a drop box or
delete duplicates; those side effects are not used for sorting.

0.3.0 implements a single-account server transaction. It orders grade, subgrade,
type, acquisition time and stable ID; moves through temporary nonconflicting
slots; verifies the final order and all other inventory columns; and commits a
request receipt. It does not delete units, clear storage or change rewards.
The existing native mail procedure is backed up and wrapped for a reserved,
non-player recipient. Ordinary requests execute the unchanged saved procedure.
Installation/removal verify module hashes and roll back on conflict.

The client constructs the native 0x0891/0x159 request used by 00817360. A replay
executes exactly that 157-byte producer (SHA-256
`41c21e95b7572fb33bdc6a8656ab948bf20f59473a1b31b2e637bef72294aa01`),
with only memset/strncpy replaced, and checks all 345 bytes against the Mod.
No game entry point or network executes. Sender authentication remains the
native server's @v_idx context, outside this client request; the exact native
StoreServer assignment path was not located. The SQL adapter additionally
matches the server-provided account to its pilot name. No client-supplied target
account, SQL credential or management API participates.

No specific native mail completion acknowledgement was established. Consequently
the UI reports submission and requires a new login, never success merely because
the send returned nonzero. The committed SQL receipt is separate evidence.
End-to-end real native transport and persistence remain manual acceptance.

The previous duplicate planner treated the whole normalized 153-byte record as
identity, retaining KeepCopies independently in every state group. Protected or
trained copies also did not count toward that minimum. This can report zero for
a type with many actual copies. The new planner counts by UnitNumber/type and
counts protected copies toward the minimum, selecting only eligible untrained
instances. Full bytes still revalidate each exact instance before either
confirmation or native dismantle. Different untrained paint variants now count
as the same type, matching the old scripts; the preview explicitly says to lock
copies the player intends to retain. No exchange runs during automated tests.

The native 1330.uir dialog covers the screen rather than the right-hand grid.
0.3.0 reserves its bottom page-selector area, moves all custom action rows above
it, bounds preview content to the right, and uses one clipped header-adjacent
status line on the ordinary page. Tests cover the actual 1024x768 geometry and
long result text, not just a synthetic small right-hand panel.

SQL verification uses two newly created fixture databases, then removes exactly
those databases. It covers durable order, stable active UID, locked/deck/trained
content, other owners, GB/storage preservation, idempotent request IDs, rollback
on unexpected content mutation, normal mail forwarding, installation readback,
restoration and reinstallation. It never invokes the new procedure on a real
player. Manager HTTP/session checks, native adapter/planner/page tests and mouse
plus packet-producer replay cover their respective layers separately.
