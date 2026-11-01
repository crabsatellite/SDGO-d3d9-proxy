# 商城模块与独立显示补丁验证记录

验证日期：2026-11-01。Win32 Release，MSVC 19.44，Windows SDK 10.0.26100。
两个模块分别打包，使用 SDGO framework API 1；原生功能测试在独立工作树和隔离数据库进行。

后续已在本机主客户端与 WorldServer 首次安装组件，保留现有服务进程，空 catalog 应用后原 2474 件商品和
3718 个选项保持原样。新增服主窗口沿用 CurrencyGrants 的 WinForms 布局；实际查看主窗口、物品列表、
商品编辑弹窗，点击新增、选择物品、保存草稿和保存本机草稿已验证。20 项窗口业务检查通过，
包括与真实原生应用工具衔接的隔离 SQL 新增、改价、下架、删除、服务端字段读回及玩家标签发布。
导入遗漏保留已删除编号、禁止更改已有 SKU、外部文件冲突和同目录单窗口约束也已覆盖。

## 已完成

- CTest 12/12：服主窗口业务、显示补丁实际 DLL 接口、配置工具、打包与安装、现有 AI、loader、
  FindWindow、原有安装器及增量文件 staging 回归。配置工具内部 11 个用例全部通过。
- 新安装器在 Windows PowerShell 5.1 下验证中文路径、初装、更新、配置导入、卸载、
  第三方 wrapper 串联、已有设置/无关 Mod/符号字节保留，以及运行中目标、坏哈希、
  错误签名、未知 core、符号和链文件冲突时的拒绝行为。
- 真实客户端二进制离线符号检查：84 个通过，0 个拒绝。
- 隔离 SQL 集成测试 26 项通过：实际 32 位原生引擎、Unicode 保存、稳定 option ID、
  原生 GB/MB 列表筛选与结算字段、合成余额 GP/GC 分别扣款、价格更新、编号碰撞整批回滚、
  下架、原生购买可见性检查、相对路径、损坏配置停用、实际 DLL 入口及未知宿主拒绝、
  打包工具应用与卸载下架、原版父商品逐行保留。
- SQL 只复制商城配置表、类别/机体元数据和选定原生过程到新建测试库，余额为新建合成账号。
  原有玩家表、真人余额、仓库与购买记录不参与测试。
- 后续安装改进另验证运行中 WorldServer 的显式首次延后安装、拒绝运行中更新，以及手动测试辅助脚本的
  Status / TestOffers / ChangePrice / ClearOffers 四个动作；动作验证仍在独立 SQL 测试库进行。

旧 staging 测试在本机 Windows PowerShell 的模块自动导入环境中曾找不到 Get-FileHash；
显式导入 Utility 后通过，完整 CTest 使用 PowerShell 7.6.5 通过。新分发安装器使用 .NET SHA256，
已直接在 Windows PowerShell 5.1 通过，不依赖该函数的自动导入。

## 复现

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
python -B tools/verify_symbols.py "D:\Game\GOnline.exe"
```

SQL 测试需服主主动指定本地宿主 INI，且 SQL 登录有新建独立测试库和登录的权限：

```powershell
python -B tests/shop_catalog_sql_test.py --server-ini "D:\Server\WorldServer.ini" --exe build/Release/shop_catalog_apply.exe --output build/shop-sql-test-new --dll build/Release/shop_catalog.dll --package-root "D:\Packages\SDGO-ShopCatalog-Mod-0.1.0"
```

测试目录和独立测试库保留证据，不自动删除。`private-*.ini` 含专用测试登录信息，只留宿主本地，
不进入源码或分发包。打包使用文件白名单、逐文件 SHA256、ZIP CRC 校验并包含许可证。

## 尚未进行的真人验收

显示 DLL 测试使用原生 ABI 模拟控件；没有把真人游戏截图、实际登录、购买发货或存档验证记为通过。
本轮这些实测由用户手动完成。可配置商品范围是原生已有类别的单物品，不包括复杂礼包和新增游戏资源。
可选 Lua 5.0 模拟未启用；它属于已有 AI 测试，不是本商城功能的验收依据。
