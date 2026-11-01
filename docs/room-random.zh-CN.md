# 每局随机机体（Beta）

房名里加一个独立的 `!rand` 或 `!random`，房间里的真人玩家在每次开局时都会从**各自机库**
重新分配本局机体。无需退房、重新建房，也无需再手动执行随机机体 SQL。

例如：`!rand`、`朋友局 !rand`、`!rand !ai3`。命令不区分英文大小写；原生房名长度仍按当前客户端限制。
支持 `!ai3 lv4 !rand`、`!rand !ai e2 a1 lv4` 等组合。命令用空格分开，AI 参数紧跟 `!ai`，
放在下一个 `!指令` 前；完整语法见[房名组合指令](room-commands.zh-CN.md)。
移除命令后，后续开局沿用原生出击流程。随机结果只写进战斗服务器本局的房间数据与战斗记录，
不更新数据库的 `GoUserUnitNum`、仓库、玩家卡组和锁定状态。

## 本局规则

- 普通、死亡竞技、队长、任务等单机体模式：开局抽一台，本局复活沿用它，下一局重新抽。
- 原生多机体模式 6/7：从各自机库选择机体副本，最多 4 格，**恰好 10 点**。
  同一库存序号不能占多格；同型号的不同副本按原生规则允许使用。
  使用原生机体数据的点数，通常 C=2、B=3、A=4、S=5。
  支持 `5+5`、`5+3+2`、`4+4+2`、`4+3+3`、`4+2+2+2`、`3+3+2+2` 等可行组合。
- 房间等级范围和仅 C 限制作用于所有真人的候选池。保留原生准备、队伍、模式、战舰等其他检查。
- 禁用、过期、耗尽能量、训练或不可出击的记录不进入候选池。锁定是防出售，锁定的可出击机体仍能抽到。
- 有其他可行结果时，单机体不会连续选同一型号，卡组不会连续选相同型号集合。
  如果只有一台可用机体或一组可行卡组，只能重复，日志会注明。
- 如果任一玩家没有可用机体，或在房间限制下无法在 4 格内凑满 10 点，本次开局整体取消，
  不让部分人随机、其余人保留旧机体，也不伪造未拥有的机体。仅 C 卡组最多 8 点，无法满足 10 点。

卡组均匀采样的是满足要求的库存副本组合，不是预先指定若干等级组合再平均抽取。
单机体模式先按型号等概率抽取，再在其合规副本中选择；卡组按不同库存序号组合，保留对应副本的等级、
特装、技能、涂装、纹章、经验与 64 位机体序号。战斗结束后的能源、经验等处理继续交给原生服务器。

这是**服务端 Mod**，沿用原生开局包同步给客户端，没有新增私有客户端协议。
与 `pvp_ai`、`coop_ai` 使用不同的 Hook 地址；AI 驾驶员继续使用 AI Mod 的机体池，
不会作为真人账号查询机库。如果同时使用 `!ai`，客户端仍按 AI Mod 原有要求安装。

## 安装

需要已经安装 SDGO Mod Framework 的战斗服务器。当前支持的程序：
`BattleServer-548A891D-0046C000`，SHA256
`f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4`。

先正常停止**要安装的那个** BattleServer，再运行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\install_room_random.ps1 `
  -ServerRoot "D:\SDGO-Server\Server\BattleServer1" `
  -DatabaseIni "D:\SDGO-Server\Server\WorldServer\WorldServer.ini"
```

增量包会自动找到自己的 `server/`；源码工作树默认从 `build/stage/Release/server` 安装，
也可通过 `-Source` 指定已构建的服务端文件树。多个 BattleServer 分别安装到实际会接收房间的实例。
安装脚本更新 core、随机 Mod，以及已安装的 `pvp_ai` / `coop_ai` DLL，使三者采用统一的房名解析规则；
保留 AI 的清单和服主设置，不自动安装或启用缺少的 AI Mod。安装脚本不会停止或启动服务，不修改游戏 EXE，
保留其余 Mod 和现有符号文件。
已有的符号期望值、注释和其他 Mod 的符号保留；只补缺少的符号，遇到地址冲突则拒绝安装。
被覆盖的 core、Mod 文件和符号文件会按原相对路径备份到 `sdgo/backups/room_random/`。

安装时只在本 Mod 的 `config.ini` 保存原生 WorldServer INI 的路径，不复制连接凭据。
之后正常启动服务器，给房名添加命令即可。配置只由服主填写一次，不再配置固定玩家 ID。

宿主使用本机 **32 位 SQL Server ODBC 驱动**。默认 Windows `SQL Server` 驱动已在本地只读验证；
不同驱动的连接设置和不同服务端数据库结构需要单独核对。

配置位于 `sdgo/mods/room_random/config.ini`：

```ini
[settings]
DatabaseIni=D:\SDGO-Server\Server\WorldServer\WorldServer.ini
Driver=SQL Server
QueryTimeoutSeconds=2
MaxRowsPerPlayer=4096
```

`DatabaseIni` 可为绝对路径，或相对于本 Mod 目录的路径。安装器支持带空格和中文的路径。
`config.ini` 存在时优先于 `mod.ini` 的旧设置。不要把原生数据库 INI 或私人 `config.ini` 放进客户端分发包。
原生 `sdgo.ini` 若显式设置 `room_random=0`，仍保持禁用；安装器尊重这个设置。
本功能属于 `gameplay` 分类，功能 ID 为 `room-random`。分类安装器可用 `-Features room-random`
选择此功能；`[categories] gameplay=0`、`[features] room-random=0` 和 `[mods] room_random=0`
均可关闭它。只装服务端部分即可随机真人；组合使用 AI 时按 AI 指南安装相应客户端 Mod。

## 实现与验证边界

旧脚本从固定账号列表里挑机体，只在 `tblg_OnUser.f_room/f_team=0` 时更新 `GoUserUnitNum`。
房间内缓存不随这种修改可靠刷新，卡组也不按每局生成。

本 Mod 在原生开局校验前执行一次只读、参数化的全房机库查询。先为所有真人计算完整方案，
然后原生人类战斗构造器使用本局数据；中途失败整体恢复。原生释放战斗驾驶员后恢复房间内
原来的出击数据，复活流程不重抽。不会调用会更新到期状态的机库存储过程。

四条原生人类开局路径都调用相同的校验和构造器：普通/任务路径、AI 11、AI 12、13/14。
最后一条还有独立的外层入口，也单独建立每局上下文；不假设所有模式都经过普通开局入口。
绑定均经核心符号验证，没有使用旧 MAP 中已漂移的地址。卡组用动态规划计算所有恰好 10 点的组合，
避免贪心拿了 5 点后错误地漏掉可行组合。

已有自动检查：

- Win32 Release 编译及本 Mod 的离线测试：精确点数、所有可行组合、连续避重、所有者隔离、14 个模式值、
  同房连续开局、死亡竞技本局固定、失败回滚、成员变化、任务房名和原生 AI 插入。
- 真实服务端 EXE 的新旧 30 个符号离线签名核对。
- 32 位原生 ODBC 对实际机库的只读查询，未修改账号、库存、出击选择或其他数据库内容。
- 仅文件的隔离安装/升级检查，包括中文路径、其他 Mod 与自定义符号保留、私人连接凭据不复制。
- core 公共命令接口、AI 参数范围、组合顺序、错误前缀、20 字节边界与旧 core 回退。

这些不是实际游戏联机验收。客户端显示、真正的扣能源/加经验、各原生模式、同房二次开局、复活和多客户端
同步仍由用户手动测试。打包记录中的历史保留字段置零；原生构造器负责战斗属性和武器，
展示及协议兼容性仍需真人测试。使用了其他会修改这些开局入口的第三方插件时，还需要确认运行时签名与 Hook 日志。

## 本地测试

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# 显式的只读检查；不会开游戏、登录账号或写数据库。仅在宿主本机运行。
build\Release\room_random_inventory_probe.exe "D:\Server\WorldServer.ini" 1234

powershell -File tests\room_random_install_test.ps1 -NativeExe "D:\Server\BattleServer.exe"
```

真人验收建议：同一房间连续开两局；死亡竞技死亡复活；多机体模式核对每个人总点数和切换/耗尽；
仅 C 等无法凑点的房间应整体拒绝；移除命令恢复原生流程；战后检查实际随机机体的经验和能源记录。
