# Capsule list price probe

This is a disabled-by-default, read-only diagnostic Mod for the fixed capsule
machine list. It is evidence tooling, not a price fix.

The Mod hooks `CapsuleMachineUI::RefreshSlotGrid` (`FUN_0079E620`) and the
verified `CUIStaticText::SetText` target (`FUN_00C72100`). The setter hook
forwards every call. It reads a text argument only while the current thread is
inside the original `RefreshSlotGrid`, the control pointer matches one of that
UI's eight verified `PointStatic` controls, the control vtable is the
`CUIStaticText` vtable, and the return address is exactly the price call at
main-module RVA `0x0039EA54`. It never reads arguments from any other UI call.

Captured text stays in the stack-scoped refresh context. It is logged only when
the UI object or zero-based page changes, with a hard lifetime limit of 64
events.

For each slot it attempts to report:

- fixed-capsule grid ID;
- machine ID;
- GP value from the native fixed-machine record;
- `PointStatic0..7` control pointer and visibility;
- `PayTypeStatic0..7` control pointer and visibility;
- the actual price setter argument when every guard and format check passes,
  otherwise `text=unknown`.

The setter ABI and 32-bit MSVC `std::wstring` argument are verified from the
caller and callee. Both inline and heap storage still undergo readable-range,
length, capacity, and terminator validation. Only 1–32 UTF-16 code units with
at least one digit, containing digits, commas, ASCII whitespace, or `GB`/`GP`
letters may be copied into an ASCII log buffer. The probe does not call a widget
getter, scan memory, or read the text color/style value at `+0x174`.

All object, vector, record, control-pointer, and visibility reads first pass
committed-readable-range checks and guarded copies. The vector must contain
exactly `page_count * 8` records of size `0x3FB`, with no more than 4096 records.
An uncertain shape or pointer is reported as `unknown` or refused; no bytes are
dumped and no game memory is written.

## Native evidence

Verified against:

- image: `GOnline.exe`
- PE timestamp: `0x54BE1568`
- image size: `0x00C97000`
- function VA: `0x0079E620`
- function RVA: `0x0039E620`
- price setter VA / RVA: `0x00C72100` / `0x00872100`
- price setter return-site VA / RVA: `0x0079EA54` / `0x0039EA54`
- `CUIStaticText` vtable VA / RVA: `0x00EF3FFC` / `0x00AF3FFC`
- record vector fields: `this+0x64`, `this+0x68`, `this+0x6C`
- current page / page count: `this+0x78`, `this+0x7C`
- record size: `0x3FB`
- machine ID / GP offsets: `+5`, `+15`
- price controls: `this+0x180 + slot*4`
- pay-type controls: `this+0x160 + slot*4`
- control visibility: `control+0x162`

The captured runtime evidence showed valid `GP=1200`, visible price and pay-type
controls, and a verified setter argument of `1200` for all eight slots. The
missing glyph was isolated to the native static-text cache:

- `CUIStaticText::SetText` at `0x00C72100` copies current text (`+0x21C`) to
  backup text (`+0x238`) before copying the incoming text to current;
- ordinary static text (`+0x288 == 0`) does not force dirty byte `+0x28A`;
- rebuild function `0x00C74B00` calls UTF-16 comparison function `0x0048BED0`
  and skips glyph-vector construction when backup/current match, the visible
  count is unchanged, and the dirty byte is clear;
- decoded `uirs/1161.uir` has design-time defaults `10000`, `1200`, then six
  copies of `999`. Slot 1 therefore matched the first runtime `1200` before its
  first glyph vector had been built.

The production repair is the fail-closed bytes patcher in
`tools/capsule_ui_layout.py`. It admits only the evidenced decoded resource
length and source/patched SHA-256, verifies each unique `PointStatic0..7`
record and fixed 64-byte UTF-16LE price field, and clears only the first UTF-16
code unit of each design-time default. This changes eight bytes without moving
records or editing layout/style fields. A second application is idempotent.
The diagnostic Mod remains available for bounded field evidence, but the
production repair does not depend on its hooks.

`+0x174` was also resolved during this investigation. The static-text
constructor initializes it to `0xFFFFFFFF`, initializes `+0x178` to
`0xFF000000`, serializes both as raw DWORD values, and passes them together
with `+0x17C` to the text renderer. `FUN_0078D220` copies a radio-state text
color into price and pay-type controls; it does not replace a render resource.

The proposed hook bytes were read from the installed image:

```text
55 8B EC 6A FF 68 58 AF DC 00 64 A1 00 00 00 00
50 81 EC B4 00 00 00 A1 10 71 FF 00
```

The verified setter bytes are:

```text
55 8B EC 56 57 8B F1 E8 24 35 00 00 6A FF 6A 00
8D BE 1C 02 00 00 57 8D 8E 38 02 00 00
```

`symbol-proposal.ini` is intentionally separate. Integration must merge the
symbol into the matching host symbol file and add the Mod/test to the build.
This directory does not deploy or enable the probe.

## Standalone logic test

The pure test covers page/record bounds, unsafe vector shapes, supported
wstring layout bounds, the restricted price-text whitelist, page-change
deduplication, and the 64-event cap. It can be compiled without the game or Mod
framework:

```powershell
clang++ -std=c++17 -Wall -Wextra -Werror `
  mods/capsule_price_probe/capsule_price_probe_test.cpp `
  -o capsule_price_probe_test.exe
.\capsule_price_probe_test.exe
```
