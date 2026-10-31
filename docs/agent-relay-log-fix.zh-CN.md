# Agent 中继日志修复

基础修复：修正 Agent 原生中继日志把整数 IP 当作字符串读取的问题。支持的 Agent 版本默认启用。
[English](agent-relay-log-fix.md)

在 Agent 宿主使用 `install_server.ps1 -Role Agent` 安装服务器部分。
联机配置见 [FRP 分发指南](frp-distribution.zh-CN.md)。
在 `sdgo/sdgo.ini` 的 `[mods]` 设置 `agent_relay_log_fix=0` 可关闭。
