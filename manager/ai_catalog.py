"""Registered AI catalog generation/installation/export without another entry."""
from pathlib import Path
import uuid
from .providers import Provider, Binary


class AiCatalog(Provider):
    id, title = 'ai_catalog', 'AI 机体清单'
    fields = {'db_export':'目录表 JSON 来源目录（二选一）', 'server_ini':'原生服务器 INI（二选一）',
              'client':'配套客户端目录','client_gdrs':'已解包的配套 gdrs 目录','rules':'服主机体分级规则 JSON（可选）',
              'previous':'上一版 catalog.json（更新时使用）','output':'生成目录',
              'source':'配套 Mod 包或框架 stage 目录','client_target':'客户端安装目录','server_target':'BattleServer 安装目录'}

    def request(self, method, path, body, headers):
        from backend import ai_catalog as api
        from backend import install_ai_catalog as installer
        from backend.export_ai_catalog import export
        c = self.config
        if method == 'GET': return {'config':c, 'actions':['check','generate','install_client','install_server','export']}
        action = path.rsplit('/',1)[-1]
        if action in ('check','generate'):
            if c.get('db_export'): tables = api.read_exports(Path(c['db_export']))
            elif c.get('server_ini'):
                from .database import connection_string
                tables = api.read_database(connection_string=connection_string(c['server_ini']))
            else: raise ValueError('请先选择目录表导出或原生 INI 来源')
            client_directory=c.get('client_gdrs')
            if not client_directory and c.get('client'):
                from .engines.operators.assets import Assets
                assets=Assets(c['client']);directory=self.workspace.root/'data/ai_catalog/client-tables'
                directory.mkdir(parents=True,exist_ok=True)
                for name in (*api.UNIT_FILES,*api.NPC_FILES):
                    try:raw=assets.read('gdrs/'+name+'.gdt')
                    except (ValueError,KeyError,FileNotFoundError):continue
                    (directory/(name+'.gdt')).write_bytes(raw)
                client_directory=str(directory)
            client = api.read_client(Path(client_directory)) if client_directory else None
            rules = api.rules_from_json(Path(c['rules'])) if c.get('rules') else api.rules_from_json(None)
            report = api.prepare(tables, client, rules)
            output = Path(c.get('output') or self.workspace.root/'exports/ai_catalog')/uuid.uuid4().hex[:12]
            if action == 'generate':
                previous=Path(c['previous']) if c.get('previous') else Path(c['generated'])/'catalog.json' if c.get('generated') else None
                binary, manifest = api.generate_catalog(tables, client, report, rules, api.previous_ids(previous))
                report.update(mode='generated_process_local_catalog',ready_to_install=True,reason='配套数据已生成，仍需安装和人工游戏验收。',remaining_requirements=['matching_mod_installations','manual_gameplay_acceptance'])
                api.write_bundle(report, output, binary, manifest)
                self.workspace.settings['modules'][self.id]['generated'] = str(output)
                self.workspace.save_workspace()
            else: api.write_plan(report, output)
            return {'output':str(output), 'report':{k:v for k,v in report.items() if k not in ('candidates','skipped')}}
        generated = c.get('generated')
        if not generated: raise ValueError('请先通过 Manager 生成配套清单')
        catalog = Path(generated)/'client/sdgo/mods/ai_catalog/catalog.bin'
        if action == 'export':
            output = self.workspace.root/'exports'/('ai-catalog-'+uuid.uuid4().hex[:12]+'.zip')
            output.parent.mkdir(parents=True, exist_ok=True)
            export(Path(c['source']), catalog, output)
            return Binary(output.read_bytes(),'application/zip','SDGO-AI-catalog-client.zip')
        if action in ('install_client','install_server'):
            role = action.split('_')[1];target=c.get(role+'_target')
            if not target: raise ValueError('请先选择安装目标')
            return {'result':installer.install(installer.Overlay(Path(target)),role,Path(c['source']),catalog)}
        raise ValueError('AI 清单操作未登记')
