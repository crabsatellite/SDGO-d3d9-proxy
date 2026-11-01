"""Manager contracts, real HTTP mode gates and lossless on-disk edits."""
import configparser
import base64
import io
import http.client
import json
from pathlib import Path
import tempfile
import threading
import unittest
import zipfile
from manager.workspace import Workspace
from manager.server import make_server
from manager.providers import attach
from manager import registry, storage

ROOT = Path(__file__).resolve().parents[1]


class ManagerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.workspace = Workspace(self.root/'state', ROOT/'mods')
        self.client = self.root/'client'
        self.client.mkdir();(self.client/'GOnline.exe').write_bytes(b'fixture')
        self.mod = self.client/'sdgo/mods/findwindow_fix'
        self.mod.mkdir(parents=True)
        (self.mod/'mod.ini').write_bytes(b'; preserve this\r\n[mod]\r\nid=findwindow_fix\r\nenabled=1\r\ncategory=fixes\r\n[settings]\r\nTrace=0\r\nUnknown=keep\r\n')
        self.workspace.add_host(self.client)
        attach(self.workspace)
        self.server = make_server(self.workspace)
        self.thread = threading.Thread(target=self.server.serve_forever,daemon=True);self.thread.start()
        self.port = self.server.server_port
        self.token = self.call('GET','/api/state')[1]['session_token']

    def tearDown(self):
        self.workspace.services.close()
        for provider in self.workspace.providers.values():
            if provider.engine is not None and hasattr(provider.engine,'close'):provider.engine.close()
        self.server.shutdown();self.server.server_close();self.thread.join();self.temp.cleanup()

    def call(self,method,path,body=None,headers=None):
        connection=http.client.HTTPConnection('127.0.0.1',self.port,timeout=5)
        h={'Origin':'http://127.0.0.1:'+str(self.port),'Content-Type':'application/json','X-ModManager-Token':getattr(self,'token','')}
        h.update(headers or {})
        connection.request(method,path,None if body is None else json.dumps(body),h)
        response=connection.getresponse();raw=response.read();status=response.status;connection.close()
        return status,json.loads(raw) if raw and raw.startswith(b'{') else raw

    def advanced(self):
        self.assertEqual(self.call('POST','/api/mode',{'mode':'developer'})[0],200)

    def test_every_source_mod_is_registered(self):
        records=registry.load(ROOT/'mods')
        self.assertGreaterEqual(len(records),19)
        for folder in (ROOT/'mods').iterdir():
            if (folder/'mod.ini').exists() or (folder/'client.ini').exists():self.assertIn(folder.name,records)

    def test_all_published_setting_keys_have_contracts(self):
        for identity,contract in self.workspace.contracts.items():
            root=ROOT/'mods'/identity
            declared={f['key'].lower() for f in contract['settings']}
            for name in ('mod.ini','config.example.ini'):
                if (root/name).exists():
                    parser=storage.ini_values((root/name).read_bytes())
                    if parser.has_section('settings'):self.assertLessEqual(set(parser['settings']),declared,identity)

    def test_default_is_daily_management_and_automatic_fixes_are_archived(self):
        state=self.call('GET','/api/state')[1]
        self.assertEqual(state['mode'],'standard');self.assertNotIn('modules',state);self.assertTrue(state['panels'])
        self.assertEqual(state['hosts'][0]['archive'][0]['settings_count'],0)
        self.assertEqual(state['hosts'][0]['mods'],[])

    def test_standard_can_use_daily_management_and_not_developer_info(self):
        host=self.workspace.settings['hosts'][0]['id']
        for identity in self.workspace.providers:
            self.assertEqual(self.call('GET','/api/providers/'+identity+'/config')[0],200)
            self.assertEqual(self.call('GET','/panels/'+identity+'.js')[0],200)
        url='/api/settings?host='+host+'&mod=findwindow_fix'
        self.assertEqual(self.call('GET',url)[1]['contract']['settings'],[])
        self.assertEqual(self.call('POST','/api/settings',{'host':host,'mod':'findwindow_fix','values':{'trace':True},'revisions':{}})[0],400)
        self.assertEqual(self.call('GET','/api/developer?host='+host+'&mod=findwindow_fix')[0],403)

    def test_ui_library_is_served_and_art_needs_a_client_texture(self):
        for path in ('/ui.css','/ui.js','/ui-kit.html','/style.css','/app.js'):
            self.assertEqual(self.call('GET',path)[0],200,path)
        self.assertIn(b"from '/ui.js'",self.call('GET','/app.js')[1])
        # The fixture client has no GPackData, and unknown keys are never read.
        self.assertEqual(self.call('GET','/api/art/hangar')[0],404)
        self.assertEqual(self.call('GET','/api/art/..%2Fmods')[0],404)

    def test_mode_downgrade_hides_developer_info_but_keeps_owner_tools(self):
        self.advanced();host=self.workspace.settings['hosts'][0]['id']
        url='/api/developer?host='+host+'&mod=findwindow_fix'
        self.assertEqual(self.call('GET',url)[0],200)
        self.call('POST','/api/mode',{'mode':'standard'})
        self.assertEqual(self.call('GET',url)[0],403)
        self.assertEqual(self.call('GET','/api/providers/rewards/config')[0],200)

    def test_advanced_mode_is_not_loaded_from_workspace_file(self):
        self.workspace.settings['mode']='developer';self.workspace.save_workspace()
        self.assertEqual(Workspace(self.workspace.root,ROOT/'mods').mode,'standard')

    def test_standard_users_can_manage_servers(self):
        world=self.root/'world';world.mkdir();(world/'WorldServer.exe').write_bytes(b'fixture')
        self.assertEqual(self.call('POST','/api/hosts',{'path':str(world)})[0],200)
        self.advanced();self.workspace.add_host(world)
        self.call('POST','/api/mode',{'mode':'standard'})
        self.assertEqual(len(self.workspace.public()['hosts']),2)

    def test_ai_config_context_has_locations_and_no_database_credentials(self):
        provider=self.workspace.providers['rewards']
        private=self.root/'private.ini';private.write_text('[DB Game]\nPWD=never-copy-this\n',encoding='utf-8')
        provider.configure({'workspace':str(self.root/'rewards'),'server_ini':str(private)})
        status,result=self.call('POST','/api/ai-context',{'provider':'rewards'})
        self.assertEqual(status,200);self.assertIn(str(private),result['text'])
        self.assertIn('http://127.0.0.1:',result['text']);self.assertNotIn('never-copy-this',result['text'])
        self.assertIn('SDGO-Mod-Framework',result['text'])

    def test_bug_handoff_targets_canonical_repository_without_private_paths(self):
        provider=self.workspace.providers['shop'];provider.configure({'workspace':str(self.root/'private-shop')})
        status,result=self.call('POST','/api/ai-context',{'provider':'shop','kind':'bug','problem':'fails; password=never-copy-this'})
        self.assertEqual(status,200);self.assertIn('gh issue create --repo crabsatellite/SDGO-Mod-Framework',result['text'])
        self.assertNotIn(str(self.root),result['text']);self.assertNotIn('never-copy-this',result['text'])
        self.assertIn('先检查已有 issue',result['text'])

    def test_user_can_copy_ai_support_for_archived_fixes(self):
        host=self.workspace.settings['hosts'][0]['id']
        status,result=self.call('POST','/api/ai-context',{'host':host,'mod':'findwindow_fix'})
        self.assertEqual(status,200);self.assertIn(str(self.mod.resolve()),result['text'])

    def test_services_have_one_manager_api_and_legacy_worker_api_is_gone(self):
        status,data=self.call('GET','/api/services')
        self.assertEqual(status,200)
        self.assertEqual([service['id'] for service in data['services']],['rewards.wallet'])
        self.assertEqual(data['services'][0]['owner'],'manager')
        self.assertEqual(self.call('POST','/api/services/rewards.wallet',{'action':'start'},{'Origin':'https://foreign.invalid'})[0],403)
        status,_=self.call('POST','/api/providers/rewards/api/rewards/worker',{'action':'start'})
        self.assertNotEqual(status,200)

    def test_csrf_and_dns_rebinding_are_rejected(self):
        for headers in ({'Origin':'http://evil.invalid'},{'X-ModManager-Token':'bad'},{'Host':'evil.invalid:'+str(self.port)}):
            self.assertEqual(self.call('POST','/api/mode',{'mode':'developer'},headers)[0],403)
        self.assertEqual(self.workspace.mode,'standard')

    def test_no_legacy_web_entry_is_exposed(self):
        self.advanced()
        for url in ('/operators','/rewards','/api/operators','/api/bootstrap','/app.exe','/panels/../workspace.json'):
            self.assertNotEqual(self.call('GET',url)[0],200)

    def test_config_preserves_unknown_keys_comments_and_newlines(self):
        self.advanced();host=self.workspace.settings['hosts'][0]['id'];details=self.workspace.details(host,'findwindow_fix')
        self.workspace.configure(host,'findwindow_fix',{'trace':True},details['revisions'])
        raw=(self.mod/'mod.ini').read_bytes()
        self.assertIn(b'; preserve this\r\n',raw);self.assertIn(b'Trace=1\r\n',raw);self.assertIn(b'Unknown=keep\r\n',raw)

    def test_config_rejects_concurrent_edits(self):
        self.advanced();host=self.workspace.settings['hosts'][0]['id'];details=self.workspace.details(host,'findwindow_fix')
        with (self.mod/'mod.ini').open('ab') as file:file.write(b'; external edit\r\n')
        before=(self.mod/'mod.ini').read_bytes()
        with self.assertRaises(storage.Conflict):self.workspace.configure(host,'findwindow_fix',{'trace':True},details['revisions'])
        self.assertEqual(before,(self.mod/'mod.ini').read_bytes())

    def test_unknown_keys_and_injected_paths_are_not_writable(self):
        self.advanced();host=self.workspace.settings['hosts'][0]['id'];details=self.workspace.details(host,'findwindow_fix')
        with self.assertRaises(ValueError):self.workspace.configure(host,'findwindow_fix',{'dll':'evil.dll'},details['revisions'])
        with self.assertRaises(ValueError):self.workspace.details(host,'../../elsewhere')

    def test_manifest_and_global_switch_are_updated_together(self):
        host=self.workspace.public()['hosts'][0];mod=host['archive'][0]
        self.workspace.toggle(host['id'],mod['id'],False,{'global':host['global_revision'],'manifest':mod['revision']})
        self.assertFalse(self.workspace.public()['hosts'][0]['archive'][0]['enabled'])
        host=self.workspace.public()['hosts'][0];mod=host['archive'][0]
        self.workspace.toggle(host['id'],mod['id'],True,{'global':host['global_revision'],'manifest':mod['revision']})
        self.assertTrue(self.workspace.public()['hosts'][0]['archive'][0]['enabled'])

    def test_utf16_config_and_duplicate_keys(self):
        raw=b'\xff\xfe'+'; 中文\r\n[settings]\r\nTrace=0\r\n'.encode('utf-16-le')
        value=storage.patch_ini(raw,[('settings','Trace','1')])
        self.assertTrue(value.startswith(b'\xff\xfe'));self.assertIn('中文',value[2:].decode('utf-16-le'))
        with self.assertRaises(ValueError):storage.patch_ini(b'[settings]\nTrace=0\nTrace=1\n',[('settings','Trace','0')])

    def test_hangar_overlay_imports_rules_registration_and_native_symbols(self):
        host=self.workspace.settings['hosts'][0]['id'];buffer=io.BytesIO()
        with zipfile.ZipFile(buffer,'w') as archive:
            for name in ('mod.ini','manager.json','units.tsv'):
                archive.writestr('client/sdgo/mods/hangar_manager/'+name,(ROOT/'mods/hangar_manager'/name).read_bytes())
            archive.writestr('client/sdgo/mods/hangar_manager/hangar_manager.dll',b'synthetic DLL')
            archive.writestr('symbols.fragment.ini','[host]\ntimestamp=0x54BE1568\nsize_of_image=0x00C97000\n[hangar.init]\nkind=code\nrva=0x202EA0\nexpect=55 8B EC\n')
        status,result=self.call('POST','/api/import',{'host':host,'package':base64.b64encode(buffer.getvalue()).decode()})
        self.assertEqual(status,200);self.assertEqual(result['installed_files'],5)
        root=self.client/'sdgo/mods/hangar_manager'
        self.assertEqual((root/'units.tsv').read_bytes(),(ROOT/'mods/hangar_manager/units.tsv').read_bytes())
        self.assertTrue((root/'manager.json').is_file());self.assertTrue((self.mod/'mod.ini').is_file())
        self.assertIn('[hangar.init]',(self.client/'sdgo/symbols/GOnline-54BE1568-00C97000.ini').read_text())

    def test_hangar_overlay_rejects_scripts_before_writing_any_files(self):
        buffer=io.BytesIO()
        with zipfile.ZipFile(buffer,'w') as archive:
            archive.writestr('client/sdgo/mods/hangar_manager/units.tsv','synthetic rules')
            archive.writestr('client/sdgo/mods/hangar_manager/run.ps1','throw "never execute"')
        status,_=self.call('POST','/api/import',{'host':self.workspace.settings['hosts'][0]['id'],'package':base64.b64encode(buffer.getvalue()).decode()})
        self.assertNotEqual(status,200);self.assertFalse((self.client/'sdgo/mods/hangar_manager').exists())


if __name__=='__main__':unittest.main()
