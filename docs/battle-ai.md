# Battle AI 0.3 / 通用战斗 AI（开发预览）

`battle_ai` 按当前机体、形态、武装数据选择打法。配置完成后随正常游戏启动生效；
它不管理 FRP、服务端或数据库，也没有额外的联机启动入口。

## 默认难度与房间控制

客户端装包后默认使用 **Medium**。房主沿用对战 Mod 的房名命令，朋友不必逐人改配置。

| 房名例子 | 效果 |
|---|---|
| `!ai` / `!ai3` | 原有机器人数量规则，默认 Medium |
| `!ai3 easy` | Easy：原版 AI |
| `!ai e3 a1 medium` | Medium：原版基础决策，改善瞄准/预判，并请求合法必杀 |
| `!ai3 hard` | Hard：启用通用战术和高级动作 |

`lv1`–`lv5` 仍是既有 NPC 池筛选，不代表机体真实等级，也不代表难度。
房名优先于本机 `Difficulty` 设置；房名没有难度时，未自定义的客户端使用 Medium。
切换房间后读取新难度。只由拥有 AI 控制权的客户端执行决策。

## 本轮实现状态

已实现、仅完成隔离自动验证：三档切换、原生射击记录、协同绕后、近战补枪、80% 甩刀、
90% 掩体机会选择、原生动画许可下的走 A，以及 Medium 以上的合法必杀请求。
必杀检查玩家 SP/动作许可，随后走原生请求与通知链；普通攻击不受 SP 门槛影响。
原生离线/本地模拟可走本地通知中转，因此通知本身不算真实服务器授权或扣 SP 的实机证据。
正在接入：真实 12 级/EX、合法部件/限次道具、盾朝向和防背身。这些未完成项尚未计为交付。

## 已实现功能

- **通用武器选择**：读取当前形态的真实 0-based 武器槽、射程、弹量、冷却、估计伤害和移动射击标志，
  比较当前距离与武器输出；空弹切换其它可用武器。原生切枪是请求，必须观察到实际提交后才发起攻击。
  不按 Gunfighter/Infighter 等脚本名决定远近战，不要求为每台机体手写配置。
- **攻击频率与意图**：减少 NPC 额外等待，提供反应时间、攻击间隔、连续进攻与短暂停顿设置。
  只在原生中立状态调节发起下一次攻击的意图；充能、持续光束、funnel、rush、连段等活动状态保留原生按住输入。
  实际弹药、武器冷却、动作和伤害仍由原生 FSM 执行。
- **低血量集火**：在可见、可攻击、处于可用武器有效射程的候选中，优先剩余 HP 少的敌人。
  极远残血不会抢走眼前可交战目标；短暂目标保持避免不停取消攻击。死亡或失效立即重选。
- **对射升空与横移**：观察原生射击事件，记住短促射击；对射时交替左右移动，并按间隔请求跳跃。
  尊重原生跳跃请求许可、推进余量及运动 FSM；移动射击受限的武器在射击时保持其原有限制。
  这里验证的是正确发出动作请求，是否在某台机体和地图上实际升空仍需实机确认。
- **瞄准与预判**：移除受控 NPC 的人为瞄偏，保留原始射线起点，修正射击方向。
  对有已验证速度的物理弹体求拦截提前量；脚本弹体通过连续三次稳定、非曲线/非变速的实际飞行样本学习速度。
  未知速度、Bezier 或变速轨迹不硬套直线模型；导弹原生引导和所有实际碰撞继续执行。
  用户选择的是“消除误差、实弹/导弹预判，朝约 90% 命中调校”，不是强制命中或补发伤害。
- **导航与恢复**：使用本客户端的原生路径搜索，逐点前进，释放原生路径数组时使用游戏自己的分配器。
  停滞时左右尝试，再重新规划；原地反复跳跃不算水平前进。暂时不可达的目标有独立冷却。
  目标被遮挡后仅短暂追踪最后可见位置；记忆过期后使用原生随机巡逻，不实时跟踪墙后位置。
- **逐目标视线**：使用本客户端的纯碰撞原语和 Mod 自有查询缓冲，分别检查每个目标；
  避免复用旧 AI 的两秒共享缓存，也不调用会改变移动/仇恨的“射击测试”作为只读查询。

只作用于负责模拟的本机战斗机器人（保留的 AI NPC 编号范围）；远端副本、真人控制和任务 Boss AI 不接管。
“任意机体”指机器人引用的合法原生机体/武装记录，不会自动修补缺失模型、武器或非法数据库内容。
原生生成机器人仍由既有 `match_ai` / `mission_ai` / 服务端配套 Mod 负责。

## 安装和分发

先安装本项目 Mod 框架，支持 `GOnline-54BE1568-00C97000`。关闭要更新的那份客户端，运行：

```powershell
./tools/install_battle_ai.ps1 -GameDir D:/Games/SDGO -Source ./build/stage/Release/client
```

增量安装只更新 `sdgo/mods/battle_ai/` 的五个产品文件及清单要求的符号段，
保留其它符号、其它 Mod、全局配置和用户 `config.ini`；不会停止任何进程或启动游戏。
预览包不含游戏文件、账号、SQL、FRP 配置或开发测试 VM。打包目录里的安装器可直接找到同包 `client/`。

`config.example.ini` 列出默认值。需要调整时复制成同目录 `config.ini`；未创建时使用默认值。
更新覆盖示例而保留用户文件。`mod.ini` 是开发者清单，不要求玩家写 IP、端口或本机路径。

关键设置：

| 设置 | 含义 |
|---|---|
| `Difficulty=Medium` | 默认；可写 Easy/Medium/Hard，房名有明确难度时优先使用房名 |
| `ReactionMs` / `AttackIntervalMs` | 反应时间与发起攻击的额外间隔；不是改写武器冷却 |
| `BurstMs` / `BurstPauseMs` | 连续进攻与暂停时间；活动充能/持续攻击不会被此项截断 |
| `TargetHoldMs` | 目标保持时间，减少抖动 |
| `FarTargetSlack` / `FarTargetRatioPercent` | 防止极远残血引走正在近距离交战的 AI |
| `AirStrafe` / `StrafeSwitchMs` | 对射横移与换向间隔 |
| `SwingCancelChance` / `CoverChance` | Hard 每次近战起手/掩体机会的概率，默认 80/90 |
| `OrbwalkStep` / `OrbwalkMs` | Hard 原生取消窗口后的短步距离和时长，不修改武器 CD |
| `JumpMs` / `JumpCooldownMs` / `BoostReservePercent` | 跳跃请求时长、间隔与推进余量 |
| `AimAssist` / `ProjectileLead` | 精准方向与弹体提前量开关 |
| `MaxLeadMs` | 最大外推时间，避免不可信的长距离预判 |
| `RepathMs` / `StuckMs` | 重新规划间隔与停滞检测时间 |
| `TargetMemoryMs` / `PatrolMs` | 最后可见位置记忆和无目标巡逻间隔 |
| `LogDecisions=1` | 记录目标、攻击、规避、路径等变化原因，不逐帧刷日志 |

`Enabled=0` 在两秒内交还原生控制并取消增强层的移动/射击意图；
或在全局 `sdgo.ini` 的 `[mods]` 下写 `battle_ai=0` 后重启客户端。
观察/数据读取或策略失败会回退原生；代码符号签名不符时由框架拒绝加载。

## 自动验证

测试在独立目录和可执行文件中进行，不连接任何游戏后端。

```text
测试用官方 Lua 5.0.3: https://www.lua.org/ftp/lua-5.0.3.tar.gz
SHA256 1193a61b0e08acaa6eee0eecf29709179ee49c71baebc59b682a25c3b5a45671
```

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DSDGO_LUA50_SOURCE=D:/deps/lua-5.0.3
cmake --build build --config Release --parallel 2
./tests/battle_ai_test.ps1 -NativeScriptDir D:/extracted-client/scp -GameExe D:/Games/SDGO/GOnline.exe
./tests/battle_ai_install_test.ps1 -GameExe D:/Games/SDGO/GOnline.exe
python tools/verify_symbols.py D:/Games/SDGO/GOnline.exe
```

覆盖通用目标/武器决策、形态切换、升空横移请求、充能保持、真实 core/DLL 接入夹具、
原始 Lua 字节码兼容回放、Boost/弹药库布局、逐目标纯视线、弹道数学、安装升级保留配置。
原生夹具验证调用约定、opaque 参数/AL 返回、射线起点、运行时弹速学习、曲线排除、原生路径分配器及禁用清理。
另行读取支持版本的实际 EXE，核对虚表的字节偏移、函数清栈和返回值约定，避免夹具重复了错误接口假设。
这些是接口与受控场景验证，不是实际游戏引擎或真实地图已经验收。

独立弹道基准把目标按固定速度移动、弹体按固定速度积分，在 225 个三维场景中全部进入半径 8 的目标范围，
最大离散误差约 4.134 世界单位。该结果检验预判公式与单位一致性，不能解释为实战命中率 100% 或已达 90%。
变向、加速、掩体、曲线弹道、原生引导与网络状态仍需实际游戏调校。

## 隔离环境与手动验收

`tools/prepare_battle_ai_sandbox.ps1 -ClientDir D:/Games/SDGO` 只在当前工作树
`build/battle_ai_sandbox/client` 复制资产、清除保存的登录数据并安装当前构建；
不启动客户端，不复制启动器或聊天记录，不连接 FRP 或任何现有后端。
手动登录/对战须另行指定独立测试后端。

本轮不触碰原客户端、FRP 工作树、服务、数据库、端口和隧道。
仍需手测：不同机体/变形、对射横摆及推进消耗、充能/持续光束、同脚本多台机器人、
拐角/狭道/高低差/可破坏障碍、多人端同步、受击/死亡/复活，以及有真实分母的实战命中率。
原生导航数据缺失或路径不可达时保守处理，不承诺所有地图都能完成三维飞行寻路。

## English summary

The optional client add-on selects tactics from live form/weapon data, focuses comparable
in-range low-HP enemies, drives native attack intent and ranged-duel air strafing, follows
native routes and corrects native shot rays with validated/measured projectile lead.
Ammo, cooldowns, physics, homing and damage remain native. Automated adapter/VM/geometry
fixtures do not prove real-map or multiplayer acceptance, nor a live 90% hit rate.
