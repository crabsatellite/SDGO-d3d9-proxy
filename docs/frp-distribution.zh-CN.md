# 可分发的 FRP 网络 Mod

此包沿用 SDGO Mod Framework。客户端安装 `net_route`，Agent 安装
`agent_udp_pool` 和 `agent_relay_log_fix`。公共框架为本分支最新的 1.1.3 开发版本，
沿用 master 的 Mod API。每次安装、更新都替换为随包最新 core 和 loader，完成哈希回读；
旧包低于已安装框架时拒绝降级，提示取得新版包。AI、输入法、启动修复和全局开关保持原样。
现有真人多人游戏验收仍由服主手动完成。

## 从哪里开始

使用完整 `SDGO-ModFramework` 发行包时，先进入其中的 `frp` 文件夹；它与单独下载的
FRP Mod 包内容相同。以下命令均在该文件夹中执行。客户端和 Agent 目录先自行退出对应程序，
安装器不会替你停服或关闭其他游戏。主机保留 Sakura/frpc 转发工具，玩家只需游戏和 Mod。
首次安装必须提供 `-Profile`，避免未配置的 Agent 意外启用默认端口池；后续更新可省略该参数，保留已有配置。

## 服主配置一次，玩家照常启动

1. 服主用自己的 Sakura / frpc / 其他 TCP、UDP 转发工具设置隧道并保持运行。
   Mod 不管理账号、令牌、隧道生命周期、Docker 或数据库。
2. 填写 `owner-tools/owner-profile.example.json` 的逻辑服务地址、UDP 端口池、
   公网域名和实际分配的端口。每个 UDP 池端口需要一条 UDP 转发；公网端口无需连续。
   `udp_bind_ip` 必须与原生 Agent 通告给客户端的地址一致。该值不会替你修改服务配置。

   假设服务端实际使用 `192.0.2.10`，登录 TCP 为 `15001`，UDP 池为 `10001..10004`，
   服主转发工具的配置与示例文件应对应如下。地址和公网端口都须替换为你自己的实际值：

   | 协议 | 主机本地目标 | 公网入口 |
   |---|---|---|
   | TCP | `192.0.2.10:15001` | `node.example.invalid:25001` |
   | UDP | `192.0.2.10:10001` | `node.example.invalid:26017` |
   | UDP | `192.0.2.10:10002` | `node.example.invalid:27103` |
   | UDP | `192.0.2.10:10003` | `node.example.invalid:28842` |
   | UDP | `192.0.2.10:10004` | `node.example.invalid:29531` |

   公网端口不必等于本地端口，也不必连续。这里展示四个 UDP 槽；实际多人会话仍需手动验收。
3. 可选的离线工具生成配置：

   ```powershell
   python owner-tools/make_frp_profile.py --profile 我的服主配置.json --out 我的导出目录
   ```

   不运行生成器时，分别填写 `payload/client/mods/net_route/config.example.ini` 和
   `payload/agent/mods/agent_udp_pool/config.example.ini`，各另存为 `config.ini`，作为下面
   `-Profile` 的输入。这两份模板与 JSON 示例都包含同样的四个 UDP 槽。
4. 将 Agent 配置安装到每个参与服务的 Agent 目录。先自行关闭相应程序，安装器不会停服：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install_frp.ps1 -Target D:\SDGO\Agent1 -Role Agent -Profile D:\导出\agent\sdgo\mods\agent_udp_pool\config.ini
   ```

5. 玩家首次安装通用 Mod 包和这位服主的客户端配置：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\install_frp.ps1 -Target D:\Games\SDGO -Role Client -Profile D:\导出\client\sdgo\mods\net_route\config.ini
   ```

   然后使用原来的游戏启动器。无需“开始联机”脚本，无需安装 Sakura、Python 或 .NET。
   Mod 在已验证的原生登录参数解析调用点使用 `LoginIPv4` / `LoginPort`，再将它映射到
   配置的公网入口；不修改启动器文件、真实命令行、启动事件或其余参数。
   原启动器自身需要的服务、权限和事件仍须正常存在。
6. 后续改服或更新端点，只需给玩家 `player-profile.zip`，解压到游戏目录并重启游戏。
   这个小包只有玩家 `config.ini` 和说明；没有主机配置、账号、游戏本体或通用 Mod DLL。
   服主也可把已配置的 Mod 连同自己有权分发的客户端交给玩家。

## 文件与兼容性

```text
sdgo/mods/net_route/
  net_route.dll
  mod.ini               开发者清单：版本、宿主、依赖、enabled
  config.ini            服主交给玩家的连接设置
```

Agent 的配置位于 `sdgo/mods/agent_udp_pool/config.ini`。
`config.ini` 存在时，它的 `[settings]` 是该 Mod 的完整配置，不与旧参数逐项混合；
没有该文件则继续读取旧 `mod.ini` 的 `[settings]`。旧的纯地址映射配置可保持登录项为空。
`config.example.ini` 只是模板，不参与运行。

增量安装器更新公共框架和本角色的 FRP Mod，保留现有 `config.ini`（除非明确传入 `-Profile`）、
旧清单的设置和启用状态。明确传入配置会启用该角色的 Mod，但不会改全局 `[mods]` 开关。
如你曾在 `sdgo.ini` 写过 `net_route=0` / `agent_udp_pool=0`，仍由该开关控制。
安装器只追加缺失的 FRP 符号节，保留已有文件；同名符号不同就拒绝安装，避免旧包覆盖新 AI。
更新前的文件存入目标目录的 `sdgo/install-receipts/`。不提供保留旧 core 的兼容路径。
安装前校验代码、DLL 与运行时元数据的 SHA256 清单；缺失、混入旧 DLL 或框架目录多出未列出的文件都会拒绝写入。
服主填写的示例配置和说明文档可以修改，不会影响代码完整性校验。
安装器可直接接受已经放在目标 Mod 目录内的 `config.ini`；修改该文件后再次执行安装不会自拷贝报错。

`net_probe` 与 `net_route` 都占用 `sendto`，映射时应关闭诊断用的 `net_probe`。
框架拒绝重复 Hook 并回滚失败 Mod，安装器不替你关闭任务 AI、PvP AI 或其他功能。
Mod 只在启动时读取配置，不支持运行中装卸；修改配置后重启对应程序。

## 单独禁用和卸载

在 `sdgo/sdgo.ini` 的 `[mods]` 中写 `net_route=0` 可禁用客户端路由。
Agent 对应 `agent_udp_pool=0`；该池启用时需要 `agent_relay_log_fix`。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\install_frp.ps1 -Target D:\Games\SDGO -Role Client -Uninstall
powershell -NoProfile -ExecutionPolicy Bypass -File .\install_frp.ps1 -Target D:\SDGO\Agent1 -Role Agent -Uninstall
```

只移走本角色的 FRP Mod 到安装记录目录，保留其配置、公共框架、符号和其他 Mod。
符号只是地址定义，保留它们不会继续打补丁。卸载客户端路由后恢复原网络路径；若仍要
连接 FRP，应保留路由 Mod。通用 `install_client.ps1 -Uninstall` / `install_server.ps1 -Uninstall`
是卸载整套框架，不适用于只移除 FRP。

## 验证边界

精确地址映射不会把未知 P2P 地址全部重定向到 Agent。原生直连/转发策略仍由游戏执行。
普通启动入口、配置隔离、框架更新、安装保护和 TCP/UDP 传输可在无游戏测试程序里验证；
登录实际账号、真人加入房间和完整多人对局需由用户手动验收。构建或模拟传输通过不代表
所有游戏模式已完成外网验收。最多 32 条映射，其中 UDP 池最多 31 个端口。

正式 Mod 包不依赖 Sakura SDK、特定盘符、隧道名称、SQL、客户端副本或桌面快捷方式。
