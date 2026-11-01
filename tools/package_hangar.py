"""Package only the hangar Mod, optional base framework, installer and docs."""
import argparse
import configparser
import hashlib
import io
from pathlib import Path
import zipfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,default=Path('build/hangar'))
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--manager-overlay',action='store_true',help='Mod-only import for the unified Manager; no launcher, core or installer scripts')
    a=p.parse_args();repo=Path(__file__).resolve().parents[1]
    stage=a.build_dir.resolve()/'stage/Release/client'
    files={name:stage/name for name in ['d3d9.dll','sdgo/core.dll','sdgo/symbols/GOnline-54BE1568-00C97000.ini',
        'sdgo/mods/hangar_manager/hangar_manager.dll','sdgo/mods/hangar_manager/mod.ini','sdgo/mods/hangar_manager/manager.json','sdgo/mods/hangar_manager/units.tsv']}
    packaged={('client/'+name):path for name,path in files.items()}
    for name in ['install_hangar.ps1','install_client.ps1','install_common.ps1','export_hangar_rules.py']:
        packaged[name]=repo/'tools'/name
    packaged['README.zh-CN.md']=repo/'docs/hangar-manager.zh-CN.md'
    packaged['licenses/MinHook-LICENSE.txt']=repo/'third_party/minhook/LICENSE.txt'
    for name in ['LICENSE','THIRD_PARTY_NOTICES.md']:
        if (repo/name).exists():packaged[name]=repo/name
    for path in packaged.values():
        if not path.is_file():p.error(f'Missing build artifact {path}')
    staged_symbols=configparser.ConfigParser(interpolation=None);staged_symbols.read(files['sdgo/symbols/GOnline-54BE1568-00C97000.ini'],encoding='utf-8-sig')
    manifest=configparser.ConfigParser(interpolation=None);manifest.read(files['sdgo/mods/hangar_manager/mod.ini'],encoding='utf-8-sig')
    required=manifest.get('mod','requires').split(',')
    if any(not staged_symbols.has_section(name) for name in required):p.error('Staged native symbols are stale; build target sdgo_data before packaging')
    fragment=None
    if a.manager_overlay:
        packaged={name:path for name,path in packaged.items() if name.startswith('client/sdgo/mods/hangar_manager/') or name=='README.zh-CN.md'}
        symbols=staged_symbols
        selected=configparser.ConfigParser(interpolation=None)
        for section in symbols.sections():
            if section=='host' or section.startswith('hangar.') or section in ('game.app','net.send_packet'):selected[section]=dict(symbols[section])
        buffer=io.StringIO();selected.write(buffer,space_around_delimiters=False);fragment=buffer.getvalue()
    if a.output.exists():p.error('Choose a new archive filename; existing files are preserved')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(a.output,'x',compression=zipfile.ZIP_DEFLATED) as z:
        for name,path in sorted(packaged.items()):z.write(path,name)
        if fragment is not None:z.writestr('symbols.fragment.ini',fragment)
    with zipfile.ZipFile(a.output) as z:
        if z.testzip():raise RuntimeError('Package CRC verification failed')
    print(str(a.output.resolve()))
    print('SHA256 '+hashlib.sha256(a.output.read_bytes()).hexdigest())


if __name__=='__main__':main()
