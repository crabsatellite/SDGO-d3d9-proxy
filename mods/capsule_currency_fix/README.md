# 活动扭蛋机 9301 币种显示修复

这个 Mod 只修复一个已确认的客户端显示错位：

- 活动扭蛋机 9301 的客户端记录是 `Type=0, Price=99`，所以界面显示 `99 G币`。
- 服务器 `tbl_ItemMachinePrice` 的同一机台是 `Type=2, Price=99`。
- `WorldServer.exe` 的 `CRequest::ReqPickItemMachine` 静态反编译表明，`Type=0`
  经 `SP_PocketGP_IO` 扣 G币，`Type=2` 经 `SP_GameCoin_IO` 扣代币；抽取失败时按同一币种退款。

因此正确修复是让 9301 的客户端界面显示 `99 代币`，而不是更改服务器的真实扣款。

## 运行方式

Mod 通过 core 的 named symbol 和 `HookCode` 挂接
`CapsuleMachineUI::LoadCapsuleItemData`。原函数完成后，Mod 检查它刚生成的
活动机显示向量副本：

1. `begin <= end <= capacity`；
2. 使用 179 字节记录边界；
3. 槽位数等于 `页数 × 8`，且不超过 4096；
4. 恰好存在一条 `MachineID=9301`；
5. 该条目仍为 `Price=99`；
6. 币种只允许已正确的 `Type=2`，或已知错误的 `Type=0`。

只有六项全部成立且币种为 0 时，才把该显示副本的 `Type` 一个字节改成 2。
它不修改 `0099.gdt`、catalog overlay、服务器数据库、价格、奖池、扣款或退款逻辑。
错误价格、未知枚举、重复 9301、损坏的 vector 或其他布局都会 fail closed，保留原数据。

## 文件

- `capsule_currency_fix.cpp`：原生 HookCode 入口和内存保护检查。
- `capsule_currency_logic.*`：可单元测试的严格向量检查与单字节修复。
- `capsule_currency_fix_test.cpp`：覆盖补丁、幂等、错误价格、未知枚举、重复条目、
  缺失条目和错误页形状。
- `verify_source_fixture.py`：只读解密并核对当前底包 `0099.gdt` 的 SHA-256、
  93 条记录、12 页/96 槽形状，以及唯一 9301 的 `Type0/Price99` 前提。
- `symbol-proposal.ini`：独立的、可离线验证的 symbol 提案。
- `evidence/`：WorldServer 静态反编译证据；不是运行时抽奖证明。

## 离线验证

从仓库根目录执行：

```powershell
python tools/verify_symbols.py `
  E:\Dev\sd-re\SDGO-client\GOnline.exe `
  mods\capsule_currency_fix\symbol-proposal.ini

python mods\capsule_currency_fix\verify_source_fixture.py `
  E:\Dev\sd-re\extracted_data\gdrs\0099.gdt
```

单元 fixture 可由父级 CMake 目标把 `capsule_currency_logic.cpp` 与
`capsule_currency_fix_test.cpp` 一起编译。测试成功只证明记录检查和修改范围；
它不证明游戏内 Hook 已加载，也不证明真实账号扣款。游戏内进入活动扭蛋页和实际抽取由用户手动验收。
