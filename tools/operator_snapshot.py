"""Read-only import of operator definitions. Never exports account data or credentials."""
import argparse
import hashlib
import json
import secrets
from pathlib import Path
from assistant_modules.operators import model, server


def export(server_ini, output):
    output=Path(output)
    if output.exists():raise ValueError('快照输出目录已存在')
    pending=output.with_name(output.name+'.pending-'+secrets.token_hex(4))
    pending.mkdir(parents=True,exist_ok=False)
    cn=server.connect(server_ini)
    try:
        cur=cn.cursor()
        files={}
        target=pending/'GOnlineGame'
        target.mkdir()
        sources={name:'*' for name in ('tbl_OperatorData','tbl_OperatorDropDice','tbl_ItemCategory',
                                     'tbl_MixTreeFile','tbl_PaintItem','tbl_StickerItem','tbl_EtcItems')}
        sources['tbl_Units']='UnitNumber'
        for name,columns in sources.items():
            cur.execute('SELECT '+columns+' FROM dbo.['+name+']')
            keys=[v[0] for v in cur.description]
            rows=[dict(zip(keys,row)) for row in cur.fetchall()]
            path=target/('dbo.'+name+'.json')
            model.save(path,rows)
            files[path.relative_to(pending).as_posix()]={'rows':len(rows),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        if cur.execute("SELECT OBJECT_ID('dbo.sdgo_OperatorProfiles')").fetchone()[0] is not None:
            cur.execute('SELECT * FROM dbo.sdgo_OperatorProfiles')
            keys=[v[0] for v in cur.description]
            rows=[dict(zip(keys,row)) for row in cur.fetchall()]
            path=target/'dbo.sdgo_OperatorProfiles.json'
            model.save(path,rows)
            files[path.relative_to(pending).as_posix()]={'rows':len(rows),'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
        model.save(pending/'manifest.json',{'schema_version':1,'scope':'non-user operator configuration','files':files})
        pending.rename(output)
        return files
    finally:
        cn.close()


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--server-ini',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    print(json.dumps(export(a.server_ini,a.output),ensure_ascii=False))
