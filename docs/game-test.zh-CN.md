# 原生 GameTest

开发用可选 Mod，默认关闭。测试在真实客户端进程内执行，通过正常业务请求
与真实服务端通信。外部运行器负责准备独立客户端、启动和收集结果，不操作桌面。

每个场景按“发起请求 → 等待真实回包 → 断言状态”执行。登录、服务器数据、
建房和战斗加载均走正常游戏流程，不伪造机库、房间、机体记录或服务端回包。

测试账号与注册重试规则保存在本地私有配置中。缺少步骤、断言失败、原生异常
和超时都会判为失败，结果输出为 JSON 和 JUnit XML。仅安装 Mod 不会自动运行测试。

测试默认使用独立后台桌面，副本中的 BGM、音效和语音全部静音。后台窗口与前台
归属有独立回执；不满足条件就失败。本机直连测试只绑定回环地址，不创建公网监听，
不修改系统防火墙或通知设置。非本机测试需要预先具备其专用防火墙规则的管理权限；
准备失败时不会弹出提权窗口或回退到交互运行。

默认复用 `%LOCALAPPDATA%\SDGO-GameTest\account.ini`，也可用 `--account-profile`
指定已有配置。普通重跑不会注册新账号；只有显式 `--create-test-account` 才允许
首次创建持久账号配置，且不会覆盖已有文件。

新账号初始化包括昵称、新手测试退出、首次机体抽取和必要的游戏内确认。
这些步骤同样等待正常服务端结果，随后才运行对战场景。

开发者可通过 [`game_test.h`](../include/sdgo/game_test.h) 注册按帧执行的测试，
在原生主线程等待实际业务事件并进行断言。

构建后，用已有正常客户端作为模板。模板需要安装对战 AI，目标服务器需要启用
`pvp_ai`。运行器只启动独立客户端，不重启现有服务器。

```powershell
python tools/game_test.py prepare --client-template E:\Games\SDGO --stage build/stage/Release --run-dir build/gametest/pvp --mode-sequence 1,3,9
python tools/game_test.py run --run-dir build/gametest/pvp --timeout 3600
```

仅首次没有测试账号配置时，在 `prepare` 后加 `--create-test-account`；以后直接
复用同一配置。每次使用新的 `run-dir` 保存独立回执，同一账号禁止并发运行。
连接地址默认 `127.0.0.1:5001`，可用 `--target-host` 和 `--login-port` 修改；
服务器和频道由原客户端正常流程自动选择。
默认 `--network-policy direct` 仅在测试副本关闭继承的地址转发，并在登录前核对
该进程实际建立的 TCP 连接。要专门测试服主的 FRP 路由，显式选择 `preserve`。

示例依次测试普通、死亡竞赛和队长模式。每种模式都经过
建房、真实开局、AI 状态断言和正常结束返回；也可指定单个模式排查。
Grid（`6`）和 Tag（`7`）会通过原生业务切换 VS 频道，并要求账号已有完整战舰编队。
缺少编队或服务端拒绝会判为失败。
回执位于运行目录的 `receipts/report.json` 和 `receipts/junit.xml`。

准备账号时可单独使用 `--bootstrap-only`、`--battleship-only`、`--gift-requests-only`
或 `--deck-only`，这些场景不计为对战通过。领舰走原生教学；赠送走现有账号的
正常邮件业务，需目标服务器启用正常赠送服务并独立验证处理结果。编队从私有
账号配置的 `[deck] unit_uid0` 至 `unit_uid3` 读取四台实际持有的机体，等待原生
保存和出击舰回包。准备完成后重新登录同一账号运行对战，不补写机体或数据库记录。

两个组件均作为开发类 Mod 接入 Mod Manager。Python 运行器是执行有限测试任务的
开发命令，不是玩家启动器或常驻服务。账号配置和运行回执不得放入分发包或源码仓库。

开发者明确需要可见录制时，`prepare` 加 `--visible --sfx-only`，构建 `game_test_audio` 目标，再运行
`python tools/game_test_record.py --run-dir build/gametest/pvp --output E:\videos\SDGO-Mod-GameTest`。
仅测试副本关闭 BGM、启用音效。每场通过开局和 AI 断言后录制最多 25 秒，画面和声音
只取自该测试游戏进程。失败、静音或黑屏片段会清理；保留素材仍需检查交战情况与构图，
AI 断言通过本身不代表画面适合剪辑。
