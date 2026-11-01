"""Server-side operator adapter. Store listings and prices are never changed."""
import configparser
import hashlib
import json
from pathlib import Path
import re
from .model import registrations, revision, validate


def validate_pools(cur, catalog):
    """Recheck typed references against the live host, without name matching."""
    clauses={
        'blueprints':("C.CatalogueID/100000=15 AND EXISTS(SELECT 1 FROM dbo.tbl_MixTreeFile M "
                      "JOIN dbo.tbl_Units U ON U.UnitNumber=M.OutUnitID WHERE M.Idx=C.RefIndex)"),
        'stickers':"C.CatalogueID/100000=17 AND EXISTS(SELECT 1 FROM dbo.tbl_StickerItem S WHERE S.StickerID=C.RefIndex)",
        'paints':"C.CatalogueID/100000=16 AND EXISTS(SELECT 1 FROM dbo.tbl_PaintItem P WHERE P.PaintID=C.RefIndex AND P.Type IN (0,1))",
        'polish':"C.CatalogueID/100000=16 AND EXISTS(SELECT 1 FROM dbo.tbl_PaintItem P WHERE P.PaintID=C.RefIndex AND P.Type=2)",
    }
    used={r['pool'] for o in catalog['operators'] if o['managed'] and o['enabled'] for r in o['drops'] if 'pool' in r}
    for key in used:
        pool=catalog['drop_pools'][key]
        clause=clauses.get(key,'1=1')
        valid={r[0] for r in cur.execute('SELECT C.CatalogueID FROM dbo.tbl_ItemCategory C WHERE '+clause)}
        if not set(pool['category_ids'])<=valid:
            raise ValueError('随机物品池与当前服务端类型/配方不一致：'+pool['label'])


def migrate_default_aliases(cur, catalog):
    if catalog.get('default_pack_version') != 1:return 0
    from .presets import LEGACY_ALIASES
    changed=0
    enabled={o['id'] for o in catalog['operators'] if o['managed'] and o['enabled']}
    for identity,alias in LEGACY_ALIASES.items():
        if identity not in enabled:continue
        # Only exact aliases written by the historical script are migrated.
        # The following fixed-name migration replaces these historical aliases
        # with an ASCII token and a completed-name flag in the same transaction.
        cur.execute('''INSERT dbo.sdgo_OperatorLegacyNameBackup
            SELECT U.UserOperatorNumber,U.AccountNumber,U.OperatorNickname,U.IsSetName
            FROM dbo.tbl_UserOperators U WHERE U.OperatorNumber=?
            AND U.OperatorNickname COLLATE Latin1_General_100_BIN2=?
            AND NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorLegacyNameBackup B WHERE B.UserOperatorNumber=U.UserOperatorNumber)''',identity,alias)
        row=cur.execute('''SELECT COUNT(*) FROM dbo.tbl_UserOperators
            WHERE OperatorNumber=? AND OperatorNickname COLLATE Latin1_General_100_BIN2=?''',identity,alias).fetchone()
        changed+=row[0]
    return changed


def migrate_fixed_default_names(cur,catalog):
    from .presets import fixed_name_ids
    changed=0
    for identity in sorted(fixed_name_ids(catalog)):
        token='@SDGO:'+str(identity)
        cur.execute('''INSERT dbo.sdgo_OperatorFixedNameBackup
            SELECT U.UserOperatorNumber,U.AccountNumber,U.OperatorNumber,U.OperatorNickname,U.IsSetName
            FROM dbo.tbl_UserOperators U WHERE U.OperatorNumber=? AND ISNULL(U.Forbidden,0)=0
            AND (U.IsSetName<>1 OR U.OperatorNickname COLLATE Latin1_General_100_BIN2<>?)
            AND NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorFixedNameBackup B WHERE B.UserOperatorNumber=U.UserOperatorNumber)''',identity,token)
        cur.execute('''UPDATE dbo.tbl_UserOperators SET OperatorNickname=?,IsSetName=1
            WHERE OperatorNumber=? AND ISNULL(Forbidden,0)=0
            AND (IsSetName<>1 OR OperatorNickname COLLATE Latin1_General_100_BIN2<>?)''',token,identity,token)
        changed+=max(0,cur.rowcount)
    return changed


def install_fixed_name_guard(cur):
    name='dbo.sdgo_OperatorFixedNames'
    row=cur.execute('SELECT definition FROM sys.sql_modules WHERE object_id=OBJECT_ID(?)',name).fetchone()
    text=(MODULE_DIR/'server/naming.sql').read_text(encoding='utf-8')
    if row:
        if '-- SDGO_OPERATOR_FIXED_NAMES_V1' not in row[0]:
            raise ValueError('固定副官命名触发器已被其他内容占用，未覆盖')
        text=re.sub(r'^CREATE TRIGGER','ALTER TRIGGER',text,count=1)
    cur.execute(text)


def apply_default_mission_parts(cur,catalog):
    from .presets import fixed_name_ids
    enabled=fixed_name_ids(catalog)
    backups={r[0]:r[1] for r in cur.execute('SELECT OperatorID,OriginalGParts FROM dbo.sdgo_OperatorMissionPartsBackup')}
    for identity,original in backups.items():
        if identity not in enabled:
            cur.execute('UPDATE dbo.tbl_OperatorData SET GParts=? WHERE OperatorID=?',original,identity)
    changed=0
    for identity in sorted(enabled):
        row=cur.execute('SELECT GParts FROM dbo.tbl_OperatorData WHERE OperatorID=?',identity).fetchone()
        if row is None:raise ValueError('默认副官任务部件登记不存在：'+str(identity))
        if identity not in backups:
            cur.execute('INSERT dbo.sdgo_OperatorMissionPartsBackup VALUES(?,?)',identity,row[0])
        if row[0]!=1:
            cur.execute('UPDATE dbo.tbl_OperatorData SET GParts=1 WHERE OperatorID=?',identity)
            changed+=1
    return changed


def migrate_default_entitlements(cur, catalog):
    """Replace old default-officer entitlements in place; equipped IDs stay valid."""
    if catalog.get('default_pack_version') != 1:return 0
    from .presets import NAMES
    changed=0
    enabled={o['id'] for o in catalog['operators'] if o['managed'] and o['enabled'] and o['delivery']=='all_players'}
    for identity in NAMES:
        if identity not in enabled:continue
        cur.execute('''INSERT dbo.sdgo_OperatorLegacyEntitlementBackup
            SELECT U.UserOperatorNumber,U.AccountNumber,U.OperatorNumber,U.OPType,U.RunOut,U.DateEnd
            FROM dbo.tbl_UserOperators U WHERE U.OperatorNumber=? AND ISNULL(U.Forbidden,0)=0
            AND (U.OPType<>0 OR U.RunOut<>0 OR U.DateEnd IS NOT NULL)
            AND NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorLegacyEntitlementBackup B WHERE B.UserOperatorNumber=U.UserOperatorNumber)''',identity)
        cur.execute('''UPDATE dbo.tbl_UserOperators SET OPType=0,RunOut=0,DateEnd=NULL
            WHERE OperatorNumber=? AND ISNULL(Forbidden,0)=0
            AND (OPType<>0 OR RunOut<>0 OR DateEnd IS NOT NULL)''',identity)
        changed+=max(0,cur.rowcount)
    return changed

_ROOT = Path(__file__).resolve().parents[3]
MODULE_DIR = Path(__file__).with_name('resources')
WRAPPED = ("sp_OperatorDrop", "sp_GetUserOperatorPage", "sp_GetPilotOperator")
MARKER = "-- SDGO_OPERATOR_CATALOG_V1"


def read_ini(server_ini):
    parser = configparser.ConfigParser(interpolation=None)
    raw=Path(server_ini).read_bytes()
    if raw.startswith((b'\xff\xfe',b'\xfe\xff')):
        text=raw.decode('utf-16')
    else:
        try:
            text=raw.decode('utf-8-sig')
        except UnicodeDecodeError:
            text=raw.decode('mbcs')
    parser.read_string(text)
    return parser


def connect(server_ini):
    import pyodbc
    parser=read_ini(server_ini)
    db = parser["DB Game"]
    drivers = [d for d in pyodbc.drivers() if "SQL Server" in d]
    if not drivers:
        raise ValueError("本机缺少 SQL Server ODBC 驱动")
    def escape(value):
        return "{" + str(value).replace("}", "}}") + "}"
    connection = (f"DRIVER={escape(drivers[-1])};SERVER={escape(db['DBServerName'])};"
                  f"DATABASE={escape(db['DBName'])};UID={escape(db['ID'])};PWD={escape(db['PWD'])};")
    try:
        result = pyodbc.connect(connection, timeout=5, autocommit=True)
        result.timeout = 30
        return result
    except pyodbc.Error as error:
        raise ValueError(f"服务端连接失败，SQLSTATE={error.args[0]}") from None


def source_hash(text):
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def alter(text):
    result, count = re.subn(r"(?im)^\s*CREATE\s+(PROC(?:EDURE)?)\b", r"ALTER \1", text, count=1)
    if count != 1:
        raise ValueError("原生过程定义的 CREATE 入口无法确认")
    return result


def wrap(name, original):
    if MARKER in original:
        raise ValueError("不能重复包装副官过程")
    if name == "sp_OperatorDrop":
        anchor = r"(?im)^\s*DECLARE\s+@v_Probability\s+INT\s*;?\s*$"
        injected = f"""
{MARKER}
IF EXISTS(SELECT 1 FROM dbo.sdgo_OperatorProfiles WHERE OperatorID=@OperatorNumber AND Enabled=1)
BEGIN
    EXEC dbo.sdgo_OperatorManagedDrop @v_AccountID,@v_UserOperatorNumber,
        @v_DropCategoryID OUTPUT,@v_Amount OUTPUT,@v_RetVal OUTPUT;
    RETURN;
END;
"""
        matches = list(re.finditer(anchor, original))
        if len(matches) != 1 or "@OperatorNumber" not in original[:matches[0].start()]:
            raise ValueError("原生副官掉落过程版本不匹配，未做改动")
        # Injection is after the native ownership/DropType checks and native
        # prelude, preserving existing bonus logic exactly once.
        result = original[:matches[0].start()] + injected + original[matches[0].start():]
    elif name in ("sp_GetUserOperatorPage", "sp_GetPilotOperator"):
        anchor = r"(?im)^[ \t]*SET[ \t]+NOCOUNT[ \t]+ON[ \t]*;?"
        matches = list(re.finditer(anchor, original))
        if len(matches) != 1 or not re.search(r"@v_AccountNum\s+INT", original, re.I):
            raise ValueError("原生副官读取过程版本不匹配，未做改动")
        point = matches[0].end()
        result = original[:point] + f"\n{MARKER}\nEXEC dbo.sdgo_EnsureFreeOperators @v_AccountNum;\n" + original[point:]
    else:
        raise ValueError("不允许包装此过程")
    return alter(result)


def definition(cur, name):
    row = cur.execute("SELECT definition FROM sys.sql_modules WHERE object_id=OBJECT_ID(?)", "dbo." + name).fetchone()
    if row is None or row[0] is None:
        raise ValueError(f"服务端缺少原生过程 {name}")
    return row[0]


def install_procedures(cur):
    for file in ("random.sql", "drop.sql", "free.sql"):
        text = (MODULE_DIR / "server" / file).read_text(encoding="utf-8")
        match = re.search(r"CREATE\s+PROCEDURE\s+dbo\.([A-Za-z0-9_]+)", text, re.I)
        name = match[1]
        if cur.execute("SELECT OBJECT_ID(?)", "dbo." + name).fetchone()[0] is not None:
            text = alter(text)
        cur.execute(text)
    for name in WRAPPED:
        current = definition(cur, name)
        backup = cur.execute("SELECT OriginalDefinition,InstalledHash FROM dbo.sdgo_OperatorProcedureBackup WHERE ProcedureName=?", name).fetchone()
        if backup:
            original, installed_hash = backup
            if source_hash(current) != installed_hash:
                raise ValueError(f"{name} 已被其他修改改变，未覆盖")
        else:
            original = current
        cur.execute(wrap(name, original))
        installed = definition(cur, name)
        if backup:
            cur.execute("UPDATE dbo.sdgo_OperatorProcedureBackup SET InstalledHash=? WHERE ProcedureName=?", source_hash(installed), name)
        else:
            cur.execute("INSERT dbo.sdgo_OperatorProcedureBackup VALUES(?,?,?)", name, original, source_hash(installed))


def apply_catalog(catalog, server_ini):
    validate(catalog)
    cn = connect(server_ini)
    try:
        cur = cn.cursor()
        cur.execute("SET XACT_ABORT ON; BEGIN TRANSACTION")
        # Serialize only this module's changes; no unrelated configuration is reset.
        cur.execute("DECLARE @R INT; EXEC @R=sys.sp_getapplock @Resource=N'SDGO:operator-catalog',@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000; SELECT @R")
        if cur.fetchone()[0] < 0:
            raise ValueError("另一项副官更新正在进行，请稍后重试")
        cur.execute((MODULE_DIR / "server" / "schema.sql").read_text(encoding="utf-8"))
        validate_pools(cur,catalog)
        managed = [o for o in catalog["operators"] if o["managed"]]
        cur.execute("""UPDATE O SET DropType=CASE WHEN B.WasPresent=1 THEN B.OriginalDropType ELSE 0 END
                       FROM dbo.tbl_OperatorData O JOIN dbo.sdgo_OperatorDataBackup B ON O.OperatorID=B.OperatorID""")
        for op in managed:
            operator_id, category_id = op["id"], op["category_id"]
            exists = cur.execute("SELECT RefIndex FROM dbo.tbl_ItemCategory WHERE CatalogueID=?", category_id).fetchone()
            owned = cur.execute("SELECT OperatorID,OwnedCategory FROM dbo.sdgo_OperatorItemRegistry WHERE CategoryID=?", category_id).fetchone()
            native_item = next((x for x in catalog["items"] if x["id"] == category_id), None)
            if exists and (exists[0] != operator_id or (not owned and native_item is None)):
                raise ValueError(f"物品登记编号 {category_id} 被其他内容占用")
            if owned and owned[0] != operator_id:
                raise ValueError("物品登记归属不一致")
            if not exists:
                cur.execute("INSERT dbo.tbl_ItemCategory(CatalogueID,Name,RefIndex,TimeLimit,Option1,Option2) VALUES(?,?,?,0,0,0)",
                            category_id, f"SDGOop{operator_id}", operator_id)
            if not owned:
                cur.execute("INSERT dbo.sdgo_OperatorItemRegistry VALUES(?,?,?)", category_id, operator_id, 0 if exists else 1)
            backup = cur.execute("SELECT WasPresent,OriginalDropType FROM dbo.sdgo_OperatorDataBackup WHERE OperatorID=?", operator_id).fetchone()
            original = cur.execute("SELECT DropType FROM dbo.tbl_OperatorData WHERE OperatorID=?", operator_id).fetchone()
            if not backup:
                cur.execute("INSERT dbo.sdgo_OperatorDataBackup VALUES(?,?,?)", operator_id, int(original is not None), original[0] if original else None)
                backup = (int(original is not None), original[0] if original else None)
            if original is None:
                # Clone server skill/voice policy from a registered native template.
                columns = ["OperatorID", "OperatorName", "TeamWin", "TeamLose", "BattleResult", "ChangeSp",
                           "ServiceType", "AddOn1", "AddOn2", "AddOn3", "AddOn4", "DropType", "GParts"]
                row = cur.execute("SELECT " + ",".join(columns) + " FROM dbo.tbl_OperatorData WHERE OperatorID=?", op["template_id"]).fetchone()
                if row is None:
                    raise ValueError("服务端副官技能模板不存在")
                values = list(row)
                values[0], values[1] = operator_id, f"SDGOop{operator_id}"
                cur.execute("INSERT dbo.tbl_OperatorData(" + ",".join(columns) + ") VALUES(" + ",".join("?" for _ in columns) + ")", *values)
            drop_type = 1 if op["enabled"] else (backup[1] if backup[0] else 0)
            cur.execute("UPDATE dbo.tbl_OperatorData SET DropType=? WHERE OperatorID=?", drop_type, operator_id)
        cur.execute("UPDATE dbo.sdgo_OperatorProfiles SET Enabled=0")
        rev = revision(catalog)
        from .presets import fixed_name_ids
        fixed_names=fixed_name_ids(catalog)
        for op in managed:
            row = cur.execute("SELECT 1 FROM dbo.sdgo_OperatorProfiles WHERE OperatorID=?", op["id"]).fetchone()
            values = (op["category_id"], int(op["enabled"]), op["delivery"], op["name"], rev, int(op['id'] in fixed_names), op["id"])
            if row:
                cur.execute("UPDATE dbo.sdgo_OperatorProfiles SET CategoryID=?,Enabled=?,DeliveryMode=?,DisplayName=?,Revision=?,FixedName=? WHERE OperatorID=?", *values)
            else:
                cur.execute("INSERT dbo.sdgo_OperatorProfiles(CategoryID,Enabled,DeliveryMode,DisplayName,Revision,FixedName,OperatorID) VALUES(?,?,?,?,?,?,?)", *values)
            cur.execute("DELETE FROM dbo.sdgo_OperatorRuleItems WHERE OperatorID=?", op["id"])
            cur.execute("DELETE FROM dbo.sdgo_OperatorRules WHERE OperatorID=?", op["id"])
            cumulative = 0
            for index, rule in enumerate(op["drops"]):
                if not cur.execute("SELECT 1 FROM dbo.tbl_ItemCategory WHERE CatalogueID=?", rule["category_id"]).fetchone():
                    raise ValueError("掉落物品在服务端不存在")
                cur.execute("INSERT dbo.sdgo_OperatorRules VALUES(?,?,?,?,?,?)", op["id"], index,
                            rule["category_id"], rule["chance_bps"], rule["min_amount"], rule["max_amount"])
                if 'pool' in rule:
                    for item_index,category_id in enumerate(catalog['drop_pools'][rule['pool']]['category_ids']):
                        cur.execute('INSERT dbo.sdgo_OperatorRuleItems VALUES(?,?,?,?)',op['id'],index,item_index,category_id)
                cumulative += rule["chance_bps"]
            if cumulative > 10000:
                raise ValueError("概率合计超过100%")
        install_procedures(cur)
        legacy_aliases=migrate_default_aliases(cur,catalog)
        entitlements=migrate_default_entitlements(cur,catalog)
        fixed_names_changed=migrate_fixed_default_names(cur,catalog)
        install_fixed_name_guard(cur)
        mission_parts_changed=apply_default_mission_parts(cur,catalog)
        cur.execute("COMMIT TRANSACTION")
        return {"revision": rev, "managed_operators": len(managed), "storefront_changes": False,
                "free_grant": "on normal native operator login/page reads", "player_nicknames_changed": bool(legacy_aliases or fixed_names_changed),
                "non_default_nicknames_changed":False,
                "legacy_script_aliases_cleared":legacy_aliases,
                "existing_default_entitlements_upgraded":entitlements,
                "fixed_default_names_applied":fixed_names_changed,
                "default_mission_parts_enabled":mission_parts_changed}
    except Exception:
        try:
            cn.cursor().execute("IF XACT_STATE()<>0 ROLLBACK TRANSACTION")
        except Exception:
            pass
        raise
    finally:
        cn.close()
