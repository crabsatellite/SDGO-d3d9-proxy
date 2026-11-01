# 借助 AI 排查和修复

项目：[SDGO-Mod-Framework](https://github.com/crabsatellite/SDGO-Mod-Framework)。
可以从 Manager 每个功能下的“复制给 AI”开始。没有开发环境时，先收集可复现的问题并创建 issue；
如果愿意尝试修复，再由 AI 根据你的环境引导读取源码、安装构建工具、建立分支和运行测试。

## 开发环境与步骤

1. Fork 仓库，在自己的分支或独立工作区上修改，保留当前游戏、配置和账号数据。
2. 先读相关功能文档与源码，以及仓库中的 `AGENTS.md`（如有）。定位行为，不根据名字猜实现。
3. 原生 Mod 使用 Windows、Visual Studio 2022 C++ 工具和 Win32 CMake 构建；参见[框架开发文档](framework.md)。
4. Manager 的开发依赖在 `manager/requirements-build.txt`，开发入口是 `tools/mod_manager.py`。
   运行 `python -B -m unittest discover -s tests -p test_mod_manager.py -v` 验证 Manager 接口和配置行为。
   用 `tools/build_mod_manager.ps1` 构建单一 EXE，另做其实际启动与界面检查。
5. 先复现问题，再运行与修改相关的测试。涉及游戏内表现时，区分离线测试、运行时证据和人工对局验收。
6. 检查 diff，只提交相关源码与通用配置。不要提交原生游戏文件、凭据、真实账号、存档、私有快照或未经脱敏日志。
7. 创建 PR，按模板写明问题、改动后的行为、真实验证结果和未验证部分。

## PR 必须披露使用的模型

PR 正文必须有 `AI models:` 一行，写实际使用的模型名称和版本。有多个模型就分别列出。
工具没有披露精确版本时明确写未知，不能按产品称呼或经验猜测；没有使用 AI 时写 `None`。
填写 AI 参与的范围、人工核对过的内容、实际测试以及剩余不确定性，方便维护者结合证据评审。

例如：

```text
AI models: 实际模型名称 / 版本；精确子版本未知时明确说明
AI scope: 根因调查、代码修改、测试用例
Human verification: 实际核对的内容；未做时写 Not performed
Remaining uncertainty: 还没验证的环境或游戏内行为
```

提交时有 CI 检查披露字段是否填写；字段内容的真实性和修复可靠性仍需要审阅。
模型信息写在 PR 正文，不作为提交作者或 `Co-Authored-By` 行。
