# IME input fix

Fixes text typed with an IME (Chinese, Japanese, Korean input) at one place where the client
reads a byte count as a character count.
中文：[ime-fix.zh-CN.md](ime-fix.zh-CN.md)

| mod | host | part |
|---|---|---|
| `ime_fix` | client | the whole feature |

## Use

Nothing to do: it is on after the client install. Turn it off with `ime_fix=0` under
`[mods]` in `sdgo\sdgo.ini`.

## Settings

`sdgo\mods\ime_fix\mod.ini`, read when the game starts:

| key | default | meaning |
|---|---|---|
| `Trace` | 0 | 1: log the length of every string the IME commits at that call site |

## Logs and troubleshooting

```
ime_fix: loaded IME commit fix 1.0.0
ime_fix: GCS_RESULTSTR 4 bytes -> 2 chars (hkl 08040804)     (Trace=1)
```

| log line | meaning |
|---|---|
| `loaded IME commit fix ...` | active |
| `GCS_RESULTSTR <n> bytes -> <m> chars` | `Trace=1`: one committed string, as the game now receives it |
| `GCS_RESULTSTR error <n> normalized to 0` | `Trace=1`: the IME returned an error; the game gets an empty string |
| `required symbol ime.result_call ...` | this client build differs at that call site; the mod stays off |

## How it works

One call site reads `GCS_RESULTSTR` from `ImmGetCompositionStringW` and then treats the
returned byte count as a character count. `ime_fix` hooks the import and, for that call site
only (checked by return address, symbol `ime.result_call`), returns the length in characters,
with the string terminated inside the caller's buffer; an error result becomes an empty
string. Every other caller gets the system function's result unchanged.
