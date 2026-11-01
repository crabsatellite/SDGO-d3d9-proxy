# 商城租赁与中文选项标签补丁 0.1.0

这是独立的客户端显示补丁，和商城上架/定价功能分开安装。它明确显示「租赁 7 天」「永久」，
并支持宿主导出的中文选项文字。价格、币种、期限数值和购买流程不变。

截图商品的原始销售选项为泰文 `7วัน`。当前客户端按 CP874 读取销售选项名，这条文字在相关
显示控件中变成问号。补丁按原生数值期限和类别的租赁标志生成 Unicode 中文标签，覆盖鼠标悬停
详情、购买窗口和赠送窗口。自定义文字不能遮盖实际租赁/永久状态。

## 安装与卸载

解压到任意目录，关闭**目标目录里的**游戏。其他目录的客户端可以继续运行。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Game"
```

安装使用相同的 SDGO mod framework API 1。有框架时只安装这个 Mod，增量追加自己的符号段；
已有共享 core、sdgo.ini、其他 Mods 和其他符号段保留。没有框架时补齐随包提供的 loader/core；
已有的第三方 d3d9 wrapper 会串联，遇到已有占用的链文件会拒绝覆盖。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Game" -Uninstall
```

卸载只停用本 Mod，配置移到 `sdgo/shop-backups/`；共享框架保留，其他 Mods 不动。

支持原生 `GOnline.exe`，timestamp `54BE1568`、SizeOfImage `00C97000`。安装时验证相关代码字节，
运行时仍由 core 验证签名和 Hook 所有权。不匹配的版本不会安装。

## 可配置项

`sdgo/mods/shop_labels/mod.ini` 的 `[settings]`：

- `ShowRental=1`：显示实际租赁/有效天数。
- `ShowPermanent=1`：对永久机体明确显示永久。
- `UseUnicodeOverrides=1`：读取可选的 UTF-16LE `display.ini`。

服主使用商城配置模块生成的 `runtime-display.ini` 可作为客户端 `display.ini` 导入：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Game" -ConfigFile "D:\Share\runtime-display.ini"
```

仅当选项 ID、类别、期限、价格和币种都匹配时使用自定义文字，旧配置会回退到原生或数值期限标签。
这不是全局字体/聊天编码补丁，也不修复已在数据库中丢失成问号的任意商品名。
已确认的活动扭蛋 GB/MB 显示问题由扭蛋机分支处理；本补丁不交换任何币种值。

## 验证范围

真实 32 位 DLL 接口测试验证了悬停、购买、赠送、UTF-16 文字、过期配置回退和业务记录不变。
同时检查了原生二进制符号。测试使用原生 ABI 的模拟控件；真人游戏内的视觉与交互验收由用户进行。
安装包另经 Windows PowerShell 5.1 的中文路径、初装、更新、卸载、第三方 wrapper 串联、
文件/签名冲突及运行中目标拒绝测试；共享框架、其他 Mod 和无关符号字节保留。
