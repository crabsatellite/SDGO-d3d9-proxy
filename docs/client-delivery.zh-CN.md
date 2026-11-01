# 完整客户端安装与同步

朋友服客户端由 [Codeup 的 SDGO-client main](https://codeup.aliyun.com/65bb261da854e4c241ac5426/crabsatellite/SDGO-client) 发布。
`install.bat` 用于首次安装或完整修复，`update.bat` 用于后续同步；两者先取得当前更新器，再执行同一套完整运行文件恢复。
玩家从 Codeup 下载 `main` 的 ZIP，解压后直接运行 `install.bat`。脚本、程序和基础资源分段都从 Codeup 获取，连接失败即停止，不回退到 GitHub。

同步范围包括游戏程序、公共配置、Mod、原生基础资源及资源覆盖文件。以固定 Git 提交作为文件集来源，
恢复缺失或被修改的文件，将过时运行文件移入备份，并在结束前核对全部文件。未通过整体核对不得显示更新完成。
个人登录/设置、截图、日志、Git 状态和恢复目录保留。

基础 `GPackData.zpk` 由已验证的历史 Git 分段自动重建，完整长度和 SHA256 必须与参考客户端一致。
不再依赖 `1_SD敢达文件恢复.exe`，玩家不用手动解压或拼接资源。分段是更新器的内部数据来源，不单独分发补丁包。

开发者发布客户端变更时，在客户端仓库运行：

```powershell
python maintenance/publish_runtime.py --reference "D:\Games\SDGO"
```

该命令从服主的参考客户端生成完整运行快照，保留旧入口所需的兼容指纹；安装器从已固定的 Git 树读取实际运行文件集。
更新器代码有 Windows PowerShell 和真实 BAT 入口验证。修改基础资源交付逻辑时，应在独立空目录完成一次整套客户端安装，
再读回全部运行文件和完整资源包。不要在验证中登录或修改真实玩家的库存。

`check-client.bat` 比较实际文件，结果写入 `.sdgo-updater/client-audit.json`；它不把“下载校验通过”当成“安装内容一致”。
