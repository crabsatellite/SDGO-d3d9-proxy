# 服主管理的原生商城 Mod 0.1.0

本模块让服主通过配置增加、查询、修改和下架**由本模块管理的单物品商品**。
玩家继续使用原生商城、购买确认、扣费和发货路径。无需在玩家客户端运行 Python、SQL 工具或管理服务。
源码独立于扭蛋机、AI 和其他功能；租赁/中文显示补丁单独分发。

## 工作方式

宿主是当前原生 WorldServer（timestamp `548A86E3`、SizeOfImage `0010C000`）。
模块使用已有 `WorldServer.ini` 的 `DB Game` / `DB StoreBilling` 配置连接数据库，不把连接凭据写入导出配置。
启动时把 catalog.ini 中的定义在事务中写入原生商城表。仅更新自己的产品和销售选项，不替换任何原生过程，
不修改账号、余额、仓库或购买历史。

普通商城币种为 **GB=原生1/GP，MB=原生2/GC**。这与活动扭蛋机的另一套配置不是同一个问题。
价格是正整数；原生购买窗口把零价选项视为不可用，配置工具因此拒绝零价而不是生成无法购买的商品。
永久机体的 validity=1；租赁机体为实际天数；消耗品为原生选项所定义的数量。不可通过把租赁期限改成0
来冒充永久，必须选择对应的原生永久类别。

新增 SKU 以现有单物品商品为原生模板，使用独立产品 ID / SKU，保留稳定选项 ID。
运行时校验模板类别、页签区间、现有 ID 碰撞及拥有关系。商品必须是原生类别表里的合法物品，机体还须可出击。
不支持凭配置创造新的机体/资源、复杂多物品礼包，或覆盖其他模块/原版商品。

## 生成配置

宿主配置工具使用 Python 3 和 `pyodbc`；原生运行时模块和玩家客户端不需要 Python。
只读导出当前商品与物品类别（不读取玩家数据）：

```powershell
python host-tools/shop_catalog.py snapshot --server-ini "D:\Server\WorldServer.ini" --output snapshot.json
python host-tools/shop_catalog.py init --id my-server-shop --file shop.json
python host-tools/shop_catalog.py items --snapshot snapshot.json --search Zaku
python host-tools/shop_catalog.py add --file shop.json --snapshot snapshot.json --key starter-zaku --category 1120022 --price 500 --currency GB
python host-tools/shop_catalog.py compile --file shop.json --snapshot snapshot.json --output catalog.ini
```

`catalog.ini` 是 UTF-16LE，只有商品定义，没有 IP、SQL 密码、账号或本机绝对路径。

```powershell
python host-tools/shop_catalog.py list --file shop.json
python host-tools/shop_catalog.py show --file shop.json --key starter-zaku
python host-tools/shop_catalog.py update --file shop.json --key starter-zaku --patch changes.json
python host-tools/shop_catalog.py delete --file shop.json --key starter-zaku
```

`changes.json` 如 `{"price":900,"currency":"MB"}`。`delete` 是下架，不复用编号、不清空历史；旧 JSON 自动备份。
JSON API 的 `create/update/retire/validate/compile_catalog` 函数可直接供独立编辑器 UI 调用。
产品身份不可原地修改；变换页签/身份时建立新条目。

## 安装、应用与卸载

通常先关闭目标 WorldServer；脚本不会停止任何进程。其他目录/分支的服务不动。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Server\WorldServer" -ConfigFile .\catalog.ini
```

安装后正常启动 WorldServer 即会应用配置。更新配置沿用同一安装命令，已有共享框架和其他 Mods 保留。
没有框架时自动补齐随包的 World loader/core；本模块不改 core ABI。
未导入配置时模块默认关闭，避免仅复制文件就改变商城。

首次安装且希望当前 WorldServer 继续运行时，可额外传 `-DeferActivation`。
它只允许添加尚不存在的 catalog 目录，不更新运行中已有模块，也不改名正在使用的第三方 wrapper。
启动 DLL 在下次正常重启时加载；在此之前可使用下方原生工具应用商品配置，现有商城列表会继续走原生查询。
安装记录会明确写出 `startup_activation=deferred`，不能把文件落盘当作 DLL 已加载。

原生宿主管理工具也使用与 DLL 完全相同的应用代码，可在维护时显式应用：

```powershell
.\payload\shop_catalog\shop_catalog_apply.exe --host-ini "D:\Server\WorldServer\WorldServer.ini" --catalog .\catalog.ini --output "D:\Server\WorldServer\sdgo\mods\shop_catalog"
```

应用成功会生成 `runtime-display.ini`。将它交给玩家，通过独立的 ShopLabels 补丁导入，
可显示原生 CP874 销售选项无法携带的中文文字；数值租期/永久状态由显示补丁直接从原生字段生成。
玩家需要与宿主一致的原生游戏资源，新增商品使用已有类别图像和机体定义。

手动验收和增删改查示例见[测试步骤](shop-testing.zh-CN.md)。本机测试辅助脚本默认只列配置，
只有显式执行 `TestOffers` / `ChangePrice` / `ClearOffers` 才应用相应商品改动。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Install.ps1 -Target "D:\Server\WorldServer" -Uninstall
```

卸载先通过原生工具下架本 catalog 的商品，再备份移出 Mod。数据库不可连接时保留 Mod 供恢复，
不会假装商品已经下架。共享框架保留。

## 一致性与验证

事务失败不提交；未知/被占用的 ID 不覆盖。重复应用保留原生选项 ID。
配置中移除的自有商品会下架，原版条目逐行保持不变。
数据库价格、币种、期限和导出的客户端显示条件来自同一应用结果，自定义标签不能掩盖实际租赁状态。

本机验证包括实际 32 位运行时、全新隔离 SQL 库、原生列表/筛选/结算过程、合成余额的 GP/GC 扣款、
Unicode 保存、重复应用、更新、碰撞回滚和原版条目保留。没有读写真人余额/仓库。
另验证了实际 DLL 入口、打包后的原生工具、安装/卸载下架、旧页面购买可见性检查、相对路径，
以及产品配置损坏且无 Game 数据库连接时仍可停用自有商品。离线全套回归和安装包测试见
[验证记录](shop-validation.zh-CN.md)。
真人登录、游戏内实际购买/发货与视觉验收仍由用户手动完成，未把这些静态/隔离测试当作真人验收。
