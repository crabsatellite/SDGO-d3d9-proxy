# Battle AI 0.3 / 通用战斗 AI（开发预览）

这套插件为现有对战 Mod 的机器人提供三档 AI，按当前机体、形态、武装数据选择打法。
随正常游戏启动生效，没有额外的“开始联机”入口。插件不保存 FRP 地址、账号或本机专用路径。

## 默认难度与房间控制

客户端装包后默认使用 **Medium**。房主沿用对战 Mod 的房名命令，朋友不必逐人改配置。

| 房名例子 | 效果 |
|---|---|
| `!ai` / `!ai3` | 原有机器人数量规则，默认 Medium |
| `!ai3 easy` | Easy：原版 AI |
| `!ai e3 a1 medium` | Medium：Lv12、瞄准/预判、合法必杀、防背击；基础进攻决策沿用原版 |
| `!ai3 hard` | Hard：EX13、通用战术、高级动作、盾面防御、一个合法部件和一种限次道具 |

`lv1`–`lv5` 仍是既有 NPC 池筛选，不代表机体真实等级，也不代表难度。
房名优先于本机 `Difficulty` 设置；房名没有难度时，未自定义的客户端使用 Medium。
切换房间后读取新难度。只由拥有 AI 控制权的客户端执行决策。

Easy 保留原生机体等级、决策、瞄准与资源规则。新等级、部件、道具和服务端 SP 规则作用于
保留编号范围内的 PvP 机器人；原生任务、原生 AI 模式和 Boss 不接管这些玩家资源规则。
机体等级/部件在生成、换机和复活时走原生初始化，不能通过同一 generation 的重复调用叠加部件。

## 进阶行为与资源规则

- Hard 发现队友的当前刀已送达目标且目标处于真实受击状态时，尝试绕后并继续射击。
  被近战压制时优先远程，不能越过受击硬直强行射击。
- 有同目标队友远程支援时，Hard 在倒数第二刀确认接触后释放续段，合法离开近战动作，
  观察到一至两次真实出枪后重新连段并完成最后一刀。不会拿上一刀回执或计时器冒充命中/出枪。
- 甩刀默认每次合格近战起手有 80% 概率，跳过刀推并给机体短暂侧向动作，再恢复朝敌。
  原生出刀前摇、碰撞、冷却和受击仍执行，不改变玩家摄像机。
- 掩体机会默认 90% 选择，寻找较小暴露的可射击边缘；真实出枪后请求回撤，路径暂时失败会重试。
  遮挡后使用最后可见位置，重新取得真实射线后才射击。
- 走 A 使用真实出枪回执与原生动画开放的移动取消窗口；近战敌人接近或敌人正被牵制时，短步/推进与射击交替。
  它不改写真实武器 CD，也不伪造弹药。
- Medium 以上遇到背后威胁或夹击时，选择降低最坏背击风险的朝向及短途移位。
  Hard 按当前形态、武器姿态、盾的方向和命中类型使用完好盾；包括背盾、侧盾。
  盾值归零后立即放弃盾面朝向，分步快速转回正面防御。位移继续受原生硬直限制。
- Medium 以上按玩家资源和动作许可请求必杀，普通攻击不会被 SP 门槛阻挡。
  开局 SP 为零，通过原生攻/受击收益积累，复活不清掉已积累的 SP。
  服务端沿用玩家的原生入场/完成扣费及技能修正；合法受击取消窗口可优先反击实际砍自己的敌人。
  有支援的近战补枪连段不会被主动必杀中断。
- Hard 部件使用普通玩家可正常装备的新人类觉醒/格斗术，按武装潜力选择单个预设。
  混合近战主导武装也可选择格斗术；普通远程侧重推进。不会使用隐藏增强部件。
  部件遵守原生激活条件：新人类觉醒需 SP 20000 且 HP 不高于 40%，格斗术需 SP 10000。
  RangeType 3 不等于狙击资格，未证明资格时不自动给狙击专用部件。
- Hard 首次合格近战 HP 伤害可选择瞬移；三秒内两名不同敌人造成 HP 伤害可选择海市蜃楼。
  第一次成功装备后整局固定一种，复活不会补次数或改种类。次数由原生数据限制；
  瞬移/海市蜃楼的原生冷却分别为 50/45 秒，禁道具模式和死亡状态不使用。

这是原生接口与受控场景验证的开发预览。真实命中率、动画、复杂地形和多人同步仍需手动验收。
被四面包围时不能让所有方向同时处于正面；策略会减少最坏暴露并寻找移位机会。

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

先安装本项目 Mod 框架。客户端支持 `GOnline-54BE1568-00C97000`，服务端支持
`BattleServer-548A891D-0046C000`。客户端与主机需要使用同包对应组件。
关闭要更新的那一份副本后运行：

```powershell
./tools/install_battle_ai.ps1 -GameDir D:/Games/SDGO -Source ./build/stage/Release/client
# 房主在每个需要更新的 BattleServer 副本安装服务端组件：
./tools/install_battle_ai.ps1 -GameDir D:/Servers/BattleServer1 -Side Server -Source ./build/stage/Release/server
```

客户端更新 `battle_ai` 与 `match_ai`；服务端更新 `battle_ai_server` 与 `pvp_ai`。
增量安装合并各清单要求的符号，保留其它符号、其它 Mod、全局配置、用户 `config.ini` 及清单中的用户设置。
不会停止进程、启动游戏、修改 SQL/SIP 或设置隧道。预览包不含游戏文件、账号、SQL、FRP 配置或测试 VM。
同包安装器默认能找到 `client/` 或 `server/`；朋友只需要客户端部分，房主负责服务端部分。

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
./tests/battle_ai_test.ps1 -GameExe D:/Games/SDGO/GOnline.exe -ServerExe D:/Servers/BattleServer1/BattleServer.exe -AgentExe D:/Servers/AgentServer1/AgentServer.exe
./tests/battle_ai_install_test.ps1 -GameExe D:/Games/SDGO/GOnline.exe -ServerExe D:/Servers/BattleServer1/BattleServer.exe
python tools/verify_symbols.py D:/Games/SDGO/GOnline.exe
```

覆盖通用目标/武器决策、形态切换、升空横移请求、充能保持、真实 core/DLL 接入夹具、
原始 Lua 字节码兼容回放、Boost/弹药库布局、逐目标纯视线、弹道数学、安装升级保留配置。
原生夹具验证调用约定、opaque 参数/AL 返回、射线起点、运行时弹速学习、曲线排除、原生路径分配器及禁用清理。
另行读取支持版本的实际 EXE，核对虚表的字节偏移、函数清栈和返回值约定，避免夹具重复了错误接口假设。
服务端与 Agent 的原始代码校验还覆盖 SP 分支、限次道具、原生锁和状态包转发；
状态同步不生成命中或伤害，特殊状态包只更新原生 SP，不触碰 HP、受击状态或攻击动作。
这些是接口与受控场景验证，不是实际游戏引擎或真实地图已经验收。

独立弹道基准把目标按固定速度移动、弹体按固定速度积分，在 225 个三维场景中全部进入半径 8 的目标范围，
最大离散误差约 4.134 世界单位。该结果检验预判公式与单位一致性，不能解释为实战命中率 100% 或已达 90%。
变向、加速、掩体、曲线弹道、原生引导与网络状态仍需实际游戏调校。

## 隔离环境与手动验收

`tools/prepare_battle_ai_sandbox.ps1 -ClientDir D:/Games/SDGO` 只在当前工作树
`build/battle_ai_sandbox/client` 复制资产、清除保存的登录数据并安装当前构建；
不启动客户端，不复制启动器或聊天记录，不连接 FRP 或任何现有后端。
手动登录/对战须另行指定独立测试后端。

本轮没有修改原客户端、FRP 工作树、运行服务、数据库、端口和隧道。
仍需手测：不同机体/变形、对射横摆及推进消耗、充能/持续光束、同脚本多台机器人、
拐角/狭道/高低差/可破坏障碍、多人端同步、受击/死亡/复活，以及有真实分母的实战命中率。
原生导航数据缺失或路径不可达时保守处理，不承诺所有地图都能完成三维飞行寻路。

手动验收重点：

1. `!ai3`、`!ai3 easy`、`!ai3 hard` 切换房间，检查默认 Medium、Lv12/EX 与无配置分发。
2. Hard 对射横摆、接近、卡墙恢复、掩体射击后回撤、走 A；使用不同射击/近战/变形机体。
3. 队友抓住敌人时的绕后射击、倒数第二刀停段、一至两枪后完整连段；没有真实接触/支援时不误触发。
4. 背盾/两侧盾受击转向、盾破当帧开始回正、换形态/武器后的方向更新；夹击时重新站位。
5. SP 从零积累、受击许可窗口反击、完成/中断后扣费与技能关闭；普通射击不受 SP 影响。
6. 瞬移和海市蜃楼分别触发，检查同局互斥、复活不补次数、禁道具模式、次数耗尽和冷却。
7. 房主与朋友端同时观察动作、HP/SP、道具效果和复活/换机；实际命中率需记录真实分母。

## English summary

The matching client/host add-ons default to Medium and accept room-title difficulty.
Medium uses level 12, accurate aim, legal specials and rear-threat defence. Hard adds
EX13, teamwork, melee weaving, dash cancellation, cover/orbwalking, live shield posture,
one ordinary player part and one limited native consumable. Native player SP checks,
costs and activation rules apply. Ammo, cooldowns, physics and damage remain native.
Automated fixtures do not prove real-map/multiplayer acceptance or a live 90% hit rate.
