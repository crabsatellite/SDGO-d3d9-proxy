# 军衔回退修复

[English](rank-progression-fix.md) · [返回基础修复](../README.zh-CN.md)

原生普通战斗和 Grid/Tag 结算过程在比较军衔时，错误地将“大军衔更高”与“子等级更高”直接用
`OR` 连接。例如已保存为 `15-1`、经验为 30,090 的角色再获得 20 经验，经验区间对应 `5-2`，
旧判断会因为子等级 `2 > 1` 将已保存军衔覆盖为 `5-2`。

修复按“大军衔优先，同军衔再比较子等级”判断，保留原生的正常晋级、排名军衔限制、经验、
奖励和存储过程参数。登录时根据排行榜重新计算排名军衔属于另一套机制，不在此修复中更改。

## 服主使用

这是服务端数据库修复，不需要更换客户端，也不需要重启游戏服务。共享同一个游戏数据库的
World/Store/Battle 实例只需应用一次。不同的独立数据库需要分别检查，不能根据相同库名推断已修复。

维护工具从源码目录运行，需要 Python、`pyodbc`、SQL Server ODBC 驱动，以及能够修改目标过程的
数据库权限。连接设置从服主现有服务端 INI 的 `[DB Game]` 读取，连接凭据不写入导出文件。

先检查（默认也是只读检查）：

```powershell
python tools/rank_progression_fix.py audit --server-ini "D:\SDGO\StoreServer\StoreServer.ini"
```

两个过程显示 `affected` 时应用：

```powershell
python tools/rank_progression_fix.py apply --server-ini "D:\SDGO\StoreServer\StoreServer.ini"
```

再运行 `audit`，两个过程都应显示 `already_fixed`：

- `sp_SetBattleExp_V4`
- `sp_SetGridBattleExp_V2`

`ALTER PROCEDURE` 需要 SQL 架构锁，建议选择维护时段；工具设置了超时，不会停止客户端或服务器进程。
它只修改上述两个过程的比较式，不会修改玩家记录、经验值或历史军衔。

## 备份与回滚

应用前会在 INI 旁的 `sdgo/backups/rank_progression_fix/` 创建唯一备份；也可用 `--backup` 指定
一个尚不存在的文件。备份包含原生过程定义，应保存在服主本地，不随客户端包分发。

```powershell
python tools/rank_progression_fix.py apply --server-ini "D:\SDGO\StoreServer\StoreServer.ini" --backup "D:\Maintenance\rank-fix.json"
python tools/rank_progression_fix.py rollback --server-ini "D:\SDGO\StoreServer\StoreServer.ini" --backup "D:\Maintenance\rank-fix.json"
```

工具在同一事务内修复两个过程，保留 SQL 选项并读取定义指纹确认。重复应用或重复回滚不重复修改。
不认识的过程版本会拒绝修改；若安装后有人编辑了过程，回滚也会拒绝覆盖该编辑。

连接中断时，提交结果可能暂时不明确，应先运行只读 `audit`、检查备份，再决定是否重试。
修复用于防止之后的错误回退，不会猜测并补写过去丢失的军衔。

## 已完成的验证

- 7 项离线测试：比较式转换、注释/字符串、作用范围、定义指纹和版本拒绝。
- 隔离 SQL 数据库内执行两个完整原生结算过程，分别复现修复前 `15-1 → 5-2`。
- 修复后通过 148 个原生 SQL 用例，覆盖经验区间边界、正常大/小等级晋级、已授予较高军衔、
  原生排名军衔限制，并检查持久化经验及战斗/胜利次数。
- 验证重复安装、回滚保护，以及第二个过程修改失败时完整撤回第一处修改。

隔离测试仅使用合成角色/卡组，不复制真实玩家记录；无关的 Haro 记账使用测试空实现。
数据库部署后的 `audit` 和定义指纹确认，只证明目标库已安装修复。真人对局、切频道、重新登录后的
显示与持久化仍需由玩家手动检查。
