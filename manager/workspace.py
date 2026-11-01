"""One explicit local workspace, defaulting to the ordinary player view."""
from __future__ import annotations
import copy
from pathlib import Path
import re
import uuid
from . import registry, storage

EXES = {'GOnline.exe': 'client', 'BattleServer.exe': 'battle', 'AgentServer.exe': 'agent', 'WorldServer.exe': 'world'}


class Workspace:
    def __init__(self, root, contracts):
        self.root = Path(root).resolve()
        self.path = self.root / 'manager.json'
        self.contracts = registry.load(Path(__file__).parent/'addons')
        self.contracts.update(registry.load(contracts))
        self.mode = 'standard'
        self.providers = {}
        self.settings = storage.read_json(self.path, {'schema_version': 1, 'hosts': [], 'modules': {}})
        if self.settings.get('schema_version') != 1 or not isinstance(self.settings.get('hosts'), list):
            raise ValueError('Manager 工作区版本无效')

    def require_developer(self):
        if self.mode != 'developer':
            raise PermissionError('此信息仅在开发信息视图提供')

    def save_workspace(self):
        storage.atomic(self.path, storage.json_bytes(self.settings))

    def set_mode(self, mode):
        if mode not in ('standard', 'developer'):
            raise ValueError('界面模式无效')
        self.mode = mode
        return self.public()

    def add_host(self, path):
        path = Path(path).resolve(strict=True)
        role = next((role for exe, role in EXES.items() if (path / exe).is_file()), None)
        if role is None:
            candidates=[child for child in path.iterdir() if child.is_dir() and not child.is_symlink()]
            for child in list(candidates):
                if child.name.casefold()=='server':candidates.extend(c for c in child.iterdir() if c.is_dir() and not c.is_symlink())
            found=[child for child in candidates if child.resolve().is_relative_to(path) and any((child/exe).is_file() for exe in EXES)]
            if not found:raise ValueError('这个目录中没有找到客户端或服务器，请选择单机版目录或具体服务器目录')
            for child in found:self.add_host(child)
            return self.public()
        for host in self.settings['hosts']:
            if Path(host['path']).resolve() == path:
                return self.public()
        self.settings['hosts'].append({'id': uuid.uuid4().hex[:12], 'path': str(path), 'name': path.name, 'role': role})
        self.save_workspace()
        return self.public()

    def host(self, identity):
        for host in self.settings['hosts']:
            if host['id'] == identity:
                return host
        raise ValueError('宿主不存在')

    def _installed(self, host):
        root = Path(host['path']) / 'sdgo/mods'
        result = []
        if not root.is_dir():
            return result
        for folder in sorted(root.iterdir()):
            if not folder.is_dir() or not registry.ID.fullmatch(folder.name):
                continue
            ini = folder / 'mod.ini'
            if not ini.is_file():
                continue
            raw = ini.read_bytes()
            value = storage.ini_values(raw)
            if not value.has_section('mod') or value.get('mod', 'id', fallback=folder.name) != folder.name:
                continue
            contract = self.contracts.get(folder.name)
            # An addon can register its own contract in its installed directory.
            if contract is None and (folder / 'manager.json').is_file():
                contract = registry.load(root).get(folder.name)
            global_path = Path(host['path']) / 'sdgo/sdgo.ini'
            global_raw = global_path.read_bytes() if global_path.exists() else b''
            global_ini = storage.ini_values(global_raw)
            enabled = value.getint('mod', 'enabled', fallback=1) != 0
            category = value.get('mod', 'category', fallback=contract.get('category','other') if contract else 'other')
            if self.mode != 'developer' and category == 'development':
                continue
            feature = value.get('mod', 'feature', fallback=folder.name)
            blocked = []
            for section, key in [('categories', category), ('features', feature), ('mods', folder.name)]:
                if global_ini.get(section, key, fallback='1').strip() == '0':
                    blocked.append(section + '.' + key)
            title = contract['title'] if contract else value.get('mod', 'name', fallback=folder.name)
            result.append({'id': folder.name, 'title': title, 'category': category, 'feature': feature,
                           'description': contract.get('description', '') if contract else '此 Mod 未登记详细配置。',
                           'enabled': enabled and not blocked, 'manifest_enabled': enabled,
                           'blocked_by': blocked, 'registered': contract is not None,
                           'version': value.get('mod', 'version', fallback=''),
                           'revision': storage.revision(raw),
                           'archived': (contract.get('presentation')=='automatic_fix' if contract else category=='fixes'),
                           'configured': 'installed', 'runtime_status': 'unverified',
                           'settings_count': len([f for f in contract.get('settings',[]) if f['audience']=='user' or self.mode=='developer']) if contract else 0})
        return result

    def public(self):
        hosts = []
        for host in self.settings['hosts']:
            path = Path(host['path']) / 'sdgo/sdgo.ini'
            mods=self._installed(host)
            raw=path.read_bytes() if path.exists() else b'';global_ini=storage.ini_values(raw)
            hosts.append({**host, 'mods': [m for m in mods if not m['archived']], 'archive':[m for m in mods if m['archived']], 'global_revision': storage.revision(raw),
                          'categories':{key:global_ini.get('categories',key,fallback='1')!='0' for key in ('gameplay','network')}})
        result = {'mode': self.mode, 'hosts': hosts, 'schema_version': 1,
                  'panels':[{'id':key,'title':provider.title} for key,provider in self.providers.items()]}
        return result

    def details(self, host_id, mod_id):
        host = self.host(host_id)
        contract = self.contracts.get(mod_id)
        mod_dir = storage.safe_child(host['path'], 'sdgo/mods/'+mod_id)
        if not (mod_dir / 'mod.ini').is_file():
            raise ValueError('此宿主未安装该 Mod')
        if contract is None:
            contract = registry.load(mod_dir.parent).get(mod_id)
        if contract is None:
            raise ValueError('此 Mod 尚未登记详细配置')
        contract=copy.deepcopy(contract)
        contract['settings']=[field for field in contract.get('settings',[]) if field['audience']=='user' or self.mode=='developer']
        files = {}
        values = {}
        for field in contract.get('settings', []):
            name = field['file']
            if name not in files:
                target = mod_dir / name
                raw = target.read_bytes() if target.exists() else b''
                files[name] = {'raw': raw, 'revision': storage.revision(raw), 'values': storage.ini_values(raw)}
            value = files[name]['values'].get('settings', field['key'], fallback=None)
            if value is None and name == 'config.ini':
                value = storage.ini_values((mod_dir / 'mod.ini').read_bytes()).get('settings', field['key'], fallback=None)
            if value is None:
                value = field.get('default', '')
            if field['type'] == 'boolean':
                value = str(value).lower() in ('1', 'true')
            elif field['type'] == 'integer':
                value = int(value) if str(value).strip() else 0
            elif field['type'] == 'number':
                value = float(value)
            values[field['id']] = value
        return {'contract': contract, 'values': values, 'revisions': {name: entry['revision'] for name, entry in files.items()}}

    def configure(self, host_id, mod_id, values, revisions):
        with storage.LOCK:
            current = self.details(host_id, mod_id)
            fields = {field['id']: field for field in current['contract'].get('settings', [])}
            if not isinstance(values, dict) or set(values) - fields.keys():
                raise ValueError('请求包含未登记的配置项')
            updates = {}
            for key, value in values.items():
                field = fields[key]
                updates.setdefault(field['file'], []).append(('settings', field['key'], registry.validate(field, value)))
            host = self.host(host_id)
            root = storage.safe_child(host['path'], 'sdgo/mods/'+mod_id)
            candidates = {}
            for name, changes in updates.items():
                path = root / name
                raw = path.read_bytes() if path.exists() else b''
                if revisions.get(name) != storage.revision(raw):
                    raise storage.Conflict('配置已被其他工具改变，请刷新后重试')
                candidates[path] = (raw, storage.patch_ini(raw, changes))
            written = []
            try:
                for path, (old, new) in candidates.items():
                    storage.atomic(path, new)
                    written.append(path)
            except OSError:
                for path in reversed(written):
                    storage.atomic(path, candidates[path][0])
                raise
            return {**self.details(host_id, mod_id), 'saved': True, 'activation': 'restart_required'}

    def toggle(self, host_id, mod_id, enabled, expected):
        if type(enabled) is not bool or not registry.ID.fullmatch(mod_id):
            raise ValueError('Mod 开关无效')
        host = self.host(host_id)
        if mod_id not in {item['id'] for item in self._installed(host)}:
            raise ValueError('Mod 未安装')
        path = storage.safe_child(host['path'],'sdgo/sdgo.ini')
        with storage.LOCK:
            raw = path.read_bytes() if path.exists() else b''
            manifest = storage.safe_child(host['path'],'sdgo/mods/'+mod_id+'/mod.ini')
            mod_raw = manifest.read_bytes()
            if not isinstance(expected, dict) or expected.get('global') != storage.revision(raw) or expected.get('manifest') != storage.revision(mod_raw):
                raise storage.Conflict('全局配置已改变，请刷新后重试')
            storage.atomic(manifest, storage.patch_ini(mod_raw, [('mod', 'enabled', '1' if enabled else '0')]))
            try:
                storage.atomic(path, storage.patch_ini(raw, [('mods', mod_id, '1' if enabled else '0')]))
            except OSError:
                storage.atomic(manifest, mod_raw)
                raise
        return {**self.public(), 'saved': True, 'activation': 'restart_required'}

    def category(self,host_id,key,enabled,expected):
        if key not in ('gameplay','network') or type(enabled) is not bool:raise ValueError('功能分类开关无效')
        host=self.host(host_id);path=storage.safe_child(host['path'],'sdgo/sdgo.ini')
        with storage.LOCK:
            raw=path.read_bytes() if path.exists() else b''
            if expected!=storage.revision(raw):raise storage.Conflict('配置已经改变，请刷新后重试')
            storage.atomic(path,storage.patch_ini(raw,[('categories',key,'1' if enabled else '0')]))
        return {**self.public(),'saved':True,'activation':'restart_required'}
