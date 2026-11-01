# Visible inventory buttons rejected in the live menu

The live client log confirmed `hangar_manager 0.2.2` and `storage_claim 0.1.0`
loaded. The user reported no response in both native pages. The previous native
mouse replay verified skin state transitions, but replaced the service receiver
and did not exercise the menu gate with the real app-state object layout.

Both Mods read `CGameApp+4A4 -> service+0C` unconditionally and rejected any
nonzero value as an active battle. The current PE and retained `0048E6D0`
`CGameApp::SetState` show why this is invalid:

- State 5 allocates only four bytes for the main hub service (`0048EA74` call
  to operator new), then stores 5 in app+4A0 at `0048EAC6`.
- State 6 allocates eight bytes for game/mission selection (`0048EC06`), then
  stores 6 at `0048EC59`.
- State 7 is also eight bytes. Their offset `+0C` is outside the service object;
  arbitrary neighboring heap bytes or unreadable memory caused rejection.
- States 21, 22 and 24 are load, game-main and briefing services with different
  types/sizes. A battle-mode pointer is meaningful only for the matching state.

The repair reads the state discriminant at app+4A0 and accepts known menu states
5–20 and 26–29, subject to the existing native page visibility, session and
inventory checks. Login, transition, loading, active combat, briefing and unknown
states stay blocked. It never dereferences app+4A4 for inventory gating.

Production adapter tests now use state 6 with a non-null invalid service pointer,
nonzero neighbor bytes, and an eight-byte service placed immediately before a
PAGE_NOACCESS guard page. The old guard would reject/fault; the new one accepts
the menu without reading the service. Both adapters reject combat/transition
states. Storage confirmation also remains armed through native 102/103 hover
events; the previous handler reset it on any unrelated event between clicks.

New info logs distinguish native page attachment and accepted button events from
Mod startup. No inventory IDs, account names, credentials or packet payloads are
logged. Real window/receiver acceptance remains separate from native snippet
and synthetic adapter tests; no live sorting, deletion or reward claim is used
to establish this fix.
