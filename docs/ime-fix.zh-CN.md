# 输入法修复

修复用输入法（中文、日文、韩文）打字的问题：客户端有一处把输入法提交的字节数当成了字符数。
English: [ime-fix.md](ime-fix.md)

| mod | 运行在 | 负责 |
|---|---|---|
| `ime_fix` | 客户端 | 整个功能 |

## 使用

无需操作，安装客户端后即生效。关闭：在 `sdgo\sdgo.ini` 的 `[mods]` 下写 `ime_fix=0`。

## 设置

`sdgo\mods\ime_fix\mod.ini`，游戏启动时读取：

| 键 | 默认值 | 含义 |
|---|---|---|
| `Trace` | 0 | 1：在这一处每次提交文字时，把长度写进日志 |

## 日志与排错

```
ime_fix: loaded IME commit fix 1.0.0
ime_fix: GCS_RESULTSTR 4 bytes -> 2 chars (hkl 08040804)     （Trace=1 时）
```

| 日志 | 含义 |
|---|---|
| `loaded IME commit fix ...` | 已生效 |
| `GCS_RESULTSTR <n> bytes -> <m> chars` | `Trace=1`：一次提交的文字，以及游戏现在收到的字符数 |
| `GCS_RESULTSTR error <n> normalized to 0` | `Trace=1`：输入法返回了错误，游戏收到空字符串 |
| `required symbol ime.result_call ...` | 客户端版本在这一处不同，本 mod 不启用 |

## 原理

客户端有一处用 `ImmGetCompositionStringW` 读取 `GCS_RESULTSTR`，却把返回的字节数当成字符数用。
`ime_fix` 挂钩这个导入函数，只对这一处调用（按返回地址判断，符号 `ime.result_call`）返回字符数，
并在调用方的缓冲区内补上结尾；返回错误时改为空字符串。其它调用方拿到的仍是系统函数的原始结果。
