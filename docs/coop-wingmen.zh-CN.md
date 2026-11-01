# 任务模式 AI 僚机

任务模式里，AI 驾驶员按游戏自带 AI 对战模式加入机器人的方式加入你的队伍，使用游戏原生的对战 AI，
击破会被正常计算，也会被击落。English: [coop-wingmen.md](coop-wingmen.md)

![任务 AI 僚机](coop-wingmen-demo.webp)

| mod | 运行在 | 负责 |
|---|---|---|
| `coop_ai` | 对战服务器 | 任务开局时把 AI 驾驶员加入房间 |
| `mission_ai` | 客户端 | 在任务里生成僚机、运行其 AI、让敌人可以锁定僚机 |

两边都要装：对战服务器装 `coop_ai`，客户端装 `mission_ai`。没装客户端部分的玩家看不到僚机。

* [使用](#使用)
* [设置](#设置)
* [增加更多僚机机体](#增加更多僚机机体)
* [日志与排错](#日志与排错)
* 实现原理：见英文版 [How it works](coop-wingmen.md#how-it-works)

## 使用

在任务房间标题里写指令：

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

## 长房名与组合指令

启用[长房名三端配套](room-titles.zh-CN.md)并更新 `coop_ai` 后，会读取完整房名中的指令，
上限为 128 个 Unicode 标量 / 512 字节 UTF-8。例如：
`周末朋友任务房，慢慢打一起练习 !ai3 lv4 !rand`。
它请求 3 个 4 级僚机；另装并启用 `room_random` 后，真人每局随机机体。
僚机仍受空位限制，客户端仍需 `mission_ai`。`lv4` 要写在 `!ai` 后、`!rand` 前。
未启用长房名配套时，原生上限仍为 20 字节。更多例子见[房名组合指令](room-commands.zh-CN.md)。

## 设置

`mission_ai 2.0.1` 修正两类任务兼容问题：部分近战脚本只搜索真人，任务里真人都是队友时可能
一直没有目标；现在仅对任务僚机改用原生任务敌人搜索，继续遵守敌我、距离和视线判定。
原生关卡脚本调用 `PlayerWarp` 换区时，本机控制的存活僚机同时使用同一个原生传送接口，
前往真人的实际落点并清除旧攻击／移动请求。不会复活死亡僚机，不接管远端副本，不增加
`GetNumPlayer()` 的人数，也不按距离在战斗中随意瞬移。各客户端需正常重启以加载新版 DLL；
普通走路进入新区、未调用 `PlayerWarp` 的任务仍需单独验证跟随情况。

写在各自 `mod.ini` 的 `[settings]` 里，值后面不能跟行内注释。

`coop_ai`，对战服务器，`sdgo\mods\coop_ai\mod.ini`（改完需重启对战服务器）：

| 键 | 默认 | 含义 |
|---|---|---|
| `RequireRoomCommand` | 1 | 1：只有标题含 `!ai` 的房间；0：所有任务房间 |
| `DefaultCount` | 0 | 只写 `!ai` 时的僚机数；0 表示补满空位 |
| `SquadSize` | 4 | 任务房间人数上限；僚机数不超过 SquadSize − 真人数 |
| `DefaultLevel` | 2 | 标题没写 `lvN` 时的等级 |
| `LogRoom` | 1 | 每次任务开局记录大厅房间和标题 |
| `Level1`..`Level5` | 见文件 | 各等级的 NPC 数据 id；不在 AI-NPC 名单里的 id 会被跳过 |

`mission_ai`，客户端，`sdgo\mods\mission_ai\mod.ini`（2 秒内生效，任务中途也可以改）：

| 键 | 默认 | 含义 |
|---|---|---|
| `EnemiesTargetWingmen` | 1 | 1：僚机比敌人当前目标更近时，敌人可以改打僚机 |
| `EnemyAggroRange` | 2500 | 敌人会改打的僚机的最远距离 |
| `LogCombat` | 0 | 1：每 5 秒记录每个僚机的 HP 和目标 |

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

检查：对战服务器日志出现 `slot N: AI pilot ... (NPC 3604031) joined on team 1`，
房主客户端日志出现 `AI participant ... joined as object ... (NPC 3604031 ...)`。

## 日志与排错

对战服务器（其目录下的 `sdgo\logs\sdgo.log`）：

```
coop_ai: mission start: lobby room 1 (user 6228) title '!ai3 lv4', 1 human(s), 0 AI; wingmen requested
coop_ai: slot 1: AI pilot 80000002 (NPC 3604014) joined on team 1
coop_ai: 3 wingmen added (level 4)
```

客户端（游戏目录下的 `sdgo\logs\sdgo.log`）：

```
mission_ai: AI participant 80000002 joined as object 1014 (NPC 3604014, team 1, slot 1, AI runs here)
mission_ai: mission 1: 3 wingmen
```

| 现象 / 日志 | 含义 |
|---|---|
| 没有 `coop_ai: mission start`（`LogRoom=1` 时） | 该对战服务器没有加载 `coop_ai`（看日志开头），或者房间不是任务模式 |
| `wingmen not requested` | 标题没有 `!ai`，且 `RequireRoomCommand=1` |
| `no NPC of LevelN is in the server's AI-NPC list` | 该服务器数据库里 3604001–3607000 范围内没有这个池的任何 id，见[增加更多僚机机体](#增加更多僚机机体) |
| 服务器加了僚机，某客户端日志没有 `AI participant` | 该客户端没装 `mission_ai` |
| 客户端：`AI participant ...: creating NPC N failed` | 该客户端没有 NPC 记录 N（客户端 `0198` 表） |
