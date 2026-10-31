# 开游戏随机崩溃修复

修复开游戏时随机崩溃的问题：游戏刚启动、在读取界面偶尔闪退（`0x008FE610` 访问冲突，游戏自己的崩溃日志
从 2024 年起就有）。游戏会随机挑一组读取界面，其中个别组引用的机体在客户端的机体数据里不存在，抽到它就会
闪退；现在这种组会换成最近的一组机体存在的读取界面，正常继续。
English: [startup-crash-fix.md](startup-crash-fix.md)

| mod | 运行在 | 负责 |
|---|---|---|
| `startup_crash_fix` | 客户端 | 整个功能 |

## 使用

无需操作，安装客户端后即生效。关闭：在 `sdgo\sdgo.ini` 的 `[mods]` 下写 `startup_crash_fix=0`。

## 设置

无。

## 日志与排错

第一次进入启动读取界面时，mod 会列出机体缺失的读取组（具体编号取决于客户端数据）：

```
startup_crash_fix: loading set <组编号>: unit <机体编号> is not in this client's unit data
startup_crash_fix: <N> loading sets, <M> with a missing unit
startup_crash_fix: loading set <组编号> picked: its unit <机体编号> is missing, set <组编号> (unit <机体编号>) shown instead
```

| 日志 | 含义 |
|---|---|
| `N loading sets, M with a missing unit` | 共 N 组，其中 M 组被抽中时原本会闪退 |
| `loading set ... picked: its unit ... is missing` | 这次启动原本会闪退；读取界面改为显示日志里写的那一组 |
| 点登录时闪退，`access violation at 008FEE6B` | mod 版本 1.0.0（框架 1.1.0 版）的问题；请更新本 mod |
| `required symbol gate.show_set_unit ...` | 客户端版本在这里不同，本 mod 不启用 |

发现问题的客户端上：共 9 组读取界面，其中 2 组的机体缺失（第 27106 组：机体 25452；第 27107 组：机体 25454）。

## 原理

游戏启动时（`CGameApp::SetState`，`SERVICE_LOGO`），读取界面（`0x0090C320`）用
`srand(timeGetTime())` 随机挑一组读取界面（`0x00904180`：类型 5 的数据，从编号 27102 起，
范围等于有机体的组数），把编号存到 `+0x20E0`。然后从 27101 起逐组遍历，对每个没有机体的组和被抽中的组
调用显示函数（`0x008FE380`，符号 `gate.show_set_unit`）。对有机体的组，该函数到机体数据里查找
（类型 2，`0x0059D2B0`，符号 `grid.find`），在 `+0x207C` 创建机体模型并按这一组的其余数据设置好，
期间不检查是否找到，就读取机体记录 `+0x264` 处的名字。游戏自己的崩溃日志（`zoa_log\Exp_*.txt`）
从 2024 年起就有这个闪退，早于任何 mod。

读取界面离不开这个模型：点登录时，`0x008FEE30` 会取编号为 `+0x20E0` 的那一组，在 `+0x207C` 的模型上
播放它的动作，两者都不检查。所以 `startup_crash_fix` 挂钩 `gate.show_set_unit`，把机体缺失的组换成另一组，
就像随机本来就抽到了那一组：优先取它之前最近的一组机体存在的，没有再取它之后最近的一组。替换组的编号写入
`+0x20E0`。替换组在它之前时，遍历已经走过，当场显示；在它之后时，由遍历走到那里时显示。无论哪种，
界面上都恰好显示一台机体，按替换组的数据设置。

mod 1.0.0 版（框架 1.1.0 版）的做法是不显示机体。这样 `+0x207C` 没有被设置，只要抽中了机体缺失的组，
点登录时就会闪退（`0x008FEE6B` 访问冲突）。
