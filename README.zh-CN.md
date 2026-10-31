# SDGO mod 框架

SD敢达 Online 游戏客户端（`GOnline.exe`）与对战服务器（`BattleServer.exe`）的 mod 加载框架，以及基于它的
mod。主打功能是**任务模式 AI 僚机**：AI 驾驶员按游戏自带 AI 对战模式加入机器人的方式加入你的队伍，
使用游戏原生的对战 AI，击破会被正常计算，也会被击落。

非官方爱好者项目，不包含任何游戏文件；SD敢达 Online 及其素材归其权利人所有。
English: [README.md](README.md)（框架开发文档 [docs/framework.md](docs/framework.md)，各 mod 详细说明
[docs/mods.md](docs/mods.md)）。

## 包内容

```
SDGO-ModFramework-<版本>/
  client/             放到 GOnline.exe 同目录      d3d9.dll + sdgo\
  server/             放到 BattleServer.exe 同目录  winmm.dll + sdgo\
  install_client.ps1  把 client\ 安装到游戏目录
  install_server.ps1  把 server\ 安装到对战服务器目录
```

| 部件 | 位置 | 作用 |
|---|---|---|
| `d3d9.dll` | 游戏目录 | 加载器：GOnline.exe 启动时加载它，它启动 `sdgo\core.dll`，并把 Direct3D 转发给真正的 `d3d9.dll` |
| `winmm.dll` | 对战服务器目录 | BattleServer.exe 用的同一种加载器（转发 `timeGetTime`） |
| `sdgo\core.dll` | 两边 | 框架核心：核对 exe 版本，校验每个代码地址，统一管理 hook，加载 mod |
| `sdgo\symbols\*.ini` | 两边 | 每个支持的 exe 版本一份：已校验的地址与字节特征 |
| `sdgo\mods\<id>\` | 两边 | 每个 mod 一个目录：`<id>.dll`、`mod.ini`（清单 + 设置） |

| mod | 运行在 | 作用 |
|---|---|---|
| `coop_ai` | 对战服务器 | 房间标题含 `!ai` 的任务房间开局时加入 AI 僚机 |
| `mission_ai` | 客户端 | 在任务里生成这些僚机，并让任务敌人可以锁定僚机 |
| `ime_fix` | 客户端 | 修复一处把输入法提交的字节数当成字符数的问题 |
| `net_probe` | 客户端 | 开发用的发包日志，默认关闭 |

## 支持的游戏版本

框架只改动它能校验的代码。每个 exe 版本有一个 *host key*（`<exe 名>-<TimeDateStamp>-<SizeOfImage>`），
`sdgo\symbols\` 里要有对应文件：

| exe | host key | 已知发行版 |
|---|---|---|
| GOnline.exe | `GOnline-54BE1568-00C97000` | SDGO Ultimate Repack v1.2（及其底包客户端） |
| BattleServer.exe | `BattleServer-548A891D-0046C000` | 常见私服服务端包（BattleServer1..N 目录） |

安装脚本会打印你的 exe 的 host key。其它版本也能装，但框架会记录"没有匹配的 symbol 文件"并关闭所有
mod，游戏本身不受影响。

## 安装

需要 Windows 自带的 Windows PowerShell 5.1，不需要 VC++ 运行库。解压发布包，在解压目录打开 PowerShell。

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
* 重复运行即为更新，并保留你改过的 `.ini`（加 `-OverwriteConfig` 恢复默认）。

手动安装：把 `client\` 里的内容复制到游戏目录（GOnline.exe 旁边）。

检查：进游戏后 `sdgo\logs\sdgo.log` 开头应有 `core: SDGO core started by d3d9 loader ...`、
`symbols: N of N verified`，以及每个 mod 一行 `loaded <mod>`。

### 对战服务器（架服务器的人）

先停止对战服务器，然后：

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server"
```

`-ServerRoot` 可以是单个对战服务器目录（含 `BattleServer.exe`），也可以是包含多个的上级目录
（`BattleServer1`、`BattleServer2`……）；`-Only BattleServer3` 只装指定目录。装完再启动对战服务器；用什么方式
启动（服务端管理器、批处理、服务）都可以。手动安装：把 `server\` 的内容复制到每个对战服务器目录。

僚机需要两边都装：对战服务器装 `coop_ai`，客户端装 `mission_ai`。没装客户端部分的玩家看不到僚机。

## 卸载

```powershell
powershell -ExecutionPolicy Bypass -File .\install_client.ps1 -GameDir "D:\Games\SDGO" -Uninstall
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO-Server\Server" -Uninstall
```

删除加载器 DLL 和 `sdgo\`（进回收站），并把串联的 `d3d9.dll`/`winmm.dll` 恢复原名。只想关掉某个 mod：
在 `sdgo\sdgo.ini` 的 `[mods]` 下写 `<mod id>=0`。

## 任务模式僚机

在任务房间标题里写指令（标题最多 20 字节）：

| 标题 | 效果 |
|---|---|
| `!ai` | 用僚机补满空位 |
| `!ai2` | 两个僚机（仍不超过空位） |
| `!ai3 lv4` | 三个 4 级池的僚机 |
| 没有 `!ai` | 不加僚机（除非 `RequireRoomCommand=0`） |

* 开局时由对战服务器按 AI 对战模式的方式把 AI 驾驶员加入你的队伍，游戏把它们当作真正的参战者：
  击破、被攻击、被击落都正常计算。
* 使用游戏原生的 AI 对战机器人 AI（AI 对战机器人的 `VSAI_*` 脚本），在房主客户端上运行。
* 总人数不超过 `SquadSize`（4）：僚机数 = SquadSize − 真人数，`!aiN` 要求更少时按 N。
* 等级从 AI 对战机器人机体中选（`tbl_NpcData` 3604001–3604030）。默认：`lv1` 力天使、主天使、能天使、
  剑装灾厄、炮装/剑装强袭、地狱死神、迅雷、贝尔加·基罗斯、帕拉斯·雅典娜；`lv2` 勇者、蓝色决斗、空战强袭、
  维格纳·吉纳、高达（MC）、百式；`lv3` MA（瓦尔·瓦罗、大魔、精神感应高达、阿普萨拉斯 II）；`lv4`/`lv5`
  强袭自由、Z 高达（觉醒）、海盗 X3、沙漠、无限正义、炽天使、尊者、地狱死神（EW）、重武装（EW）、新吉翁号。
  不写 `lvN` 时用 `DefaultLevel`（2）。
* 任务敌人原本只锁定真人玩家；客户端 mod 让敌人在僚机比当前目标更近时改打僚机（`EnemiesTargetWingmen`）。

设置：对战服务器的 `sdgo\mods\coop_ai\mod.ini`，客户端的 `sdgo\mods\mission_ai\mod.ini`。

## 增加更多僚机机体

等级池只能使用对战服务器认作 AI 对战机器人的 NPC。对战服务器启动时生成这份名单：
`GOnlineGame.dbo.tbl_NpcData` 里 `NpcDataID` 在 **3604001–3607000** 之间的每一行都会成为 AI 机器人
（原版数据库有 30 个：3604001–3604030）。mod 不会写数据库；名单里没有的 id 会被跳过，整个池都缺失时
服务器日志会写 `no NPC of LevelN is in the server's AI-NPC list`。

新增一个机体需要三处互相对应：

1. **服务器数据库**：在 `tbl_NpcData` 加一行，`NpcDataID` 取范围内的新值。`UseUnitID` 是机体，
   `UseUnitHP`、`NpcPoint`、`NpcUnitLevel` 决定强度。最简单是复制现有机器人：

   ```sql
   INSERT INTO GOnlineGame.dbo.tbl_NpcData
     (NpcDataID, NpcName, UseUnitID, UseUnitHP, NpcPoint, ObjectHpUp, NpcType, AddUnitEXP,
      AddPoint, BossFlag, NpcUnitLevel, ItemBuffNumber, CreateUnitHp, DestroyUnitHp)
   SELECT 3604031, 'MY NEW BOT', UseUnitID, 16000, NpcPoint, ObjectHpUp, NpcType, AddUnitEXP,
          AddPoint, BossFlag, NpcUnitLevel, ItemBuffNumber, CreateUnitHp, DestroyUnitHp
   FROM GOnlineGame.dbo.tbl_NpcData WHERE NpcDataID = 3604001;
   ```

   机体（`UseUnitID`）必须存在于服务器的机体数据里。原版机器人用专用 AI 机体 3607001–3607030；
   非 AI 专用机体尚未测试。

2. **客户端 NPC 记录**：同一个 id，每个玩家的客户端都要有，否则无法生成该机体。NPC 记录在客户端
   grid 数据的 `0198` 表里（位于 `GPackData`，已加密），每行 598 字节：

   | 偏移 | 字段 |
   |---|---|
   | `0x00` | NPC 数据 id（u32），例如 3604031 |
   | `0x05` | 显示名（UTF-16） |
   | `0x69` | 机体 id（u32），例如 3607001 |
   | `0x6D` | AI 脚本名（UTF-16），取 AI 对战机器人脚本之一：`VSAI_Gunfighter_01`–`10`、`VSAI_Infighter_01`–`07`、`VSAI_Tanker_01`–`05`、`VSAI_Stoker_01`–`08` |
   | `0xE5` | HP（u32） |

   机体也要存在于客户端的机体表里（AI 机体在 `0199`、`0245` 表）。本仓库没有 grid 数据编辑器，
   任何能解密并重写客户端 `.gdt` 表的工具都可以。

3. **加入等级池**：把 id 加到对战服务器 `sdgo\mods\coop_ai\mod.ini` 的某行 `LevelN=`，然后重启对战服务器
   （NPC 表和 mod 设置在启动时读取）。

检查：对战服务器日志（其目录下的 `sdgo\logs\sdgo.log`）出现 `slot N: AI pilot ... (NPC 3604031) joined on team 1`，
房主客户端日志出现 `AI participant ... joined as object ... (NPC 3604031 ...)`。

## 配置

`sdgo\sdgo.ini`（每个 exe 目录一份）：

| 节 / 键 | 含义 |
|---|---|
| `[core] log_level` | 0 调试、1 信息（默认）、2 警告、3 错误 |
| `[loader] chain_d3d9` / `chain_winmm` | 代替系统副本转发到的另一个 DLL，相对 exe 目录（安装脚本自动设置） |
| `[mods] <id>=0` | 关闭某个 mod |

各 mod 的设置在其 `mod.ini` 的 `[settings]`，每项都有注释。

## 排错

所有诊断都写在 exe 旁边的 `sdgo\logs\sdgo.log`（上一次运行是 `sdgo.prev.log`）。

| 日志 | 含义 |
|---|---|
| 完全没有 `sdgo.log` | 加载器没运行：DLL 不在 exe 旁边，或被其它程序替换了 |
| `no symbol file for host ...` | 不支持的 exe 版本；所有 mod 关闭，游戏不受影响 |
| `symbol X rejected: byte +N is .., expected ..` | 该地址的代码不同（其它补丁或不同版本）；需要 X 的 mod 关闭 |
| `required symbol X ...` / `0 of N mods active` | 某个 mod 需要的 symbol 校验失败，未启动 |
| 服务器：`mission start: ... title '...'` | 服务器读到了房间标题；`wingmen not requested` 表示标题没有 `!ai` |

其它版本的 `GOnline.exe` / `BattleServer.exe` 需要为该版本新增 `sdgo\symbols\<host key>.ini`
（见 [docs/framework.md](docs/framework.md#symbol-files)），在此之前框架会加载但什么都不做。

## 从源码构建

Visual Studio 2022（C++ 工具）与 CMake 3.20+，两端都是 32 位：

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release
powershell -ExecutionPolicy Bypass -File tools\package.ps1     # 生成 dist\SDGO-ModFramework-<版本>.zip
```

## 许可

MIT，见 [LICENSE](LICENSE)。第三方代码见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)（MinHook，BSD 2-Clause）。
