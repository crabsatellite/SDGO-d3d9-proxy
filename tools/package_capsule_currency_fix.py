"""Build the independent, script-free Manager overlay for the currency fix."""
from __future__ import annotations
import argparse
import configparser
import hashlib
import json
from pathlib import Path
import struct
import zipfile

ROOT=Path(__file__).resolve().parents[1]


def build(binary:Path,output:Path):
    native=binary.read_bytes()
    if len(native)<256 or native[:2]!=b'MZ':raise ValueError('Build the native DLL first')
    pe=struct.unpack_from('<I',native,0x3c)[0]
    if native[pe:pe+4]!=b'PE\0\0' or struct.unpack_from('<H',native,pe+4)[0]!=0x14c:raise ValueError('Native x86 DLL required')
    folder=ROOT/'mods/capsule_currency_fix';prefix='client/sdgo/mods/capsule_currency_fix/'
    payload={prefix+'capsule_currency_fix.dll':native}
    for name in ('mod.ini','manager.json'):payload[prefix+name]=(folder/name).read_bytes()
    payload['symbols.fragment.ini']=(folder/'symbol-proposal.ini').read_bytes()
    payload['README.zh-CN.md']=(ROOT/'docs/capsule-currency-fix.zh-CN.md').read_bytes()
    payload['README.md']=(ROOT/'docs/capsule-currency-fix.md').read_bytes()+b'\n\n'+(ROOT/'LICENSE').read_bytes()
    ini=configparser.ConfigParser(interpolation=None);ini.read(folder/'mod.ini',encoding='utf-8-sig')
    value={'schema_version':1,'id':'capsule_currency_fix','version':ini['mod']['version'],'activation':'restart_required',
           'files':{name:hashlib.sha256(data).hexdigest()for name,data in payload.items()}}
    payload['manifest.json']=json.dumps(value,indent=2).encode('utf-8')
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(output,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=9)as archive:
        for name,data in sorted(payload.items()):archive.writestr(name,data)
    with zipfile.ZipFile(output)as archive:
        if archive.testzip() is not None:raise ValueError('Invalid ZIP CRC')
    return {'path':str(output.resolve()),'sha256':hashlib.sha256(output.read_bytes()).hexdigest(),'files':len(payload)}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,default=ROOT/'build/Release/capsule_currency_fix.dll');parser.add_argument('--output',type=Path,required=True);args=parser.parse_args();print(json.dumps(build(args.binary,args.output)))
