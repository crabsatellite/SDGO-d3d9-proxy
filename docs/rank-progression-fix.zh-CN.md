# 军衔回退修复 Mod

[English](rank-progression-fix.md) · [返回基础修复](../README.zh-CN.md)

`rank_progression_fix` 是默认启用的基础修复，支持
`WorldServer-548A86E3-0010C000`。通过现有 Mod 框架安装，玩家无需更新客户端，
也无需安装 Python、ODBC 驱动或额外填写数据库连接配置。

## 安装

停止目标 WorldServer，使用 Base 或 Full 包的现有安装器：

```powershell
powershell -ExecutionPolicy Bypass -File .\install_server.ps1 -ServerRoot "D:\SDGO\Server\WorldServer" -Role World
```

然后按原方式启动 WorldServer。此 Mod 自身只包含
`sdgo/mods/rank_progression_fix/mod.ini` 和 `rank_progression_fix.dll`，
共用框架的 `winmm.dll`、`sdgo/core.dll`、宿主符号和全局配置，无需另跑维护工具。

Mod 只读取 WorldServer 现有 `[DB Game]` 的数据库名，复用宿主当前借出的 OLE DB 会话，
不读取、不导出连接密码，也不另开游戏数据库连接。现有数据库账号需要能够读取两个原生过程定义，
并创建 Mod 自己的 schema、状态表和过程。每个参与结算的 WorldServer 都需要加载 Mod，
仅共享数据库不会让未安装 Mod 的进程自动改走修复路径。

## 修复范围

原生 `dbo.sp_SetBattleExp_V4`、`dbo.sp_SetGridBattleExp_V2` 错误地将“大军衔更高”与
“子等级更高”直接用 `OR` 连接。例如军衔 `15-1`、经验 30,090 的角色获得 20 经验，
经验区间对应 `5-2`，旧判断因子等级 `2 > 1` 将军衔覆盖为 `5-2`。

Mod 从当前数据库的原生定义生成自己的副本，只更换过程名称和这一处比较式：

```sql
IF(@UserLevelNumber < @newlevel OR
   (@UserLevelNumber = @newlevel AND @UserLevelStep < @NewLevelStep))
```

匹配到结算调用时，在同一事务和应用锁内准备
`sdgo_rank_progression_fix` schema 下的两个过程，检查归属、比较式版本、定义指纹和 SQL 选项。
未知定义、别人的同名 schema、被外部修改的副本都会拒绝覆盖。原生 `dbo` 过程保持不变，
分发包不携带原生 SQL 定义。

仅两个完全匹配的原生结算命令会被重定向；参数绑定、输出处理、经验、奖励和战斗次数仍由
原生流程处理，调用后恢复命令记录中的原始指针。保留正常晋级、原生 15-5/排名军衔限制和
登录时的排行榜重算，不推测或补写过去丢失的军衔。

`sdgo/logs/sdgo.log` 区分“Hook 已装载”和“修复路径已激活”。对应结算出现
`rank policy active` 才表示命令已切到 Mod 路径。初始化失败会记录错误并保留原生调用；
如果原生 SQL 本身有问题，该次调用仍可能发生原有的军衔回退。

## 关闭或移除

在 `sdgo/sdgo.ini` 设置并重启目标 WorldServer：

```ini
[mods]
rank_progression_fix=0
```

也支持统一的 `[features] rank-progression-fix=0`、`[categories] fixes=0`。
关闭后原生调用不使用 Mod 自己的 SQL 对象，可以保留这些闲置对象。
关闭 Mod 不会撤销旧版独立工具或管理员曾经对原生 SQL 做过的修改。

## 开发验证

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

C++ 离线测试覆盖定义转换、框架入口、两个执行 Hook、精确匹配、失败回退、调用记录和输出保留。
打包测试核对 Base/Full 目录和包内 World 安装器。

可选真实 SQL 测试使用现有 INI，以及本地保留的原始两过程恢复备份：

```powershell
powershell -ExecutionPolicy Bypass -File tests\rank_progression_db_test.ps1 -ServerIni "D:\SDGO\Server\WorldServer\WorldServer.ini" -OriginalBackup "D:\Maintenance\original-rank-procedures.json" -BuildDir build
```

测试创建带归属标记的隔离库，只复制表结构和军衔配置，使用合成角色/卡组，不复制真实玩家记录。
通过真实 SQLOLEDB 和正式 Mod 的 SQL 策略执行完整原生过程。在已测试的 65 个经验区间配置下，
152 次原生调用覆盖原有回退、启用/关闭 SQL 路径、经验边界、正常晋级和排名军衔上限，
并检查重复初始化、归属和外部修改保护、第二个过程创建失败时的完整回滚、来源库定义未变。
仅 Haro 记账为空实现，其他意外依赖会报错。
另有 4 次带参数绑定的 RPC 调用，检查正式 OLE DB 命令语法、返回码，以及已绑定参数的命令
借出会话进行策略初始化后仍可执行；同时检查 Mod 不会提交或撤回宿主已有事务。

临时凭据文件在成功或失败时都会删除。成功时只删除本次带归属标记的隔离库，失败库名记录在
本地 `result.json`。这些测试不等于真人对局、切频道或重新登录验收。
