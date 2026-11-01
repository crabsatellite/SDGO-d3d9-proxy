# 活动扭蛋机币种显示修复（独立包）

这个包只安装 `capsule_currency_fix`。它与新增扭蛋机的 `capsule_catalog` 分开，
不要求安装 catalog，也不包含 catalog 的 DLL、配置或资源。

它修复 9301 号活动扭蛋机的客户端显示：界面从错误的 `99 G币` 改为
`99 代币`。服务器价格、奖池、真实扣款和退款逻辑均不修改。

## 支持范围

- Host key：`GOnline-54BE1568-00C97000`
- `GOnline.exe` SHA-256：
  `E2DBA73C10302B50CB0E6F2E58DD0A14EFFA8D224989AE570A7F8E05AF79FF92`
- Mod 唯一拥有的 Hook：`capsule.activity.load_items`
- `capsule_catalog` 使用其它入口，两者没有 Hook 所有权冲突。

安装器会同时核对 host key、EXE SHA-256、包内文件 SHA-256 和 symbol 字节。
其它客户端会直接拒绝安装，不会用某个 feature installer 的执行结果代替兼容性证明。

## 推荐安装

先关闭游戏。在本目录打开 Windows PowerShell 5.1：

```powershell
powershell -ExecutionPolicy Bypass -File .\Verify-Package.ps1

powershell -ExecutionPolicy Bypass -File .\Install-CapsuleCurrencyFix.ps1 `
  -GameDir "D:\你的SDGO客户端"
```

`-GameDir` 必须是含 `GOnline.exe` 的目录。脚本不会启动游戏或登录账号。

安装器的范围：

- 安装或更新本包附带的 `d3d9.dll` 与 `sdgo\core.dll`；
- 已有 `sdgo\sdgo.ini` 原样保留；
- 已有其它 `sdgo\mods\*` 原样保留；
- 只新增或更新 `sdgo\mods\capsule_currency_fix`；
- 只合并 `[capsule.activity.load_items]`，不覆盖整个 host symbol 文件；
- 如果已有其它用途的 `d3d9.dll`，改名为 `d3d9_chain.dll` 并串联；
- 被替换的框架文件和本 Mod 旧版本会备份到
  `sdgo\backups\capsule_currency_fix\<时间>`。

## 手动安装

已有 SDGO Mod 框架时：

1. 把
   `payload\client\sdgo\mods\capsule_currency_fix`
   复制到客户端的 `sdgo\mods\`。
2. 打开客户端的
   `sdgo\symbols\GOnline-54BE1568-00C97000.ini`。
3. 从
   `symbols\GOnline-54BE1568-00C97000.capsule_currency_fix.symbol-proposal.ini`
   复制整个 `[capsule.activity.load_items]` 节到现有文件末尾。
4. 不要用 proposal 覆盖已有 symbol 文件，否则其它 Mod 的 symbol 会丢失。

没有框架时，推荐使用安装器。若坚持手动安装：

1. 把 `payload\client\` 内的内容复制到 `GOnline.exe` 同目录。
2. 把 symbol proposal 复制为
   `sdgo\symbols\GOnline-54BE1568-00C97000.ini`。
3. 如果客户端原本已有不同的 `d3d9.dll`，请改用安装器处理串联。

## 人工验收

启动游戏后查看 `sdgo\logs\sdgo.log`：

```text
symbol capsule.activity.load_items ok at 007A69E0
loaded capsule_currency_fix
activity machine 9301 display currency corrected from type 0 ... to type 2 ...
```

然后进入活动扭蛋机页面：

1. 找到 93 号 Full Armor Unicorn / Cherudim Saga 海报。
2. 确认价格显示为 `99 代币`，不再显示 `99 G币`。
3. 91 号仍为 `3 代币`，92 号仍为 `10000 G币`。

实际抽取由用户手动决定。若要验证扣款，应先记录 G币和代币余额；抽取一次后应扣
99 代币，G币不变。离线构建和 Hook 加载日志本身不等于真实账号扣款证明。

## 卸载

关闭游戏后：

```powershell
powershell -ExecutionPolicy Bypass -File .\Install-CapsuleCurrencyFix.ps1 `
  -GameDir "D:\你的SDGO客户端" -Uninstall
```

卸载只把 `sdgo\mods\capsule_currency_fix` 放入回收站，并移除
`[capsule.activity.load_items]`。框架、`sdgo.ini`、其它 Mod 和其它 symbol 节均保留。

临时停用而不卸载，可在 `sdgo\sdgo.ini` 添加：

```ini
[mods]
capsule_currency_fix=0
```
