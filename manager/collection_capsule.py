"""Collection gameplay uses the shared Manager session, paths and revisions."""
from __future__ import annotations

import contextlib
import io
import json
from pathlib import Path
import subprocess
import sys
import uuid
import zipfile

from . import storage
from .providers import Binary, Provider


ROOT = Path(__file__).resolve().parents[1]


class CollectionCapsule(Provider):
    id, title = 'collection_capsule', '收藏扭蛋（Beta）'
    fields = {
        'server_ini': '目标 WorldServer 配置文件',
        'client': '配套客户端目录（本机安装目标）',
        'source': '配套完整框架包或服主工具包目录',
        'cover': '活动海报 PNG（361×322，留空使用随包海报）',
        'snapshot': '公开目录快照（开发时替代数据库导出）',
    }

    @property
    def state_file(self):
        return self.workspace.root / 'data/collection_capsule/last-build.json'

    def revision(self):
        return storage.revision(storage.json_bytes(self.config) +
                                (self.state_file.read_bytes() if self.state_file.exists() else b''))

    def check_revision(self, body):
        if body.get('revision') != self.revision():
            raise storage.Conflict('收藏扭蛋配置或构建已改变，请刷新后重试')

    def stage(self):
        source = Path(self.config['source']) if self.config.get('source') else (
            Path(sys.executable).parent if getattr(sys, 'frozen', False) else ROOT)
        for candidate in (source / 'collection/framework', source / 'framework',
                          source / 'build/stage/Release', source):
            if ((candidate / 'client/sdgo/core.dll').is_file() and
                    (candidate / 'server/sdgo/mods/collection_capsule/collection_capsule.dll').is_file() and
                    (candidate / 'support/capsule-registry/capsule_catalog.dll').is_file()):
                return candidate
        raise ValueError('请选择包含收藏扭蛋和共享机台支持的配套完整框架包或服主工具包')

    def generated(self):
        state = storage.read_json(self.state_file, {})
        if state.get('machine_kind') != 'activity':
            raise ValueError('请先生成新版活动收藏扭蛋资源')
        if state.get('config_revision') != storage.revision(storage.json_bytes(self.config)):
            raise ValueError('工作区配置已改变，请重新生成配套资源')
        identity = state.get('build_id', '')
        if len(identity) != 32 or any(c not in '0123456789abcdef' for c in identity):
            raise ValueError('请先生成收藏扭蛋配套资源')
        return self.state_file.parent / 'builds' / identity, state

    def public(self):
        state = storage.read_json(self.state_file, {})
        current = state.get('config_revision') == storage.revision(storage.json_bytes(self.config))
        return {'revision': self.revision(), 'generated': bool(current and state.get('machine_kind') == 'activity'),
                'machine_id': state.get('machine_id', 990001), 'price_gp': 100000, 'machine_kind': 'activity',
                'rank_weights': {'C': 35, 'B': 30, 'A': 20, 'S': 15},
                'eligible_roots': state.get('eligible_roots'),
                'catalog_revision': state.get('catalog_revision'),
                'placement': state.get('placement'),
                'validation': 'Beta；实际游戏动画、扣费和重启持久化由服主手动验收'}

    def generate(self):
        from tools.collection_capsule import build
        from owner.collection_capsule.client import build_client_registry
        from owner.collection_capsule.deployment import connection, export_configuration
        from tools.package_collection_capsule_client import package_client_registry
        if not self.config.get('client') or not self.config.get('server_ini'):
            raise ValueError('请选择配套客户端和目标 WorldServer 配置文件')
        stage = self.stage()
        identity = uuid.uuid4().hex
        output = self.state_file.parent / 'builds' / identity
        output.mkdir(parents=True, exist_ok=False)
        snapshot = Path(self.config['snapshot']) if self.config.get('snapshot') else output / 'source'
        if not self.config.get('snapshot'):
            with contextlib.closing(connection(Path(self.config['server_ini']))) as db:
                export_configuration(db, snapshot)
        world = Path(self.config['server_ini']).resolve().parent
        settings = world / 'sdgo/mods/collection_capsule/config.ini'
        if not settings.is_file():
            settings = world / 'sdgo/mods/collection_capsule/mod.ini'
        machine_id = storage.ini_values(settings.read_bytes() if settings.is_file() else b'').getint(
            'settings', 'MachineID', fallback=990001)
        client = Path(self.config['client'])
        server = build(snapshot, output / 'server-package', machine_id, [], stage / 'server', client_root=client)
        model = storage.read_json(output / 'server-package/model.json')
        current = client / 'sdgo/mods/capsule_catalog'
        cover = Path(self.config['cover']) if self.config.get('cover') else ROOT / 'mods/collection_capsule/assets/cover.png'
        registry = build_client_registry(client_root=client,
            current_registry=current if (current / 'runtime.ini').is_file() else None,
            source_units=model['pool']['units'], unit_items=model['unit_items'], machine_id=machine_id, cover=cover,
            output=output / 'client-registry')
        package_client_registry(registry=output / 'client-registry', framework_stage=stage / 'client',
            support_dll=stage / 'support/capsule-registry/capsule_catalog.dll', output=output / 'player')
        storage.atomic(self.state_file, storage.json_bytes({
            'build_id': identity, 'config_revision': storage.revision(storage.json_bytes(self.config)),
            'machine_id': machine_id, 'machine_kind': 'activity', 'eligible_roots': server['resolved_roots'],
            'catalog_revision': registry['catalog_revision'], 'placement': registry.get('placement')}))
        return self.public()

    def client_install(self, output, check_only=False):
        args = ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                str(output / 'player/Install-Client.ps1'), '-GameDir', self.config['client']]
        if check_only:
            args.append('-CheckOnly')
        result = subprocess.run(args, capture_output=True, text=True)
        if result.returncode:
            raise ValueError('客户端检查或安装未完成；请确认该客户端已关闭，版本、符号和安装回执一致')
        return {'client_checked': True, 'client_applied': not check_only,
                'activation': 'restart_required'}

    def player(self, output):
        from tools.package_collection_capsule_client import _sha256
        directory = output / 'player'
        manifest = storage.read_json(directory / 'bundle-manifest.json')
        files = {p.relative_to(directory).as_posix(): p for p in directory.rglob('*') if p.is_file()}
        if set(files) != set(manifest['files']) | {'bundle-manifest.json'}:
            raise ValueError('玩家包文件清单已改变，请重新生成')
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w', compression=zipfile.ZIP_DEFLATED) as archive:
            for name, path in sorted(files.items()):
                raw = path.read_bytes()
                if name != 'bundle-manifest.json' and _sha256(raw) != manifest['files'][name]:
                    raise ValueError('玩家包内容已改变，请重新生成')
                archive.writestr(name, raw)
        return Binary(stream.getvalue(), 'application/zip', 'SDGO-CollectionCapsule-Beta-Player.zip')

    def request(self, method, path, body, headers):
        if method == 'GET' and path == '/api/state':
            return self.public()
        if method != 'POST':
            raise ValueError('收藏扭蛋操作未登记')
        self.check_revision(body)
        if path == '/api/generate':
            return self.generate()
        if path == '/api/import-history':
            from owner.collection_capsule.deployment import connection, import_history
            with contextlib.closing(connection(Path(self.config['server_ini']))) as db:
                return {**import_history(db, body['history']), 'revision': self.revision()}
        if path == '/api/register-family':
            from owner.collection_capsule.deployment import connection, register_unit_family
            with contextlib.closing(connection(Path(self.config['server_ini']))) as db:
                return {**register_unit_family(db, body['family']), 'revision': self.revision()}
        output, state = self.generated()
        from tools.collection_capsule import install_server
        if path == '/api/check-install':
            server = install_server(output / 'server-package', Path(self.config['server_ini']), check_only=True)
            return {**server, **self.client_install(output, check_only=True), 'revision': self.revision()}
        if path == '/api/install-server':
            try:
                result = install_server(output / 'server-package', Path(self.config['server_ini']))
            except RuntimeError as error:
                raise ValueError(str(error)) from None
            return {**result, 'revision': self.revision()}
        if path == '/api/install-client':
            return {**self.client_install(output), 'revision': self.revision()}
        if path == '/api/export':
            return self.player(output)
        raise ValueError('收藏扭蛋操作未登记')
