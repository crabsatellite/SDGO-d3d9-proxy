# 原生 GameTest

开发用可选 Mod，默认关闭。测试在真实客户端进程内执行，通过正常业务请求
与真实服务端通信。外部运行器负责准备独立客户端、启动和收集结果，不操作桌面。

每个场景按“发起请求 → 等待真实回包 → 断言状态”执行。登录、服务器数据、
建房和战斗加载均走正常游戏流程，不伪造机库、房间、机体记录或服务端回包。

测试账号与注册重试规则保存在本地私有配置中。缺少步骤、断言失败、原生异常
和超时都会判为失败，结果输出为 JSON 和 JUnit XML。仅安装 Mod 不会自动运行测试。

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

示例依次测试普通、死亡竞赛和队长模式。每种模式都经过
建房、真实开局、AI 状态断言和正常结束返回；也可指定单个模式排查。
这三种模式已完成真实客户端全流程测试。运行器也接受 Grid（`6`）和 Tag（`7`），
但仍需适配原生 VS 频道选择，尚未验证其成功路径；服务端拒绝会判为失败。
回执位于运行目录的 `receipts/report.json` 和 `receipts/junit.xml`。

两个组件均作为开发类 Mod 接入 Mod Manager。Python 运行器是执行有限测试任务的
开发命令，不是玩家启动器或常驻服务。账号配置和运行回执不得放入分发包或源码仓库。
