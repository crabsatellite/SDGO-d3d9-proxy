# Native storage claim-all boundaries

Supported client: `GOnline-54BE1568-00C97000`, SHA-256
`e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.
The investigation reads current PE bytes with pefile/Capstone and compares the
retained Ghidra output for the storage service. It does not claim a fresh
decompiler run or observed gameplay.

Only `uirs/1396.uir` was read from the local ZDX/ZPK using the bounded archive
reader. The 22,932-byte resource has SHA-256
`3cc0cfe36f2105532ed6a12db3fb83a9c49262d0d88c58e478ade7eca710fec2`.
Its dialog is 450×540, TabRadio0 is (35,64,124,23), TabRadio1 is
(158,64,124,23), pagination ends at y=439, the expiry warning is y=448,
and the footer close button is (120,484,220,31). The new action occupies
(291,64,124,23), status y=520. These are native coordinates, not screen pixels.
Radio normal/highlight skins supply the action's three states through the host
allocator. No archive, game asset or native code bytes ship in the addon.

| VA | Native boundary |
|---|---|
| `0057C650` | storage layout init, thiscall, ret 0; dialog at service+70 |
| `0057E8C0` | UI event router, four stack arguments (service,event,second,control), ret 10 |
| `0057F570` | packet router, five stack arguments, command at EBP+14 and packet at EBP+18, ret 14 |
| `00577E40` | native request busy gate, AL return; service+190 must also indicate readiness |
| `00578180` | native item preflight sender: 06CB, header6 + full 64-bit ID, 14 bytes |
| `005795F0` | native successful item preflight: 04E7 -> sends 09CC, header6 + same ID |
| `00575B80` | gift preflight sender: 0906, header6 + note32 + timestamp64, 18 bytes |
| `005791A0` | gift preflight 0722: flags +32..+35 require manual selection; otherwise sends 0630 with note32 and timestamp64 |
| `0057EB00` | item list cache: vector at +180..+184, 28-byte rows; total at +168 |
| `0057E9C0` | gift list cache: vector at +170..+174, 402-byte rows; claimed +156, timestamp +166 |

Native page requests are 09A1/09A2, 10 bytes, with one-based page number at +6;
the original sender casts that page to a byte. Responses 07BD/07BE update the
cache. Successful claims 07E8/0A23 keep the native grant/update handler. Errors
08AF/08B0 (list), 05D9/0814 (preflight), and 08DA/053E (claim) stop the batch.
Gift claim acknowledgments use a sign-extended 32-bit note ID in their 64-bit ID
field, matching the stock comparison. The timestamp remains part of the outgoing
gift identity and the batch target, never inferred from a list slot.

The Mod owns its outstanding preflight replies and consumes them before the
native auto-claim branches. No synthetic entry is added to native pending-name
tracking. This makes cancellation before mutation effective, including late
preflight replies and reconnects. Claims already sent can complete once; no
automatic retry or optimistic rollback is performed. A real Win32 thread timer
drives the batch through the normal message pump, avoiding the hangar Mod's
owned PeekMessage import hook. Default/unsupported UI events remain native.

Fixtures test production page/model/packet adapters against synthetic caches and
server replies, including 12-row pagination, gift claimed flags, one confirmation,
stop before/after claim, choice/capacity refusal, hidden page, changed connection,
unknown/late result and partial initialization. A real thread-timer/message-pump
check runs without a game. The optional replay reads the local client, verifies
the complete file and individual snippet hashes, and executes the native button
mouse/update logic plus all success/error routes in the native storage router.
Only hit testing, focus, drawing, locks and final service-handler boundaries are
synthetic. The main executable entry point, imports, networking and account are
never started. Live display, server grants, item timing and reconnect persistence
remain manual acceptance tasks.
