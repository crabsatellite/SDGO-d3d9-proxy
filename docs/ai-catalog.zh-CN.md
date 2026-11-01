# 独立 AI 机体目录 Mod

`ai_catalog` 将服主数据库中可出击的玩家机体加入 `coop_ai` / `pvp_ai` 的 LV1–LV5 机体池。
服主生成一份配套目录，安装到对战服务器和所有玩家客户端；玩家继续使用原来的启动器。
English: [ai-catalog.md](ai-catalog.md)。

此功能需要现有 AI 功能：任务使用 `coop_ai` + `mission_ai`，PvP 使用 `pvp_ai` + `match_ai`。
它扩充机体目录，不替换计分、房间、僚机控制或 PvP 逻辑。原生 AI 对战模式 11/12 的机体名单保持原样。
脚本按原生 AI 模板作默认选择，支持服主覆盖；特殊武器、变形、视觉效果和真人联机仍需手动游戏验收。

也可配合[长房名 Mod](room-titles.zh-CN.md)，例如
`朋友练习局，慢慢打欢迎新手 !ai e2 a1 lv4 !rand`：AI 使用目录里的 4 级池，
另装并启用 `room_random` 后，真人每局随机。第 20 字节后的指令需要长房名客户端、Agent、Battle
三端配套及更新后的 AI / 随机机体 DLL；目录数据和两端 `ai_catalog` 仍须单独安装。
组合语法见[房名组合指令](room-commands.zh-CN.md)。

## 安装位置和开关

两端各自的 EXE 目录下：

```text
sdgo/mods/ai_catalog/
  ai_catalog.dll
  mod.ini
  catalog.bin       服主生成；服务器和所有客户端必须相同
```

客户端和服务器 DLL 不同，Mod ID 都是 `ai_catalog`，默认关闭。`mod.ini` 的 `[mod] enabled=1/0`
控制它，`sdgo.ini` 的 `[mods] ai_catalog=0` 也会关闭它。修改后要**重新启动相应进程**，不支持对局中热卸载。
服务端和所有参与者必须成套启停、使用相同目录；未装配套 Mod 的客户端不支持扩展机体房间。
框架 1.1.4 起也支持在 `sdgo.ini` 中用 `[features] ai-catalog=0` 或 `[categories] gameplay=0`
统一关闭。它仍是独立附加包，不随 Base / Full 包自动安装。

启用时，服务器从独立目录提供等级池；不再受旧 `LevelN=` 单行 1024 字节限制。
关闭后，`coop_ai` / `pvp_ai` 继续读取各自原来的 `LevelN` 设置并按原生名单过滤，原有配置不被改写。
因此不用恢复 SQL，也不用把等级池改回去。

## 服主准备一次

需要已有的 Mod 框架、匹配的 AI 功能版本和 Python 3.10+。只有服主的生成/安装工具需要 Python。
客户端 GDT 布局对应 `GOnline-54BE1568-00C97000`，服务端对应 `BattleServer-548A891D-0046C000`。

1. 生成目录。使用自己的 `GOnlineGame` 目录表 JSON 导出和配套客户端提取的 `gdrs`：

   ```powershell
   python .\backend\ai_catalog.py --db-export "D:\OwnerData\GOnlineGame" --client-gdrs "D:\OwnerData\gdrs" --generate --out ".\my-roster"
   ```

   工具不负责提取游戏包。要求当前版本使用的六份机体表和六份 NPC 表完整，缺项、冲突或错误格式会拒绝生成。
   也可用 `--connection-env SDGO_SQL_CONNECTION` 替代 `--db-export`，读取服主在本机环境变量里准备的 ODBC
   连接字符串；此模式另需 `pyodbc` 和 SQL Server 驱动。连接信息不进目录或玩家包。

2. 关闭要安装的那份游戏/对战服务器，用增量安装器安装。`--source` 是解压后的独立 Mod 包根目录，
   从源码测试时也可以是 `build/catalog/stage/Release`：

   ```powershell
   python .\backend\install_ai_catalog.py install --role server --target "D:\Server\BattleServer1" --source "." --catalog ".\my-roster\server\sdgo\mods\ai_catalog\catalog.bin"
   python .\backend\install_ai_catalog.py install --role client --target "D:\Games\SDGO" --source "." --catalog ".\my-roster\client\sdgo\mods\ai_catalog\catalog.bin"
   ```

   每个参与的 BattleServer 目录各装一次。安装器只更新此 Mod、补齐它需要的 symbol，并在已装
   `coop_ai` / `pvp_ai` 的服务器上更新这两个 Mod 的可选目录桥接 DLL；不安装未存在的 AI 功能，
   不改它们的 `mod.ini`、全局设置、加载器或 `core.dll`。它会保存更新前的文件以便卸载恢复。
   `--host-exe MyBattleCopy.exe` 支持经过服主重命名的同版本副本。

3. 给玩家导出增量包：

   ```powershell
   python .\backend\export_ai_catalog.py --source "." --catalog ".\my-roster\client\sdgo\mods\ai_catalog\catalog.bin" --out ".\给玩家的AI机体目录.zip"
   ```

   玩家先装匹配的框架和 AI 功能，关闭游戏，把包解压到 `GOnline.exe` 旁边，再运行一次：

   ```powershell
   powershell -NoProfile -ExecutionPolicy Bypass -File .\Install-AiCatalog.ps1
   ```

   此步骤只补齐 Mod 必需的符号定义，保留其他 Mod 定义，发现冲突则拒绝写入，并保存原文件备份。
   以后照常用原启动器，不用每次运行安装脚本。包只带客户端 `ai_catalog` 文件、配套符号、一次性
   安装工具、校验清单和说明，不带服主后端、数据库连接、账号或原游戏文件；玩家无需 Python。

生成结果中的 `catalog.json` 是服主的编号分配记录，**更新时必须保留**。更新生成时传：

```powershell
python .\backend\ai_catalog.py --db-export "D:\OwnerData\GOnlineGame" --client-gdrs "D:\OwnerData\gdrs" --generate --previous-catalog ".\my-roster\catalog.json" --out ".\my-roster-next"
```

记录同时保留已经移除的机体编号，避免日后把旧 NPC ID 分给不同机体。输出目录必须是新的；不会覆盖已分发结果。
然后更新两端的 Mod/目录并重新启动，不要混用不同生成版本。

## 分组和服主覆盖

默认只收 `tbl_Units` 中 `SallyReady=1` 且 HP>0 的记录。分组为 C→LV1、B→LV2、A→LV3、
其他 S→LV4、SS/SR→LV5。它是机体池分组，不是机体经验等级，也不是实测的 AI 难度评级。

复制 `backend/ai_catalog.rules.example.json`，用 `--rules` 指定：

```json
{
  "exclude": [10001],
  "levels": {"10002": 3},
  "templates": {"10002": 3604001}
}
```

这里的编号只是格式示例。`templates` 只能指向服务器和客户端都有的原生 AI NPC 模板。
默认按武器类型、等级和 HP 选择原生脚本，主武器为空时偏向射击模板；缺少武器槽的机体仍需实际战斗检查。
AI 保留原有效初始武器索引，否则选第一个有效武器；这一修正在 AI 的私有副本中进行，不修改玩家机体。

不加 `--generate` 时只输出 `audit.json` 和五份 CSV，保持只读规划模式，不产生可安装目录。

## 关闭与卸载

关闭目标进程后：

```powershell
python .\backend\install_ai_catalog.py disable --target "D:\Games\SDGO"
python .\backend\install_ai_catalog.py enable --target "D:\Games\SDGO"
python .\backend\install_ai_catalog.py uninstall --target "D:\Games\SDGO"
```

对每个 BattleServer 和客户端分别操作，重新启动后生效。启用时如发现全局 `ai_catalog=0`，安装器会提示，
不会悄悄改全局文件。卸载恢复首次安装前被更新的文件，保留其他文件及 Mod 设置。
如其他更新器已经改过受管 DLL 或 symbol，卸载会在写入前拒绝恢复，以免覆盖新工作；关闭本 Mod 仍可用。
安装操作写入中断记录并自动回滚一般复制失败；进程被强行终止后可用 `recover --target ...` 恢复，
若文件已被别人修改则拒绝自动覆盖。

## 数据隔离和检查范围

运行时不连接数据库、不插入 `tbl_NpcData` / `tbl_UnitNpc`、不修改加密游戏包。
服务器只在构造扩展 AI 时提供私有机体副本；异常或嵌套调用后恢复线程上下文。
客户端在最后一份 NPC 表加载后，将目录记录注册到**当前进程**的原生查找树和校验表，因此原生创建函数
内部直接查树也能找到它们。记录、包装和节点都用游戏自身分配器，由正常退出时的原生析构释放。
普通机体字节和原有查表行为保持不变。全部源机体指纹与编号冲突预检通过后才注册；发现冲突或源数据
变化则拒绝这次注册，不覆盖原记录。创建函数遇到缺失或不同的目录记录会安全拒绝创建。
关闭 Mod 并重新启动后只加载原始游戏包中的记录，不需要恢复客户端数据库。

本 Mod 扩充现有房名指令使用的随机等级池，**没有新增机体选择界面**。
例如房名 `!ai e2 a1 lv2` 从扩展后的 LV2 池抽取 2 个敌人和 1 个队友。

`catalog.bin` 含服主从自己的数据生成的 NPC 记录及匹配指纹，属于服主的配套游戏数据；项目通用包不包含它。
不要将某位服主的生成结果提交到代码仓库或当作通用默认值。

自动测试覆盖真实框架/DLL 的开关、冲突、缺文件、源数据变化、加载延迟和 hook 失败回滚；
安装测试比较安装→关闭→开启→卸载前后的所有原文件字节及设置。可选原生测试映射调用方提供的
BattleServer / GOnline 副本，用自有数据调用真实构造或原生查表、插入、校验和析构函数，
不执行游戏主程序或连接服务器。
这些检查不等同于视觉、AI 战斗、变形、真人多人的手动验收。
