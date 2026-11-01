"""Package only the hangar Mod, optional base framework, installer and docs."""
import argparse
import hashlib
from pathlib import Path
import zipfile


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,default=Path('build/hangar'))
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();repo=Path(__file__).resolve().parents[1]
    stage=a.build_dir.resolve()/'stage/Release/client'
    files={name:stage/name for name in ['d3d9.dll','sdgo/core.dll','sdgo/symbols/GOnline-54BE1568-00C97000.ini',
        'sdgo/mods/hangar_manager/hangar_manager.dll','sdgo/mods/hangar_manager/mod.ini','sdgo/mods/hangar_manager/units.tsv']}
    packaged={('client/'+name):path for name,path in files.items()}
    for name in ['install_hangar.ps1','install_client.ps1','install_common.ps1','export_hangar_rules.py']:
        packaged[name]=repo/'tools'/name
    packaged['README.zh-CN.md']=repo/'docs/hangar-manager.zh-CN.md'
    packaged['licenses/MinHook-LICENSE.txt']=repo/'third_party/minhook/LICENSE.txt'
    for name in ['LICENSE','THIRD_PARTY_NOTICES.md']:
        if (repo/name).exists():packaged[name]=repo/name
    for path in packaged.values():
        if not path.is_file():p.error(f'Missing build artifact {path}')
    if a.output.exists():p.error('Choose a new archive filename; existing files are preserved')
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(a.output,'x',compression=zipfile.ZIP_DEFLATED) as z:
        for name,path in sorted(packaged.items()):z.write(path,name)
    with zipfile.ZipFile(a.output) as z:
        if z.testzip():raise RuntimeError('Package CRC verification failed')
    print(str(a.output.resolve()))
    print('SHA256 '+hashlib.sha256(a.output.read_bytes()).hexdigest())


if __name__=='__main__':main()
