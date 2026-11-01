# SDGO Mod 框架

为 SD敢达 Online 客户端和私服提供基础修复与可选扩展。
非官方爱好者项目，不包含游戏文件。[English](README.md)

[安装说明](docs/setup.zh-CN.md) · [统一 Mod Manager](docs/mod-manager.zh-CN.md) · [最新发布](https://github.com/crabsatellite/SDGO-Mod-Framework/releases/latest)

## 安装与使用

1. 按[安装说明](docs/setup.zh-CN.md)安装所选功能需要的客户端或服务器组件。
2. 通过 **SDGO Mod Manager** 安装、配置和管理功能；玩家仍使用平常的游戏启动器。
3. 各 Mod 的配置、玩法、分发和验证细节，进入下表对应文档查看。

先安装基础修复，再按需选择扩展。Beta、Experimental 是发布标记，与功能分类独立。
包内 `CONTENTS.md` 和 `sdgo/catalog.json` 列出该包实际包含的功能。

## 基础修复

<!-- mod-category:fixes -->

| 功能 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| 输入法提交修复 | 修正输入法文本提交 | `ime_fix` | 客户端 | [使用说明](docs/ime-fix.zh-CN.md) |
| 活动扭蛋币种显示 | 显示与原生扣费一致的币种 | `capsule_currency_fix` | 客户端 | [使用说明](docs/capsule-currency-fix.md) |
| 输入法卡死修复 | 恢复兼容的窗口查找 | `findwindow_fix` | 客户端 | [使用说明](docs/findwindow-fix.zh-CN.md) |
| 启动崩溃修复 | 跳过引用缺失机体的读取界面 | `startup_crash_fix` | 客户端 | [使用说明](docs/startup-crash-fix.zh-CN.md) |
| Agent 中继日志修复 | 修正中继日志 | `agent_relay_log_fix` | Agent 服务器 | [使用说明](docs/agent-relay-log-fix.zh-CN.md) |
| 军衔回退修复 | 防止军衔错误回退 | `rank_progression_fix` | World 服务器 | [使用说明](docs/rank-progression-fix.zh-CN.md) |
| 新账号起始资料修复 | 配置初始货币、军衔和机体 | `new_account_profile` | World 服务器 | [使用说明](docs/new-account-profile.zh-CN.md) |
| 商城租赁与中文选项修复 | 明确租赁期限与商品标签 | `shop_labels` | 客户端 | [使用说明](docs/shop-labels.zh-CN.md) |

<!-- /mod-category:fixes -->

## 玩法扩展

<!-- mod-category:gameplay -->

| 功能 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| 任务 AI 僚机 | 为任务队伍加入 AI 队友 | `coop_ai`、`mission_ai` | 对战服务器 + 客户端 | [使用说明](docs/coop-wingmen.zh-CN.md) |
| 对战 AI（Beta） | 为 PvP 加入 AI 对手和队友 | `pvp_ai`、`match_ai` | 对战服务器 + 所有参战客户端 | [使用说明](docs/pvp-ai.zh-CN.md) |
| AI 机体目录（Beta，独立附加包） | 用配套目录数据扩充 AI 机体池 | `ai_catalog` | 对战服务器 + 所有参战客户端 | [使用说明](docs/ai-catalog.zh-CN.md) |
| 战斗贡献奖励（Beta） | 配置战斗贡献奖励与倍率 | `battle_rewards` | 对战服务器 + Manager | [使用说明](docs/battle-rewards.zh-CN.md) |
| 收藏扭蛋（Beta） | 抽取未曾获得的机体形态 | `collection_capsule` | World 服务器 + 客户端资源 | [使用说明](docs/collection-capsule.zh-CN.md) |
| 每局随机机体（Beta） | 每场随机使用真人已拥有的机体 | `room_random` | 对战服务器 | [使用说明](docs/room-random.zh-CN.md) |
| 长房名（Beta） | 支持更长房名与组合指令 | `room_titles`、`room_titles_agent`、`room_titles_server` | 客户端 + Agent + Battle | [使用说明](docs/room-titles.zh-CN.md) |
| 战斗 AI 难度（Beta） | 选择 Easy、Medium 或 Hard 战斗 AI | `battle_ai`、`battle_ai_server` | 对战服务器 + 所有玩家 | [使用说明](docs/battle-ai.md) |
| 机库管理（Beta） | 整理机体，预览并确认重复兑换 | `hangar_manager` | 客户端 | [使用说明](docs/hangar-manager.zh-CN.md) |
| 副官补给与管理（Beta） | 配置副官头像与补给奖励 | `operator_catalog` | 客户端 + 服主 SQL 适配器 | [使用说明](docs/operator-assistant.zh-CN.md) |
| 扭蛋机编辑器（Beta） | 配置机台、奖池与头像 | `capsule_catalog` | 客户端 + 服主 SQL 适配器 | [使用说明](docs/capsule-catalog.zh-CN.md) |
| 每日双倍时间（Beta） | 配置每日双倍奖励时段 | `daily_rewards` | 对战服务器 | [使用说明](docs/daily-rewards.zh-CN.md) |

<!-- /mod-category:gameplay -->

<a id="long-room-title-commands"></a>

AI 与房名组合指令：[完整语法和示例](docs/room-commands.zh-CN.md)。

## 联机与服主工具

<!-- mod-category:network -->

| 功能 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| FRP 联机（Beta） | 配置联机端点与 UDP 端口映射 | `net_route`、`agent_udp_pool` | 客户端 + Agent 服务器 | [使用说明](docs/frp-distribution.zh-CN.md) |
| 商城商品管理（Beta） | 在 Mod Manager 中管理原生商城商品 | `shop_catalog` | World 服务器 + Manager | [使用说明](docs/shop-catalog.zh-CN.md) |

<!-- /mod-category:network -->

## 开发诊断

诊断默认关闭。自动修复在 Manager 的归档中查看，内部字段与诊断参数在开发视图展开。

<!-- mod-category:development -->

| 工具 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| 发包日志 | 检查游戏发包 | `net_probe` | 客户端 | [使用说明](docs/net-probe.md) |
| 扭蛋价格探针（Experimental） | 检查扭蛋价格刷新 | `capsule_price_probe` | 客户端 | [使用说明](docs/capsule-price-probe.md) |
| GameTest（Experimental） | 在隔离的原生游戏实例中运行断言 | `game_test_client`、`game_test_server` | 客户端 + 对战服务器 | [使用说明](docs/game-test.zh-CN.md) |

<!-- /mod-category:development -->

## 项目文档

[构建与开发接口](docs/framework.md) · [贡献与 AI 披露规则](docs/contributing.zh-CN.md) · [配置与排错](docs/setup.zh-CN.md)

MIT，见 [LICENSE](LICENSE)。第三方许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
