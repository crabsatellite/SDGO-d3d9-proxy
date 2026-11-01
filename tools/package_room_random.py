"""Build a small, host-only, incremental preview; never package game/private files."""
import argparse
import configparser
import io
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


def main():
    repo=Path(__file__).resolve().parents[1]
    config=configparser.ConfigParser(interpolation=None)
    config.read(repo/'mods/room_random/mod.ini',encoding='utf-8')
    server_version=config['mod']['version']
    client_config=configparser.ConfigParser(interpolation=None)
    client_config.read(repo/'mods/room_random_client/mod.ini',encoding='utf-8')
    version=max((server_version,client_config['mod']['version']),key=lambda v:tuple(map(int,v.split('.'))))+'-beta'
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binaries',type=Path,default=repo/'build/Release')
    parser.add_argument('--out',type=Path,default=repo/f'dist/SDGO-RoomRandom-{version}.zip')
    parser.add_argument('--client-out',type=Path,default=repo/f'dist/SDGO-RoomRandomClient-{version}.zip')
    args=parser.parse_args()
    files={
        'server/sdgo/core.dll':args.binaries/'core.dll',
        'server/sdgo/mods/pvp_ai/pvp_ai.dll':args.binaries/'pvp_ai.dll',
        'server/sdgo/mods/coop_ai/coop_ai.dll':args.binaries/'coop_ai.dll',
        'server/sdgo/mods/room_random/room_random.dll':args.binaries/'room_random.dll',
        'server/sdgo/mods/room_random/mod.ini':repo/'mods/room_random/mod.ini',
        'server/sdgo/mods/room_random/manager.json':repo/'mods/room_random/manager.json',
        'server/sdgo/mods/room_random/config.example.ini':repo/'mods/room_random/config.example.ini',
        'server/sdgo/symbols/BattleServer-548A891D-0046C000.ini':repo/'symbols/BattleServer-548A891D-0046C000.ini',
        'tools/install_room_random.ps1':repo/'tools/install_room_random.ps1',
        'docs/room-random.zh-CN.md':repo/'docs/room-random.zh-CN.md',
        'docs/room-random.md':repo/'docs/room-random.md',
        'docs/room-commands.zh-CN.md':repo/'docs/room-commands.zh-CN.md',
        'LICENSE':repo/'LICENSE',
        'licenses/MinHook-LICENSE.txt':repo/'third_party/minhook/LICENSE.txt',
    }
    payload={name:path.read_bytes() for name,path in files.items()}
    payload['README.txt']=(
        f'SDGO Room Random {version}\n\n'
        'SERVER component. Existing Mod Framework required on both sides.\n'
        f'All players must import SDGO-RoomRandomClient-{version}.zip through Mod Manager and restart the client.\n'
        'Add !rand or !random to a room title; every human gets owned loadouts each round.\n'
        'Single-unit modes keep the unit through respawns; native deck modes total exactly 10 points.\n'
        'The client companion applies full-unit/ship notices immediately, before queued start commands.\n'
        'It also supplies the matching complete record to native human construction when the room retains an old snapshot.\n'
        'Shared commands: !rand !ai3, or !ai e2 a1 lv4 !rand (19 bytes).\n'
        'Updates core plus already installed pvp_ai/coop_ai DLLs; preserves their settings.\n'
        'Read docs/room-random.zh-CN.md before installing into a STOPPED BattleServer.\n'
        'tools/install_room_random.ps1 -ServerRoot <one server folder> -DatabaseIni <host WorldServer.ini>\n'
        'No game executable, account data, database password, or private configuration is included.\n'
        'One mission controlled-model retest passed; broader mode/multiplayer/reward acceptance remains pending. No public release.\n'
    ).encode('utf-8')
    revision=subprocess.run(['git','rev-parse','HEAD'],cwd=repo,capture_output=True,text=True,check=True).stdout.strip()
    manifest={'schema_version':1,'version':version,'server_component_version':server_version,'client_component_version':client_config['mod']['version'],'source_commit':revision,
        'status':'beta preview; one mission controlled-model retest passed; broader acceptance pending',
        'supported_host':'BattleServer-548A891D-0046C000',
        'supported_host_sha256':'f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4',
        'files':{name:hashlib.sha256(raw).hexdigest() for name,raw in payload.items()}}
    payload['manifest.json']=(json.dumps(manifest,indent=2)+'\n').encode()
    args.out.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.out,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name,raw in sorted(payload.items()):archive.writestr(name,raw)
    with zipfile.ZipFile(args.out) as archive:
        assert archive.testzip() is None
        assert set(archive.namelist())==set(payload)
        for name,h in manifest['files'].items():assert hashlib.sha256(archive.read(name)).hexdigest()==h
        assert not any(name.endswith('/config.ini') or name.endswith('.exe') or 'logs/' in name for name in archive.namelist())
    symbols=configparser.ConfigParser(interpolation=None)
    symbols.read(repo/'symbols/GOnline-54BE1568-00C97000.ini',encoding='utf-8')
    fragment=configparser.ConfigParser(interpolation=None)
    for name in ['host',*client_config['mod']['requires'].split(',')]:fragment[name]=dict(symbols[name])
    text=io.StringIO();fragment.write(text,space_around_delimiters=False)
    client_payload={f'client/sdgo/mods/room_random_client/{name}':
        ((args.binaries/name) if name.endswith('.dll') else (repo/'mods/room_random_client'/name)).read_bytes()
        for name in ['room_random_client.dll','mod.ini','manager.json']}
    client_payload['symbols.fragment.ini']=text.getvalue().encode('ascii')
    client_payload['README.txt']=(
        'Random loadout CLIENT synchronization '+version+'\n'
        'Close GOnline, open Mod Manager, select the client and import this player package.\n'
        'Start with the normal launcher. No additional settings or server credentials.\n'
        'All participants need this companion; the server must also have room_random installed.\n'
    ).encode('utf-8')
    args.client_out.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(args.client_out,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for name,data in sorted(client_payload.items()):archive.writestr(name,data)
    print(json.dumps({'zip':str(args.out.resolve()),'files':len(payload),'bytes':args.out.stat().st_size,
        'sha256':hashlib.sha256(args.out.read_bytes()).hexdigest(),
        'client_overlay':str(args.client_out.resolve()),'client_sha256':hashlib.sha256(args.client_out.read_bytes()).hexdigest()},ensure_ascii=False))


if __name__=='__main__':main()
