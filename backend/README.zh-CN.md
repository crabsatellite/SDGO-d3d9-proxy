# AI 机体目录服主工具

随独立 `ai_catalog` Mod 分发。完整安装、分发、开关、卸载说明见
[中文指南](../docs/ai-catalog.zh-CN.md)；独立包根目录的 `README.zh-CN.md` 也包含完整命令。

* `ai_catalog.py`：默认只读检查/分级；加 `--generate` 生成服务器和客户端相同的 `catalog.bin`。
* `install_ai_catalog.py`：安装到选中的已停止客户端/对战服务器，保存原文件；启用、关闭、卸载和中断恢复。
* `export_ai_catalog.py`：导出客户端 Mod、服主目录、必需符号和一次性 PowerShell 安装工具的玩家增量 ZIP。
* `install_ai_catalog_client.ps1`：玩家包内的一次性符号合并，不覆盖其他 Mod 定义；以后仍用原启动器。
* `ai_catalog.rules.example.json`：服主自己的排除、LV 和原生 AI 模板覆盖规则。

这些工具需要 Python 3.10+，玩家不需要。JSON 模式不需第三方库；ODBC 读取另需 `pyodbc` 和驱动。
不连接或写入本项目作者的环境；输入、安装目录、数据库连接均由服主在自己的机器上指定。

生成器仅读取 `tbl_Units`、`tbl_UnitNpc`、`tbl_NpcData`、`tbl_UnitGrades`、`tbl_WeaponsRev` 五张目录表，
不读取账号、机库、聊天等表。运行时 Mod 不访问数据库，也不写游戏原表或加密游戏包。

默认 LV 分组为 C/B/A/S/SS-SR → 1/2/3/4/5，允许覆盖；这是机体池分组，不是实测难度。
生成结果保留手动游戏验收状态，尤其是原生 AI 脚本对特殊武器和变形机体的适配。
更新时传上一版 `catalog.json` 分配记录；不能用 `catalog.bin` 代替，因为分配记录还保留退役 ID。

输出含服主从配套游戏数据生成的记录，不应作为通用默认数据提交或发布。
