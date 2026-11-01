"""Exercise the real adapter in a NEW, synthetic-only SQL database.

Reads native procedure definitions from the selected host; never invokes them in
that host's game database. The fixture is retained with its receipt for review.
"""
import argparse
import configparser
import copy
import datetime
import json
from pathlib import Path
import re
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from manager.engines.operators import model,server,presets
from manager.engines.operators.assets import Assets


def last_row(cur):
    found=None
    while True:
        if cur.description:
            values=cur.fetchall()
            if values:found=values[-1]
        if not cur.nextset():return found


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--server-ini',type=Path,required=True)
    p.add_argument('--client',type=Path,required=True)
    p.add_argument('--snapshot',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('--default-pack',action='store_true')
    args=p.parse_args();args.output.mkdir(parents=True,exist_ok=True)
    stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%d_%H%M%S_%f')
    db='sdgo_operator_fixture_'+stamp
    assert re.fullmatch(r'sdgo_operator_fixture_[0-9_]+',db)
    native=server.connect(args.server_ini)
    try:
        n=native.cursor()
        source_db=n.execute('SELECT DB_NAME()').fetchone()[0]
        assert source_db!=db
        names=['sp_OperatorDrop','sp_GetUserOperatorPage','sp_GetPilotOperator','sp_CreateUserOperator',
               'sp_CheckUserOperatorTimeLimit','sp_GeOperatorProbability','sp_CreateUserMixTreeItem','sp_SetUserOperatorName','sp_GetUserOperator']
        definitions={name:server.definition(n,name) for name in names}
        if n.execute('SELECT OBJECT_ID(?)','dbo.sdgo_OperatorProcedureBackup').fetchone()[0] is not None:
            for name in server.WRAPPED:
                row=n.execute('SELECT OriginalDefinition FROM dbo.sdgo_OperatorProcedureBackup WHERE ProcedureName=?',name).fetchone()
                if row:definitions[name]=row[0]
        n.execute('CREATE DATABASE ['+db+']')
    finally:native.close()
    source_ini=configparser.ConfigParser(interpolation=None)
    source_ini.read(args.server_ini,encoding='utf-8-sig')
    ini=configparser.ConfigParser(interpolation=None)
    ini['DB Game']=dict(source_ini['DB Game']);ini['DB Game']['DBName']=db
    fixture_ini=args.output/'fixture-private.ini'
    with fixture_ini.open('w',encoding='utf-8') as f:ini.write(f)
    cn=server.connect(fixture_ini);cur=cn.cursor()
    assert cur.execute('SELECT DB_NAME()').fetchone()[0]==db
    schema="""
    CREATE TABLE dbo.tbl_Users(AccountNumber INT PRIMARY KEY,GoUserOperatorNum BIGINT DEFAULT 0);
    CREATE TABLE dbo.tbl_OperatorData(OperatorID INT PRIMARY KEY,OperatorName VARCHAR(64),TeamWin INT,TeamLose INT,
      BattleResult INT,ChangeSp INT,ServiceType INT,AddOn1 INT,AddOn2 INT,AddOn3 INT,AddOn4 INT,DropType TINYINT,GParts INT);
    CREATE TABLE dbo.tbl_ItemCategory(CatalogueID INT PRIMARY KEY,Name VARCHAR(100),RefIndex INT,TimeLimit TINYINT,Option1 INT,Option2 INT);
    CREATE TABLE dbo.tbl_UserOperators(UserOperatorNumber BIGINT IDENTITY PRIMARY KEY,AccountNumber INT,SlotNumber INT,
      OperatorNumber INT,OperatorNickname VARCHAR(16) DEFAULT '',DateGet DATETIME DEFAULT GETDATE(),BuyType INT,
      DateEnd DATETIME NULL,CategoryNumber INT,ShopOperator TINYINT DEFAULT 0,RunOut TINYINT DEFAULT 0,
      Forbidden TINYINT DEFAULT 0,OPType TINYINT DEFAULT 0,ShopItemId VARCHAR(6) DEFAULT '',ShopItemtblidx INT DEFAULT 0,
      SerialNum BIGINT DEFAULT 0,IsSetName TINYINT DEFAULT 0,RemainCount SMALLINT DEFAULT 0);
    CREATE TABLE dbo.tbl_UserOperatorSlotNumberList(UserOperatorSlotNumber INT PRIMARY KEY);
    CREATE TABLE dbo.tbl_UserDropItemBox(AccountNumber INT,ItemCategory INT,Amount INT,DropConditionID INT);
    CREATE TABLE dbo.tbls_AccountPocket(f_idx INT PRIMARY KEY,f_GC INT,f_GP INT);
    CREATE TABLE dbo.tbl_OperatorDropDice(Idx INT PRIMARY KEY,OperatorNumber INT,CategoryID INT,Prob INT,ItemName VARCHAR(100),Amount INT,Opt INT,SumProb INT);
    CREATE TABLE dbo.tbl_OperatorIndex(OperatorNumber INT,ReadIndex INT);
    CREATE TABLE dbo.tbl_DiceList_1000(Idx INT PRIMARY KEY,Probability INT);
    CREATE TABLE dbo.FixtureSerial(ID BIGINT IDENTITY PRIMARY KEY);
    CREATE TABLE dbo.FixtureRoll(Roll INT,Quantity INT);
    CREATE TABLE dbo.tbl_MixTreeFile(Idx INT PRIMARY KEY,OutUnitID INT);
    CREATE TABLE dbo.tbl_Units(UnitNumber INT PRIMARY KEY);
    CREATE TABLE dbo.tbl_PaintItem(PaintID INT PRIMARY KEY,Type INT);
    CREATE TABLE dbo.tbl_StickerItem(StickerID INT PRIMARY KEY);
    CREATE TABLE dbo.tbl_UserMixTreeItemsSlotNumberList(SlotNumber INT PRIMARY KEY);
    CREATE TABLE dbo.tbl_UserMixTreeItems(UserMixTreeItemNumber BIGINT IDENTITY PRIMARY KEY,AccountNumber INT,
        SlotNumber INT,MixTreeItemID INT,CategoryNumber INT,RemainCount INT,BuyType INT,ShopItemtblidx INT);
    INSERT dbo.FixtureRoll VALUES(0,0);
    """
    cur.execute(schema)
    cur.execute("CREATE PROCEDURE dbo.sp_CreateSerial @Account INT,@Serial BIGINT OUTPUT,@Ret INT OUTPUT AS BEGIN SET NOCOUNT ON;INSERT dbo.FixtureSerial DEFAULT VALUES;SET @Serial=SCOPE_IDENTITY();SET @Ret=0;END")
    catalog=model.import_catalog(args.snapshot,Assets(args.client),include_installed=False)
    default_catalog=presets.initialize(catalog,args.snapshot,Assets(args.client)) if args.default_pack else None
    data=catalog['native_server_data']
    cols=['OperatorID','OperatorName','TeamWin','TeamLose','BattleResult','ChangeSp','ServiceType','AddOn1','AddOn2','AddOn3','AddOn4','DropType','GParts']
    for source in data.values():cur.execute('INSERT dbo.tbl_OperatorData VALUES('+','.join('?' for _ in cols)+')',*[source[x] for x in cols])
    cat_rows=model.load(args.snapshot/'GOnlineGame/dbo.tbl_ItemCategory.json')
    needed={1070001,2220010}
    needed.update(o['category_id'] for o in catalog['operators'] if any(c['CatalogueID']==o['category_id'] for c in cat_rows))
    dice=model.load(args.snapshot/'GOnlineGame/dbo.tbl_OperatorDropDice.json')
    needed.update(x['CategoryID'] for x in dice)
    if default_catalog:
        needed.update(r['category_id'] for o in default_catalog['operators'] if o['managed'] for r in o['drops'])
        needed.update(c for pool in default_catalog['drop_pools'].values() for c in pool['category_ids'])
    for source in cat_rows:
        if source['CatalogueID'] in needed:
            cur.execute('INSERT dbo.tbl_ItemCategory VALUES(?,?,?,?,?,?)',source['CatalogueID'],'fixture item',source['RefIndex'],source['TimeLimit'],source['Option1'],source['Option2'])
    for source in dice:
        cur.execute('INSERT dbo.tbl_OperatorDropDice VALUES(?,?,?,?,?,?,?,?)',source['Idx'],source['OperatorNumber'],source['CategoryID'],source['Prob'],'fixture',source['Amount'],source['Opt'],source['SumProb'])
    for i in range(100):cur.execute('INSERT dbo.tbl_UserOperatorSlotNumberList VALUES(?)',i+1)
    for i in range(1000):cur.execute('INSERT dbo.tbl_DiceList_1000 VALUES(?,?)',i,i)
    for i in range(1,121):cur.execute('INSERT dbo.tbl_UserMixTreeItemsSlotNumberList VALUES(?)',i)
    if default_catalog:
        for table,columns in [('tbl_MixTreeFile',['Idx','OutUnitID']),('tbl_Units',['UnitNumber']),
                              ('tbl_PaintItem',['PaintID','Type']),('tbl_StickerItem',['StickerID'])]:
            for r in model.load(args.snapshot/'GOnlineGame'/('dbo.'+table+'.json')):
                cur.execute('INSERT dbo.'+table+' VALUES('+','.join('?' for _ in columns)+')',*[r[c] for c in columns])
    for name,text in definitions.items():
        text=re.sub(r'\[GDStoreBilling\]',lambda _: '['+db+']',text,flags=re.I)
        assert 'GDStoreBilling' not in text
        cur.execute(text)
    for account in [7770001,7770002,7770003]:
        cur.execute('INSERT dbo.tbl_Users VALUES(?,0)',account)
        cur.execute('INSERT dbo.tbls_AccountPocket VALUES(?,100,200)',account)
    cur.execute("INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber,OperatorNickname,OPType,RunOut,DateEnd,IsSetName) VALUES(7770001,1,60003,1210002,'keep-personal',1,1,'20000101',1)")
    cur.execute('UPDATE dbo.tbl_Users SET GoUserOperatorNum=(SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001) WHERE AccountNumber=7770001')
    for slot in range(1,101):cur.execute('INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber,OPType) VALUES(7770002,?,60001,1210001,0)',slot)
    catalog=model.update_operator(catalog,60003,{'delivery':'all_players','drops':[{'category_id':2220010,'chance_bps':10000,'min_amount':2,'max_amount':5}]})
    catalog,new_id=model.create_operator(catalog,60003)
    catalog=model.update_operator(catalog,new_id,{'name':'registered only','delivery':'product'})
    first=server.apply_catalog(catalog,fixture_ini)
    assert first['storefront_changes'] is False
    def page(account):
        return last_row(cur.execute('DECLARE @G BIGINT,@N INT,@R INT;EXEC @R=dbo.sp_GetUserOperatorPage ?,0,@G OUTPUT,@N OUTPUT;SELECT @R,@G,@N',account))
    page(7770001)
    row=cur.execute('SELECT OperatorNickname,OPType,RunOut,DateEnd,IsSetName FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=60003').fetchone()
    assert tuple(row)==('keep-personal',0,0,None,1)
    assert not cur.execute('SELECT 1 FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?',new_id).fetchone()
    page(7770003);page(7770003)
    assert cur.execute('SELECT COUNT(*) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60003').fetchone()[0]==1
    page(7770002)
    assert tuple(cur.execute('SELECT COUNT(*),MIN(SlotNumber),MAX(SlotNumber) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770002').fetchone())==(100,1,100)
    assert cur.execute('SELECT 1 FROM dbo.sdgo_OperatorGrantIssues WHERE AccountNumber=7770002 AND OperatorID=60003').fetchone()
    user_op=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=60003').fetchone()[0]
    def drop(account=7770001,number=user_op):
        return last_row(cur.execute('DECLARE @C INT,@A INT,@R INT;EXEC dbo.sp_OperatorDrop ?,?,@C OUTPUT,@A OUTPUT,@R OUTPUT;SELECT @C,@A,@R',account,number))
    for _ in range(10):
        r=drop();assert r[0]==2220010 and 2<=r[1]<=5 and r[2]==0
    assert cur.execute('SELECT f_GC FROM dbo.tbls_AccountPocket WHERE f_idx=7770001').fetchone()[0]==10100
    count=cur.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox').fetchone()[0]
    r=drop(7770003,user_op);assert r[2]!=0
    assert cur.execute('SELECT COUNT(*) FROM dbo.tbl_UserDropItemBox').fetchone()[0]==count
    catalog=model.update_operator(catalog,60003,{'drops':[]})
    server.apply_catalog(catalog,fixture_ini);r=drop();assert tuple(r)==(1070001,1,0)
    # Test the SQL cumulative-boundary implementation with a fixture-only RNG.
    catalog=model.update_operator(catalog,60003,{'drops':[{'category_id':2220010,'chance_bps':2500,'min_amount':2,'max_amount':5}]})
    server.apply_catalog(catalog,fixture_ini)
    cur.execute("ALTER PROCEDURE dbo.sdgo_OperatorUniform @Bound INT,@Value INT OUTPUT AS BEGIN SET NOCOUNT ON;SELECT @Value=CASE WHEN @Bound=10000 THEN Roll ELSE Quantity END FROM dbo.FixtureRoll;END")
    for roll,q,expected in [(0,0,(2220010,2,0)),(2499,3,(2220010,5,0)),(2500,0,(1070001,1,0)),(9999,0,(1070001,1,0))]:
        cur.execute('UPDATE dbo.FixtureRoll SET Roll=?,Quantity=?',roll,q);assert tuple(drop())==expected
    # Reinstall the production crypto RNG and verify repeatable module apply.
    server.apply_catalog(catalog,fixture_ini)
    for bound in [1,4,10000,65535]:
        value=last_row(cur.execute('DECLARE @V INT;EXEC dbo.sdgo_OperatorUniform ?,@V OUTPUT;SELECT @V',bound))[0]
        assert 0<=value<bound
    original=server.definition(cur,'sp_GetPilotOperator')
    modify=server.alter(original) if re.search(r'(?im)^\s*CREATE\s+PROC',original) else original
    cur.execute(modify+'\n-- another module changed this procedure')
    conflict_catalog=model.update_operator(catalog,60003,{'name':'must rollback'})
    try:server.apply_catalog(conflict_catalog,fixture_ini)
    except ValueError:pass
    else:raise AssertionError('Procedure conflict was overwritten')
    assert cur.execute('SELECT DisplayName FROM dbo.sdgo_OperatorProfiles WHERE OperatorID=60003').fetchone()[0]!='must rollback'
    cur.execute(modify)
    source_hash=server.source_hash(server.definition(cur,'sp_GetPilotOperator'))
    assert cur.execute('SELECT InstalledHash FROM dbo.sdgo_OperatorProcedureBackup WHERE ProcedureName=?','sp_GetPilotOperator').fetchone()[0]==source_hash
    default_results={}
    if default_catalog:
        cur.execute("INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber,OperatorNickname,OPType,IsSetName) VALUES(7770001,2,?,?,'my-custom',0,1)",new_id,model.native_category(new_id))
        cur.execute("INSERT dbo.tbl_UserOperators(AccountNumber,SlotNumber,OperatorNumber,CategoryNumber,OperatorNickname,OPType,RunOut,DateEnd,IsSetName) VALUES(7770003,2,60013,1210012,'Mizuki(EXP)',1,1,'20000101',1)")
        applied=server.apply_catalog(default_catalog,fixture_ini)
        assert applied['legacy_script_aliases_cleared']==1
        assert applied['existing_default_entitlements_upgraded']==1
        assert tuple(cur.execute('SELECT OPType,RunOut,DateEnd FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60013').fetchone())==(0,0,None)
        assert tuple(cur.execute('SELECT OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60013').fetchone())==('@SDGO:60013',1)
        assert cur.execute('SELECT OperatorNickname FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=60003').fetchone()[0]=='@SDGO:60003'
        assert tuple(cur.execute('SELECT OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?',new_id).fetchone())==('my-custom',1)
        page(7770003);page(7770003)
        assert cur.execute('SELECT COUNT(DISTINCT OperatorNumber) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber IN (60003,60013,60018,60025,60041)').fetchone()[0]==5
        assert not cur.execute('SELECT 1 FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber IN (60003,60013,60018,60025,60041) GROUP BY OperatorNumber HAVING COUNT(*)>1').fetchone()
        assert cur.execute("SELECT COUNT(*) FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber IN (60003,60013,60018,60025,60041) AND IsSetName=1 AND OperatorNickname='@SDGO:'+CONVERT(VARCHAR(5),OperatorNumber)").fetchone()[0]==5
        # Repair a permanent record before the early ownership CONTINUE, too.
        cur.execute('DISABLE TRIGGER dbo.sdgo_OperatorFixedNames ON dbo.tbl_UserOperators')
        cur.execute("UPDATE dbo.tbl_UserOperators SET OperatorNickname='',IsSetName=0 WHERE AccountNumber=7770003 AND OperatorNumber=60003")
        page(7770003)
        assert cur.execute('SELECT IsSetName FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60003').fetchone()[0]==1
        cur.execute('ENABLE TRIGGER dbo.sdgo_OperatorFixedNames ON dbo.tbl_UserOperators')
        default_number=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60003').fetchone()[0]
        assert last_row(cur.execute("DECLARE @R INT;EXEC @R=dbo.sp_SetUserOperatorName 7770003,?,'manual';SELECT @R",default_number))[0]==10
        # Other operator IDs retain the normal native naming flow.
        custom_number=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770001 AND OperatorNumber=?',new_id).fetchone()[0]
        cur.execute("UPDATE dbo.tbl_UserOperators SET OperatorNickname='',IsSetName=0 WHERE UserOperatorNumber=?",custom_number)
        assert last_row(cur.execute("DECLARE @R INT;EXEC @R=dbo.sp_SetUserOperatorName 7770001,?,'chosen';SELECT @R",custom_number))[0]==0
        # The immediate item receipt path also finalizes a fixed name, including
        # a purchased default after its delivery mode was changed to product.
        cur.execute("UPDATE dbo.sdgo_OperatorProfiles SET DeliveryMode='product' WHERE OperatorID=60013")
        cur.execute("UPDATE dbo.tbl_UserOperators SET OperatorNickname='',IsSetName=0 WHERE AccountNumber=7770003 AND OperatorNumber=60013")
        receipt_number=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=60013').fetchone()[0]
        cur.execute('EXEC dbo.sp_GetUserOperator 7770003,?',receipt_number)
        while cur.nextset():pass
        assert tuple(cur.execute('SELECT OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE UserOperatorNumber=?',receipt_number).fetchone())==('@SDGO:60013',1)
        server.apply_catalog(default_catalog,fixture_ini)
        # Legacy Manager builds call the original creation procedure and do not
        # set a name afterward. The table guard finalizes only the fixed defaults
        # and does not disturb the native identity/output arguments.
        cur.execute('INSERT dbo.tbl_Users VALUES(7770004,0)')
        def native_create(identity,category):
            return last_row(cur.execute("DECLARE @S INT,@U BIGINT,@Serial BIGINT,@R INT;EXEC @R=dbo.sp_CreateUserOperator 7770004,'',0,?,NULL,0,?,0,6,@S OUTPUT,@U OUTPUT,@Serial OUTPUT;SELECT @R,@S,@U",category,identity))
        created=native_create(60018,next(o['category_id'] for o in default_catalog['operators'] if o['id']==60018))
        assert created[0]==0 and created[1]>0 and created[2]>0
        assert tuple(cur.execute('SELECT OperatorNumber,OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE UserOperatorNumber=?',created[2]).fetchone())==(60018,'@SDGO:60018',1)
        custom_created=native_create(new_id,model.native_category(new_id))
        assert custom_created[0]==0
        assert tuple(cur.execute('SELECT OperatorNickname,IsSetName FROM dbo.tbl_UserOperators WHERE UserOperatorNumber=?',custom_created[2]).fetchone())==('',0)
        cur.execute("ALTER PROCEDURE dbo.sdgo_OperatorUniform @Bound INT,@Value INT OUTPUT AS BEGIN SET NOCOUNT ON;SELECT @Value=CASE WHEN @Bound=10000 THEN Roll ELSE Quantity END FROM dbo.FixtureRoll;SET @Value=@Value % @Bound;END")
        # Exercise the first and last member of every typed pool in SQL.
        for op in default_catalog['operators']:
            if not op['managed']:continue
            number=cur.execute('SELECT UserOperatorNumber FROM dbo.tbl_UserOperators WHERE AccountNumber=7770003 AND OperatorNumber=?',op['id']).fetchone()[0]
            offset=0
            for rule in op['drops']:
                members=default_catalog['drop_pools'][rule['pool']]['category_ids'] if 'pool' in rule else [rule['category_id']]
                for index in (0,len(members)-1):
                    cur.execute('UPDATE dbo.FixtureRoll SET Roll=?,Quantity=?',offset,index)
                    got=tuple(drop(7770003,number))
                    assert got[0]==members[index] and rule['min_amount']<=got[1]<=rule['max_amount'] and got[2]==0
                offset+=rule['chance_bps']
        # Every accepted design can be passed to the real native inventory proc.
        blueprints=default_catalog['drop_pools']['blueprints']['category_ids']
        for category in blueprints:
            # Test-only account can reuse empty synthetic blueprint slots.
            cur.execute('DELETE dbo.tbl_UserMixTreeItems WHERE AccountNumber=7770003')
            result=last_row(cur.execute('DECLARE @Count INT,@Number BIGINT,@R INT;EXEC dbo.sp_CreateUserMixTreeItem 7770003,0,?,1,11,@Count OUTPUT,@Number OUTPUT,@R OUTPUT;SELECT @R,@Count,@Number',category))
            assert result[0]==0 and result[1]==1 and result[2]>0
        server.apply_catalog(default_catalog,fixture_ini)
        assert cur.execute('SELECT COUNT(*) FROM dbo.sdgo_OperatorLegacyEntitlementBackup').fetchone()[0]==1
        assert 'CRYPT_GEN_RANDOM' in server.definition(cur,'sdgo_OperatorUniform')
        default_results={'default_five_permanent_grants':True,'fixed_defaults_skip_native_naming':True,'other_operators_can_name_normally':True,
                         'legacy_creator_fixed_names_and_identity':True,
                         'existing_entitlements_migrated_in_place':True,
                         'typed_pool_sql_sampling':True,'native_blueprint_inventory_proc':len(blueprints)}
    model.save(args.output/'catalog.json',catalog)
    model.save(args.output/'result.json',{'database':db,'synthetic_accounts_only':True,'source_game_database_written':False,
        'native_procedures_exercised':True,'all_players_grant':True,'permanent_upgrade_preserves_nickname':True,
        'product_registration_without_storefront':True,'full_inventory_preserved':True,'cross_account_drop_rejected':True,
        'native_bonus_preserved_once':True,'zero_drop':True,'probability_boundaries':True,'quantity_bounds':True,
        'production_rng_restored':True,'idempotent_apply':True,'procedure_conflict_rolls_back':True,**default_results})
    print(json.dumps({'database':db,'result':'PASS','source_game_database_written':False}))
    cn.close()


if __name__=='__main__':main()
