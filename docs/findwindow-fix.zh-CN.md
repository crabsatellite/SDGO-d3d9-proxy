# 通用输入法修复（FindWindowW）

修复输入法让游戏卡死或崩溃的问题，不区分输入法。整合包的 `SDPlus.dll` 破坏了游戏进程里所有 DLL 的
窗口查找，本修复为游戏本体以外的所有代码恢复正常查找。English: [findwindow-fix.md](findwindow-fix.md)

| mod | 运行在 | 负责 |
|---|---|---|
| `findwindow_fix` | 客户端 | 整个功能 |

与 [`ime_fix`](ime-fix.md)（修正游戏读取已提交文字的长度）相互独立。

## 使用

无需操作，安装客户端后即生效。本 mod 需要同一版本的 `sdgo\core.dll`（使用 `HookSystemExport`），
客户端安装脚本会一起更新。关闭：在 `sdgo\sdgo.ini` 的 `[mods]` 下写 `findwindow_fix=0`。

## 设置

`sdgo\mods\findwindow_fix\mod.ini`：

| 键 | 默认 | 含义 |
|---|---|---|
| `Trace` | 0 | 1：每次外部窗口查找都记录返回的句柄（不记录窗口名）；0：只记录第一次成功的查找 |

## 日志与排错

```
findwindow_fix: hooked system export user32.dll!FindWindowW
findwindow_fix: ready: normal FindWindowW for every caller except GOnline.exe and SDPlus.dll
findwindow_fix: loaded FindWindowW fix 1.0.0
findwindow_fix: window lookup for an external caller: 000105DC
```

| 日志 | 含义 |
|---|---|
| `window lookup for an external caller: ...` | 游戏以外的某个 DLL 查找窗口，得到了真实句柄 |
| `core lacks HookSystemExport` | `sdgo\core.dll` 比本 mod 旧，请重新安装整个客户端包 |
| `SDPlus.dll is not loaded; nothing to fix` | 该游戏目录没有 `SDPlus.dll`，窗口查找没有被破坏，本 mod 不启用 |
| `hook system export user32.dll!FindWindowW failed: ...` | 其它 mod 已经占用了这个 hook |

本修复只针对窗口查找。如果某个输入法仍然让游戏卡死，原因不在这里。

## 实现原理

`SDPlus.dll`（整合包的汉化插件，由其 `GOnline.exe` 导入）把 `user32!FindWindowW` 改成了
`SDPlus.dll+0x3020` 处的一段代码，它根本不查找窗口：

* 类名 `_GONLINE_` 且标题 `Gundam Online` 时返回 NULL（"没有在运行"）；
* 其它任何查询返回 -1 或 +1（字符串比较的符号），这不是窗口句柄；
* 类名为 NULL 或 atom 时按字符串读取，游戏直接崩溃。

`GOnline.exe` 只调用一次 `FindWindowW`（`0x004914F6`），参数正是这组类名和标题：单开检查。SDPlus 的
返回值让游戏可以多开。进程里其它查找窗口的代码则拿到假句柄或者崩溃。输入法运行在游戏进程里，所以任何需要
查找窗口的输入法（例如查找自己后台进程的窗口）一开启就会让游戏卡死或崩溃。实际发现并在游戏里验证的是讯飞：
它的输入法服务查找服务端窗口（`iFlyPlatform.exe`），把初始化请求发给假句柄，然后在游戏的界面线程上无限
等待回复。从不查找窗口的输入法（例如微软拼音）不受影响。

本修复不判断调用者是哪个输入法（或其它 DLL），游戏本体和 SDPlus 以外的所有调用都恢复正常查找。

`findwindow_fix` 通过核心的 `HookSystemExport`（MinHook）hook 同一个导出函数，trampoline 把 SDPlus 的
改动保留为上一层 hook。加载时记录 `GOnline.exe` 和 `SDPlus.dll` 的地址范围，按每次调用的返回地址区分：

* 来自这两个模块的调用交给 SDPlus 的代码，单开检查（多开）保持不变；
* 其它所有调用（包括之后才加载的 DLL 和动态生成的代码）改用
  `FindWindowExW(GetDesktopWindow(), NULL, class, title)`，与 `FindWindowW` 的查找范围一致：只找顶层窗口
  （父窗口为 NULL 时 `FindWindowExW` 还会返回 message-only 窗口），atom 和 NULL 参数含义不变，且不改动
  last error。

SDPlus 还改了 `MultiByteToWideChar` 和 `WideCharToMultiByte`，但只是把代码页 874（泰文）换成另一个，
其它代码页原样通过，不影响输入法。

测试：构建后运行 `tests/findwindow_fix_test.ps1`，在独立桌面上用真实的核心和 mod 对照一份 SDPlus 代码的
副本，调用方模拟为 `SDPlus.dll` 和 `generic_input_method.dll`。检查内容：有游戏窗口时单开查询仍返回 NULL；
外部查找（后启动的服务端、不存在的服务端、message-only 窗口、atom、NULL 类名、大小写）与 `FindWindowW`
一致且从不进入 SDPlus 的代码；last error；关闭开关；旧核心被拒绝；`HookSystemExport` 加载失败时的回滚。
