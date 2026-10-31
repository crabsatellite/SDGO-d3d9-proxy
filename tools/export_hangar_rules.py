"""Read-only export of native unit grades and dismantle rewards for the Mod UI.

No accounts, inventory, credentials, or server addresses are written to output.
This mirrors reward previews; the native server remains authoritative.
"""
import argparse
import configparser
import json
from pathlib import Path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--server-ini", type=Path)
    src.add_argument("--snapshot", type=Path, help="Directory containing GOnlineGame/dbo.tbl_*.json")
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    if args.server_ini:
        import pyodbc
        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read(args.server_ini, encoding="utf-8-sig")
        db = cfg["DB Game"]
        def esc(v): return "{" + str(v).replace("}", "}}") + "}"
        drivers = [d for d in pyodbc.drivers() if "SQL Server" in d]
        if not drivers: p.error("A SQL Server ODBC driver is required on the host")
        conn = (f"DRIVER={esc(drivers[-1])};SERVER={esc(db['DBServerName'])};DATABASE={esc(db['DBName'])};"
                f"UID={esc(db['ID'])};PWD={esc(db['PWD'])};")
        try: cn = pyodbc.connect(conn, timeout=5, autocommit=True)
        except pyodbc.Error as e: raise SystemExit(f"Read-only SQL unavailable; SQLSTATE={e.args[0]}") from None
        try:
            cn.timeout=15
            cur=cn.cursor()
            cur.execute("SELECT UnitNumber,UnitGradeNumber,UnitGradeNumber2,UnitModelName FROM dbo.tbl_Units")
            units=[dict(zip([d[0] for d in cur.description], row)) for row in cur.fetchall()]
            cur.execute("SELECT UnitGradeNumber,UnitGradeNumber2,DeleteUnitPoint,DeleteUnitItemCategory,DeleteUnitItemAmount FROM dbo.tbl_UnitGradeInfo")
            grades=[dict(zip([d[0] for d in cur.description], row)) for row in cur.fetchall()]
            cur.execute("SELECT CatalogueID,Name FROM dbo.tbl_ItemCategory WHERE CatalogueID IN (SELECT DeleteUnitItemCategory FROM dbo.tbl_UnitGradeInfo)")
            item_names={int(row[0]):str(row[1]).split('\0')[0] for row in cur.fetchall()}
        finally: cn.close()
    else:
        def load(name):
            base=args.snapshot/"GOnlineGame"
            files=sorted(base.glob(f"dbo.{name}.part*.json"))
            if (base/f"dbo.{name}.json").exists(): files=[base/f"dbo.{name}.json"]
            if not files: raise ValueError(f"Missing table {name}")
            return [r for f in files for r in json.loads(f.read_text(encoding="utf-8-sig"))]
        units,grades=load("tbl_Units"),load("tbl_UnitGradeInfo")
        item_names={int(r['CatalogueID']):str(r['Name']).split('\0')[0] for r in load('tbl_ItemCategory')}
    lookup={(int(r['UnitGradeNumber']),int(r['UnitGradeNumber2'])):r for r in grades}
    lines=["# unit_id\trank\tsubrank\tGB\titem_category\titem_amount\tname\titem_name (UTF-8)"]
    seen=set()
    for u in sorted(units,key=lambda r:int(r['UnitNumber'])):
        uid=int(u['UnitNumber']);key=(int(u['UnitGradeNumber']),int(u['UnitGradeNumber2']))
        if uid in seen: raise ValueError("Duplicate unit rule")
        seen.add(uid);g=lookup[key]
        name=str(u['UnitModelName']).split('\0')[0].replace('\t',' ').replace('\r',' ').replace('\n',' ')
        item_name=item_names.get(int(g['DeleteUnitItemCategory']),'原生道具').replace('\t',' ').replace('\r',' ').replace('\n',' ')
        if item_name.startswith('EXP Pack_'):
            item_name='经验胶囊 '+item_name[len('EXP Pack_'):]
        lines.append('\t'.join(map(str,[uid,*key,int(g['DeleteUnitPoint']),int(g['DeleteUnitItemCategory']),int(g['DeleteUnitItemAmount']),name,item_name])))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('w',encoding='utf-8',newline='\n') as f: f.write('\n'.join(lines)+'\n')
    print(f"Exported {len(seen)} non-user unit rules; no game state changed.")


if __name__=='__main__': main()
