# AI 机体目录检查和扩充分级规划

`ai_catalog.py` 是随 Mod 框架分发的**只读准备工具**。它读取服主自己的机体目录，
检查扩充 AI 机体池的条件，输出 LV1–LV5 候选清单。当前没有安装、写库、SQL 生成或客户端改包功能；
输出中的 `ready_to_install=false` 表示尚不能直接导入开玩。

## 使用

需要 Python 3.10+。从服主现有数据库导出的 `GOnlineGame` 目录读取：

```powershell
python .\backend\ai_catalog.py --db-export "D:\Server\db_backup\GOnlineGame" --out ".\roster-plan"
```

也可以检查服主从**配套客户端**提取的 `gdrs` 数据（不是游戏安装目录；工具不负责解包）：

```powershell
python .\backend\ai_catalog.py --db-export "D:\Server\db_backup\GOnlineGame" --client-gdrs "D:\OwnerData\gdrs" --out ".\roster-plan-checked"
```

或使用服主在本机环境变量中准备的 ODBC 连接字符串：

```powershell
python .\backend\ai_catalog.py --connection-env SDGO_SQL_CONNECTION --client-gdrs "D:\OwnerData\gdrs" --out ".\roster-plan-live"
```

ODBC 模式额外需要 `pyodbc` 和服主已有的 SQL Server 驱动。建议使用只能读取目录表的账号。
连接字符串由服主在本机配置，不通过命令行参数传递，不写进输出，也不随 Mod 发布。
程序只执行五个固定的 `SELECT`，不启动/重启任何游戏服务。

JSON 模式只读这五张目录表：`tbl_Units`、`tbl_UnitNpc`、`tbl_NpcData`、`tbl_UnitGrades`、
`tbl_WeaponsRev`。文件名为 `dbo.<表名>.json` 或连续的 `dbo.<表名>.part1.json` 等；
缺表、缺分卷或重复 ID 会报错。不读取玩家账号、机库、邮件或其他备份。

输出目录必须不存在。输出 `audit.json`、五份 `level-N.csv` 和说明文件；候选编号只是建议，
**不要复制进 `LevelN=` 配置**。工具不会生成 SQL、DLL、Mod 配置或启动脚本，也不会覆盖现有文件。
输出可能包含服主自己的机体名称和属性，不应自动作为项目通用数据公开发布。

## 默认分级

| 分组 | 规则 |
|---|---|
| LV1 | C 系列（`UnitGradeNumber=1`） |
| LV2 | B 系列（2） |
| LV3 | A 系列（3） |
| LV4 | 其他 S 系列（4） |
| LV5 | SS / SR（4，且 `UnitGradeNumber2` 为 3 / 4） |

这是**机体池分组**，不是机体经验等级、AI 技术水平或经过对局验证的战力评级。
默认只收 `SallyReady=1` 且 HP 大于 0 的玩家机体记录。
未标记可出击的记录单独报告；不因其存在于数据库便自动加入池子。
变形关系、空武器槽、缺失武器和客户端机体记录也会列入报告。

服主可以复制 `ai_catalog.rules.example.json`，填自己的排除和 LV 覆盖规则：

```json
{
  "exclude": [10001],
  "levels": {"10002": 3}
}
```

通过 `--rules "D:\OwnerData\ai-rules.json"` 使用；示例编号不是通用推荐。规则仅影响清单，
不会把原本不可出击的记录变成候选。

## 为什么还不能只加 SQL

当前 Mod 的 `LevelN` 配置使用 AI NPC ID，而不是玩家机体 ID。
扩充必须同时覆盖：

1. 服务端能加载的 NPC 记录及其机体、变形、武器数据。
2. 每个客户端能查到的对应 NPC 记录，以及已有机体资源。
3. 适合该机体武器槽和行为的 AI 脚本。
4. 能读取完整大机体池的 Mod 配置；现有 `coop_ai` / `pvp_ai` 每行只有 1024 字节缓冲区。
5. 生成、战斗、变形、死亡/重生的实际验收。

当前客户端原生加载的 NPC 表中有脚本名；服务端 `tbl_NpcData` 没有该字段。
数据库增加 NPC 不会自动为客户端产生对应记录或脚本。机体/武器编号存在也不能证明 AI 能正确使用它。
报告中的 `missing_server_npc_unit_ids` 是对 `tbl_UnitNpc` 的目录差异，不能单独视为原生运行时失败证明。

最终安装工具应由服主在后端一次生成**配对的服务端变更、客户端目录和等级配置**，
包含编号冲突检查、幂等安装及可追踪回滚。玩家只接收配套 Mod/目录并照常启动游戏。
Mod 不携带本机数据库、账号、路径或某位服主的生成清单。当前工具只完成这个流程的预检和规划部分。

本工具的客户端文件布局仅针对当前支持的 `GOnline-54BE1568-00C97000`；其他版本需重新核实。
它检查解密记录的编号、尺寸和关键引用，不验证模型、动画和 AI 对局表现。
