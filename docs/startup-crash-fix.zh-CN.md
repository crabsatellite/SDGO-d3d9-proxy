# 开游戏随机崩溃修复

修复开游戏时随机崩溃的问题：游戏刚启动、在读取界面偶尔闪退（`0x008FE610` 访问冲突，游戏自己的崩溃日志
从 2024 年起就有）。游戏会随机挑一组读取界面，其中个别组引用的机体在客户端的机体数据里不存在，抽到它就会
闪退；现在这种组不显示机体，正常继续。
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
startup_crash_fix: loading set <组编号> picked: its unit <机体编号> is missing, no unit shown
```

| 日志 | 含义 |
|---|---|
| `N loading sets, M with a missing unit` | 共 N 组，其中 M 组被抽中时原本会闪退 |
| `loading set ... picked: its unit ... is missing` | 这次启动原本会闪退；读取界面不显示机体 |
| `required symbol gate.show_set_unit ...` | 客户端版本在这里不同，本 mod 不启用 |

发现问题的客户端上：共 9 组读取界面，其中 2 组的机体缺失（第 27106 组：机体 25452；第 27107 组：机体 25454）。

## 原理

游戏启动时（`CGameApp::SetState`，`SERVICE_LOGO`），读取界面（`0x0090C320`）用
`srand(timeGetTime())` 随机挑一组读取界面（`0x00904180`：类型 5 的数据，编号从 27101 起），
再显示这一组的机体（`0x008FE380`，符号 `gate.show_set_unit`）。该函数在这一组没有机体（编号 0）时
直接返回；否则到机体数据里查找（类型 2，`0x0059D2B0`，符号 `grid.find`），不检查是否找到，
就读取机体记录 `+0x264` 处的名字。游戏自己的崩溃日志（`zoa_log\Exp_*.txt`）从 2024 年起就有这个
闪退，早于任何 mod。

`startup_crash_fix` 挂钩 `gate.show_set_unit`：这一组的机体不在机体数据里时，按"没有机体"的方式直接返回。
原函数在这个提前返回之前只做一件事：查找界面的 `unit_pos` 节点并存到 `+0x2084`，而这个值只有该函数自己会读，
所以直接返回不会改变其它任何东西。
