# Random crash at game start

Stops the game from crashing now and then right after it starts, on the loading screen
(access violation at `0x008FE610`; the game's own crash log has it since 2024). The game
picks a loading set at random and crashes when that set's unit is missing from the client's
unit data; such a set is now replaced by the nearest set whose unit is known.
中文：[startup-crash-fix.zh-CN.md](startup-crash-fix.zh-CN.md)

| mod | host | part |
|---|---|---|
| `startup_crash_fix` | client | the whole feature |

## Use

Nothing to do: it is on after the client install. Turn it off with `startup_crash_fix=0` under
`[mods]` in `sdgo\sdgo.ini`.

## Settings

None.

## Logs and troubleshooting

At the first start-up screen the mod lists the loading sets whose unit is missing (the
numbers depend on the client's data):

```
startup_crash_fix: loading set <set id>: unit <unit id> is not in this client's unit data
startup_crash_fix: <N> loading sets, <M> with a missing unit
startup_crash_fix: loading set <set id> picked: its unit <unit id> is missing, set <set id> (unit <unit id>) shown instead
```

| log line | meaning |
|---|---|
| `N loading sets, M with a missing unit` | M of the sets would have crashed the game when picked |
| `loading set ... picked: its unit ... is missing` | this start would have crashed; the screen shows the named set instead |
| crash at login, `access violation at 008FEE6B` | mod version 1.0.0 (framework release 1.1.0); update the mod |
| `required symbol gate.show_set_unit ...` | this client build differs there; the mod stays off |

On the client this was found on: 9 loading sets, 2 with a missing unit (set 27106: unit 25452,
set 27107: unit 25454).

## How it works

While the game starts (`CGameApp::SetState`, `SERVICE_LOGO`), the gate loading screen
(`0x0090C320`) picks a loading set at random (`0x00904180`: grid data of type 5, one of the
ids from 27102 on, as many as there are sets with a unit; `srand(timeGetTime())`) and keeps
its id at `+0x20E0`. It then walks the loading sets from 27101 and shows each set without a
unit and the picked set (`0x008FE380`, symbol `gate.show_set_unit`). For a set with a unit,
that function looks the unit up in the unit data (grid data type 2, `0x0059D2B0`, symbol
`grid.find`), creates the unit's model at `+0x207C`, sets it up from the rest of the set, and
reads the unit record's name at `+0x264` without checking that the unit was found. The game's
own crash log (`zoa_log\Exp_*.txt`) has this crash since 2024, before any mod.

The screen needs that model: when logging in, `0x008FEE30` plays an animation of the set whose
id is at `+0x20E0` on the model at `+0x207C`, without checking either. So `startup_crash_fix`
hooks `gate.show_set_unit` and replaces a set whose unit is missing by another set, as if the
random pick had landed there: the nearest set before it whose unit is known, else the nearest
one after it. The replacement's id goes to `+0x20E0`. A replacement before it has already been
passed by the walk and is shown at once; one after it is shown by the walk when it gets there.
Either way the screen shows exactly one unit, set up from that set.

Mod version 1.0.0 (framework release 1.1.0) showed no unit instead. That left `+0x207C` unset,
and the game crashed at login (access violation at `0x008FEE6B`) whenever a set with a missing
unit had been picked.
