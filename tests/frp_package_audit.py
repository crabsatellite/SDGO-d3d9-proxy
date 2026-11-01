"""Install/uninstall the packaged kit on a minimal COPY of a supplied client.

No game is executed. The reference is read only; tests need neither game assets
nor credentials. Outputs are kept in a new caller-selected directory.
"""
from __future__ import annotations
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from package_frp import file_version


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def audit(kit: Path, reference: Path, profile: Path, work: Path) -> dict:
    work.mkdir(parents=True, exist_ok=False)
    target=work/'existing client copy';target.mkdir()
    for rel in ('GOnline.exe','d3d9.dll','sdgo/core.dll','sdgo/sdgo.ini',
                'sdgo/symbols/GOnline-54BE1568-00C97000.ini'):
        destination=target/rel;destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copy2(reference/rel,destination)
    shutil.copytree(reference/'sdgo/mods',target/'sdgo/mods')
    old_core=digest(target/'sdgo/core.dll')
    protected={p.relative_to(target).as_posix():digest(p) for p in target.rglob('*')
               if p.is_file() and 'symbols' not in p.parts and p.relative_to(target).as_posix() not in ('sdgo/core.dll','d3d9.dll')}
    symbol=target/'sdgo/symbols/GOnline-54BE1568-00C97000.ini';before=symbol.read_bytes()
    def install(root: Path, uninstall: bool = False) -> None:
        args=['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(kit/'install_frp.ps1'),
              '-Target',str(root),'-Role','Client']
        args += ['-Uninstall'] if uninstall else ['-Profile',str(profile)]
        result=subprocess.run(args,capture_output=True,text=True,errors='replace')
        with (work/'install-output.txt').open('a',encoding='utf-8') as f:
            f.write(result.stdout+result.stderr+'\n')
        if result.returncode:
            raise RuntimeError(result.stdout+result.stderr)
    install(target)
    for name in ('sdgo/core.dll','d3d9.dll'):
        assert digest(target/name)==digest(kit/'framework/client'/name)
    assert all(digest(target/rel)==value for rel,value in protected.items())
    assert symbol.read_bytes().startswith(before)
    symbols=configparser.ConfigParser(interpolation=None,strict=False)
    symbols.read(symbol,encoding='utf-8-sig')
    mods={}
    for path in (target/'sdgo/mods').glob('*/mod.ini'):
        manifest=configparser.ConfigParser(interpolation=None,strict=False)
        manifest.read(path,encoding='utf-8-sig')
        required=[s.strip() for s in manifest.get('mod','requires',fallback='').split(',') if s.strip()]
        missing=[s for s in required if not symbols.has_section(s)]
        assert not missing,(path,missing)
        mods[path.parent.name]={'requirements':len(required),'missing':missing}
    # This reference client supplies the prior framework build. An older kit
    # must fail before modifying the updated target, rather than silently downgrade.
    older=work/'older kit';shutil.copytree(kit,older)
    shutil.copy2(reference/'sdgo/core.dll',older/'framework/client/sdgo/core.dll')
    snapshot={p.relative_to(target).as_posix():digest(p) for p in target.rglob('*') if p.is_file()}
    command=['powershell','-NoProfile','-ExecutionPolicy','Bypass','-File',str(older/'install_frp.ps1'),
             '-Target',str(target),'-Role','Client','-Profile',str(profile)]
    attempt=subprocess.run(command,capture_output=True,text=True,errors='replace')
    assert attempt.returncode and 'Kit checksum mismatch' in attempt.stderr, attempt.stderr
    assert snapshot=={p.relative_to(target).as_posix():digest(p) for p in target.rglob('*') if p.is_file()}
    inventory=json.loads((older/'SHA256.json').read_text())
    inventory['framework/client/sdgo/core.dll']=digest(older/'framework/client/sdgo/core.dll')
    (older/'SHA256.json').write_text(json.dumps(inventory),encoding='utf-8')
    attempt=subprocess.run(command,capture_output=True,text=True,errors='replace')
    assert attempt.returncode and 'Stale framework file' in attempt.stderr, attempt.stderr
    assert snapshot=={p.relative_to(target).as_posix():digest(p) for p in target.rglob('*') if p.is_file()}
    (older/'framework/VERSION').write_text('.'.join(str(n) for n in file_version(reference/'sdgo/core.dll')[:3])+'\n',encoding='ascii')
    inventory['framework/VERSION']=digest(older/'framework/VERSION')
    (older/'SHA256.json').write_text(json.dumps(inventory),encoding='utf-8')
    attempt=subprocess.run(command,capture_output=True,text=True,errors='replace')
    assert attempt.returncode and 'older than the installed framework' in attempt.stderr, attempt.stderr
    assert snapshot=={p.relative_to(target).as_posix():digest(p) for p in target.rglob('*') if p.is_file()}
    install(target,True)
    assert all(digest(target/rel)==value for rel,value in protected.items())
    assert not (target/'sdgo/mods/net_route').exists()
    for name in ('sdgo/core.dll','d3d9.dll'):
        assert digest(target/name)==digest(kit/'framework/client'/name)
    fresh=work/'fresh second layout';fresh.mkdir()
    shutil.copy2(reference/'GOnline.exe',fresh/'GOnline.exe')
    install(fresh)
    assert digest(fresh/'sdgo/core.dll')==digest(kit/'framework/client/sdgo/core.dll')
    assert {p.name for p in (fresh/'sdgo/mods').iterdir() if p.is_dir()}=={'net_route'}
    archive=kit.with_name(kit.name+'.zip')
    with zipfile.ZipFile(archive) as z:
        assert z.testzip() is None
        names=z.namelist()
        assert not any(n.endswith('.exe') or 'frp_room' in n or 'friend_launch' in n or
                       'sakura_room_bridge' in n for n in names)
        for name in names:
            if name.endswith(('.py','.ps1','.json','.ini','.md')):
                data=z.read(name)
                for private in (b'127.88.0.1',b'frp-fly.com',b'SDGO-client-frp',b'SDGO_TCP',b'SDGO_UDP'):
                    assert private not in data,(name,private)
    result=dict(archive=str(archive),archive_bytes=archive.stat().st_size,archive_sha256=digest(archive),
                archive_entries=len(names),protected_original_files=len(protected),
                previous_core_sha256=old_core,installed_core_sha256=digest(target/'sdgo/core.dll'),
                core_and_loader_updated_to_bundle=True,stale_kit_refused_without_writes=True,older_kit_refused_without_writes=True,mods_requirements_after_install=mods,
                incremental_install_uninstall_preserves_existing_files=True,fresh_install=True,
                no_game_execution=True,no_original_modification=True,receipt=str(work))
    (work/'result.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for argument in ('kit','reference-client','profile','out'):
        parser.add_argument('--'+argument,type=Path,required=True)
    args=parser.parse_args()
    print(json.dumps(audit(args.kit.resolve(),args.reference_client.resolve(),args.profile.resolve(),args.out.resolve()),indent=2))
