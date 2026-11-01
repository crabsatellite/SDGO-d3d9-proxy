# 房名组合指令

core 提供统一的 `FindRoomCommand` 公共接口；`room_random`、`pvp_ai`、`coop_ai` 使用同一套解析规则。
各 Mod 分别读取自己的命令与参数，不消费、不删除、不改写房名。后续 Mod 可复用这个接口。

| 房名 | 效果 |
|---|---|
| `!rand !ai` | 真人每局从各自机库随机；AI 按该模式默认规则补位 |
| `!rand !ai3` | 真人随机；PvP 请求 3 个敌方 AI，合作任务请求 3 个僚机，人数仍受空位限制 |
| `!ai3 lv4 !rand` | 同上，AI 使用 4 级机体池 |
| `!rand !ai e2 a1 lv4` | PvP 请求 2 个敌人、1 个队友，AI 使用 4 级池；真人每局随机 |
| `!ai e2 a1 lv4 !rand` | 与上一行相同，指令顺序可交换 |
| `!rand !ai0` | PvP 只随机真人，不添加 AI；合作任务的 `!ai0` 沿用原有的默认补位含义 |

命令之间以空格分开，英文大小写不敏感。`lv4`、`e2`、`a1` 要放在 `!ai` 后、下一个 `!指令` 前。
例如 `!ai !rand lv4` 中的 `lv4` 属于 `!rand`，不会改变 AI 等级；应写成 `!ai lv4 !rand`。
未知的 `!指令` 同样划分参数范围，避免以后增加 Mod 时发生参数串用。
同名命令重复时使用第一个有效命令。`!aim`、`房间!ai3`、`!ai3x`、`!rand!ai3` 不会当作 `!ai`。

当前原生房名最多 **20 字节**，组合不会扩大这个限制。上表两个完整 PvP 组合各为 19 个 ASCII 字节。
加入中文房名时应留意客户端实际使用的编码与字节数；复杂组合建议使用短命令 `!rand`。

随机 Mod 只处理真人；AI 的机体池、数量、队伍与复活继续由 AI Mod 控制。三个服务端 Mod 使用不同的
Hook 地址，core 仍执行每个 Hook 只能有一个所有者的规则。AI 所需的客户端 Mod 继续按原有安装说明配置。

## 开发接口

公共 C ABI 位于 `include/sdgo/sdgo_api.h`。`SdgoRoomCommand` 返回原房名中的命令和参数字节范围，
可选数字后缀通过 `SDGO_ROOM_COMMAND_ALLOW_COUNT` 启用，例如 `!ai3`。解析有界、无状态、可并发调用。
API 1 的可选字段通过 `api->size` 检查，不改变既有字段的布局。
已有每日奖励 Mod 使用的 `HookSite` 扩展保持原位置，房名接口位于其后。
早期房名预览包 0.1.1/0.1.2 尚未包含这个兼容安排，应通过新版安装器一起更新 core 和三个服务端 DLL，
不要混用早期预览 DLL 与新版 core。现有每日奖励 DLL 无需重编译。

建议通过 `include/sdgo/room_commands.h` 调用：

```cpp
SdgoRoomCommand command{};
if (sdgo::room_commands::Find(api, title, titleLength, "ai",
                            SDGO_ROOM_COMMAND_ALLOW_COUNT, &command)) {
    // command.arguments_offset / arguments_length only cover this command.
}
```

有新 core 时使用公共服务；旧 core 缺少该可选字段时，SDK 使用完全相同的公共解析实现。
现有 Mod 需要重新构建才能采用新规则。Beta 安装包同时提供新 core、随机 Mod，以及已安装 AI Mod 的 DLL 更新，
保留 AI 的 `mod.ini`、`config.ini` 与服主设置；不会自动启用或新增尚未安装的 AI Mod。
若宿主已装有可选的 `ai_catalog`，更新后的 AI Mod 继续读取该进程内提供的机体池；不安装或修改其数据。
未安装此提供者时，继续使用 AI Mod 原有等级池。

离线检查覆盖顺序互换、参数隔离、数字边界、错误前缀、20 字节边界、旧 core 回退、
真实 core 的 API 调用与合作任务组合指令的僚机插入。实际多人游戏表现仍需要手动验收。
