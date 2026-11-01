"""Install missing default support officers without replacing managed user settings."""
import argparse
import json
from pathlib import Path
from manager.engines.operators import presets
from manager.engines.operators.workspace import Workspace


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('workspace','client','snapshot'):p.add_argument('--'+name,type=Path,required=True)
    args=p.parse_args()
    workspace=Workspace(args.workspace,args.client,args.snapshot)
    catalog=presets.initialize(workspace.read(),args.snapshot,workspace.assets)
    workspace.commit(catalog)
    print(json.dumps({'default_pack_version':presets.VERSION,'default_operators':[
        {'id':o['id'],'name':o['name'],'delivery':o['delivery']}
        for o in catalog['operators'] if o['id'] in presets.NAMES]},ensure_ascii=False))


if __name__=='__main__':main()
