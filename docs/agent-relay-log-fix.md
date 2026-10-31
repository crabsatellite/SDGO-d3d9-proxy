# Agent relay log fix

Core fix for the Agent server: corrects a native relay log format that can read
an integer IP address as a string. Enabled by default on supported Agent builds.
中文：[agent-relay-log-fix.zh-CN.md](agent-relay-log-fix.zh-CN.md)

Install the server package on Agent hosts with `install_server.ps1 -Role Agent`.
The [FRP guide](frp-distribution.md) covers owner/player networking configuration.
The fix can be disabled with `[mods] agent_relay_log_fix=0` in `sdgo/sdgo.ini`.
