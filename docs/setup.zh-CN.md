# 安装与配置

[English](setup.md) · [返回功能分类](../README.zh-CN.md)

先安装基础修复，再选择需要的玩法扩展或联机工具。跨客户端/服务端的功能需要安装配套部分。

## 支持的游戏版本

框架只改动它能校验的代码。每个 exe 版本有一个 *host key*（`<exe 名>-<TimeDateStamp>-<SizeOfImage>`），
`sdgo\symbols\` 里要有对应文件：

| exe | host key | 已知发行版 |
|---|---|---|
| GOnline.exe | `GOnline-54BE1568-00C97000` | SDGO Ultimate Repack v1.2（及其底包客户端） |
| BattleServer.exe | `BattleServer-548A891D-0046C000` | 常见私服服务端包（BattleServer1..N 目录） |
| AgentServer.exe | `AgentServer-548A86BC-000D7000` | 原生 Agent 中继宿主 |
| WorldServer.exe | `WorldServer-548A86E3-0010C000` | 原生 World 结算宿主 |

安装脚本会打印你的 exe 的 host key。其它版本也能装，但框架会记录"没有匹配的 symbol 文件"并关闭所有
mod，游戏本身不受影响，见[其它游戏版本](#其它游戏版本)。

## 安装

需要 Windows 自带的 Windows PowerShell 5.1，不需要 VC++ 运行库。从
[最新发布](https://github.com/crabsatellite/SDGO-d3d9-proxy/releases/latest)下载 `SDGO-ModFramework-<版本>.zip`，
解压后在解压目录打开 PowerShell。

### 客户端（每个玩家）

先关闭游戏，然后：

```powershell
powershell -ExecutionPolicy Bypass -File .\install_client.ps1 -GameDir "D:\Games\SDGO"
```

`-GameDir` 是 `GOnline.exe` 所在目录。

* 用什么方式启动游戏都可以（任何启动器、快捷方式、直接运行 GOnline.exe）：Windows 总是从 GOnline.exe
  所在目录加载 `d3d9.dll`，框架也以 GOnline.exe 的位置找自己的文件。
* 不影响其它 mod。通过其它 DLL 加载的插件（例如 Ultimate Repack 的 `dinput8.dll` + `SDPlus.dll`）照常工作。
  如果目录里已经有别的 mod 的 `d3d9.dll`（例如画面增强），安装脚本会把它改名为 `d3d9_chain.dll` 并串联，
  两者同时生效。
* 重复运行即为更新：保留 `sdgo\sdgo.ini`；每个 mod 的 `mod.ini` 换成新版，再把你自己的值写回去
  （`[mod] enabled` 和 `[settings]` 里与新文件不同的每一项），输出里会列出写回了哪些。
  加 `-OverwriteConfig` 则两者都按包里的原样安装。

手动安装：把 `client\` 里的内容复制到游戏目录（GOnline.exe 旁边）。这样 `sdgo.ini` 和所有 `mod.ini`
都会被包里的版本覆盖。

检查：进游戏后 `sdgo\logs\sdgo.log` 开头应有 `core: SDGO core started by d3d9 loader ...`、
`symbols: N of N verified`，以及每个 mod 一行 `loaded <mod>`。

### 对战服务器（架服务器的人）

只有"安装在"写了对战服务器的功能才需要。先停止对战服务器，然后：

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server"
```

`-ServerRoot` 可以是单个对战服务器目录（含 `BattleServer.exe`），也可以是包含多个的上级目录
（`BattleServer1`、`BattleServer2`……）；`-Only BattleServer3` 只装指定目录。装完再启动对战服务器；用什么方式
启动（服务端管理器、批处理、服务）都可以。已有的 `winmm.dll` 的串联、更新时保留设置，都和客户端一样。
手动安装：把 `server\` 的内容复制到每个对战服务器目录。

### Agent 服务器

联机工具和中继日志修复使用同一个服务器安装器，指定 Agent 角色：

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server\AgentServer1" -Role Agent
```

默认角色仍为 Battle。服主和玩家的配置分发见 [FRP 分发指南](frp-distribution.zh-CN.md)。

### World 服务器

WorldServer 基础修复使用同一安装器的 `-Role World`，先停止目标 WorldServer：

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server\WorldServer" -Role World
```

详见[军衔修复 Mod](rank-progression-fix.zh-CN.md)，此修复无需客户端组件。
卸载 World 宿主时也应指定相同角色。

## 卸载

```powershell
powershell -ExecutionPolicy Bypass -File .\install_client.ps1 -GameDir "D:\Games\SDGO" -Uninstall
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server" -Uninstall
```

删除加载器 DLL 和 `sdgo\`（进回收站），并把串联的 `d3d9.dll`/`winmm.dll` 恢复原名。只想关掉某个 mod：
在 `sdgo\sdgo.ini` 的 `[mods]` 下写 `<mod id>=0`。

## 配置

`sdgo\sdgo.ini`（每个 exe 目录一份）：

| 节 / 键 | 含义 |
|---|---|
| `[core] log_level` | 0 调试、1 信息（默认）、2 警告、3 错误 |
| `[loader] chain_d3d9` / `chain_winmm` | 代替系统副本转发到的另一个 DLL，相对 exe 目录（安装脚本自动设置） |
| `[mods] <id>=0` | 关闭某个 mod |

各 mod 的设置在其 `mod.ini` 的 `[settings]`，每项都有注释，功能说明里也有列表。
每局随机机体由服主在 `room_random/config.ini` 配置现有 WorldServer INI 的路径后启用，
玩家使用原客户端，在房名加入 `!rand`；可与 `!ai` 组合。
见[每局随机机体](room-random.zh-CN.md)及[房名组合指令](room-commands.zh-CN.md)。

## 排错

所有诊断都写在 exe 旁边的 `sdgo\logs\sdgo.log`（上一次运行是 `sdgo.prev.log`）。框架本身的日志：

| 日志 | 含义 |
|---|---|
| 完全没有 `sdgo.log` | 加载器没运行：DLL 不在 exe 旁边，或被其它程序替换了 |
| `no symbol file for host ...` | 不支持的 exe 版本；所有 mod 关闭，游戏不受影响 |
| `symbol X rejected: byte +N is .., expected ..` | 该地址的代码不同（其它补丁或不同版本）；需要 X 的 mod 关闭 |
| `required symbol X ...` / `0 of N mods active` | 某个 mod 需要的 symbol 校验失败，未启动 |

各功能自己的日志见其说明。

### 其它游戏版本

其它版本的 `GOnline.exe` / `BattleServer.exe` 需要为该版本新增 `sdgo\symbols\<host key>.ini`
（见 [docs/framework.md](framework.md#symbol-files)），在此之前框架会加载但什么都不做。

## 从源码构建

Visual Studio 2022（C++ 工具）、CMake 3.20+ 与 Python 3.10+（仅构建工具使用），两端都是 32 位：

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\package.ps1     # 生成 dist\SDGO-ModFramework-<版本>.zip
```

安装脚本也能直接从构建目录安装：`tools\install_client.ps1 -GameDir ... -Source build\stage\Release\client`。


## 按分类或功能开关

在对应宿主的 `sdgo/sdgo.ini` 中设置：

```ini
[categories]
gameplay=0
[features]
pvp-ai=0
[mods]
net_probe=0
```

`0` 分别关闭整个分类、一项完整功能或一个 Mod。分类/功能开关不能强行开启自身已禁用的 Mod。
旧配置不添加这些段落时，保持原有行为。分类 id 为 `fixes`（基础修复）、`gameplay`（玩法扩展）、
`network`（联机工具）、`development`（开发诊断）；诊断工具默认关闭。
跨宿主功能需要在相应客户端和服务器分别设置。

## 包内容与更新

所有功能共用框架：客户端 `d3d9.dll`、服务端 `winmm.dll`、`sdgo/core.dll`、
符号文件和 `sdgo/sdgo.ini`。Mod 仍放在 `sdgo/mods/<id>/`，分类不改变加载路径。

包内 `CONTENTS.md` 和 `sdgo/catalog.json` 列出实际选入的功能；一项功能可以包含多个客户端/服务端 Mod。
更新会保留你的开关、设置、声明的用户配置文件及其他已安装 Mod。
需要恢复随包默认值时再使用 `-OverwriteConfig`。
