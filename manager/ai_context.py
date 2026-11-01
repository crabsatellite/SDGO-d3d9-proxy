"""Explicit, credential-free handoffs to an AI, including actionable bug reports."""
from __future__ import annotations
import json
from pathlib import Path
import re
import sys
from . import storage

SENSITIVE = re.compile(r'password|passwd|pwd|secret|token|credential|connection.?string|api.?key|cookie',re.I)


def repository(): return storage.read_json(Path(__file__).with_name('repository.json'))


def redact(value):
    if isinstance(value,dict): return {key:('[已隐藏]' if SENSITIVE.search(key) else redact(item)) for key,item in value.items()}
    if isinstance(value,list): return [redact(item) for item in value]
    if isinstance(value,str) and re.search(r'(?i)\b(?:pwd|password|token|secret|api_key)\s*=',value):
        return '[已隐藏：疑似凭据文本]'
    return value


def build(workspace, base_url, *, host_id=None, mod_id=None, provider_id=None, kind='configure', problem=''):
    base_url=base_url.rstrip('/')
    if kind not in ('configure','bug'): raise ValueError('AI 说明类型无效')
    repo=repository();paths=[];addresses=[];settings={};version='';title='';mechanism=''
    if provider_id:
        provider=workspace.providers.get(provider_id)
        if provider is None: raise ValueError('功能未登记')
        title=provider.title
        stories={
            'shop':'商城业务库从服主所选服务器读取原生物品和模板。新增、修改、删除先保存在草稿，应用时由已安装的商城 Mod 工具更新其管理的商品，删除会保留身份编号。',
            'operators':'副官业务模块读取配套客户端资源和现有副官目录，维护外观、发放方式、掉落规则和客户端更新包。服务器应用与客户端更新是分别核对的步骤。',
            'capsules':'扭蛋业务模块维护固定槽位、自定义候选池、概率模板和价格。保存目录后可编译配套客户端数据、导出更新包并应用到所选服务器。沿用原有版本和概率校验。',
            'rewards':'奖励策略由 Manager 发布到原生 BattleServer 读取的运行目录；GB 沿用游戏原结算，MB 由受管工作器处理持久队列。界面以真实心跳和回执判断连接，保存不等于运行中已加载。',
            'frp':'服主填写实际 Sakura FRP 端点。生成器只导出客户端需要的地址映射；Agent 复用已登记的 UDP 池，玩家使用原启动器，不需要 Sakura 账号或专用联机入口。',
            'ai_catalog':'根据服主的原生目录表和配套客户端表生成一致的 AI 机体清单，在客户端和 BattleServer 装配同一份数据，保留旧编号分配记录。它不自动等于 AI 实战验收通过。'}
        stories['collection_capsule']='收藏扭蛋固定 100,000 GP，动态解析 live 机体族，交付从未获得的初始形态。形态入口来自原生目录或内容包声明，历史持久保留；旧账号缺少可信完整历史时拒绝抽取。World 原生预扣/失败补偿，Mod 只事务交付。生成、安装与真人游戏验收是不同状态。'
        mechanism=stories.get(provider_id,'此功能由登记的业务模块提供配置和管理操作。')
        settings=redact(provider.config)
        capabilities={
            'shop':{'operations':['GET /api/shop/catalog','POST /api/shop/create','POST /api/shop/update','POST /api/shop/delete','POST /api/shop/restore','POST /api/shop/apply','GET /api/shop/export'],
                    'editable':['名称','选项标签','GB/MB 售价','数量或租赁天数','上架状态'],'revision_header':'X-Shop-Revision','identity':'商品编号、SKU 和模板由系统分配，更新不能重用或改写旧身份'},
            'operators':{'operations':['GET /api/operators','POST /api/operators/new','PUT /api/operators/{id}','DELETE /api/operators/{id}','POST /api/operators/avatar','POST /api/operators/apply','GET /api/operators/package'],
                         'editable':['显示名称','原生或上传头像','全员免费/注册商品','掉落物品、概率、数量范围'],'revision_header':'X-Catalog-Revision','scope':'副官模块登记物品；上架和售价由商城模块管理'},
            'capsules':{'operations':['GET /api/bootstrap','GET /api/machine','GET /api/units','POST /api/machines/create','POST /api/machines/update','POST /api/machines/retire','POST /api/machines/restore','POST /api/compile','POST /api/apply','POST /api/export'],
                        'editable':['固定槽位和概率模板','自定义机体候选池','价格、币种和展示位置'],'revision_field':'expected_revision','scope':'按当前目录的机体等级和候选池规则校验，不猜测概率、不绕过限制'},
            'rewards':{'operations':['GET /api/rewards','POST /api/rewards/multiplier','POST /api/rewards/policy','POST /api/rewards/preview','GET/POST /api/rewards/worker'],
                       'editable':['基础倍率','完整奖励规则','模拟结算条件','Manager 所有的钱包服务'],'revision_header':'X-Reward-Revision','scope':'不得通过浏览器构造发币作业；只处理原生结算持久队列'},
            'frp':{'operations':['GET/POST /api/profile','POST /api/export'],'editable':['逻辑登录端点','Sakura TCP/UDP 端点','连续 UDP 端口池'],'revision_field':'revision','scope':'只导出客户端连接配置，不导出 Sakura 凭据'},
            'ai_catalog':{'operations':['POST /api/check','POST /api/generate','POST /api/install_client','POST /api/install_server','POST /api/export'],'editable':['配套客户端/服务器来源','机体分级规则','上一版编号分配记录'],'scope':'更新必须保留编号分配记录；生成与安装后仍需人工游戏验收'}}
        capabilities['collection_capsule']={'operations':['POST /api/generate','POST /api/check-install','POST /api/install-server','POST /api/install-client','POST /api/export','POST /api/import-history','POST /api/register-family'],'editable':['目标 WorldServer INI','配套客户端','完整框架/服主工具包','182x108 封面'],'scope':'所有写操作需当前 revision；只操作选定目标。不得伪造完整历史、重置 Seen 或猜测双向形态。游戏程序由用户关闭，Manager 不管理其进程。'}
        settings={**settings,'管理操作契约':capabilities.get(provider_id,{})}
        paths=[str(value) for key,value in settings.items() if isinstance(value,str) and key not in ('demo',) and value]
        addresses=[base_url+'/api/providers/'+provider_id+'/config']
        if provider_id=='frp' and provider.config.get('profile'):
            file=Path(provider.config['profile'])
            if file.is_file():
                from tools.make_frp_profile import configurations
                _,_,endpoints=configurations(storage.read_json(file))
                settings={**settings,'已登记端点':endpoints}
    elif host_id and mod_id:
        host=workspace.host(host_id)
        installed=next((mod for mod in workspace._installed(host) if mod['id']==mod_id),None)
        if installed is None: raise ValueError('当前宿主没有这个功能')
        contract=workspace.contracts.get(mod_id,{})
        title=installed['title'];version=installed['version']
        mechanism=contract.get('mechanism',contract.get('description','通过已安装的 Mod 及框架提供此功能。'))
        root=storage.safe_child(host['path'],'sdgo/mods/'+mod_id)
        paths=[str(root),str(root/'mod.ini')]
        for name in ('config.ini',mod_id+'.dll'):
            if (root/name).is_file():paths.append(str(root/name))
        settings={'当前配置启用':installed['enabled'],'实际游戏运行状态':'尚未由本次说明确认','可调设置':[]}
        for field in contract.get('settings',[]):
            if SENSITIVE.search(field['key']):continue
            item={key:field[key] for key in ('key','label','type','default','minimum','maximum','activation') if key in field}
            file=root/field['file']
            ini=storage.ini_values(file.read_bytes()) if file.is_file() else storage.ini_values(b'')
            value=ini.get('settings',field['key'],fallback=None)
            if value is None and field['file']=='config.ini':
                value=storage.ini_values((root/'mod.ini').read_bytes()).get('settings',field['key'],fallback=None)
            item['current']=redact(value if value is not None else field.get('default'))
            settings['可调设置'].append(item)
        addresses=[base_url+'/api/settings?host='+host_id+'&mod='+mod_id]
    else: raise ValueError('请选择一个具体功能再复制 AI 说明')
    executable=str(Path(sys.executable).resolve()) if getattr(sys,'frozen',False) else str(Path(__file__).resolve().parents[1]/'tools/mod_manager.py')
    lines=[f'# SDGO Mod Framework · {title}',f'项目仓库：{repo["url"]}',f'Issue 地址：{repo["issues"]}',
           f'Manager 版本：{repo["manager_version"]}',f'Mod 版本：{version or "见该功能安装回执"}',
           '工作原理：'+mechanism]
    if kind=='bug':
        lines += ['','请帮助我排查真实问题，并在上面的 GitHub 仓库创建一个 bug issue。',
                  '我描述的问题：'+(re.sub(r'(?i)\b(password|pwd|token|secret|api_key)\s*[:=]\s*\S+',r'\1=[已隐藏]',problem.strip()) or '尚未填写；请先问我复现步骤、预期行为和实际结果，不要编造报告。'),
                  '先检查已有 issue，避免重复。只有问题信息充分时再提交；标题写清功能和触发条件。',
                  'Issue 请包含：复现步骤、预期/实际结果、版本、影响范围、最小脱敏日志、是否完成真实游戏验收。',
                  '不要上传本机绝对路径、IP/端口、数据库 INI、密码、令牌、账号、游戏资源、玩家存档或完整私有快照。',
                  '如果有本机访问能力，可先只读调查；变更配置前说明计划，不得重置玩家数据或停止无关游戏服务。',
                  f'可用 GitHub CLI 指向明确仓库：gh issue list --repo {repo["slug"]} --state all；然后 gh issue create --repo {repo["slug"]}。',
                  '如果没有 GitHub 写入能力，请给我可直接粘贴的 issue 标题和正文，并明确尚未提交。']
    else:
        lines += ['','请帮助我配置或调试这个功能。以下路径和地址只用于我授权的本机排查，不要发送到公开 issue。',
                  'Manager 入口：'+executable,'Manager 本机地址：'+base_url,
                  '组件/工作区位置：',*['- '+path for path in paths],
                  '相关本机接口地址：',*['- '+address for address in addresses],
                  '已登记的非敏感配置与可调点：',json.dumps(redact(settings),ensure_ascii=False,indent=2),
                  '接口写入有当前会话的 Origin/令牌和版本检查；不要将会话令牌复制给外部服务。',
                  '先说明你是否能访问同一台电脑。如不能，要求我提供所需的脱敏配置或有限日志，不要假装已经读取或修改。',
                  '先只读核对目标目录、配置版本与安装回执；使用 Manager 登记的配置/操作接口，不运行旧的独立 UI。',
                  '区分已保存、已安装、需要重启、实际运行确认和人工对局验收。不要把保存配置当作功能已生效。',
                  '不要猜测或覆盖未登记的配置键，不修改真实玩家数据，不影响无关客户端或服务器。']
    lines += ['', '## 如果我有开发环境并愿意尝试修复',
              '结合我已经说明的情况判断是否愿意修复、是否有源码和开发环境；信息缺失时再问，已经明确的不要重复确认。没有环境时给出适合我的安装/构建指导；不愿意时以 issue 报告为止。',
              '开发说明：'+repo['url']+'/blob/master/docs/contributing.zh-CN.md',
              '读取相关源码和功能文档，在独立分支或 fork 中复现与修复，保留当前游戏与真实数据；运行与改动相关的测试并报告未验证部分。',
              '愿意提交修复时，引导创建指向本仓库的 PR，并使用仓库 PR 模板。',
              'PR 必须有 AI models: 一行，注明实际参与修复的模型名称和版本；有多个模型分别列出。',
              '如果工具未披露精确型号或版本，明确写 Unknown 和原因，不要根据“DeepSeek”“ChatGPT”等产品称呼猜模型。未使用 AI 时写 None。',
              '同时填写 AI scope、Human verification、真实测试命令/结果和 Remaining uncertainty，便于维护者结合证据判断。',
              '模型信息写在 PR 正文；提交作者仍是用户，不添加模型或代理的 Co-Authored-By。']
    return {'title':title,'text':'\n'.join(lines),'repository':repo,'kind':kind}
