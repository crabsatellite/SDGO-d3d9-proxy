# 商城与显示补丁手动测试

先按各包 README 安装。客户端显示补丁要在重新启动游戏后加载；首次免停服安装的 World 模块
会在下次正常重启加载。原生 `shop_catalog_apply.exe` 可以先应用商品配置，不要求现在重启服务。

## 显示补丁

1. 正常启动游戏并进入商城。找到 `DUAL GUNDAM ASSAULT SHROUD + BUSTER GUNDAM`，
   原商品 ProductId=1010465，售价 9900 MB。
2. 分别看鼠标悬停详情、购买选项、可用时的赠送选项。原 `???` 期限应显示为「租赁 7 天」。
   无需确认购买就能检查这些文字。
3. 查看一个原生永久机体，其选项应明确显示「永久」。原商品价格、币种不应被显示补丁改变。
4. 客户端 `sdgo/logs/sdgo.log` 应出现 `shop_labels: loaded` 或
   `shop_labels: rental/permanent labels enabled`，且不应有本模块的符号/Hook 失败。

补丁范围是商城销售选项的租期与可选 Unicode 标签，其他商品标题、聊天或数据库已丢失的文字需另查。
活动扭蛋机 GB/MB 问题属于扭蛋机模块。

## 配置商品

服主先用 `shop_catalog.py snapshot` 读取本服现有配置，再 `init/add/compile` 建立自己的定义，
命令见[商城说明](shop-catalog.zh-CN.md)。JSON 的 `enabled=false` 表示隐藏，`delete` 表示保留编号并下架。
每次改动编译后，通过原生 helper 应用，再将成功的 catalog.ini 留在 Mod 目录供下次启动使用。
修改价格和下架后关闭再打开商城，避免只看旧页面。

本机安装附有一个独立测试目录及 `Shop-Test.cmd`；它的三件示例商品**不会自动上架**。
在该目录打开终端：

```powershell
.\Shop-Test.cmd Status
.\Shop-Test.cmd TestOffers
```

然后在原生商城机体页搜索 `SHOPTEST`，检查：

| 商品标题 | 币种/价格 | 销售选项 |
|---|---|---|
| SHOPTEST GB | 500 GB | Zaku II (Sale)，永久 |
| SHOPTEST MB | 500 MB | Zaku II (Sale)，永久 |
| SHOPTEST RENTAL | 500 MB | DUAL GUNDAM ASSAULT SHROUD + BUSTER GUNDAM，租赁 7 天 |

悬停和打开购买确认框即可核对价格/标签。若要验证结算，用你选定的测试账号分别买 GB 和 MB 商品，
记录前后两种余额以及仓库物品和租期；应只扣对应币种。实际点击购买会正常扣款和发货，不自动退款或删除机体。

```powershell
.\Shop-Test.cmd ChangePrice
```

重新打开商城，`SHOPTEST GB` 应变为 600 GB，其他两件保持 500 MB。修改的是服务端真实商品配置，
本机脚本不登录、不执行购买、不修改玩家余额或仓库。

```powershell
.\Shop-Test.cmd ClearOffers
```

重新打开商城，三件测试商品应消失，原商城商品应保留。已打开的旧购买页面也应被原生可见性检查拒绝。
下架保留编号和已完成的购买记录；若已经买过物品，下架不会从玩家仓库收回。

辅助脚本只在专用测试 catalog 内管理这三件商品；发现其他商品时会拒绝测试操作，避免覆盖服主内容。
实际服主配置使用 `Apply`，默认 `Status` 不应用任何改动。

## 反馈信息

若不符合预期，提供对应页面截图、商品标题、是悬停/购买/赠送窗口，以及客户端日志里 `shop_labels`
相关行。不要发送宿主数据库 INI 或账号密码。画面/实际发货仍属于手动验收，离线与隔离测试记录见
[验证记录](shop-validation.zh-CN.md)。
