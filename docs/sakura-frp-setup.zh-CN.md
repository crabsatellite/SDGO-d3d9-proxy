# Sakura FRP 联机设置

当前安装与分发流程统一见[可分发 FRP 网络 Mod](frp-distribution.zh-CN.md)。

服主使用自己的 Sakura 设置 TCP 入口和 UDP 池隧道，将实际公网端点填入配置；
通用 Mod 不绑定账号、节点、盘符、隧道名称或在线人数。玩家安装 Mod 和公开连接配置后，
使用原来的启动器，不需要“开始联机”脚本，也不需要安装 Sakura。

客户端使用 `net_route`；每个 Agent 使用 `agent_udp_pool` 和 `agent_relay_log_fix`。
网络设置放在各 Mod 的 `config.ini`，旧 `mod.ini` 设置继续兼容。首装、更新、单独卸载、
配置生成器、32 条路由限制及验收边界均在分发指南说明。

数据库、服务管理和本机多开不属于网络 Mod 的依赖。自动验证与手动游戏验收的边界见
[分发指南](frp-distribution.zh-CN.md#验证边界)。自动测试不等于完整真人外网对局验收。
