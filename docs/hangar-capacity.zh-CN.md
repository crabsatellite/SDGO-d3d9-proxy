# 自动机库容量（Beta）

[English](hangar-capacity.md)

服务端 Mod `hangar_capacity` 在原生读取机库容量前，为新旧账号保障**至少 1200 格**。
玩家正常登录，客户端沿用原生分页初始化；无需客户端 Mod、额外启动器、端口或 FRP 设置。

通过通用安装器装到已停止的 WorldServer：

```powershell
powershell -ExecutionPolicy Bypass -File install_server.ps1 -ServerRoot "D:\Server\WorldServer" -Role World
```

随包默认关闭。在 `sdgo/mods/hangar_capacity/mod.ini` 中设 `[mod] enabled=1`，再正常启动选中的
WorldServer。分类、整项功能和单 Mod 关闭开关仍生效。服主自己的 `<EXE名>.ini` 需有
`[DB Game] DBName`；Mod 借用服务器已认证的原生数据库会话，不复制或分发连接密码。
数据库账号需要相关表的读取、容量更新和空槽编号插入权限。

每个账号读取容量时执行事务策略：验证库名、表结构和原生容量过程参数，补齐空槽编号 `1..1200`，
再将该账号不足的 `ExpandSlotCount` 提升到 **1176**。原生总容量为 **24 + 此字段**。
已有更大的有效数值保持原值；本 Mod 新增容量的边界为 1200，不创建机体记录。
原生过程、库存、货币和出击选择均不改写。

策略失败会回滚，并走原生数据库失败回包，避免客户端初始化半完成的数据；同一账号可重试。
已有事务、隐式事务或不兼容结构会被拒绝。日志不输出账号身份、SQL 或凭据。
这一版还会拒绝容量表上启用的 DML 触发器，以及扩容字段/空槽字段的 identity、计算列变体，
避免未知数据库插件通过这些写入暗中修改其他数据。

| 设置 | 默认 | 含义 |
|---|---:|---|
| `LockTimeoutMs` | 1000 | 策略锁等待，范围 0..5000 毫秒 |
| `QueryTimeoutSeconds` | 3 | 单条命令超时，范围 1..10 秒，需大于锁等待 |

容量在客户端首次分配机库前应用。已经打开机库的客户端在下次正常登录生效；这一版不做会话中动态扩槽。
关闭或卸载后停止后续策略调用，已经增加的格子及获得的机体保留，不自动缩容。

独立的容量读取挂点避开军衔和新账号 Mod 已占用的入口；机库整理 Mod 的原生 1200 格配置可继续使用。
超过 1200 的自定义容量、在线扩张属于后续兼容工作。

构建和自动检查使用隔离夹具。最后一页、1200 号槽、领取机体、保存重登及真实联机表现仍需手动验收。

开发者可用本机已有的 SQL Server Docker 镜像运行隔离 SQL 检查：

```powershell
python tests/run_hangar_capacity_sql_test.py --build-dir "D:/Source/build" --config Release --work-root "D:/Source/buildwork/hangar-capacity-sql" --image "<本机SQL镜像>"
```

先构建 `hangar_capacity_sql_emit`、`hangar_capacity_sql_test`、`hangar_capacity` 和 `sdgo_core`。
夹具新建独立容器，不发布端口，使用临时随机凭据并仅清理该容器；不会读取真实 WorldServer INI 或连接已有游戏库。

[原生证据](investigations/hangar-capacity-native.md)记录了 1200 格边界、回包/适配器 ABI 和符号核对命令。
[验证范围与手动清单](hangar-capacity-verification.md)记录自动检查的覆盖范围及真人验收边界。
