# Hangar actions in the native unit-management page

The Manager owns installation and preferences. Player actions live in the
game's unit-management page. `hangar_manager` 0.2.0 removes its desktop popup,
Win32 child toolbar, F8 polling and OS confirmation message box.

## Native interface evidence

Supported image: `GOnline-54BE1568-00C97000`, SHA-256
`e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`.
Retained Ghidra pseudocode was checked against this image's PE bytes and x86
instructions with pefile/Capstone. This is static analysis, not observed gameplay.
The current REA route could not start: its configured tool paths were stale;
the existing local Ghidra was found but installed JDKs lack `lib/modules`.
No tool installation or global configuration change was made.

| VA | Interface / checked boundary |
|---|---|
| `00602EA0` | `Cubbyhole::InitDlg`, `ret 0`; loads `uirs/1330.uir`, stores dialog at service+44 and `UnitDeleteBtn` at +4C |
| `00605390` | Native button-click callback, `thiscall(control*)`, `ret 4`; compares original lock/delete/page controls |
| `00C64C80` | Dialog button factory, **six arguments**, `ret 18`; allocates 220 bytes, calls C68F10, copies name through host allocator, appends child to dialog list |
| `00C68F10` | Button constructor, parent at +15C, native type 4 and vtable EF39FC |
| `00C63930` | Named child lookup, `(HostWString24*, type)`, `ret 8`; iterates dialog+23C child list |
| `00C77470` | Text item setter, eleven arguments, `ret 2C`; deep-copies text into host-owned item, obtains font through C60B90 |
| `00C770B0` | Add skin texture, `thiscall(path, mode, offsetX, offsetY)`, `ret 10`; deep-copies the resource name and allocates the skin/sprite through the host |

All new interfaces have separately named `hangar.*` symbols and expected bytes.
The standalone installer merges these sections; the Manager overlay includes a
matching fragment. Other Mods' symbols are preserved.

## Page behavior

- Native host-allocated child buttons sit beside the existing lock/delete buttons.
  Position derives from the original delete button and dialog bounds. Unsupported
  geometry fails closed, with no external-window fallback.
- Duplicate selection, status, nine-row pages, reward choice and two-step
  confirmation replace the right-hand grid's content while viewing the list.
  All controls use the game's native renderer, input dispatch, parent coordinates
  and font. No game resource or executable is repackaged or modified.
- Original child visibility is restored around native refreshes, then captured
  again and hidden. Returning to the grid restores current native visibility.
  Leaving the page invalidates the plan and armed confirmation.
- The native dialog owns its controls. Reinitialization drops old references;
  service/dialog identity checks reject stale attachments.
- The existing authenticated protocol, stable unit IDs, state protection,
  fresh-detail scan and one-request-at-a-time acknowledgement logic remain.
  The second confirmation revalidates the snapshot. There is no delete request
  during preview or the first confirmation.

## Validation boundary

Win32 Release builds and the planner, native adapter and native-page fixtures
pass. The adapter fixture exercises production page events through synthetic
detail replies, both confirmations, exchange and sorting replies. It also checks
that a changed snapshot or closed page cannot execute a stale plan. Page fixtures
check host ownership, bounds, pagination, visibility restoration, reinitialization
and unsupported-layout rejection. All 246 client symbols in this repair branch match the image.

Real-client typography, input placement, full-screen/resolution behavior, reward
delivery and persistence still require manual acceptance. No real unit is sorted
or recycled by automated tests. The deleted Win32 preview capture is not evidence
for this native UI.

## 0.2.1: visible controls that cannot click

The reported screenshot shows the 0.2.0 injected controls, including square-bracket
captions and unrelated red/cyan rectangles. The original fixture called the
service button callback directly and did not test native mouse dispatch.

Current PE bytes and retained decompilation establish two connected omissions:

- `CUIBase` construction at `00C76C80` leaves the skin vector at `+1A4..+1A8`
  empty and enables the debug quad at `+168`, whose random color is at `+1CC`.
  `00C6B630` draws that quad, explaining the different colored backgrounds.
- Native mouse processing at `00C69080` arms a press when `+18C == 1`, then
  dispatches event `101` on release only when `+18C == 2` and the button has
  focus. The update function at `00C6B400` requires at least two/three skin
  entries to enter these states. Merely setting visible `+162` and input `+167`
  cannot make a skinless button clickable.

The repair disables the debug quad on all injected controls and uses the host
skin allocator to load the first three states of `UnitDeleteBtn`. Each new
button owns its own resources; no native vectors, strings or sprites are aliased.
Action geometry matches the template so its skins do not need unsafe scaling.
Captions use the template font/color, centered without square brackets. Labels
remain text-only and input-disabled. Hiding/disabling a control clears pending
press/hover state. Missing or incomplete template skins reject attachment.

`hangar_native_input_test` reads a local client with the exact SHA-256 listed
above, then privately copies only the verified 512-byte mouse snippet (including
its switch table) and 548-byte update snippet. It relocates the table/direct
calls and supplies synthetic hit-test, focus, event, lock and rendering sinks.
No game entry point, imports, renderer or networking starts. The test reproduces
the old empty-skin failure and executes hover/down/up with the production page's
new controls, disabled gates, cancel and outside release. The two snippets are
also individually hash-pinned; no game bytes enter source or packages.

The REA target inventory confirmed the client's identity. Fresh decompilation was
unavailable because the configured Ghidra/JDK paths are missing; conclusions use
the current PE/Capstone instructions and retained decompilation, not a fresh REA
decompiler run. Actual on-screen appearance and live-account operations remain
manual acceptance tasks.
