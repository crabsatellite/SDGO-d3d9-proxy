# SDGO Mod 框架

为 SD敢达 Online 客户端和私服提供基础修复与可选扩展。所有 Mod 共用版本校验、加载、配置和更新框架。
非官方爱好者项目，不包含游戏文件。[English](README.md)

**统一入口：[SDGO Mod Manager](docs/mod-manager.zh-CN.md)。** 日常界面支持玩家和服主管理客户端、服务器、
商城、副官、扭蛋机、奖励与联机；自动 Bug 修复收在归档，内部代号和诊断参数在开发视图展开。
每个功能可复制 AI 配置说明或 GitHub Bug 报告指令；[修复 PR 必须披露实际使用的模型](docs/contributing.zh-CN.md)。

**先安装基础修复，再按需要选择扩展。** Beta 是功能的发布标记，与分类独立。

[安装与配置](docs/setup.zh-CN.md) · [最新发布](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest) · [开发接口](docs/framework.md)

## 基础修复

包含客户端兼容性修复、服务端结算修复与新账号起步规则。各项均可单独停用。

<!-- mod-category:fixes -->
| 功能 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| 输入法提交修复 | 修正输入法提交内容的长度计算 | `ime_fix` | 客户端 | [使用说明](docs/ime-fix.zh-CN.md) |
| 输入法卡死修复 | 恢复输入法需要的窗口查找，兼容已有插件 | `findwindow_fix` | 客户端 | [使用说明](docs/findwindow-fix.zh-CN.md) |
| 启动崩溃修复 | 避开引用缺失机体的随机读取界面 | `startup_crash_fix` | 客户端 | [使用说明](docs/startup-crash-fix.zh-CN.md) |
| Agent 中继日志修复 | 修正原生中继日志的格式错误 | `agent_relay_log_fix` | Agent 服务器 | [使用说明](docs/agent-relay-log-fix.zh-CN.md) |
| 军衔回退修复 | 防止较高子等级误将当前军衔降级，例如 `15-1 → 5-2` | `rank_progression_fix` | World 服务器 | [使用说明](docs/rank-progression-fix.zh-CN.md) |
| 新账号起始资料修复 | 调整新账号的初始货币、军衔和机体发放，保留已有进度 | `new_account_profile` | World 服务器 | [使用说明](docs/new-account-profile.zh-CN.md) |
| 商城租赁与中文选项修复 | 悬停、购买和赠送窗口明确显示租赁/永久，支持配套 Unicode 标签；可独立分发 | `shop_labels` | 客户端 | [使用说明](docs/shop-labels.zh-CN.md) |
<!-- /mod-category:fixes -->

军衔修复通过 WorldServer 的框架加载，将对应结算调用交给 Mod 自己的修正版过程，保留原生过程，无需更新玩家客户端。

<details>
<summary>查看输入法修复效果</summary>

![游戏大厅中使用输入法](docs/findwindow-fix-ime.png)

</details>

## 玩法扩展

按需使用，让单机或联机玩法更完整。NPC、机体池、随机机体等玩法能力归入这一类。

<!-- mod-category:gameplay -->
| 功能 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| 任务 AI 僚机 | 用 `!ai` 给任务队伍加入 AI 队友；[长房名组合用法](#long-room-title-commands) | `coop_ai`、`mission_ai` | 对战服务器 + 客户端 | [使用说明](docs/coop-wingmen.zh-CN.md) |
| 对战 AI（Beta） | 在 PvP 加入 AI 对手和队友，支持计分、复活、卡组和观战；[长房名组合用法](#long-room-title-commands) | `pvp_ai`、`match_ai` | 对战服务器 + 所有参战客户端 | [使用说明](docs/pvp-ai.zh-CN.md) |
| AI 机体目录（Beta，独立附加包） | 用服主生成的配套数据扩充 LV1–LV5 机体池，关闭并重启后恢复原池；[长房名组合用法](#long-room-title-commands) | `ai_catalog` | 对战服务器 + 所有参战客户端 | [使用说明](docs/ai-catalog.zh-CN.md) |
| 战斗贡献奖励（Beta） | 原生贡献结算、持久 MB 入账和服主倍率控制 | `battle_rewards` | 对战服务器 + 服主工作器 | [使用说明](docs/battle-rewards.zh-CN.md) |
| 收藏扭蛋（Beta） | 十万 GP 抽取未曾获得的初始形态；通过 Mod Manager 配置和分发 | `collection_capsule` | World 服务器 + 客户端机台资源 | [使用说明](docs/collection-capsule.zh-CN.md) |
| 每局随机机体（Beta） | 用 `!rand` 从各自机库每局随机，卡组严格十点，可与 `!ai` 组合；[长房名组合用法](#long-room-title-commands) | `room_random` | 对战服务器 | [使用说明](docs/room-random.zh-CN.md) |
| 长房名（Beta） | 任务与对战使用完整房名，读取第 20 字节后的规则 | `room_titles`、`room_titles_agent`、`room_titles_server` | 客户端 + Agent + Battle | [说明](docs/room-titles.zh-CN.md) |
| 机库管理（Beta） | 一键整理可移动机体，预览并确认重复机体兑换原生 GB 或道具奖励 | `hangar_manager` | 客户端 | [使用说明](docs/hangar-manager.zh-CN.md) |
| 副官补给与管理（Beta） | 配置头像与掉落，默认 5 位全员永久补给官，旧记录原位迁移 | `operator_catalog` | 客户端 + 服主助手/SQL 适配器 | [使用说明](docs/operator-assistant.zh-CN.md) |
| 每日双倍时间（Beta） | 默认服务器本地时间 18:00–22:00，正常对战的 G 币、驾驶员与机体经验翻倍，服主可配置时段 | `daily_rewards` | 对战服务器 | [使用说明](docs/daily-rewards.zh-CN.md) |
<!-- /mod-category:gameplay -->

进入机库后点击「**整理机库**」「**重复兑换…**」，也可按 **F8** 打开管理窗口。
相同状态的未培养机体默认保留两台，已培养机体不参与兑换；锁定、出击、舰队、租赁和未知状态机体均受保护。
正常操作由原生回包同步并刷新机库，无需重新登录；结果无法确认时停止批次，提示重新登录。
真实游戏效果仍需手动验收；安装、预览规则和奖励领取见[机库管理说明](docs/hangar-manager.zh-CN.md)。

每日双倍仅需服主更新对战服务器，玩家沿用现有客户端。配置、关闭和实测步骤见
[每日双倍教程](docs/daily-rewards.zh-CN.md#手动验收)。

副官管理通过包内 `tools/Start-Assistant.ps1` 打开服主 UI，可修改名称、头像和掉落概率、数量。
自带设计图、经验、装饰、强化套件、扭蛋币 5 位全员永久补给官；旧持有记录原位迁移，保留当前装备与个人昵称。
服主应用服务端配置并分发客户端更新包后，玩家仍使用原启动器。商品只登记，上架和定价由商城模块负责。
游戏内显示、领取和结算仍需服主手动验收。[完整配置与迁移说明](docs/operator-assistant.zh-CN.md#默认补给副官)。

机体目录是**默认关闭的独立 Mod**，Base / Full 框架包不会直接安装它。服主用自己的数据库目录表和
配套客户端表生成 `catalog.bin`，安装到两端，再导出玩家增量包；玩家只需运行包内的一次性安装工具，
以后仍用原启动器。房名如 `!ai e2 a1 lv2` 会从扩展后的对应等级池随机抽取，不增加选择界面。
不改写 SQL 原表或游戏数据包；完整步骤见[服主安装与玩家分发教程](docs/ai-catalog.zh-CN.md)。

<a id="long-room-title-commands"></a>

### 长房名与组合指令

任务僚机、对战 AI、随机机体可以配合[长房名 Mod](docs/room-titles.zh-CN.md)，
把说明文字和多个指令放进同一房名。三端启用后，上限为 **128 个 Unicode 标量 / 512 字节 UTF-8**，
第 20 字节之后的指令也会读取。例如：

| 场景 | 完整房名示例 |
|---|---|
| 任务：请求 3 个 4 级僚机，真人每局随机 | `周末朋友任务房，慢慢打一起练习 !ai3 lv4 !rand` |
| 对战：2 个敌人、1 个队友，真人每局随机 | `朋友练习局，慢慢打欢迎新手 !ai e2 a1 lv4 !rand` |

人数仍受原有空位限制。`lv4`、`e2`、`a1` 必须放在 `!ai` 后、下一个 `!指令` 前。
服主需要启用客户端 `room_titles`、Agent 的 `room_titles_agent`、Battle 的 `room_titles_server`，
并更新使用中的 `coop_ai` / `pvp_ai` / `room_random`；玩家使用分发好的客户端正常启动。
这三个房名 Mod 默认关闭；AI 和随机机体功能仍需各自安装、启用和配置。
未启用长房名配套时，继续使用原生 20 字节短房名。
[完整语法与组合示例](docs/room-commands.zh-CN.md)。


<details>
<summary>查看 AI 僚机与对战短片</summary>

任务模式 AI 僚机：

![任务 AI 僚机](docs/coop-wingmen-demo.webp)

对战 AI（Beta），也支持死亡竞赛、Grid、队长和 Tag 模式：

![对战 AI 的队友与敌人交战](docs/pvp-ai-demo.webp)

</details>

## 联机与服主工具

连接、中继、端口映射和服主部署工具单独分类，由服主按网络环境配置。
这些工具与玩法扩展分别管理；安装包内的功能目录列出实际包含的工具。

<!-- mod-category:network -->
| 功能 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| FRP 联机（Beta） | 服主配置公网地址、端口映射与可复用 UDP 端口池 | `net_route`、`agent_udp_pool` | 客户端 + Agent 服务器 | [使用说明](docs/frp-distribution.zh-CN.md) |
| 商城商品管理（Beta） | Windows 服主窗口增删改查、定价、上下架与配置分发；沿用原生商城结算 | `shop_catalog` | World 服务器 + 服主管理窗口 | [使用说明](docs/shop-catalog.zh-CN.md) |
<!-- /mod-category:network -->
商城服主双击 `SdgoShopManager.exe` 管理商品，操作方式与发钱工具一致。新增/编辑先存草稿，
点击“应用到商城”后生效；玩家继续使用原生商城。[按钮测试步骤](docs/shop-testing.zh-CN.md)。
租赁/中文选项修复与商城管理仍分别提供独立安装包；商品配置不包含服主数据库密码。


## 奖励规则与验证

[战斗贡献奖励（Beta）](docs/battle-rewards.zh-CN.md)已接入原生结算和服主钱包工作器。
服主在辅助入口的 `/rewards` 页面调整 GB/MB 基础倍率；玩家继续使用原客户端和启动器。
击杀、实际伤害、助攻、任务小怪与 Boss 贡献参与结算，对战 MVP 额外获得 MB。
带 AI 的对战与纯真人同档；每多一名有效真人再乘 `1.1`，并与已有双倍时段相乘。
例如基础十倍、双倍时段、四名真人为 `10 × 2 × 1.1³ = 26.62` 倍。

安装完整包中的 `rewards/` 工具到所有参与结算的 BattleServer，共用一个服主 workspace，
再用 `tools/Start-Battle-Rewards.ps1` 启动钱包工作器与辅助入口。
该入口可挂载已安装的副官助手，在同一端口保留原有模块；关闭浏览器不会停止入账工作器。
更新前需停止目标 BattleServer，安装后重启；随后调倍率可以热更新。
具体步骤见[安装、启动与恢复教程](docs/battle-rewards.zh-CN.md)，
数值与封顶见[完整奖励规则](docs/battle-reward-design.zh-CN.md)。
真实账号对局、HUD 和重连仍由服主手动验收；安装保留其他功能与原有状态。

## 开发诊断

面向开发与排错，默认关闭。

<!-- mod-category:development -->
| 工具 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| 发包日志 | 记录发包，辅助协议排错 | `net_probe` | 客户端 | [使用说明](docs/net-probe.md) |
| GameTest（Experimental） | 在独立的原生游戏实例里运行断言 | `game_test_client`、`game_test_server` | 客户端 + 战斗服务器 | [使用说明](docs/game-test.zh-CN.md) |
<!-- /mod-category:development -->

## 安装与使用

1. 按[安装说明](docs/setup.zh-CN.md#安装)安装所选功能对应的宿主部分；新账号规则和军衔修复只安装到 World，玩家无需客户端组件。
2. 使用平常的启动器进入游戏，按各功能说明启用扩展。
3. 更新使用安装脚本，保留个人配置；分类、整项功能和单个 Mod 都有独立开关。

包内 `CONTENTS.md` 和 `sdgo/catalog.json` 是该包的功能目录。
[安装、卸载、配置与排错](docs/setup.zh-CN.md) · [构建与新增扩展](docs/framework.md#adding-a-feature)

MIT，见 [LICENSE](LICENSE)。第三方许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
