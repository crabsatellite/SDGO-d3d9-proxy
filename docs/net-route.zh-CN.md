# 客户端地址与端口映射（Beta）

`net_route` 把游戏尝试连接的逻辑地址转换为 Sakura 分配的公网地址和端口，并在
UDP 收包时恢复游戏期望的来源地址。它是Beta 客户端 Mod，默认关闭。
**当前地址映射层不等同于真人 PvP relay 已接通。**

请按[增量分发指南](frp-distribution.zh-CN.md)安装，更新框架，保留其他 Mod 和设置。
服主填写自己的隧道端点，玩家收到连接配置后沿用原启动器，无需专用启动脚本。
设置文件为 `sdgo/mods/net_route/config.ini`：

```ini
[settings]
LoginIPv4=192.0.2.10
LoginPort=15001
RouteCount=2
Trace=0
Route1=tcp,192.0.2.10,15001,your-node.example.invalid,25001
Route2=udp,192.0.2.10,10001,your-node.example.invalid,26017
```

`mod.ini` 保留版本、宿主、依赖和启用开关。没有 `config.ini` 时兼容旧清单的设置；
有该文件时整份采用新配置，不逐项混用。两个登录项都为空时保持旧的纯路由行为。
配置登录项时，必须为该逻辑 IPv4 和端口提供精确 TCP 规则。Mod 只在已验证的游戏登录
参数解析调用点使用它；其他参数解析、额外启动参数和启动器事件保持原样。

每条规则依次为：`协议,游戏逻辑IPv4,逻辑端口,公网域名或IPv4,公网端口`。
主机将隧道指向真实监听的本地端口。只对实际客户端连接使用的端点配置规则，数据库
和服务器间端口不会因为出现在服务器配置中就成为客户端路由。玩家无需服主的 Sakura 凭据。

规则数为 1–32。TCP/UDP 分开匹配；同协议下不允许重复逻辑端点，UDP 不允许两个规则
解析到相同的公网端点。TCP 可用多个逻辑地址映射到同一个入口。配置生成器中的
`login` 与 `login.aliases` 共享入口；可选的顶层 `tcp` 数组为其他已观测 TCP 服务
逐项指定独立入口。三组逻辑 TCP 地址/端口必须唯一，不同 TCP 规则仍可指向同一
物理入口；全部 TCP 与 UDP 规则合计不超过 32。逻辑地址须为精确 IPv4，没有全局
通配或“所有 UDP 都发 Agent”规则。DNS 在启动时解析一次；更改配置、节点 DNS
或端口后重启客户端。

`Trace=1` 记录每条规则头八次收发与最初 32 次未匹配目标，不记录包内容或登录字段。
`Trace=0` 只保留启动诊断。未知目标保持原行为；配置非法时整个 Mod 拒绝加载。

`net_probe` 也使用 `sendto` Hook，映射会话中请保持它关闭（默认就是关闭）。
Hook 冲突会触发框架回滚，不会只留下部分映射。此版覆盖主 EXE 的三个 Winsock
导入，不自动覆盖其他 DLL 或通过 GetProcAddress 动态调用的网络 API。

实现不改动本地 bind 端口、包内容或原始 sockaddr，保留错误码、非阻塞语义与
flags；UDP 来源还原覆盖空包、窥读及截断包。Mod 不操作 frpc 生命周期，仍由官方
Sakura 启动器管理隧道。

主机原生 UDP 端口池与配置生成方式见 [Sakura 设置说明](sakura-frp-setup.zh-CN.md)。
原生 relay 与双真人外网验收边界见[分发指南](frp-distribution.zh-CN.md#验证边界)。
