"""Explicit opt-in SQL fixture in two newly-created, disposable databases.

Never invokes sorting against the selected server's real game/member tables.
"""
from pathlib import Path
import argparse,configparser,io,sys,tempfile,uuid
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from manager import storage
from manager.database import connect
from manager.engines.hangar.server import Server,HEADER,PARAMETERS,ident


def run(ini):
    tag=uuid.uuid4().hex[:12]
    game='SDGO_Hangar_TestGame_'+tag;common='SDGO_Hangar_TestCommon_'+tag
    names=[game,common]
    cn=connect(ini,'DB Member',autocommit=True)
    original=storage.ini_values(Path(ini).read_bytes())
    cur=cn.cursor()
    try:
        for name in names:cur.execute('CREATE DATABASE '+ident(name))
        cur.execute('USE '+ident(game))
        cur.execute('CREATE TABLE dbo.tbl_Users(AccountNumber int PRIMARY KEY,GoUserUnitNum bigint); CREATE TABLE dbo.tbl_Units(UnitNumber int PRIMARY KEY,UnitGradeNumber int,UnitGradeNumber2 int); CREATE TABLE dbo.tbl_UserUnits(UserUnitNumber bigint PRIMARY KEY,AccountNumber int,UserUnitSlotNumber int,UnitNumber int,DateGet datetime,LockState tinyint,BattleShipCount int,UnitLevelNumber tinyint,Paint int,CONSTRAINT UniqueSlots UNIQUE(AccountNumber,UserUnitSlotNumber)); CREATE TABLE dbo.Untouched(GB bigint,DropItems int); INSERT dbo.Untouched VALUES(12345,9); INSERT dbo.tbl_Users VALUES(1,1001),(2,2001); INSERT dbo.tbl_Units VALUES(10,1,2),(20,4,2),(30,3,4); INSERT dbo.tbl_UserUnits VALUES(1001,1,1,10,\'20260101\',1,1,9,77),(1002,1,3,20,\'20260102\',0,0,1,12),(1003,1,4,30,\'20260103\',0,0,1,13),(2001,2,8,20,\'20260104\',1,1,8,88)')
        cur.execute('USE '+ident(common))
        cur.execute("CREATE TABLE dbo.tbl_Account_Pilot(f_idx int PRIMARY KEY,f_pilot nvarchar(20)); INSERT dbo.tbl_Account_Pilot VALUES(1,N'FixtureA'),(2,N'FixtureB')")
        cur.execute('CREATE PROCEDURE dbo.sp_SendMsgGame '+HEADER+' AS SET @retVal=77; SET @v_outIdx=42; RETURN;')
        # A short-lived INI stays local and is removed by TemporaryDirectory.
        root=Path(__file__).resolve().parents[1]/'build/delivery'
        root.mkdir(parents=True,exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='hangar-fixture-',dir=root) as temp:
            temp=Path(temp).resolve();assert temp.is_relative_to(root.resolve())
            for section,name in [('DB Game',game),('DB Member',common)]:original[section]['DBName']=name
            out=io.StringIO();original.write(out)
            fixture_ini=temp/'server.ini';fixture_ini.write_text(out.getvalue(),encoding='utf-8')
            server=Server(fixture_ini)
            assert not server.status()['installed']
            assert server.install()['installed'] and server.install()['installed']
            token=str(uuid.uuid4())
            def mail(sender=1,pilot='FixtureA',recipient='SDGOHangar',body=None):
                body=body or '!sdgo-sort-v1 '+token
                cur.execute('USE '+ident(common))
                cur.execute("DECLARE @out int,@r int; EXEC dbo.sp_SendMsgGame ?,?,0,?,?, '1',0,0,0,0,0,0,0,0,0,0,@out OUTPUT,@r OUTPUT; SELECT @r,@out",sender,pilot,recipient,body)
                while not cur.description:assert cur.nextset()
                return tuple(cur.fetchone())
            assert mail(pilot='wrong')==(5,0)
            assert mail(body='!sdgo-sort-v1 invalid')==(5,0)
            assert mail(recipient='OrdinaryPilot')==(77,42)
            assert mail()==(0,1)
            cur.execute('SELECT UserUnitNumber,UserUnitSlotNumber FROM '+ident(game)+'.dbo.tbl_UserUnits WHERE AccountNumber=1 ORDER BY UserUnitSlotNumber')
            assert [tuple(x) for x in cur.fetchall()]==[(1002,1),(1003,2),(1001,3)]
            cur.execute('SELECT UserUnitSlotNumber,LockState,BattleShipCount,UnitLevelNumber,Paint FROM '+ident(game)+'.dbo.tbl_UserUnits WHERE UserUnitNumber=1001')
            assert tuple(cur.fetchone())==(3,1,1,9,77)
            cur.execute('SELECT GoUserUnitNum FROM '+ident(game)+'.dbo.tbl_Users WHERE AccountNumber=1')
            assert cur.fetchone()[0]==1001
            cur.execute('SELECT UserUnitSlotNumber FROM '+ident(game)+'.dbo.tbl_UserUnits WHERE AccountNumber=2')
            assert cur.fetchone()[0]==8
            assert mail()==(0,1)
            cur.execute('SELECT COUNT(*) FROM '+ident(game)+'.dbo.sdgo_mod_HangarSortReceipt');assert cur.fetchone()[0]==1
            cur.execute('SELECT GB,DropItems FROM '+ident(game)+'.dbo.Untouched');assert tuple(cur.fetchone())==(12345,9)
            cur.execute('USE '+ident(game))
            cur.execute("CREATE TRIGGER dbo.FixtureBadSideEffect ON dbo.tbl_UserUnits AFTER UPDATE AS UPDATE dbo.tbl_UserUnits SET Paint=Paint+1 WHERE AccountNumber=1")
            cur.execute('UPDATE dbo.tbl_UserUnits SET UserUnitSlotNumber=UserUnitSlotNumber+10 WHERE AccountNumber=1')
            cur.execute('SELECT UserUnitNumber,UserUnitSlotNumber,Paint FROM dbo.tbl_UserUnits ORDER BY UserUnitNumber');before=[tuple(x) for x in cur.fetchall()]
            token=str(uuid.uuid4());assert mail()==(5,0)
            cur.execute('SELECT UserUnitNumber,UserUnitSlotNumber,Paint FROM '+ident(game)+'.dbo.tbl_UserUnits ORDER BY UserUnitNumber')
            assert [tuple(x) for x in cur.fetchall()]==before
            assert server.remove()['installed'] is False
            assert mail()==(77,42)
            assert server.install()['installed']
            assert server.remove()['installed'] is False
        print('PASS: isolated SQL install/readback/idempotence, authenticated ownership, durable sorting, locks/ships/active UID preserved, other owner/GB/dropbox unchanged, rollback, original mail restoration')
    finally:
        cur.execute('USE master')
        for name in reversed(names):
            assert name.startswith(('SDGO_Hangar_TestGame_','SDGO_Hangar_TestCommon_')) and name.endswith(tag)
            cur.execute('IF DB_ID(?) IS NOT NULL BEGIN ALTER DATABASE '+ident(name)+' SET SINGLE_USER WITH ROLLBACK IMMEDIATE; DROP DATABASE '+ident(name)+'; END',name)
        cn.close()


if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--server-ini',type=Path,required=True)
    run(parser.parse_args().server_ini)
