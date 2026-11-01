"""Cases called only inside hangar_sort_sql_fixture's disposable databases."""
import struct,uuid
from manager.engines.hangar.server import ident


def run_gb_cases(cur,game,common,billing,mail):
    assert all(name.startswith('SDGO_Hangar_Test') for name in (game,common,billing))
    g,c,b=map(ident,(game,common,billing))
    def seed(owner,count=6,wallet=10000):
        cur.execute(f'INSERT {c}.dbo.tbl_Account_Pilot VALUES(?,?)',owner,'Fixture'+str(owner))
        cur.execute(f'INSERT {g}.dbo.tbl_Users(AccountNumber,GoUserUnitNum) VALUES(?,?)',owner,owner*1000+999)
        cur.execute(f'INSERT {b}.dbo.tbls_AccountPocket VALUES(?,?)',owner,wallet)
        for index in range(1,count+1):
            cur.execute(f"INSERT {g}.dbo.tbl_UserUnits(UserUnitNumber,AccountNumber,UserUnitSlotNumber,UnitNumber,DateGet,LockState,BattleShipCount,UnitLevelNumber,Paint) VALUES(?,?,?,10,'20260101',0,0,1,?)",owner*1000+index,owner,index,index)
        cur.execute(f"INSERT {g}.dbo.tbl_UserUnits(UserUnitNumber,AccountNumber,UserUnitSlotNumber,UnitNumber,DateGet,LockState,BattleShipCount,UnitLevelNumber,Paint) VALUES(?,?,?,20,'20260101',0,0,9,77)",owner*1000+999,owner,count+1)
    def bodies(owner,entries,keep=2,gb=None,sort=False):
        token=str(uuid.uuid4());prefix='!sdgo-gb-v1 '+token+' '
        header=prefix+f'H {keep} {len(entries)} {len(entries)*400 if gb is None else gb} {int(sort)}'
        data=[prefix+'D '+str(i//8)+' '+b''.join(struct.pack('>QIH',uid,unit,slot) for uid,unit,slot in entries[i:i+8]).hex().upper() for i in range(0,len(entries),8)]
        return token,[header,*data,prefix+'C']
    def send(owner,body):return mail(sender=owner,pilot='Fixture'+str(owner),body=body)
    def gp(owner):cur.execute(f'SELECT f_GP FROM {b}.dbo.tbls_AccountPocket WHERE f_idx=?',owner);return cur.fetchone()[0]
    def units(owner):cur.execute(f'SELECT UserUnitNumber,UserUnitSlotNumber,Paint FROM {g}.dbo.tbl_UserUnits WHERE AccountNumber=? ORDER BY UserUnitNumber',owner);return [tuple(x) for x in cur.fetchall()]
    seed(3)
    cur.execute(f'UPDATE {g}.dbo.tbl_UserUnits SET LockState=1 WHERE UserUnitNumber=3004; UPDATE {g}.dbo.tbl_UserUnits SET UnitLevelNumber=9 WHERE UserUnitNumber=3005; UPDATE {g}.dbo.tbl_UserUnits SET BattleShipCount=1 WHERE UserUnitNumber=3006')
    token,requests=bodies(3,[(3001,10,1),(3002,10,2),(3003,10,3)])
    before=units(3)
    assert send(3,requests[-1])==(0,3) and send(3,requests[1])==(0,3)
    assert units(3)==before and gp(3)==10000 # commit cannot run without a complete header
    assert send(3,requests[1])==(0,3) # duplicate fragment is harmless
    assert send(3,requests[0])==(0,3) # out-of-order arrival completes the one transaction
    assert gp(3)==11200 and [x[0] for x in units(3)]==[3004,3005,3006,3999]
    for request in requests:assert send(3,request)==(0,3)
    assert gp(3)==11200
    cur.execute(f'SELECT COUNT(*) FROM {g}.dbo.NativeDeleted WHERE UserUnitNumber BETWEEN 3001 AND 3003');assert cur.fetchone()[0]==3
    seed(4,count=500)
    token,requests=bodies(4,[(4000+i,10,i) for i in range(1,499)])
    assert len(requests)==65
    for request in requests[:-1]:assert send(4,request)==(0,4)
    assert len(units(4))==501 and gp(4)==10000
    assert send(4,requests[-1])==(0,4)
    assert len(units(4))==3 and gp(4)==10000+498*400
    for owner,options in [(5,{'wallet':2147483600}),(6,{}),(7,{}),(8,{}),(10,{}),(11,{})]:seed(owner,count=3,**options)
    cur.execute(f'UPDATE {g}.dbo.tbl_UserUnits SET LockState=1 WHERE UserUnitNumber=8001')
    for owner,entries,expected in [(5,[(5001,10,1)],400),(7,[(7001,10,1),(7002,10,2)],800),(8,[(8001,10,1)],400),(6,[(6001,10,1)],401),(10,[(3004,10,4)],400),(11,[(11001,10,2)],400)]:
        original=units(owner);wallet=gp(owner);token,requests=bodies(owner,entries,gb=expected)
        for request in requests[:-1]:assert send(owner,request)==(0,owner)
        assert send(owner,requests[-1])==(5,0)
        assert units(owner)==original and gp(owner)==wallet
        assert send(owner,requests[-1])==(5,0)
    # A native per-unit failure after an earlier selected unit was deleted must
    # roll back both inventory and its native recovery log, plus all GB credits.
    seed(9,count=4)
    cur.execute('USE '+g)
    cur.execute('ALTER PROCEDURE dbo.sp_DeleteUserUnit @a int,@u bigint,@t int,@d tinyint,@ot int OUTPUT,@ti tinyint OUTPUT,@sh tinyint OUTPUT,@r tinyint OUTPUT,@s bigint OUTPUT,@c int OUTPUT,@sl int OUTPUT,@ret int OUTPUT AS BEGIN SET NOCOUNT ON; IF @u=9002 BEGIN SET @ret=9; RETURN; END; DELETE dbo.tbl_UserUnits WHERE AccountNumber=@a AND UserUnitNumber=@u AND UnitNumber=@t; SET @ret=CASE WHEN @@ROWCOUNT=1 THEN 0 ELSE 1 END; IF @ret=0 INSERT dbo.NativeDeleted VALUES(@u); END')
    before=units(9);token,requests=bodies(9,[(9001,10,1),(9002,10,2)])
    for request in requests[:-1]:assert send(9,request)==(0,9)
    assert send(9,requests[-1])==(5,0) and units(9)==before and gp(9)==10000
    cur.execute(f'SELECT COUNT(*) FROM {g}.dbo.NativeDeleted WHERE UserUnitNumber IN(9001,9002)');assert cur.fetchone()[0]==0
    seed(12,count=4);token,requests=bodies(12,[(12001,10,1)],sort=True)
    for request in requests:assert send(12,request)==(0,12)
    assert gp(12)==10400 and len(units(12))==4
    assert sorted(x[1] for x in units(12))==[1,2,3,4]
    seed(13,count=951)
    assert send(13,'!sdgo-sort-v1 '+str(uuid.uuid4()))==(0,13)
    cur.execute(f'SELECT ExpandSlotCount FROM {g}.dbo.tbl_Users WHERE AccountNumber=13');assert cur.fetchone()[0]+24==954
    assert len(units(13))==952
    assert gp(2)==777
    print('PASS: server GB batch (498 units), quick chunk staging, explicit commit, out-of-order/duplicate messages, native dismantle, atomic wallet, protected units, keep floor, stale rewards, overflow, rollback, idempotence; all in isolated fixture databases')
