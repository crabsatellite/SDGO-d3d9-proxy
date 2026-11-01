# 收藏扭蛋（Beta）

收藏扭蛋是一项可独立安装和停用的玩法 Mod。它在原生“活动扭蛋机”页面新增一台机：

- 默认机台 ID：`990001`；
- 每次固定消耗 **100,000 GP**，不接受其他货币；
- 第一阶段优先从尚未解锁的图鉴机体中抽取可永久持有的基础/初始形态；
- 全部符合条件的机体族解锁后，第二阶段改为抽取已解锁、但当前永久机库和领取箱都没有的机体；
- 当前阶段先按 **C 35% / B 30% / A 20% / S 15%** 抽等级，再在该等级候选机体中等概率抽一台；
- 不消耗玩家现有机体，也不把不可独立持有的变形后形态当作奖品。

某等级在当前阶段没有候选机体时，该等级移出选择，其余等级按原权重归一；例如只剩 B/A/S 时，
权重仍为 30:20:15。同等级的机体数量不会改变该等级的总概率。

活动机台沿用游戏的**奖品领取箱**：抽中后先获得一份永久、1 级机体奖品，再在游戏内领取。
领取箱中的永久奖品会同时解锁对应图鉴，并视为“已经在手”，所以领取前不会重复抽到。机库暂时
放不下时可稍后领取；原生领取失败不会删除已经抽到的奖品。它不会在活动回包里伪装成普通机体回包。

原生 World 流程会先按机台价格预扣 GP。Mod 只负责选择与交付，不会再扣第二笔。
图鉴已满且所有机体都在永久机库或领取箱中、交付元数据不完整、数据库失败或结果校验失败时，
原生失败路径应补回整笔费用，因此玩家的净扣款为 0。

## 运行方式与分发边界

服务端安装 `collection_capsule` World Mod、SQL schema 和机台价格。客户端使用共享
`capsule_catalog` 提供机台注册与资源。服主必须从同一次 `build-kit` 构建中取出
`server-package\` 和 `player\`，避免两边的机台、资源或 framework 版本不一致。

玩家包不包含服务器 INI、数据库连接、账号、图鉴解锁数据或 FRP 凭据。服主只需把
`player\` 目录发给玩家；玩家关闭客户端后双击安装脚本即可。

本功能要求当前最新 framework/core。构建或安装时保留旧 core 而没有换成配套的新版本，
属于更新失败，不是受支持的兼容模式。

## Mod Manager 服主教程

日常配置统一使用 **SDGO Mod Manager**，左侧打开「收藏扭蛋（Beta）」。World 功能卡也能进入
同一面板。不会启动另一个配置程序、服务或游戏启动器。

1. 选择目标 `WorldServer.ini`、配套客户端和包含本功能的完整框架包或服主工具包。路径只保存在
   服主本地工作区。海报可选，为 361×322 PNG；留空使用随包的收藏扭蛋海报。
2. 点击「生成 / 更新配套资源」。Manager 只导出公开目录，读取原生初始形态，并保留客户端已有的
   共享机台。状态显示本次实际可用初始形态数量，不是固定白名单。
3. 关闭要安装的客户端，点击「检查安装条件」。停止选定的 WorldServer 后，分别安装目标 World
   和本机客户端。Manager 不会停止或启动游戏服务。
4. 重启选定 World，点击「导出玩家包」，把包发给朋友。玩家安装后照常启动游戏，在活动扭蛋机页面
   查找收藏机台。Manager 和玩家包说明会给出本次实际页码、格号；抽中后到奖品领取箱领取机体。
5. 安装时会用当前永久机体和领取箱中的永久机体建立图鉴解锁基线，无需导入完整获得历史。导入内容
   的双向形态用「登记导入机体的初始形态」，它可以在首次构建之前运行。

普通扭蛋目录更新会保留活动海报、领取箱类别和生成资源。更换收藏海报或新增机体内容后，再在收藏
面板生成配套包并分发。发布器会拒绝旧四表共享组件，避免更新普通机台时把领取箱所需元数据覆盖掉。
功能启停使用 World 功能卡，保存后重启该 World；最小图鉴解锁集合会保留。价格固定 100,000 GP，
内部机台编号仅在开发视图显示，不能单改编号而不更新配套 SQL 和客户端。

## 开发与自动化命令

以下命令供开发或自动化使用，在包含 `tools\collection_capsule.py` 的源码树或便携式服主工具包根目录执行。
示例全部使用可替换的 `D:\...` 路径。

### 1. 准备最新 framework

从源码构建时，先完成一次完整 Release 构建，让 loader、core、Mod、符号、数据和共享
`capsule_catalog` 都进入同一份 stage：

```powershell
cmake --build build --config Release
```

便携式服主工具包会把 framework 放在工具包根目录的 `framework\`。`build-kit` 默认按以下
顺序选择 framework：

1. `ROOT\framework\`；
2. 如果前者不存在，使用 `ROOT\build\stage\Release\`。

只有需要改用另一份已验证 stage 时才传：

```powershell
--framework-stage D:\SDGO\FrameworkStage
```

这个目录必须包含配套的 `client\`、`server\` 和
`support\capsule-registry\capsule_catalog.dll`。

### 2. 从目标服务器导出公开配置快照

```powershell
python tools/collection_capsule.py export `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --out D:\SDGO\CollectionCapsuleWork\public-snapshot
```

输出目录必须事先不存在。该命令只导出构建奖池所需的公开配置：

- `dbo.tbl_Units` 机体定义；
- `dbo.tbl_MachinePrice` / `dbo.tbl_ItemMachinePrice` 机台定义；
- `dbo.tbl_ItemCategory` 原生奖品类别，以及本 Mod 已分配且需保留的单位礼物类别；
- 已发布的导入机体族元数据；
- 文件行数、hash 与导出时间回执。

它不导出数据库凭据、账号、机库、图鉴解锁集合或其他私有数据。这个快照可以交给另一台
构建机使用；服务器 INI 本身不能随包分发。

### 3. 一次生成服务端包和玩家包

```powershell
python tools/collection_capsule.py build-kit `
  --snapshot D:\SDGO\CollectionCapsuleWork\public-snapshot `
  --client D:\SDGO\Client `
  --out D:\SDGO\CollectionCapsuleWork\collection-capsule-kit
```

输出目录也必须事先不存在。构建器会读取真实客户端目录，核对原生机体目录中的初始形态，
并用同一模型生成服务端奖池和普通客户端机台。

如果服主客户端已经安装共享机台目录，工具会自动检测
`D:\SDGO\Client\sdgo\mods\capsule_catalog\runtime.ini`，读取其所在目录作为当前 registry，
保留其他普通机台、自定义机台和生成资源后再追加或更新本机台。registry 位于其他位置时，
可显式传入：

```powershell
--current-registry D:\SDGO\SharedCapsuleRegistry
```

如本次还要声明新导入的多形态机体族，可重复使用：

```powershell
--unit-family D:\SDGO\UnitPackages\family-a.json `
--unit-family D:\SDGO\UnitPackages\family-b.json
```

独立、无歧义的普通基础机体无需声明。`--unit-family` 提供的是“哪个形态可作为库存初始形态”
的元数据，不是固定奖品白名单。

默认海报是 `mods\collection_capsule\assets\cover.png`。替换图必须是精确的
**361×322 PNG**；建议先制作同等比例的大图再缩小：

海报文案必须同时说明两个阶段。建议短句使用 **“先补图鉴 · 图鉴满后补机库”**，不要继续写
“必得从未获得的机体”，因为第二阶段会补回已经解锁、但当前不在机库或领取箱中的机体。

```powershell
python tools/collection_capsule.py build-kit `
  --snapshot D:\SDGO\CollectionCapsuleWork\public-snapshot `
  --client D:\SDGO\Client `
  --cover D:\SDGO\Artwork\collection-capsule.png `
  --out D:\SDGO\CollectionCapsuleWork\collection-capsule-kit
```

构建结果包含：

- `server-package\`：仅供服主使用的 World Mod、framework、SQL 和模型；
- `player\`：可直接发给玩家的无 Python 客户端包；
- `client-registry\`：本次生成的完整共享机台目录，供审计或后续显式更新；
- `build-receipt.json`：服务端、registry 与玩家包的配套 revision 和文件回执。

只分发 `player\`。不要分发 `server-package\`、服务器 INI 或服务端图鉴解锁数据。

### 4. 检查并安装服务端

先执行只读检查：

```powershell
python tools/collection_capsule.py install-server `
  --package D:\SDGO\CollectionCapsuleWork\collection-capsule-kit\server-package `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --check-only
```

当前 `--check-only` 会校验包的 manifest/hash、配套 server framework、目标 World
可执行文件、既有服主机台 ID、符号合并兼容性和数据库连接，并报告目标数据库；它不会
复制文件或执行 SQL。

检查通过后，只停止该 INI 对应的目标 WorldServer。无需停止其他 Agent、Battle 或另一套
服务器。移除 `--check-only` 执行正式安装：

```powershell
python tools/collection_capsule.py install-server `
  --package D:\SDGO\CollectionCapsuleWork\collection-capsule-kit\server-package `
  --server-ini D:\SDGO\Server\World\WorldServer.ini
```

正式安装会先创建限定到本次 server payload 的恢复备份，再开启外层数据库事务。SQL 安装
及机台 ID、100,000 GP 价格、启用状态和奖池 hash 读回都在该事务中完成；随后标准安装器
更新该 World 的 framework/Mod 文件，全部成功后才提交数据库。命令捕获到失败时会回滚
数据库，并只恢复本次受控的文件；如果恢复时发现其他进程并发修改了受控文件，则保留这些
新修改并报告备份目录，避免覆盖外部工作。

这是带补偿恢复的安装流程，不是能够抵抗进程强杀、断电或磁盘故障的分布式原子事务。
发生这类中断时，应保持目标 WorldServer 停止，检查命令返回的
`sdgo-install-backups\collection-...` 目录、数据库状态和文件 hash，再重新运行检查/安装。

安装命令成功后再启动目标 WorldServer。

## 玩家教程

玩家收到 `player\` 目录后：

1. 关闭准备安装的客户端。
2. 双击 `Install-Client.cmd`。
3. 选择包含 `GOnline.exe` 的游戏目录。
4. 正常启动游戏，在活动扭蛋机列表中使用收藏扭蛋，抽中后到奖品领取箱领取。

安装器会校验客户端版本、包内文件 hash 和所需符号，并写入安装回执。它会保留现有
`sdgo.ini`、无关 Mod、无关符号段和其他共享机台。若客户端已有第三方 `d3d9.dll`，
安装器只在能够建立一层明确支持的加载链时继续。中途失败会按安装日志恢复已写入的文件。

卸载时关闭客户端，双击同一个玩家包中的 `Uninstall-Client.cmd`，再选择相同游戏目录。
卸载器按回执只恢复或删除该次安装控制的 `sdgo\mods\capsule_catalog\` 文件，并保留当前
最新 framework/core、全局设置、符号和无关 Mod。若受控文件后来被其他工具修改，卸载器
会拒绝覆盖这些新内容。

活动入口使用一张完整海报，不再放进普通机台的六格预览。奖池由服务端实时目录、最小图鉴解锁集合、
当前永久机库和领取箱共同决定。

更新从旧版普通入口迁移时，只移除本 Mod 拥有的普通机台记录及价格，保留其他机台和导入形态声明。
旧版功能自有的 `Seen`、`Coverage`、`DrawLog` 与完整历史导入流程会退役；普通扭蛋表和原生扭蛋记录
不在本次迁移范围内。旧玩家包必须更新，不能继续使用已退役的普通入口。

原生单位礼物依赖客户端与服务端一致的道具类别。构建器会复用安全的永久1级类别，并为缺失的机体
生成独立类别；这些是交付元数据，不是奖品白名单。已登记的类别在停用和更新时保留，供领取箱奖品继续
领取。完全卸载玩家资源包前应先领取这些奖品；未领取数据不会删除，重装配套资源后仍可恢复领取。

## 图鉴解锁状态与两阶段抽取

原生客户端的“机体目录/收集率”实际由当前永久机库计算。卖掉或删除机体后，原生目录会再次把它
显示为未拥有；客户端没有提供逐机体的账号级“曾经获得”标记。

因此，本 Mod 只持久保存账号与已解锁机体 ID 的最小集合。它不保存获得时间、来源、抽取事件、
完整性证明或审计历史。安装或从旧 schema 迁移时，基线只来自：

- `TimeLimit=0` 且 `RunOut=0` 的当前永久机体；
- 原生领取箱中尚未领取的永久机体奖品。

旧 `Seen`、`Coverage`、历史导入 payload 和抽取日志不会迁入新基线。抽中时，领取箱奖品与图鉴
解锁会在同一个抽取事务中建立；永久机体和单位礼物写入路径也会维护这个最小集合，所以领取后即使
出售或删除，图鉴解锁仍会保留。数据库内部可使用原生事务保护字段避免未提交写入留下幽灵解锁，
但不会把它扩展成完整获得历史。

每次抽取只使用一个阶段：

1. **先补图鉴：** 从尚未解锁的符合条件基础机体族中选择。
2. **图鉴满后补机库：** 只有全部符合条件的机体族都解锁后，才从已解锁、但当前永久机库和领取箱
   都没有的机体族中选择。

等级权重只在当前阶段的候选机体中重新归一。如果第二阶段没有候选机体，说明所有符合条件的机体都
在机库或领取箱中，本次抽取进入原生退款路径。图鉴解锁集合只保存在服务端，不进入玩家包或公开快照。

## 后续导入的新机体族

收藏扭蛋每次抽取都从当前 live `tbl_Units` 计算符合条件的库存根形态。注册信息只解决多形态
家族的初始形态歧义，不会冻结一份奖品 ID 列表，也不要求为每次新增机体重编 DLL。

新增原生内容还须具有与客户端一致的永久1级单位礼物类别。合法类别发布后，新机体自动参与抽取；
只写机体行而缺少交付类别会明确拒绝并进入退款路径。运行一次新的构建可以自动补齐缺失类别，之后
需把同一批客户端资源分发给玩家。不会为了继续收费而静默跳过这些机体。

现有原生机体的双向形态由客户端 `0220.gdt` 的启用目录入口确定，再与六张机体表及服务端
变形关系逐条核对。当前数据中有歧义的 28 个机体族均已找到唯一初始入口；构建器读取实际
目录，不写死这 28 组或目录条数，也不按名称中的 `After`、ID 大小或历史库存猜测。
无歧义的新机体会自动进入动态奖池；新增双向形态须由内容包声明初始形态。缺失或冲突的声明
会明确报错，阻止抽取并走失败退款路径，不会把该机体族静默排除后继续收费。

已经安装到 live 数据库的多形态内容，可单独发布它的声明：

```powershell
python tools/collection_capsule.py register-family `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --unit-family D:\SDGO\UnitPackages\family-a.json
```

声明采用 `sdgo.unit-family` v1，必须包含：

- `installable: true` 和稳定的 `package_id`；
- 一个明确的 `family.root_form`；
- `family.forms` 中该家族的全部形态；
- 每个形态的整数 `id`、唯一 `key` 和与 live `tbl_Units` 一致的 `transform_target`。

内容导入器应在写入机体的同一个活动事务中调用：

```python
from owner.collection_capsule.deployment import register_unit_family

register_unit_family(db, declaration, in_transaction=True)
```

这样，机体行与根形态元数据会一起提交或一起回滚。独立运行的 `register-family` 命令会管理
自己的事务；它适合内容已经安装完成后的补充注册。下一次 `export` 会把已发布的家族元数据
写入公开快照，后续 `build-kit` 会自动沿用。

## 更新、停用与恢复

更新机体目录、封面、Mod 或 framework 时：

1. 从目标数据库重新 `export` 到新的空快照目录；
2. 用新的空输出目录重新运行 `build-kit`；
3. 停止并更新目标 World；
4. 把同一次构建的新 `player\` 重新发给所有玩家。

如果构建客户端没有安装当前共享 registry，请用上一版的 `client-registry\` 作为
`--current-registry`，这样其他机台不会在新包中丢失。

停用服务端机台并保留图鉴解锁状态：

```powershell
python tools/collection_capsule.py disable `
  --server-ini D:\SDGO\Server\World\WorldServer.ini `
  --machine-id 990001
```

停用后重启目标 WorldServer，并让玩家运行对应包的 `Uninstall-Client.cmd`。停用或移除
客户端资源不会删除最小图鉴解锁集合或原生领取箱中的待领奖品；以后重新安装时可继续使用该状态。

## 验证状态与手动验收

自动验证分成三类：

- Python/offline 测试覆盖形态解析、动态奖池、共享 registry 保留、玩家包、安装和卸载回执；
- 隔离 SQL 测试覆盖图鉴基线、两阶段选择、待领奖品预留、并发不重复、动态新机体族、事务回滚、
  跨排序规则安装和失败补偿模型；
- 原生 fixture 已通过实际 `core.dll`、`collection_capsule.dll` 和 OLE DB 会话验证 ABI、
  独立会话、提交后发布结果以及错误结果回滚。

其中钱包预扣/退款由 Python World-payment fixture 模拟；原生预扣及非零返回退款的判断另有
静态原生路径证据。它们都不是实际 WorldServer + GOnline 的真人游戏抽取。本功能目前仍是
Beta，尚未把真实游戏画面、动画、余额变化和重启持久化标记为已验证。

源码树的基本检查：

```powershell
cmake --build build --config Release
python -m unittest discover -s tests -p "test_collection*.py" -v
ctest --test-dir build -C Release -R "^collection_capsule_offline$" --output-on-failure
```

手动验收至少应确认：

1. 活动列表在构建回执指定的页/格出现收藏海报，显示固定 100,000 GP；
2. 抽到的永久1级初始形态奖品进入领取箱，领取后正确加入机库；
3. 第一阶段会补齐尚未解锁的机体族，待领奖品在领取前不会重复抽到；
4. 成功只净扣一次 100,000 GP；
5. 图鉴全部解锁后，已出售或删除且当前不在机库/领取箱的机体族会进入第二阶段，仍在手或待领的不会进入；
6. 所有机体都在手或待领、元数据缺失和抽取失败时净扣款为 0；领取失败时奖品保留，领取不会再次扣费；
7. 客户端和 WorldServer 重启后机体、图鉴解锁、待领奖品和机台状态一致；
8. 停用收藏后待领奖品仍可领取；全部领取后卸载玩家包，其他 Mod、共享机台和最新 framework/core 仍可正常使用。

## 自动化安装与新增内容

服主在 Mod Manager 点击安装/更新时，工具会自动执行受校验的建表、迁移、图鉴初始化与机台配置。
玩家包不含服务器 SQL，玩家只安装客户端资源并照常进入游戏；不能要求玩家手工运行 SQL。
旧库也可在 Manager 点“更新为图鉴规则”完成数据规则升级，无需停止或重启 World。

候选全集来自实时数据库的合法可持有初始机体，而不是原生图鉴白名单。没有图鉴条目的新机体在内容包
完整安装、配套永久单位奖品类别可交付后，会自动参与当前阶段；无需改奖池代码。训练、不可出击、占位和
不能独立持有的变形后记录被过滤。半安装内容缺少交付定义时会拒绝并退款，不会收钱发不出机体。
