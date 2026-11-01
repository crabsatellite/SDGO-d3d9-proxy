# Room-title native UI evidence

Pinned client:

- `GOnline.exe`
- SHA-256 `e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92`
- Image base `0x00400000`, no relocations.

The address claims below are bounded observations from that exact executable.
`mode10-waitroom-disassembly.txt`, `static-wordskip-mode10-disassembly.txt`,
and `room-details-disassembly.txt` contain the corresponding native bytes.

## Mode 10 and the shared wait-room title bar

- `0x009F5590` creates the single-mission request. At `0x009F5796` it writes
  `0A 02` to packet `+6`, so the request is category 2, mode 10.
- `0x009F5BC0` accepts a room record whose packet mode at `+0x10` is 10.
  The copy at `0x009F5C78` writes the native room id to mission record `+0`;
  `0x009F5C84` copies category to `+4`; `0x009F5C8B` copies mode 10 to `+5`.
- PvP and mission rooms use the same title-bar object and vtable
  `0x00EBCE9C`. The adjacent vtable `0x00EBCF14` is a map-selection component
  and is deliberately not used.
- The shared title bar stores a PvP room pointer at `self+0x108` and a mission
  room pointer at `self+0x10C`. Native code consistently checks `+0x108`
  first, then `+0x10C` (`0x00A45910`, `0x00A459E0`, and `0x00A53CBE`).
- Mission population `0x00A45A70` receives the mission record at event
  `+0x2C`, stores it at `self+0x10C`, and renders it through `0x00A324C0`.
  The mode-10 fixture therefore uses the normal mission waiting-room title
  path.

`native_ui::ReadTitleBarRoom` mirrors the native pointer precedence, validates
the shared vtable and readable room id, and returns the matching legacy title
from PvP `room+0x7A` or mission `room+0x54`. This gives the controller an
evidence-backed domain and native id instead of inferring a battle domain from
the event type.

## Rename entry and 0x089E domain

- The common `TitleChangeBtn` click path is `0x00A53B0D` through
  `0x00A53D46`. It requires waiting-room state `self+0x100 == 2`, rejects only
  mission category 4, and handles control `self+0x7C` with event `0x101`.
  It selects PvP `self+0x108+0x7A` or mission `self+0x10C+0x54`, then opens
  the same title-edit service with constructor `0x00A0DD70`. There is no
  host/leader test in this bounded click path.
- Shared edit init `0x00A1E160` loads `uirs\0882.uir` / `TitleDlg`, finds the
  type-8 `TitleEdit`, stores it at service `+0x50`, and initializes its text.
  The Mod's shared `PvpTitleEdit` init hook widens that edit and restores the
  current metadata title for either domain.
- Confirm `0x00A46A20` reads the edit at service `+0x50`, constructs event
  `0x0BDB` through `0x00A3B540`, and emits it to the common title-bar handler
  `0x00A53670`.
- The native packet constructor `0x0072DE80` builds opcode `0x089E`, puts the
  subtype byte at packet `+6`, and the title at `+8`.
- Battle request `0x00750A50` calls that constructor with subtype 1 at
  `0x00750B73`; mission request `0x00750BC0` calls it with subtype 2 at
  `0x00750CE3`.

The in-room full-title view does not depend on the button fitting 128
characters. `TitleChangeBtn` is the entry; opening it creates the shared edit,
and the init hook puts the full metadata title into that edit. Native anchors
cover both PvP and mission/mode 10. A live non-host click remains manual
acceptance because this investigation did not run the client.

`0x00A46380` compares an incoming title with the owned cache at
`self+0x110`. For a changed title it updates that cache before dispatching the
service request. A rejected retry must therefore restore the confirmed full
title (or the returned legacy title when no extension record exists) to
`self+0x110` before re-entering the native handler.

## Full title in room lists

- PvP lobby renderer `0x008D9CB0` and mission row renderer `0x009EC2A0` are
  hooked after their native renderers. Projection is keyed by the native room
  id rather than row order.
- Selected-room details are hooked after the native renderers at
  `0x008CDC00` (PvP) and `0x008CD760` (mission). They use the verified native
  vector bounds and find the type-3 `RoomNameStatic`.
- Host `CUIStaticText::SetWordSkip` is `0x00C6B3B0`. Its mouse/update/render
  path uses flags at `+0x288..+0x28B` and the measured full size at
  `+0x28C/+0x290` to expand overflowing text while hovered. The Mod enables it
  only on exact static-text vtables. It does not write the same offsets on
  buttons.

This gives mission rows and both selected-room detail panels a native
hover-expanded full title. Button text projection remains useful for the
ordinary title bar and PvP rows, but no claim is made that a fixed-width button
can display every 128-character title without opening the shared edit or the
static detail view.

## Focused verification

Commands:

```powershell
cmake --build build --config Release --target room_titles_native_ui_test room_titles
.\build\Release\room_titles_native_ui_test.exe
```

Result:

```text
room_titles_native_ui_test passed
```

The fixture covers both title domains, a mission record with mode byte 10,
shared title-bar identity and legacy-title recovery, full-title projection,
selected-room vector bounds, exact static vtables, one-time WordSkip
activation, and rejection of an out-of-range details index. The actual Release
`room_titles.dll` also built successfully. No client runtime, server login, or
gameplay was performed here.


The native byte signatures used by the shipping Mod are in `symbols/GOnline-54BE1568-00C97000.ini`; `tools/verify_symbols.py` validates them without starting the client. Local disassembly files above are investigation artifacts and are not bundled game assets.
