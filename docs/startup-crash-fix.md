# Random crash at game start

Stops the game from crashing now and then right after it starts, on the loading screen
(access violation at `0x008FE610`; the game's own crash log has it since 2024). The game
picks a loading set at random and crashes when that set's unit is missing from the client's
unit data; such a set now shows no unit.
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
startup_crash_fix: loading set <set id> picked: its unit <unit id> is missing, no unit shown
```

| log line | meaning |
|---|---|
| `N loading sets, M with a missing unit` | M of the sets would have crashed the game when picked |
| `loading set ... picked: its unit ... is missing` | this start would have crashed; the screen shows no unit |
| `required symbol gate.show_set_unit ...` | this client build differs there; the mod stays off |

## How it works

While the game starts (`CGameApp::SetState`, `SERVICE_LOGO`), the gate loading screen
(`0x0090C320`) picks one loading set at random (`0x00904180`: grid data of type 5 from id
27101, `srand(timeGetTime())`) and shows its unit (`0x008FE380`, symbol `gate.show_set_unit`).
That function returns at once when the set has no unit (unit id 0). Otherwise it looks the
unit up in the unit data (grid data type 2, `0x0059D2B0`, symbol `grid.find`) and reads the
unit record's name at `+0x264` without checking that the unit was found. The game's own crash
log (`zoa_log\Exp_*.txt`) has this crash since 2024, before any mod.

`startup_crash_fix` hooks `gate.show_set_unit`: when the set's unit is not in the unit data, it
returns the way the function does for a set without a unit.
