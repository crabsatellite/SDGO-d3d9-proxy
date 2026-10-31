# IME input fix

Fixes text typed with an IME (Chinese, Japanese, Korean input) at one place where the client
reads a byte count as a character count. 中文：客户端有一处把输入法提交的字节数当成字符数，
本 mod 修正这一处。

| mod | host | part |
|---|---|---|
| `ime_fix` | client | the whole feature |

## Use

Nothing to do: it is on after the client install. Turn it off with `ime_fix=0` under
`[mods]` in `sdgo\sdgo.ini`.

## Settings

`sdgo\mods\ime_fix\mod.ini`:

| key | default | meaning |
|---|---|---|
| `Trace` | 0 | 1: log every committed IME string length |

## Logs and troubleshooting

| log line | meaning |
|---|---|
| `loaded ime_fix ...` | active |
| `required symbol ime.result_call ...` | this client build differs at that call site; the mod stays off |

## How it works

One call site reads `GCS_RESULTSTR` from `ImmGetCompositionStringW` and then treats the
returned byte count as a character count. `ime_fix` hooks the import and, for that call site
only (checked by return address, symbol `ime.result_call`), returns the length in characters.
