# SDGO mod 框架

SD敢达 Online 游戏客户端（`GOnline.exe`）与对战服务器（`BattleServer.exe`）的 mod 加载框架。它在两个程序里
各放入一个小核心：核对 exe 版本、使用前校验每个代码地址、加载 mod。各项功能都以 mod 的形式建在框架之上，
可以只在客户端、只在对战服务器，或两边配合。

非官方爱好者项目，不包含任何游戏文件；SD敢达 Online 及其素材归其权利人所有。
English: [README.md](README.md)

* [功能](#功能)
* [包内容](#包内容)
* [支持的游戏版本](#支持的游戏版本)
* [安装](#安装) · [卸载](#卸载)
* [配置](#配置) · [排错](#排错)
* [从源码构建](#从源码构建) · [新增功能](#新增功能)

## 功能

独立的 [AI 机体目录 Mod](docs/ai-catalog.zh-CN.md) 读取服主生成的配套目录，扩充 LV1–LV5 机体池，
不写数据库或游戏原包。默认关闭，带独立安装器和玩家增量导出；原生 AI 脚本的实际游戏表现仍需手动验收。

每项功能由一个或多个 mod 组成，各有一份说明（使用、设置、日志、实现原理）。

| 功能 | 作用 | mod | 安装在 | 说明 |
|---|---|---|---|---|
| 服主 AI 机体目录 | 为现有 AI 功能提供配套 NPC 目录和大等级池；关闭后恢复原等级池。 | `ai_catalog` | 对战服务器 + 所有参战客户端 | [中文](docs/ai-catalog.zh-CN.md) · [English](docs/ai-catalog.md) |
| 任务模式 AI 僚机 | 任务模式里 AI 驾驶员加入你的队伍，使用游戏原生的 AI 对战机器人 AI，击破正常计算，也会被击落。在房间标题写 `!ai`。 | `coop_ai`、`mission_ai` | 对战服务器 + 客户端 | [中文](docs/coop-wingmen.zh-CN.md) · [English](docs/coop-wingmen.md) |
| 对战模式 AI 玩家（源码新增） | 在 PvP 房名里用 `!ai` 加入对手和队友，接入原有计分、复活、卡组与死亡后观战。 | `pvp_ai`、`match_ai` | 对战服务器 + 所有参战客户端 | [中文](docs/pvp-ai.zh-CN.md) · [English](docs/pvp-ai.md) |
| 输入法修复 | 修复一处把输入法（中/日/韩）提交的字节数当成字符数的问题。 | `ime_fix` | 客户端 | [中文](docs/ime-fix.zh-CN.md) · [English](docs/ime-fix.md) |
| 通用输入法修复 | 修复输入法让游戏卡死或崩溃的问题，不区分输入法。整合包的 `SDPlus.dll` 破坏了游戏进程里所有 DLL 的窗口查找（`FindWindowW`），任何需要查找窗口的输入法一开启就会卡死或崩溃。本修复为游戏本体以外的所有代码恢复正常查找。 | `findwindow_fix` | 客户端 | [中文](docs/findwindow-fix.zh-CN.md) · [English](docs/findwindow-fix.md) |
| 开游戏随机崩溃修复 | 修复开游戏时随机崩溃（读取界面闪退，`0x008FE610`）：读取界面随机挑一组，其机体在客户端数据里不存在时会闪退；现在这种组会换成最近的一组机体存在的读取界面。 | `startup_crash_fix` | 客户端 | [中文](docs/startup-crash-fix.zh-CN.md) · [English](docs/startup-crash-fix.md) |
| 发包日志 | 开发 mod 用的发包记录，默认关闭。 | `net_probe` | 客户端 | [docs/net-probe.md](docs/net-probe.md) |

"安装在"写了对战服务器的功能，需要对战服务器装服务器部分、玩家装配套的客户端部分才生效。
AI 对战房间的所有参与者应使用配套版本。已发布的 1.1.2 包不含新增的对战 AI。

### 游戏内效果

任务模式 AI 僚机：房间标题含 `!ai` 的任务房间，AI 僚机与玩家同行，攻击任务中的敌人。

![任务模式里 AI 僚机与玩家并肩作战](docs/coop-wingmen-demo.webp)

对战 AI 的新视频尚待录制，见[五段功能展示与手动验收清单](docs/pvp-ai-video.zh-CN.md)。

通用输入法修复：游戏大厅里开启输入法（红框为输入法工具栏，截图中是讯飞）。没有 `findwindow_fix` 时，任何需要查找窗口的
输入法一开启就会让游戏卡死或崩溃。

![开启输入法的游戏大厅](docs/findwindow-fix-ime.png)

## 包内容

```
SDGO-ModFramework-<版本>/
  client/                 放到 GOnline.exe 同目录
    d3d9.dll
    sdgo\core.dll、sdgo\sdgo.ini、sdgo\symbols\
    sdgo\mods\            findwindow_fix、ime_fix、mission_ai、match_ai、net_probe（默认关闭）、startup_crash_fix
  server/                 放到 BattleServer.exe 同目录
    winmm.dll
    sdgo\core.dll、sdgo\sdgo.ini、sdgo\symbols\
    sdgo\mods\            coop_ai、pvp_ai
  install_client.ps1      把 client\ 安装到游戏目录
  install_server.ps1      把 server\ 安装到对战服务器目录
  README.md、README.zh-CN.md、docs\、LICENSE、THIRD_PARTY_NOTICES.md、licenses\
```

这些 mod 就是[功能](#功能)表里的各项功能；`net_probe` 要在它的 `mod.ini` 里打开才会生效。

| 部件 | 位置 | 作用 |
|---|---|---|
| `d3d9.dll` | 游戏目录 | 加载器：GOnline.exe 启动时加载它，它启动 `sdgo\core.dll`，并把 Direct3D 转发给真正的 `d3d9.dll` |
| `winmm.dll` | 对战服务器目录 | BattleServer.exe 用的同一种加载器（转发 `timeGetTime`） |
| `sdgo\core.dll` | 两边 | 框架核心：核对 exe 版本，校验每个代码地址，统一管理 hook，加载 mod |
| `sdgo\sdgo.ini` | 两边 | 框架设置：日志级别、串联的加载器、单独关闭某个 mod（见[配置](#配置)） |
| `sdgo\symbols\*.ini` | 两边 | 每个支持的 exe 版本一份：已校验的地址与字节特征 |
| `sdgo\mods\<id>\` | 两边 | 每个 mod 一个目录：`<id>.dll`、`mod.ini`（清单 + 设置）、附带文件 |

安装脚本会复制包里的全部内容，新增功能不需要改安装脚本。

## 支持的游戏版本

框架只改动它能校验的代码。每个 exe 版本有一个 *host key*（`<exe 名>-<TimeDateStamp>-<SizeOfImage>`），
`sdgo\symbols\` 里要有对应文件：

| exe | host key | 已知发行版 |
|---|---|---|
| GOnline.exe | `GOnline-54BE1568-00C97000` | SDGO Ultimate Repack v1.2（及其底包客户端） |
| BattleServer.exe | `BattleServer-548A891D-0046C000` | 常见私服服务端包（BattleServer1..N 目录） |

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
（见 [docs/framework.md](docs/framework.md#symbol-files)），在此之前框架会加载但什么都不做。

## 从源码构建

Visual Studio 2022（C++ 工具）与 CMake 3.20+，两端都是 32 位：

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\package.ps1     # 生成 dist\SDGO-ModFramework-<版本>.zip
```

安装脚本也能直接从构建目录安装：`tools\install_client.ps1 -GameDir ... -Source build\stage\Release\client`。

对战 AI、计分条和观战逻辑的离线测试（不开游戏）见
[测试说明](docs/ai-offline-tests.md)，入口为 `tests\run_ai_tests.ps1`。

## 新增功能

新功能 = `mods/` 下的一个或多个 mod（在 `CMakeLists.txt` 注册）+ `docs/` 下的一份说明 + 两个 README
[功能](#功能)表里的一行，以及[包内容](#包内容)列表里加上它的 mod。构建会把它放进 `client\` 或 `server\`，安装脚本和打包脚本无需修改。
具体步骤、mod API、symbol 文件和说明文档的固定结构见
[docs/framework.md](docs/framework.md#adding-a-feature)（英文）。

## 许可

MIT，见 [LICENSE](LICENSE)。第三方代码见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)（MinHook，BSD 2-Clause）。
