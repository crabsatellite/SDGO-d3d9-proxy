"""Mod providers run behind the Manager's mode, session and revision checks."""
from __future__ import annotations
import base64
import json
from pathlib import Path
import re
import threading
from urllib.parse import parse_qs, urlsplit
from . import storage


class Binary:
    def __init__(self, data, mime='application/octet-stream', filename=None):
        self.data, self.mime, self.filename = data, mime, filename


class Provider:
    fields = {}
    developer_fields = {'demo','module_binary','snapshot','symbol_fragment','client_gdrs','db_export','previous','framework_stage','rules','runtime'}
    def __init__(self, workspace):
        self.workspace = workspace
        self.engine = None
        self.lock = threading.RLock()

    @property
    def config(self):
        return self.workspace.settings.get('modules', {}).get(self.id, {})

    def configure(self, values):
        allowed=self.fields if self.workspace.mode=='developer' else {k:v for k,v in self.fields.items() if k not in self.developer_fields}
        if not isinstance(values, dict) or set(values) - allowed.keys():
            raise ValueError('包含此模块未登记的工作区参数')
        if any(not isinstance(value, str) or '\0' in value or '\n' in value for value in values.values()):
            raise ValueError('工作区参数无效')
        with self.lock:
            if self.engine is not None and hasattr(self.engine, 'close'):
                self.engine.close()
            candidate=dict(self.config)
            for key,value in values.items():
                if value:candidate[key]=value
                else:candidate.pop(key,None)
            self.workspace.settings.setdefault('modules', {})[self.id] = candidate
            self.workspace.save_workspace()
            self.engine = None
        return self.describe()

    def describe(self):
        fields=self.fields if self.workspace.mode=='developer' else {k:v for k,v in self.fields.items() if k not in self.developer_fields}
        return {'id': self.id, 'title': self.title, 'fields': fields, 'config': self.config,
                'configured': bool(self.config), 'audience': 'user'}


class Rewards(Provider):
    id, title = 'rewards', '战斗奖励'
    fields = {'workspace': '奖励配置目录', 'server_ini': '游戏服务器配置文件', 'battle_target':'要安装或更新的对战服务器目录',
              'package':'已解压的框架或奖励 Mod 包目录', 'runtime': '奖励运行目录（留空使用工作区/runtime）'}

    def __init__(self, workspace):
        super().__init__(workspace)
        from .services import WalletService
        controller = WalletService()
        workspace.services.register('rewards.wallet', '奖励钱包服务', controller,
            lambda: controller.start(self.config.get('server_ini'), str(self.get_engine().runtime.root)))
        self.install_plan=None;self.install_review=None

    def configure(self,values):
        if self.workspace.services.describe('rewards.wallet')['running'] or self.workspace.services.desired('rewards.wallet'):raise ValueError('更换配置目录或服务器前，请先停止钱包服务；倍率仍可在面板中直接热更新')
        result=super().configure(values);self.install_plan=None;self.install_review=None
        return result

    def get_engine(self):
        if self.engine is None:
            if not self.config.get('workspace'):
                raise ValueError('请先设置奖励工作区')
            from owner.rewards.workspace import Workspace
            from owner.rewards.runtime_control import RuntimeControl
            self.engine = Workspace(self.config['workspace'], RuntimeControl(self.config['workspace'], self.config.get('runtime')))
        return self.engine

    def request(self, method, path, body, headers):
        if method=='POST' and path in ('/api/rewards/install-plan','/api/rewards/install'):
            from owner.rewards.deployment import build_plan,apply_plan,DeploymentApplyError
            if path.endswith('install-plan'):
                c=self.config
                if not all(c.get(k) for k in ('workspace','server_ini','battle_target','package')):raise ValueError('请选择奖励配置目录、服务器配置文件、对战服务器和 Mod 包目录')
                self.install_plan=build_plan(battle_dir=Path(c['battle_target']),workspace=Path(c['workspace']),package_root=Path(c['package']),server_ini=Path(c['server_ini']))
                self.install_review=__import__('uuid').uuid4().hex
                return {'plan':self.install_plan.public(),'review_id':self.install_review}
            if self.install_plan is None or body.get('review_id')!=self.install_review:raise ValueError('请先检查本次安装计划')
            plan=self.install_plan;self.install_plan=None;self.install_review=None
            try:return {'result':apply_plan(plan),'status':'completed'}
            except DeploymentApplyError as error:raise ValueError(str(error)+'；写入状态：'+str(error.writes_performed)+'；恢复记录：'+str(error.receipt)) from None
        engine = self.get_engine()
        if method == 'GET' and path == '/api/rewards':
            return engine.public()
        expected = headers.get('X-Reward-Revision', '')
        if method == 'POST':
            if path == '/api/rewards/preview': return engine.preview(body)
            if path == '/api/rewards/multiplier': return engine.multiplier(body['base_multiplier'], expected)
            if path == '/api/rewards/policy': return engine.save(body['policy'], expected)
        raise ValueError('奖励操作未登记')


class Operators(Provider):
    id, title = 'operators', '副官与掉落'
    fields = {'workspace': '现有副官工作区', 'client': '配套客户端目录', 'snapshot': '副官快照 JSON',
              'module_binary': '副官客户端 DLL（用于导出）', 'server_ini': '原生服务器 INI（留空只编辑草稿）'}

    def get_engine(self):
        if self.engine is None:
            from .engines.operators.workspace import Workspace
            c = self.config
            if c.get('client') and c.get('server_ini') and not c.get('snapshot'):
                from .engines.operators.snapshot import export
                c.setdefault('workspace',str(self.workspace.root/'data/operators'))
                snapshot=Path(c['workspace'])/'source'
                if not snapshot.exists():export(c['server_ini'],snapshot)
                c['snapshot']=str(snapshot)
                self.workspace.save_workspace()
            if c.get('client') and not c.get('module_binary'):
                dll=Path(c['client'])/'sdgo/mods/operator_catalog/operator_catalog.dll'
                if dll.is_file():c['module_binary']=str(dll)
            if not all(c.get(key) for key in ('workspace', 'client', 'snapshot')):
                raise ValueError('请先设置副官工作区、客户端和快照')
            self.engine = Workspace(c['workspace'], c['client'], c['snapshot'], c.get('module_binary'), c.get('server_ini'))
        return self.engine

    def request(self, method, path, body, headers):
        from .engines.operators import model
        engine = self.get_engine()
        with model.LOCK:
            if method == 'GET':
                if path == '/api/operators': return engine.public()
                if path == '/api/operators/package': return Binary(engine.package(), 'application/zip', 'SDGO-operator-client.zip')
                if path == '/api/operators/registrations':
                    return Binary(storage.json_bytes(model.registrations(engine.read())), 'application/json', 'operator-registrations.json')
                if re.fullmatch('/api/operators/portrait/[0-9]+', path):
                    identity = int(path.rsplit('/', 1)[1])
                    if identity not in engine.baseline['native_operator_ids']: raise ValueError('头像不存在')
                    return Binary(engine.assets.portrait(identity), 'image/png')
                match = re.fullmatch('/api/operators/upload/([0-9a-f]{64})', path)
                if match and match[1] in engine.read()['uploaded_assets']:
                    return Binary((engine.root / 'uploads' / (match[1] + '.png')).read_bytes(), 'image/png')
            expected = headers.get('X-Catalog-Revision', '')
            catalog = engine.check(expected)
            if method == 'POST' and path == '/api/operators/new':
                updated, identity = model.create_operator(catalog, int(body['template_id']))
                return {**engine.commit(updated), 'selected_id': identity}
            if method == 'POST' and path == '/api/operators/avatar':
                return engine.upload(base64.b64decode(body['png'], validate=True), expected)
            if method == 'POST' and path == '/api/operators/apply': return {'applied': engine.apply(expected)}
            if method == 'POST' and path == '/api/operators/update-client':
                from .packages import import_client
                client=Path(self.config['client']).resolve()
                self.workspace.add_host(client)
                host=next(h for h in self.workspace.settings['hosts'] if Path(h['path']).resolve()==client and h['role']=='client')
                return import_client(self.workspace,host['id'],base64.b64encode(engine.package()).decode('ascii'))
            if re.fullmatch('/api/operators/[0-9]+', path) and method in ('PUT', 'DELETE'):
                value = body if method == 'PUT' else {'enabled': False}
                return engine.commit(model.update_operator(catalog, int(path.rsplit('/', 1)[1]), value))
        raise ValueError('副官操作未登记')


class Capsules(Provider):
    id, title = 'capsules', '扭蛋机'
    fields = {'catalog': '现有扭蛋目录 JSON', 'patterns': '概率模板 JSON（可选）', 'assets': '预览图片目录（可选）',
              'bundle_output': '生成目录（可选）', 'snapshot': '初始化来源快照目录（可选）', 'client': '配套客户端目录（编译用）',
              'lookup': '机体图片映射 JSON（编译用）', 'install_client': '客户端安装目标（可选）',
              'server_ini': '原生服务器 INI（可选）', 'framework_stage': '已构建框架目录（安装用）', 'symbol_fragment': '符号片段（可选）'}

    def get_engine(self):
        if self.engine is None:
            from .engines.capsules.capsule_ui import CatalogStore, CapsuleApplication
            c = self.config
            if not c.get('catalog'): raise ValueError('请先选择现有扭蛋目录 JSON')
            callback = None
            if c.get('install_client'):
                from .engines.capsules.capsule_deployment import DeploymentTarget
                callback = DeploymentTarget(client_target=c['install_client'], server_ini=c.get('server_ini'),
                    framework_stage=c.get('framework_stage'), symbol_fragment=c.get('symbol_fragment')).apply
            store = CatalogStore(c['catalog'], patterns_path=c.get('patterns'), assets_path=c.get('assets'),
                bundle_path=c.get('bundle_output'), snapshot_path=c.get('snapshot'), client_path=c.get('client'),
                lookup_path=c.get('lookup'), apply_callback=callback)
            self.engine = CapsuleApplication(store)
        return self.engine

    def request(self, method, uri, body, headers):
        engine = self.get_engine()
        parsed = urlsplit(uri); path = parsed.path; query = parse_qs(parsed.query)
        one = lambda name, default='': query.get(name, [default])[0]
        if method == 'GET':
            if path == '/api/bootstrap': return engine.bootstrap()
            if path == '/api/machine': return engine.machine(one('kind'), int(one('id')))
            if path == '/api/units': return engine.units(one('q'), int(one('limit', '100')))
            if path == '/api/catalog/download': return Binary(storage.json_bytes(engine.store.read()[0]), 'application/json', 'capsule-catalog.json')
            match = re.fullmatch(r'/assets/unit/([0-9]+)\.png', path)
            if match: return Binary(engine.store.asset(int(match[1])).read_bytes(), 'image/png')
        if method == 'POST': return engine.dispatch_post(path, body)
        raise ValueError('扭蛋机操作未登记')


class Frp(Provider):
    id, title = 'frp', 'FRP 配置与分发'
    fields = {'profile': '服主 FRP 端点配置 JSON', 'output': '客户端配置包输出目录'}

    def request(self, method, path, body, headers):
        from tools.make_frp_profile import export_profile, configurations
        file = self.config.get('profile')
        if not file: raise ValueError('请先设置服主 FRP 配置文件位置')
        file = Path(file)
        if method == 'GET':
            raw = file.read_bytes() if file.exists() else b''
            return {'profile': json.loads(raw) if raw else None, 'revision': storage.revision(raw)}
        if path == '/api/profile' and method == 'POST':
            profile = body['profile'];configurations(profile)
            with storage.LOCK:
                raw = file.read_bytes() if file.exists() else b''
                if body.get('revision') != storage.revision(raw): raise storage.Conflict('FRP 配置已改变')
                storage.atomic(file, storage.json_bytes(profile))
            return self.request('GET', '', {}, {})
        if path == '/api/export' and method == 'POST':
            profile = json.loads(file.read_text(encoding='utf-8-sig'))
            output = Path(self.config.get('output') or self.workspace.root / 'exports/frp')
            output = output / __import__('uuid').uuid4().hex[:12]
            result = export_profile(profile, output)
            return Binary(Path(result).read_bytes(), 'application/zip', 'SDGO-client-connection.zip')
        raise ValueError('FRP 操作未登记')


def attach(workspace):
    from .shop import Shop
    from .ai_catalog import AiCatalog
    from .collection_capsule import CollectionCapsule
    for cls in (Rewards, Operators, Capsules, Shop, Frp, AiCatalog, CollectionCapsule):
        provider = cls(workspace)
        workspace.providers[provider.id] = provider
