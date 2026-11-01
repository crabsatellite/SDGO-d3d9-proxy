"""Launch the real self-contained EXE outside the checkout, without real game data."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.request


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--exe',type=Path,required=True);args=parser.parse_args()
    repo=Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='sdgo-manager-exe-') as temporary:
        root=Path(temporary);state=root/'state';state.mkdir();client=root/'client';client.mkdir();(client/'GOnline.exe').write_bytes(b'fixture')
        for name in ('findwindow_fix','mission_ai'):
            folder=client/'sdgo/mods'/name;folder.mkdir(parents=True);shutil.copyfile(repo/'mods'/name/'mod.ini',folder/'mod.ini')
        (state/'manager.json').write_text(json.dumps({'schema_version':1,'hosts':[{'id':'client','name':'test client','path':str(client),'role':'client'}],
             'modules':{'shop':{'workspace':str(root/'shop'),'demo':'1'},'rewards':{'workspace':str(root/'rewards')}}}),encoding='utf-8')
        environment=dict(os.environ);environment.pop('PYTHONPATH',None)
        process=subprocess.Popen([str(args.exe.resolve()),'--workspace',str(state),'--no-browser'],cwd=root,env=environment)
        base=token=None
        try:
            for _ in range(200):
                if process.poll() is not None:raise RuntimeError('Frozen Manager exited before publishing its session')
                session=state/'manager-session.json'
                if session.exists():
                    base=json.loads(session.read_text(encoding='utf-8'))['url'].rstrip('/')
                    break
                time.sleep(.1)
            if not base:raise RuntimeError('Frozen Manager did not start')
            def call(path,body=None,headers=None):
                values={'Content-Type':'application/json','Origin':base,'X-ModManager-Token':token or ''};values.update(headers or {})
                request=urllib.request.Request(base+path,None if body is None else json.dumps(body).encode(),values)
                with urllib.request.urlopen(request,timeout=30) as response:return json.load(response)
            info=call('/api/state');token=info['session_token']
            assert info['mode']=='standard'
            assert [mod['id'] for mod in info['hosts'][0]['mods']]==['mission_ai']
            assert [mod['id'] for mod in info['hosts'][0]['archive']]==['findwindow_fix']
            assert {p['id'] for p in info['panels']} >= {'shop','rewards','collection_capsule'}
            collection=call('/api/providers/collection_capsule/api/state')
            assert collection['price_gp']==100000 and not collection['generated']
            context=call('/api/ai-context',{'provider':'shop','kind':'bug'})
            assert 'SDGO-Mod-Framework' in context['text'] and 'AI models:' in context['text']
            catalog=call('/api/providers/shop/api/shop/catalog');assert catalog['demo'] and catalog['items']
            created=call('/api/providers/shop/api/shop/create',{'category_id':catalog['items'][0]['Id']},{'X-Shop-Revision':catalog['revision']})
            assert len(created['catalog']['products'])==1
            rewards=call('/api/providers/rewards/api/rewards')
            assert not rewards['progression']['config']['enabled']
            rewards=call('/api/providers/rewards/api/rewards/solo-preset',{}, {'X-Reward-Revision':rewards['revision']})
            assert rewards['policy']['progression']['daily_mb']==27000 and rewards['policy']['progression']['enabled']
            assert (root/'rewards/runtime/policies'/(rewards['revision']+'.json')).is_file()
            call('/api/exit',{});process.wait(timeout=30)
            assert process.returncode==0
            print('PASS frozen EXE: standalone launch, daily mode, fix archive, collection provider, AI/PR guidance, C# shop CRUD and solo MB preset')
        finally:
            if process.poll() is None:
                if base and token:
                    try:call('/api/exit',{})
                    except Exception:pass
                try:process.wait(timeout=10)
                except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=10)


if __name__=='__main__':main()
