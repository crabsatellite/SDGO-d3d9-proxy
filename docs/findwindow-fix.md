# General input method fix (FindWindowW)

Stops input methods from freezing or crashing the game, whichever input method it is. The
repack's `SDPlus.dll` breaks window lookup for every DLL in the game process; this fix
restores it for everything except the game itself. 中文：[findwindow-fix.zh-CN.md](findwindow-fix.zh-CN.md)

| mod | host | part |
|---|---|---|
| `findwindow_fix` | client | the whole feature |

Independent of [`ime_fix`](ime-fix.md), which fixes how the game reads committed text.

![Using an input method in the lobby](findwindow-fix-ime.png)

## Use

Nothing to do: it is on after the client install. The mod needs the `sdgo\core.dll` of the
same release (it uses `HookSystemExport`); the client installer updates both. Turn it off
with `findwindow_fix=0` under `[mods]` in `sdgo\sdgo.ini`.

## Settings

`sdgo\mods\findwindow_fix\mod.ini`:

| key | default | meaning |
|---|---|---|
| `Trace` | 0 | 1: log the handle returned for every external window lookup (no window names); 0: log only the first successful one |

## Logs and troubleshooting

```
findwindow_fix: hooked system export user32.dll!FindWindowW
findwindow_fix: ready: normal FindWindowW for every caller except GOnline.exe and SDPlus.dll
findwindow_fix: loaded FindWindowW fix 1.0.0
findwindow_fix: window lookup for an external caller: 000105DC
```

| log line | meaning |
|---|---|
| `window lookup for an external caller: ...` | a DLL other than the game looked up a window and got a real handle |
| `core lacks HookSystemExport` | `sdgo\core.dll` is older than the mod; reinstall the whole client package |
| `SDPlus.dll is not loaded; nothing to fix` | this game folder has no `SDPlus.dll`, so window lookup is not broken; the mod stays off |
| `hook system export user32.dll!FindWindowW failed: ...` | another mod already owns that hook |

The fix covers window lookup only. An input method that still freezes the game has a
different cause.

## How it works

`SDPlus.dll` (the repack's localization plugin, imported by its `GOnline.exe`) patches
`user32!FindWindowW` with a routine at `SDPlus.dll+0x3020` that never looks for a window:

* class `_GONLINE_` with title `Gundam Online` returns NULL ("not running");
* any other query returns -1 or +1 (the sign of a string comparison), which is not a window;
* a NULL or atom class name is read as a string and crashes the game.

`GOnline.exe` calls `FindWindowW` once (`0x004914F6`), with exactly that class and title: its
single-instance check. SDPlus's answer lets several clients run at once. Any other code in
the process that looks up a window gets a bogus handle or crashes. Input methods run inside
the game process, so any input method that looks up a window (for example the window of its
own background process) hangs or crashes the game as soon as it is switched on. The case
found and tested in game was iFlytek: its text service looks up its server window
(`iFlyPlatform.exe`), sends its start-up request to the bogus handle and then waits forever
for the reply, on the game's UI thread. Input methods that never look up a window, such as
Microsoft Pinyin, were not affected.

The fix does not check which input method (or other DLL) is calling; it restores the normal
lookup for every caller outside the game and SDPlus.

`findwindow_fix` detours the same export through the core's `HookSystemExport` (MinHook; the
trampoline keeps SDPlus's patch as the previous hook). It records the address ranges of
`GOnline.exe` and `SDPlus.dll` at load and checks each call's return address:

* calls from those two modules go to SDPlus's routine, so the single-instance check is
  unchanged;
* every other call, including DLLs loaded later and generated code, gets
  `FindWindowExW(GetDesktopWindow(), NULL, class, title)`. That is `FindWindowW`'s search:
  top-level windows only (with a NULL parent `FindWindowExW` would also return message-only
  windows), class atoms and NULL arguments with their usual meaning, and the last error left
  unchanged.

SDPlus also patches `MultiByteToWideChar` and `WideCharToMultiByte`, but only to swap code
page 874 (Thai) for another; other code pages pass through, so input methods are not
affected by those.

Tests: `tests/findwindow_fix_test.ps1` (after a build) runs the real core and mod against a
copy of SDPlus's routine on a private desktop, with caller fixtures named `SDPlus.dll` and
`generic_input_method.dll`. It checks that the game's single-instance query still returns
NULL with a game window present, that external lookups (late servers, missing servers,
message-only windows, atoms, NULL class, case) behave like `FindWindowW` and never reach
SDPlus's routine, the last error, the disable switch, the refusal of an old core, and the
rollback of a failed `HookSystemExport` load.
