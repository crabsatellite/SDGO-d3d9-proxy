# 活动扭蛋币种修复独立包

1.0.1 使用无脚本的 **SDGO Mod Manager 客户端配置包**。
基础框架另由 Manager 安装；本包仅包含 capsule_currency_fix DLL、管理描述、
Mod 清单、已验证符号片段、说明和文件 SHA-256。

关闭目标游戏副本，在 Manager 选择客户端，点击「导入客户端配置包」，
导入 SDGO-CapsuleCurrencyFix-1.0.1.zip。在「修复归档」确认已启用后正常启动游戏。
完整[检查步骤](../../docs/capsule-currency-fix.zh-CN.md)包含列表和确认框两个独立路径。

本包不包含共享 capsule_catalog 的 DLL、配置、图片或 GDT，导入不会修改已有目录资源。
符号片段增加 capsule.activity.load_items，并核对/补齐通用 grid.data、grid.find；
已有同名符号不同则拒绝。通用符号也被其他 Mod 使用，不应单独删除。

旧版的 PowerShell 安装包、Verify-Package.ps1 和旧 manifest 格式仅适用于旧发布物，
不用于此 Manager ZIP。日常安装、开关与分发均走 Manager。

支持 GOnline-54BE1568-00C97000；EXE SHA-256 为
E2DBA73C10302B50CB0E6F2E58DD0A14EFFA8D224989AE570A7F8E05AF79FF92。
本 Mod 只拥有 capsule.activity.load_items Hook，查找 API 为普通调用，
与共享扭蛋目录入口互不冲突。

停用时在 Manager 关闭本修复并重启游戏。内存中的改动会随进程退出清除，
磁盘 GDT、服务端配置和玩家钱包均未被补丁写入。
