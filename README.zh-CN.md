# SDGO Mod 框架

为 SD敢达 Online 客户端和私服提供基础修复与可选扩展。所有 Mod 共用版本校验、加载、配置和更新框架。
非官方爱好者项目，不包含游戏文件。[English](README.md)

**先安装基础修复，再按需要选择扩展。** Beta 是功能的发布标记，与分类独立。

[安装与配置](docs/setup.zh-CN.md) · [最新发布](https://github.com/crabsatellite/SDGO-d3d9-proxy/releases/latest) · [开发接口](docs/framework.md)

## 基础修复

推荐安装并保持启用，用于修复启动、输入法等兼容性问题。

<!-- mod-category:fixes -->
| 功能 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| 输入法提交修复 | 修正输入法提交内容的长度计算 | `ime_fix` | 客户端 | [使用说明](docs/ime-fix.zh-CN.md) |
| 输入法卡死修复 | 恢复输入法需要的窗口查找，兼容已有插件 | `findwindow_fix` | 客户端 | [使用说明](docs/findwindow-fix.zh-CN.md) |
| 启动崩溃修复 | 避开引用缺失机体的随机读取界面 | `startup_crash_fix` | 客户端 | [使用说明](docs/startup-crash-fix.zh-CN.md) |
| Agent 中继日志修复 | 修正原生中继日志的格式错误 | `agent_relay_log_fix` | Agent 服务器 | [使用说明](docs/agent-relay-log-fix.zh-CN.md) |
| 军衔回退修复 | 防止较高子等级误将当前军衔降级，例如 `15-1 → 5-2` | `rank_progression_fix` | World 服务器 | [使用说明](docs/rank-progression-fix.zh-CN.md) |
| 新账号起始资料修复 | 新账号默认 5,000 GP、0 点券、下士；首次建档自动赠送最多一台机体，保留已有养成 | `new_account_profile` | World 服务器 | [使用说明](docs/new-account-profile.zh-CN.md) |
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
| 任务 AI 僚机 | 用 `!ai` 给任务队伍加入 AI 队友 | `coop_ai`、`mission_ai` | 对战服务器 + 客户端 | [使用说明](docs/coop-wingmen.zh-CN.md) |
| 对战 AI（Beta） | 在 PvP 加入 AI 对手和队友，支持计分、复活、卡组和观战 | `pvp_ai`、`match_ai` | 对战服务器 + 所有参战客户端 | [使用说明](docs/pvp-ai.zh-CN.md) |
| 每局随机机体（Beta） | 用 `!rand` 从各自机库每局随机，卡组严格十点，可与 `!ai` 组合 | `room_random` | 对战服务器 | [使用说明](docs/room-random.zh-CN.md) |
| 长房名（Beta） | 任务与对战使用完整房名，读取第 20 字节后的规则 | `room_titles`、`room_titles_agent`、`room_titles_server` | 客户端 + Agent + Battle | [说明](docs/room-titles.zh-CN.md) |
<!-- /mod-category:gameplay -->

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
<!-- /mod-category:network -->

## 开发诊断

面向开发与排错，默认关闭。

<!-- mod-category:development -->
| 工具 | 作用 | Mod | 安装位置 | 说明 |
|---|---|---|---|---|
| 发包日志 | 记录发包，辅助协议排错 | `net_probe` | 客户端 | [使用说明](docs/net-probe.md) |
<!-- /mod-category:development -->

## 安装与使用

1. 按[安装说明](docs/setup.zh-CN.md#安装)安装客户端部分；跨端功能还需要对应的服务器部分。
2. 使用平常的启动器进入游戏，按各功能说明启用扩展。
3. 更新使用安装脚本，保留个人配置；分类、整项功能和单个 Mod 都有独立开关。

包内 `CONTENTS.md` 和 `sdgo/catalog.json` 是该包的功能目录。
[安装、卸载、配置与排错](docs/setup.zh-CN.md) · [构建与新增扩展](docs/framework.md#adding-a-feature)

MIT，见 [LICENSE](LICENSE)。第三方许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
