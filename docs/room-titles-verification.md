# 长房名验证范围

本功能的真人游戏验收按用户要求手动进行。离线夹具、原生构造器执行和构建成功均不代表双客户端实战已通过。

## 可重跑的自动检查

```powershell
cmake -S . -B build -A Win32
cmake --build build --config Release --parallel 3
ctest --test-dir build -C Release --output-on-failure
```

`SDGO_ROOM_TITLE_NATIVE_HOST` 可选指定已支持的 BattleServer.exe，为限定原生构造器夹具增加一项检查。
它校验 SHA-256，私下映射 PE、应用重定位，只执行房间信息构造器及限定 CRT 路径；不启动入口、不连接服务或数据库。
本机验证包含该项。Lua 模拟依赖未配置，本功能未修改 Lua 脚本。

| 范围 | 证据与覆盖 |
|---|---|
| Unicode / 协议 | 20/21 字节、128 标量/512 字节、多字节与非法 UTF-8、嵌套/损坏尾部、批量快照与空身份记录 |
| 状态 / 内存失败 | 权限、修订、代次、房主转移、销毁复用；逐分配点失败验证 Store 与服务端缓存不分裂 |
| 客户端组合 | 真实控制器 + 原生 UI shim，中文投影、模式 10 直接自动建房协商、两域改名、错误提示、草稿、丢确认后的同编号重试 |
| 原生传输适配 | 真实收发适配器，原生/加密包装解码一次、可变任务列表、短错误回包、LastError、无 RTMD 数据透传 |
| Agent | 实际 core + Mod DLL，启用/禁用/失败回滚、TLS、嵌套、重试、两种改名 subtype、自动建房协商及六类错误回包 |
| Battle 包装 | 真实包装与状态实现，任务/对战协商、原生回包、旧客户端隔离、同前缀改名、精确重放和原路由 |
| UI | 已验证 vtable/布局的原生适配夹具；四个输入入口、两域标题栏、模式 10 身份、列表/详情及静态文字悬停展开 |
| 规则 | 真实 coop_ai / pvp_ai / room_random 实现；第 20 字节后命令、组合边界及原生回退 |
| 原有功能 / 分发 | loader、输入法、AI、随机机体、军衔修复、分类开关、数据暂存、完整/精简/指定功能包和三种宿主安装；更新保留设置与已有 Mod |

FRP 的独立回归另通过 `net_route_test.ps1`、`net_login_test.ps1`、`agent_udp_pool_test.ps1`、
`frp_install_test.ps1` 执行，均使用隔离测试目录。未对正在运行的游戏与服务部署或重启。

当前二进制字节核对可使用 `tools/verify_symbols.py`。原生静态证据另见
[房名限制](investigations/room-title-static-audit.json)、[协议承载](investigations/room-title-carriers.json)和
[UI 路径](investigations/room-title-native-ui.md)。最终自动检查回执见
[验证回执](investigations/room-title-verification.json)。

## 真人验收（尚未执行）

1. 在隔离测试服启用配套三端 Mod，两个客户端正常启动。
2. 任务模式 5、8 分别建房、在任务设置与房名窗口修改长标题；模式 10 直接进入自动建房的等候室后改名。
3. 对战模式 1、2、3、4、6、7、9 分别建房、改名；保留原生入口和模式的可用条件。
4. 使用超过 20 字节的中文名称，并把 `!ai3 lv4` 或 `!ai e2 a1 lv4 !rand` 放在后面。
5. 两人核对房间列表、选中详情、悬停文字，以及房内打开标题窗口所见全文；只查看时取消。
6. 改名后重新入房、转移房主、销毁并重建同编号房，确认没有串名；旧房主不能继续修改。
7. 开始任务/对战，核对所用 AI 与随机机体规则来自完整标题，原有计分与玩法仍正常。
8. 用未启用长房名 Mod 的客户端/服务端核对原生短房名；新客户端遇到不支持的服务端时应提示并保留长文本。

字体、实际 SDPlus 语言映射、鼠标操作和真实多人战斗效果属于以上手动范围。
