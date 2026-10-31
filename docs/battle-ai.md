# Battle AI enhancement / 战斗 AI 增强（预览）

`battle_ai` 为原生 AI 对战机器人及 `match_ai` / `mission_ai` 创建的同类机器人增加决策和脱困。
配置完成后随正常游戏启动生效；没有额外的联机入口，也不管理 FRP、服务端或数据库。

## 安装与配置

需要先装本项目的 Mod 框架，支持 `GOnline-54BE1568-00C97000`。关闭要更新的那份客户端，运行：

```powershell
./tools/install_battle_ai.ps1 -GameDir D:/Games/SDGO -Source ./build/stage/Release/client
```

增量安装器只更新四个 `sdgo/mods/battle_ai/` 产品文件和两个已验证的 NPC 符号段，
保留其它符号、其它 Mod、全局配置及用户 `config.ini`，并备份被更新的文件。
它不会停止任何进程，不会启动游戏。

`sdgo/mods/battle_ai/config.example.ini` 列出默认值。需要调整时复制成同目录下的 `config.ini`。
未创建该文件也会使用默认值。更新只覆盖示例，不覆盖用户配置；路径相对于游戏目录。
`mod.ini` 仅为开发者清单。无需写入 IP、端口、账号或本机绝对路径。

要关闭增强，在 `sdgo/sdgo.ini` 的 `[mods]` 下写 `battle_ai=0`，重新启动客户端；
或在用户配置 `[settings]` 下写 `Enabled=0`，两秒内恢复原生决策。

## 当前行为

- 原生攻击检查允许时，缩短等待状态中的决策延迟；保持原生攻击状态和武器执行。
- Gunfighter、Tanker 在非攻击/连击状态下，距离目标过近时短暂后退。
  Gunfighter、Tanker 的 `ATTACK` 负责射击；Infighter、Stoker 的同名状态负责近战，
  由各自的原始 Init 选择武器，增强层不根据状态名字猜测动作。
- 有移动目标却持续没有位置进展时，先左移，再右移，最后调用原生随机目的地选择，
  各次尝试之间有冷却。该回退不保证能找到绕过大型建筑的路径。
- 每帧仍执行一次原生 AI；恢复窗口内只短暂重申自己的移动目标，
  原生进入攻击、死亡、受击、复活或配置禁用时及时交回控制。
- 新增状态按对象 ID 和 `InitNPCAI` 产生的代次分开，避免新一局或复活继承旧计时。
- 只包装当前 30 个原生 `VSAI_*_Update`，同时检查本机模拟标志、AI 启用标志和 NPC ID 范围。
  远端副本、真人控制和任务 Boss 脚本不会运行这层增强。
- 不写坐标、生命值、弹药、速度、碰撞开关或网络包；不直接开火、不额外填充推进槽。
  原生脚本自己的参数和动作仍保留。

把 `LogDecisions=1` 可在 `sdgo/logs/sdgo.log` 查看攻击条件、距离调整及脱困原因。
脚本错误会回退原生更新，累计三次暂停增强，修正并重新加载脚本后恢复。
配置或脚本的语法错误不会替换上一份可用策略。

## 自动测试

测试 VM 只用于开发，运行在独立可执行文件中；不连接游戏服务。使用官方 Lua 5.0.3：

```text
https://www.lua.org/ftp/lua-5.0.3.tar.gz
SHA256 1193a61b0e08acaa6eee0eecf29709179ee49c71baebc59b682a25c3b5a45671
```

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32 -DSDGO_LUA50_SOURCE=D:/deps/lua-5.0.3
cmake --build build --config Release --parallel 2
./tests/battle_ai_test.ps1 -NativeScriptDir D:/extracted-client/scp
./tests/battle_ai_install_test.ps1 -GameExe D:/Games/SDGO/GOnline.exe
python tools/verify_symbols.py D:/Games/SDGO/GOnline.exe
```

`NativeScriptDir` 使用自行持有的原始客户端脚本，不随 Mod 分发。回放直接执行
1379–1382、1409–1434 的原始 Lua 5.0 字节码，模拟游戏的场景、武器、计时和移动 API。
原生接入测试加载真实 `core.dll`、`battle_ai.dll`，检查 Hook、Lua 堆栈、远程标志、
复活代次、配置重载、晚加载函数、错误脚本回退及签名不符拒绝。

本轮结果：31 项策略场景、30 个原始脚本回放、4 类 native 接入场景及增量安装保留测试通过。
回放中的 Gunfighter 原地等待场景约 3024 ms 才进入攻击，增强后约 320 ms；
这是模拟条件下的脚本结果，不是实机战斗性能数据。

## 独立环境与手动验收

`tools/prepare_battle_ai_sandbox.ps1 -ClientDir D:/Games/SDGO` 只会在当前工作树的
`build/battle_ai_sandbox/client` 复制游戏资产并安装当前构建，不启动客户端，
不复制启动器/账号/聊天记录，也不连接任何现有后端。测试目录没有使用 FRP 的端口、
隧道、数据库或运行目录。该客户端尚需另行指定独立测试后端才能做实际登录/对战。

自动场景不代表真实地图碰撞或多人同步已验收。仍需手测墙角、狭道、高低差、
可破坏障碍、复活及多人房间；观察卡住时长、无障碍时的攻击响应和远程机选位。
原生脚本仍有脚本级共享变量，本 Mod 只保证新增的恢复/决策状态按 NPC 分开，
尚未消除所有原生共享状态。第一版也未引入全局导航、跳跃连接或飞行寻路。

## English summary

This optional client add-on decorates 30 stock VSAI callbacks on the authoritative
client. It retains one native update per frame, requests native attack states,
adds ranged spacing and bounded local recovery, and preserves user configuration.
Synthetic VM/adapter tests pass; actual terrain and human multiplayer acceptance
remain open. No server, account, tunnel or database changes are required by the mod.
