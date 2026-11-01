# 扭蛋机配置与初始化工具

当前显示与奖池规则修订见 [显示修订记录](capsule-display-fix.zh-CN.md)：105 台固定机、2 台自定义机、
511 条候选；原版框线和图片窗口按像素核对。GB 列表缺字的默认文字缓存问题已修补，实际画面待重启验收。

本分支提供服主使用的本地编辑器、客户端 `capsule_catalog` Mod，以及服务端 SQL 运行时适配器。
编辑器支持固定机与自定义机的新增、查询、修改、下架、恢复、排序、模板选择、机体图片、价格与权重。
“导出配置”生成语义 JSON；“编译客户端”生成游戏资源；配置好本机目标后，“导入到游戏”才实际安装客户端
文件并在指定服务端数据库中安装配置。每一步均显示实际结果，单独导出 JSON 不等于完成安装。

服主选择自己的游戏目录、服务器 INI 和输出目录；发布包不包含本机 IP、数据库口令、玩家存档或本地
测试服管理脚本。玩家只需安装服主生成的 `player` 包，照常进入游戏；玩家安装器不需要 Python。
服务端适配器只接管配置与奖品选择，扣费、发货和失败补偿继续走原生链路。

## 已核对的数据

2026-11-01 只读查询本地服务端，导出配置表与相关原生存储过程；未读取账号、仓库、抽奖记录等玩家表。
当前有 78 台可显示的固定机、2 台自定义机，以及 328 张设计图配方。145 个普通抽取序列表中包含
旧编号/别名，不能直接当成 145 台玩家实际能选择的机台。

固定机的完整客户端定义在 `gdrs/0033.gdt`；自定义定义和候选项分别在
`gdrs/0154.gdt`、`gdrs/0155.gdt`。固定显示记录含机体 ID 和每个机体的两条 SPR 引用。
服务端相关数据不止一张抽奖表：

| 用途 | 服务端配置 |
|---|---|
| 普通机价格、开关 | `tbl_MachinePrice` |
| 普通机原始权重 | `tbl_MachineDice` |
| 普通机预生成序列、初始化状态 | `tbl_CapsuleList1`、`tbl_CapsulePrime`、`tbl_MachineList` |
| 代币变体 | 对应的 `tbl_CoinMachine*`、`tbl_CoinCapsule*` |
| 自定义机价格、8 格限制 | `tbl_CustomCapsuleList` |
| 自定义候选项、权重、累积、加价、重置 | `tbl_CustomCapsuleInfo` |

普通机核对出 36 种精确的等级排列。其中有 `A/B/C/C/C/CR`、`A/B/C/C/C/BR`、
`AR/B/C/C/C/CR`、`AR/AR/A/A/A/A`、`AR/A/A/A/A/A`、全 A 和全 B 等。
CU/BU 位置以及普通/稀有等级差异保存在精确模板中，不能全部合并成相同概率。
槽位位置本身不决定稀有度，原生 `Prob1` 才是配置依据。

普通机原生过程沿预生成序列取数并避免连续重复。`Prob1` 和序列出现次数不完全等同于
每次独立抽样概率。新增配置采用已实现的 `independent_weighted`：服务端使用无偏随机数选择权重区间。
既有条目保留 `native_sequence`；修改既有固定奖池时自动切换为独立权重抽样，单改名称或价格不切换。
必须安装对应服务端版本，不能只修改客户端显示的百分比。

自定义机有 6 个基础格和 2 个扩展格，基础格 4–6 限普通 C，其他格按配置约束选择。
当前 GP 池只有 2 个 SR 候选项，代币池有 5 个；**候选机体数量和可选槽位数量是两件事**。
服务端配置允许继续添加候选项，无需为每个新 SR 增加一格。基础价、逐候选附加价和扩展卡
消耗分别计算；S/A 累积概率也必须保留，不能将所有 `BasicProb` 简单归一化冒充原生算法。

## 初始化依据

输入为只读配置快照与本地客户端，路径全部由参数提供，无本机绝对路径默认值。

1. 用客户端实际目录与服务器开关确定可用的普通/代币/自定义奖池，不把隐藏的旧机台当来源。
2. 通过商城 `tbls_StoreItems → tbls_SubItems.f_categoryNumber → tbl_ItemCategory.RefIndex`
   追踪实际物品，不能根据商品编号猜机体编号。租赁机不作为永久合成材料。
3. 设计图不仅要有配方记录，还要能取得图纸物品，并且所有材料都有来源。
   多张配方按 OR、单张配方内的材料按 AND 计算。重复求固定点，循环依赖不会自行产生来源。
4. 纳入已配置的任务前置链和已连接的结算掉落池。尚未还原的 `UnitMode` 收集条件单独报告。
5. 将变形状态与仓库机体分开：能变形不等于多了一台可消耗的材料机。
6. 排除训练记录、不可出击形态，以及名称为 `name` 且图形引用为默认值的待确认记录。
7. 先补低等级根节点，再重新计算配方路径；最后裁掉冗余根节点。这是确定性的包含极小覆盖，
   不是全局最少机台的数学最优解。

初始化保留既有固定机的内容与权重，新增机台；现有自定义机保留原生基础候选。
新机 ID 避开已有记录和退休编号。能匹配既有六格模板的机体进入新固定机，其他合规等级进入
自定义候选池前还要满足新增规则：只允许 SR，或完全没有设计图配方的 SS，并继承同等级的原生参数。
普通 S、有配方 SS 不进入自定义池。缺少已核实卡图的特殊等级保留在未配置报告，不使用全身小图冒充头像卡。
默认继承所选原版模板价格，例如全 A 模板 14800、全 B 模板 7500，混合池按各自模板定价。
只有明确提供 `--price` 时才统一覆盖新建机台价格；既有机台和手动价格不会因再次初始化被重定价。
每台新池最多放 4 个新根，另保留至少 2 个补位槽，按使用频次轮换原有及已实际配置的机体。

这是**配置层面的来源分析**，不代表任务/掉落已在真人游戏中验收。任务的一次性奖励数量、
同时收集多个目标时的材料消耗、活动和管理员赠送不视作已完整验证。

## 使用

只读导出宿主自己的配置。需要宿主已有的 SQL ODBC 驱动和 `pyodbc`；不会安装软件，
不会导出 INI 里的连接凭据。输出目录必须是新目录，导出失败时不覆盖已有快照。

```powershell
python tools/export_capsule_snapshot.py --server-ini "D:\Server\StoreServer.ini" --output "D:\CapsuleConfig\snapshot"

python tools/init_capsules.py inspect --snapshot "D:\CapsuleConfig\snapshot" --client "D:\Game" --output "D:\CapsuleConfig\baseline"

python tools/init_capsules.py initialize --snapshot "D:\CapsuleConfig\snapshot" --client "D:\Game" --output "D:\CapsuleConfig\initial"
```

输出有 `catalog.json`、`patterns.json`、`initialization-report.json` 和 `bundle/`。
报告包含全部补入机体、阻塞的配方、人工确认记录与素材缺口。重复运行不重复追加，
快照和客户端指纹变化时拒绝将新输入混入旧目录。

配置增删改查：

```powershell
python tools/init_capsules.py catalog list --file catalog.json
python tools/init_capsules.py catalog show --file catalog.json --id 7901
python tools/init_capsules.py catalog create --file catalog.json --id 10501 --payload machine.json
python tools/init_capsules.py catalog update --file catalog.json --id 7901 --payload changes.json
python tools/init_capsules.py catalog delete --file catalog.json --id 7901
python tools/init_capsules.py catalog list --file catalog.json --kind custom_machines
python tools/init_capsules.py catalog validate --file catalog.json
python tools/init_capsules.py catalog export --file catalog.json --output bundle
```

`changes.json` 例如 `{"price": 2400}`。创建需要一条完整机台记录，可从已有记录复制后修改 ID、
名称、机体和价格。固定机必须 6 格且正整数权重之和为 1000；自定义候选项必须满足至少一格
的等级规则。写操作会保存旧配置的内容寻址备份。删除为下架，保留编号；不删除任何玩家历史。

## UI 与运行时安装

UI 已实现可搜索机台列表、固定/自定义类型选择、模板选择、机体与图片预览、价格、
逐槽权重、候选项增删、启用/下架、导入配置、导出配置、客户端编译与目标安装。
原生组、资源引用和抽取策略在 UI 中作为只读来源信息展示；权重同步显示百分比。
普通玩家游戏内继续使用原生抽奖界面；机台定义属于服主配置，玩家个人自定义槽位属于另一种状态。

客户端和服务端 payload 共享 `catalog_revision`，manifest 校验两个文件的实际哈希。
安装工具检查客户端版本、资源哈希、两侧配置版本与数据库权限，备份被替换的文件并保留其他 Mod。
SQL 安装失败时回滚 SQL 事务并恢复客户端文件。修改或下架不会清空玩家历史。
实际账号扣费、机体发货和自定义累积概率仍需在游戏内人工验收。

客户端 Mod 拦截主程序静态链接的文件读取路径，仅对验证过的 4 张扭蛋 GDT 和专用资源覆盖生效，
不依赖客户端加载额外 Zoa DLL。GDT 校验桥仅对对应覆盖流的一次读取生效，其他文件沿用原始校验。
原始游戏文件不需要重新打包。服务端生成 `install.sql`、`verify.sql`、`rollback.sql`，
只写配置表和专用运行时表；不调用会清空共享工作表的旧建池过程，也不访问玩家表。

服主启动示例（路径由服主填写；`--server-ini` 中的凭据只在本机读取）：

```powershell
python tools/capsule_ui.py serve --catalog "D:\CapsuleConfig\initial\catalog.json" --patterns "D:\CapsuleConfig\initial\patterns.json" --assets "D:\CapsuleConfig\assets" --bundle-output "D:\CapsuleConfig\bundle" --client "D:\Game" --lookup "D:\CapsuleConfig\unit_portrait_lookup.json" --install-client "D:\Game" --server-ini "D:\Server\StoreServer.ini" --framework-stage "D:\CapsuleEditor\framework-client" --open
```

不提供安装目标时仍可编辑、导出和编译，界面不会假报“已导入”。目标客户端运行时拒绝覆盖；
安装完成后提示服主重启自己的原生服务，以刷新价格和候选池缓存。工具不会重启其他开发线。

安装返回的 `package_path` 包含 `player/`、`server/`、`semantic/` 和根 manifest。
给玩家分发整个 `player` 目录，其中有安装器、客户端文件、符号片段和哈希清单。
玩家先关闭目标游戏，再执行：

```powershell
.\Install-Client.cmd -GameDir "D:\Game" -CheckOnly
.\Install-Client.cmd -GameDir "D:\Game"
```

安装器保留已有 `sdgo.ini` 设置与其他 Mod 的符号段，遇到非本 Mod 的符号冲突时拒绝安装。
未知 D3D9 代理仅在可安全建立链式加载且不存在已有冲突时保留；不会静默丢弃原 DLL。

## 旧图片脚本

旧活动方案主要替换现有池，不负责创建完整的新机台。图片脚本有几个不能直接沿用的点：

- `load_unit_portrait` 根据 SPR 找到 TXR 后读取整张纹理，没有应用 SPR 的 UV 裁切；遇到图集会取错区域。
- 两个 SPR 状态在生成循环中没有分别绘制，不能据此保证两种显示状态正确。
- 默认仅处理 C/B/A，未覆盖本次需要补入的 S/SR/SS。
- 从多张卡共同像素推导边框可能带入空洞，需要对照原生卡逐级验证。

机体全身预览和游戏内头像卡属于不同素材。已有原始 GDT 卡图直接引用原 SPR 两态；
历史生成卡先核对等级条、背景、头像裁切和稀有遮罩，不能因映射存在就当成原生卡。
本轮已移除“全身小图＋英文名称”的回退实现，详细来源与限制见
[显示修订记录](capsule-display-fix.zh-CN.md)。

## 验证

```powershell
$env:CAPSULE_RUNTIME_HARNESS = "<构建目录>\Release\capsule_client_runtime_harness.exe"
python -B -X utf8 -m unittest discover -s tests -p "test_capsule*.py" -v
```

测试覆盖缺少图纸/材料、循环依赖、替代配方、隐藏商品、租赁、变形状态、冗余种子裁剪、
重复初始化、两类机台的增删改查、权重/价格校验、客户端/服务端版本一致性、资源编译、运行时输入边界、
UI 并发版本检查、安装备份与回滚。测试数量以当前运行输出为准；Windows 不允许创建符号链接时，该项跳过。

早期独立客户端的 e023 版本曾取得原生 GDT 注册回执：104 台固定机、78 个原有代币变体、2 台自定义机、
623 条候选记录。当前按 SR/SS 新规则调整为 511 条，主环境 SQL 新连接读回已验证；新卡游戏内读回仍待验收。
SQL 已完成安装/回滚/重装、配置读回和 1,800 次只选择奖品的并发抽样验证，未执行实际账号抽取。
真实 UI 已执行安装，随后重启独立服务并重新启动客户端：取得新的原生注册回执，与本地 Agent 建立 TCP
连接，并完成 120 秒的客户端与全部 13 个服务同时存活检查。没有登录账号或执行真实抽取。
原生注册回执不等于已看到游戏内页面，也不等于已完成扣费与发货。

独立测试环境的 World 在快速连续启动各服务时曾退出，旧崩溃也发生于本适配器安装之前。
保留完整扭蛋数据、将服务错峰启动后，两次正常启动观察通过；这是测试环境的启动时序规避，
尚未证明原生崩溃的完整根因。此私有环境管理脚本不在分发包内，服主仍使用自己的服务管理方式。

## 本次初始化后的检查位置

原生固定机列表每页 8 台，按 0033 配置记录倒序显示。105 台配置中，新增机台显示在列表前部：

| 机台 | 固定机页面 | 页内位置 |
|---|---|---|
| 10501 | 第 1 页 | 第 1 格 |
| 10301 | 第 1 页 | 第 3 格 |
| 8001 | 第 4 页 | 第 2 格 |
| 7901 | 第 4 页 | 第 3 格 |

UI 显示选中机台的原生页码和位置。下架保留 ID 与历史，从原生可见列表移除，后续位置随之顺移；
管理列表顺序单独保留。重新排序或下架后，应以本次编译的 `placements.json` 和 UI 为准。
