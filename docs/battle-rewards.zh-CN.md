# 战斗贡献奖励（Beta）

此功能已接入 BattleServer 的原生最终结算：按胜负、击杀、实际伤害、助攻、任务小怪与 Boss 贡献
计算 GB/MB；对战 MVP 另有 MB 奖励。服主通过辅助入口调倍率，玩家沿用原客户端和启动器。

**Beta 验收边界：** 自动检查已覆盖真实 core/DLL、隔离原生 SQL 钱包过程和真实 Agent 转发；
使用真实账号的完整对局、HUD 和重连仍由服主手动验收。安装不会自动停止正在使用的服务。

[English](battle-rewards.md) · [完整规则与初始数值](battle-reward-design.zh-CN.md)

## 规则与已有双倍功能

```text
最终 GB/MB = 向下取整一次(
  一倍基础和贡献奖励 × 服主基础倍率 × 双倍时段系数 × 1.1^(有效真人数−1) × 有效参战时长系数
)
```

带 AI 的对战与纯真人对战同档。真人数量按整场有效参战账号计数，AI 和观战者不算。
例如基础十倍、双倍时段、四名真人为 `10 × 2 × 1.331 = 26.62` 倍。

双倍时段由已启用的 `daily_rewards` 提供。奖励 Mod 在它之后处理 GB，使用独立的一倍值重算，
不对已经乘过双倍或改过十倍的旧表值再乘一次；MB 使用相同活动系数。角色与机体经验继续由
原来的双倍组件处理。未加载该组件时活动系数为 1。改其时段后按该组件要求重启服务；本功能的
基础倍率和奖励数值可以在辅助入口热更新，不需要重启 BattleServer。

默认三分钟达到完整时长系数。计算规则的最低有效时长为 30 秒，**仍须通过游戏原生资格检查**；
原生代码拦截的短局、无效或退出资格不会被本功能绕过。击杀为零仍保留符合资格的胜负保底。
助攻按该目标一条生命中至少 10% 的实际 HP 损失计，最后一击获得击杀、不再重复计助攻。
任务 Boss 以服主当前数据库的任务/NPC 身份映射判定，按有效贡献分配，不用外观或最后一击猜测。

同样三分钟、四名真人、十倍＋双倍：任务示例为 4,871 GB/13 MB，对战胜利示例为
21,296 GB/90 MB；同一对战同时为 MVP 则为 26,620 GB/250 MB。具体条件和全部封顶值见完整规则。

## 服主首次安装

使用完整框架包里的 `rewards/`，或独立的 Battle Rewards 工具包；以下命令在该目录执行。
需要 Python 3.10+、Windows PowerShell 5.1 和服主已有的 SQL Server ODBC 连接能力：

```powershell
python -m pip install -r requirements-rewards.txt
```

选用包含 `[DB Game]` 和 `[DB StoreBilling]` 两段的原生服务器 INI（例如 WorldServer.ini）。
凭据只在服主本机读取，不发给玩家，也不写入命令行或导出包。先自行结束相关对局并关闭要更新的
那个 BattleServer。其他服务器、客户端、存档和账号不需要停止或重置。

先生成只读计划：

```powershell
python tools/install_battle_rewards.py --package-root . --battle-dir "D:\SDGO\Server\BattleServer1" --workspace "D:\SDGO-Owner\Rewards" --server-ini "D:\SDGO\Server\WorldServer\WorldServer.ini"
```

检查计划没有阻断项后，使用相同参数加 `--apply`：

```powershell
python tools/install_battle_rewards.py --package-root . --battle-dir "D:\SDGO\Server\BattleServer1" --workspace "D:\SDGO-Owner\Rewards" --server-ini "D:\SDGO\Server\WorldServer\WorldServer.ini" --apply
```

安装器会更新随包最新的加载器、core 与本 Mod，备份改动，合并所需符号而保留其他定义，并读取
原生 Boss 身份表。数据库安装只增加本功能的版本化回执表/过程，不给任何账号试发货币，不改原生
奖励表或 `sp_PocketGC_IO`。配置写成 Windows INI 支持的 UTF-16LE，支持中文和空格路径。
发生失败时保留真实写入状态和恢复收据，并尽力保持 Mod 关闭。

每个参与结算的 BattleServer 都需安装一次，共用同一 workspace；实例身份会自动区分。
全局 `[categories] gameplay=0`、`[features] battle-rewards=0` 或 `[mods] battle_rewards=0`
仍会关闭功能，安装器不会强行改动你对其他功能的开关。处理计划中的相关提示后再启用。

## 启动奖励工作器和辅助入口

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Start-Battle-Rewards.ps1 -Workspace "D:\SDGO-Owner\Rewards" -StoreServerIni "D:\SDGO\Server\WorldServer\WorldServer.ini"
```

入口先发布策略，隐藏启动 MB 工作器，然后显示本机辅助页面的实际 URL。打开浏览器页面后，
按原来的方式启动 BattleServer。保持该服主进程运行；关闭浏览器标签不会停止工作器，结束入口
进程则按其所有权记录关闭新启动的工作器。

页面显示“已连接”需要：服务已加载当前策略、结算 Hook 与原生网络就绪、MB 工作器健康，且心跳
没有过期。仅保存配置不会伪造连接状态。改倍率后查看所有相关实例是否已加载同一版本。

原生 GB 结果字段最大为 65,535。页面报告当前整套策略的精确安全倍率上限：默认配置、最多十二名
真人和双倍时段下，基础倍率上限为 `10.440894`。超过范围会明确拒绝并显示未就绪，不会溢出、绕回
或静默截断。可在安全范围内调倍率，也可调整完整一倍数值后重新计算范围。

需要与现有辅助模块共用端口时，入口可装配**已安装助手的服务器工厂**：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Start-Battle-Rewards.ps1 -Workspace "D:\SDGO-Owner\Rewards" -StoreServerIni "D:\SDGO\Server\WorldServer\WorldServer.ini" -Port 8765 -AssistantRoot "D:\SDGO-Assistant" -AssistantWorkspace "D:\SDGO-Owner\Assistant" -AssistantClient "D:\SDGO-Client" -AssistantSnapshot "D:\SDGO-Owner\operator-snapshot.json"
```

这会启动同端口的共享实例并保留现有模块路由，不会给已经运行的 Python 进程动态打补丁或抢占端口。
已有同端口实例需由服主按自己的服务管理方式切换；入口不会停止任何旧助手或游戏。
未配置现有助手时，独立奖励页面仍可工作。

## 入账、断线和恢复

GB 使用原来的 World/钱包通路，替换最终金额后正常入账与显示，不另发第二份完整 GB。
MB 先进入持久队列，调用原生 `sp_PocketGC_IO` 并以本局/账号/计划摘要去重。工作器提交后重读当前 MB，
以已有 `0x064B` 消息通知在线客户端；不把历史回执里的旧余额重放到玩家界面。

工作器响应丢失后可重试，不会重复加 MB。已经断线或会话被回收时，保留到账结果，跳过旧路由通知，
由下次正常登录/商店现金查询刷新。工作器中断时，辅助页面会显示失联；修复并启动相同 workspace 后
继续处理原队列，不要删除 runtime 中的待处理作业或数据库回执。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Stop-Battle-Rewards.ps1 -Workspace "D:\SDGO-Owner\Rewards"
```

停止入口校验 PID、创建时间、完整命令行和路径，只处理它自己记录的工作器；不会按进程名批量关闭。

## 禁用与卸载

退出相关对局、关闭对应 BattleServer。可在其 `sdgo/sdgo.ini` 的 `[mods]` 下设置 `battle_rewards=0`，
或使用安装器先看卸载计划，再加 `--apply`：

```powershell
python tools/install_battle_rewards.py --battle-dir "D:\SDGO\Server\BattleServer1" --workspace "D:\SDGO-Owner\Rewards" --uninstall
python tools/install_battle_rewards.py --battle-dir "D:\SDGO\Server\BattleServer1" --workspace "D:\SDGO-Owner\Rewards" --uninstall --apply
```

只归档该 Mod，保留公共框架、其他 Mod、数据库账目、配置与队列。未确认完的到账作业先处理完，
不要通过删除回执来“重置奖励”。重新开启服务器后恢复原生奖励路径，已有奖励不会被回滚。

## 已测范围与手动验收

- 精确 C++/Python 奖励计算、范围拒绝、真人复利和同局策略冻结。
- 真实 core/DLL 的十种结算入口、有效伤害、任务 Boss、最终 GB 改写、MB 工作器和余额消息。
- 与独立双倍模块同载，GB/MB 只消费一次活动倍率，经验保持该组件行为。
- 复制原生钱包过程到新隔离数据库，验证真实事务、断线后重试、余额重读和零遗留测试库。
- 复制的真实 Agent 从 BATTLE 连接接收 `0x064B`，向合成客户端连接转发完全一致的十字节消息；独立端口复测通过。
- 安装/更新/卸载保护、中文路径、配置版本、进程身份、文件并发和原子发布检查。

这些检查不使用真实账号对局，也不改生产余额。服主手动验收时，记录一次对战胜/负与 MVP、一次任务
Boss、小怪/伤害变化、双倍窗口边界、倍率更新、多人数量变化和断线重连，并对照余额差额与回执。
源代码中的复现命令见 [规则文档](battle-reward-design.zh-CN.md#复现)。
