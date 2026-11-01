# Extended room titles (Beta)

[简体中文](room-titles.zh-CN.md)

An optional feature for mission and PvP room titles: up to 128 Unicode scalars,
bounded to 512 UTF-8 bytes. Use the usual create-room and rename controls.
The client, Agent bridge and Battle provider must all be installed and enabled.
It uses the authenticated game connection and needs no additional ports, account,
database migration, FRP settings or launcher.

| Host | Mod | Default |
|---|---|---|
| Every participating client | `room_titles` | Disabled |
| Every serving Agent | `room_titles_agent` | Disabled |
| Every serving Battle server | `room_titles_server` | Disabled |

Install using the [normal framework installers](setup.md), including
`install_server.ps1 -Role Agent` for Agent folders. With the relevant processes
stopped, set `[mod] enabled=1` in each feature's `sdgo/mods/<id>/mod.ini`.
Existing category, feature and per-mod disable switches still apply.
Owners can distribute the enabled client Mod with their ordinary client package;
players start the game normally. No computer-specific configuration belongs in it.

Open the room-title button in the waiting room to view the complete title in
the native scrollable edit control; cancel after viewing if no change is needed.
Lobby selected-room details and mission rows also expand overflowing static text
on hover. Native edit availability and host authorization remain in force.
Mode 10 creates its initial title automatically; after entering its waiting room,
use the same title dialog. Its native auto-create can negotiate capability on
the create ACK, so visiting another lobby first is unnecessary.

The server confirms full titles before the client permits starting a battle with
an unresolved title change. Failed text remains available for editing and retry.
The original bounded title field remains a character-safe short projection.
Clients without this feature see that short title. An old server still accepts
ordinary exactly representable short names; extended names are blocked with an
explanation instead of silently truncating them.

Updated `coop_ai`, `pvp_ai` and `room_random` read the confirmed full title through
an optional process-local provider. Their existing command grammar and settings
remain in force: `!ai e2 a1 lv4 !rand` composes normally, including after byte 20.
Text alone does not implement additional game rules. Without the provider they
continue to read native titles. See [combined commands](room-commands.md).
Update these consuming Mods with the same full framework package when using
long rules; an older AI/random DLL still has its original 20-byte reader.
Selecting only `room-titles` in the publisher's feature filter includes the
three transport/display Mods, not the optional AI/random gameplay features.

Room numbers in mission Team lobbies and Battle rooms belong to separate pools.
Metadata uses a server-instance epoch, domain, creation generation and revision;
native room storage is never enlarged. Host transfers retain the title, stale
updates are rejected, and deletion clears the record. Team-to-Battle conversion
copies explicit metadata from the verified source host; it does not guess by
matching truncated titles or room numbers.

Disable the three Mods to return to native behavior. Metadata is in memory and
does not survive a Battle process restart. No game EXE or save is patched.

For developers, `mods/room_metadata/common` is a private protocol/state library,
not another installed Mod or a core service. The Agent strips one validated RTMD
trailer before native dispatch, lets the native path stamp the authenticated
pilot, and restores it at forwarding. The Battle provider owns full titles;
clients cache only confirmed snapshots. `include/sdgo/room_titles.h` is the
bounded C ABI for optional gameplay consumers. No allocation crosses DLLs.

Automatic verification covers protocol and Unicode boundaries, native adapters,
permission and lifecycle logic, isolated real-core Agent forwarding, command
composition, default-disabled packaging and installer preservation. The bounded
native fixture executes only a known room-info constructor, not a game session.
Actual mission/PvP play and two-client visual acceptance remain manual.
The exact checklist and final receipts are in [verification](room-titles-verification.md).
