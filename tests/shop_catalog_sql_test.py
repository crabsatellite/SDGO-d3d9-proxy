"""Explicit opt-in integration test against NEW isolated SQL databases only.

Reads native definitions/configuration from the supplied host. Creates a unique
sdgo_shop_test_* database/login and synthetic balances; never writes the native
game databases or reads real account/inventory/history tables. Keeps receipts.
"""
import argparse
import configparser
import datetime as dt
import hashlib
import json
from pathlib import Path
import re
import secrets
import subprocess
import sys


def ident(value):
    return "[" + value.replace("]", "]]") + "]"


def literal(value):
    return "N'" + value.replace("'", "''") + "'"


def connection(cfg, role, *, database=None, login=None, password=None):
    import pyodbc
    section = cfg[role]
    def esc(v): return "{" + str(v).replace("}", "}}") + "}"
    text = ("DRIVER={SQL Server};SERVER=" + esc(section["DBServerName"]) + ";DATABASE=" + esc(database or section["DBName"])
            + ";UID=" + esc(login or section["ID"]) + ";PWD=" + esc(password or section["PWD"]) + ";")
    cn = pyodbc.connect(text, timeout=5, autocommit=True)
    cn.timeout = 30
    return cn


def rows(cur):
    keys = [d[0] for d in cur.description]
    return [dict(zip(keys, row)) for row in cur.fetchall()]


def results(cur):
    tables = []
    while True:
        if cur.description:
            tables.append(rows(cur))
        if not cur.nextset():
            return tables


def create_table(source, target, name):
    source.execute("""SELECT c.name,t.name AS type_name,c.max_length,c.precision,c.scale,
        c.is_nullable,c.is_identity,dc.definition FROM sys.columns c
        JOIN sys.types t ON c.user_type_id=t.user_type_id
        LEFT JOIN sys.default_constraints dc ON dc.object_id=c.default_object_id
        WHERE c.object_id=OBJECT_ID(?) ORDER BY c.column_id""", "dbo." + name)
    columns = rows(source)
    definitions = []
    for c in columns:
        kind = c["type_name"]
        if kind.lower() == "itemid": kind = "char"
        if kind in ["char", "varchar", "binary", "varbinary"]:
            kind += "(" + ("max" if c["max_length"] == -1 else str(c["max_length"])) + ")"
        elif kind in ["nchar", "nvarchar"]:
            kind += "(" + ("max" if c["max_length"] == -1 else str(c["max_length"] // 2)) + ")"
        elif kind in ["decimal", "numeric"]:
            kind += f"({c['precision']},{c['scale']})"
        definition = ident(c["name"]) + " " + kind
        if c["is_identity"]: definition += " IDENTITY(1,1)"
        definition += " NULL" if c["is_nullable"] else " NOT NULL"
        if c["definition"] is not None: definition += " DEFAULT " + c["definition"]
        definitions.append(definition)
    if not definitions:
        raise ValueError("Native table metadata missing: " + name)
    target.execute("CREATE TABLE dbo." + ident(name) + "(" + ",".join(definitions) + ")")
    source.execute("SELECT * FROM dbo." + ident(name))
    data = source.fetchall()
    names = [d[0] for d in source.description]
    if any(c["is_identity"] for c in columns): target.execute("SET IDENTITY_INSERT dbo." + ident(name) + " ON")
    sql = "INSERT dbo." + ident(name) + "(" + ",".join(map(ident, names)) + ") VALUES(" + ",".join("?" for _ in names) + ")"
    target.executemany(sql, data)
    if any(c["is_identity"] for c in columns): target.execute("SET IDENTITY_INSERT dbo." + ident(name) + " OFF")
    return len(data)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--server-ini", type=Path, required=True)
    p.add_argument("--exe", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--dll", type=Path, help="Optional actual shop_catalog DLL ABI harness")
    p.add_argument("--package-root", type=Path, help="Optional extracted Catalog package install/retire check")
    args = p.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(args.server_ini, encoding="utf-8-sig")
    stamp = dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    database = "sdgo_shop_test_" + stamp + "_" + secrets.token_hex(3)
    assert re.fullmatch(r"sdgo_shop_test_[A-Za-z0-9_]+", database)
    password = secrets.token_urlsafe(24) + "Aa1!"
    login = database
    # Only identifiers with the test prefix can reach CREATE DATABASE/LOGIN.
    admin = connection(cfg, "DB StoreBilling", database="master")
    a = admin.cursor()
    a.execute("CREATE DATABASE " + ident(database))
    a.execute("CREATE LOGIN " + ident(login) + " WITH PASSWORD=" + literal(password) + ", DEFAULT_DATABASE=" + ident(database))
    a.execute("USE " + ident(database))
    a.execute("CREATE USER " + ident(login) + " FOR LOGIN " + ident(login))
    a.execute("ALTER ROLE db_owner ADD MEMBER " + ident(login))
    target = connection(cfg, "DB StoreBilling", database=database, login=login, password=password)
    t = target.cursor()
    native = connection(cfg, "DB StoreBilling")
    source = native.cursor()
    copied = {}
    for name in ["tbls_StoreItems", "tbls_SubItems", "tbls_StoreItemsPlus"]:
        copied[name] = create_table(source, t, name)
    t.execute("SELECT * FROM dbo.tbls_StoreItems ORDER BY f_idx")
    baseline_native_products = json.dumps(rows(t), ensure_ascii=False, default=str, sort_keys=True)
    t.execute("CREATE TYPE dbo.ItemID FROM char(6) NOT NULL")
    procedures = {}
    for name in ["sp_StoreItemsList", "sp_StoreSubItemsList", "sp_SubItemsDetail", "sp_GetItemBillingCode", "sp_StoreItemInfo", "sp_ShopDisplayChk", "sp_PocketGP_IO", "sp_PocketGC_IO"]:
        source.execute("SELECT OBJECT_DEFINITION(OBJECT_ID(?))", "dbo." + name)
        definition = source.fetchone()[0]
        if not definition: raise ValueError("Native procedure definition missing: " + name)
        t.execute(definition)
        procedures[name] = hashlib.sha256(definition.encode("utf-8")).hexdigest()
    game = connection(cfg, "DB Game")
    g = game.cursor()
    t.execute("CREATE TABLE dbo.tbl_ItemCategory(CatalogueID int NOT NULL, Name nvarchar(100),RefIndex int,TimeLimit tinyint)")
    g.execute("SELECT CatalogueID,Name,RefIndex,TimeLimit FROM dbo.tbl_ItemCategory")
    t.executemany("INSERT dbo.tbl_ItemCategory VALUES(?,?,?,?)", g.fetchall())
    t.execute("CREATE TABLE dbo.tbl_Units(UnitNumber int NOT NULL,UnitGradeNumber tinyint,MasteredWeaponType tinyint,SallyReady tinyint)")
    g.execute("SELECT UnitNumber,UnitGradeNumber,MasteredWeaponType,SallyReady FROM dbo.tbl_Units")
    t.executemany("INSERT dbo.tbl_Units VALUES(?,?,?,?)", g.fetchall())
    # New synthetic account only, no original balances or account rows copied.
    t.execute("CREATE TABLE dbo.tbls_AccountPocket(f_idx int PRIMARY KEY,f_GP int,f_GC int)")
    t.execute("INSERT dbo.tbls_AccountPocket VALUES(900000001,20000,30000)")
    source.execute("SELECT OBJECT_DEFINITION(OBJECT_ID('dbo.sp_PocketGP_IO'))")
    gp = source.fetchone()[0]
    names = set(re.findall(r"tbls_LogGPInOut[0-9]*", gp, re.I))
    for name in names:
        t.execute("CREATE TABLE dbo." + ident(name) + "(f_idx int,f_GP int,f_from smallint,f_etc smallint,f_retGP int)")
    t.execute("CREATE TABLE dbo.tbls_LogGCInOut(f_idx int,f_GC int,f_from smallint,f_productID int,f_retGC int)")
    native_ini = args.output / "private-test-server.ini"
    private = configparser.ConfigParser(interpolation=None)
    for section in ["DB Game", "DB StoreBilling"]:
        private[section] = {"DBServerName": cfg["DB StoreBilling"]["DBServerName"], "DBName": database, "ID": login, "PWD": password}
    with native_ini.open("w", encoding="utf-16") as f: private.write(f)
    catalog = args.output / "catalog.ini"
    products = [dict(key="lease", product_id=1019001, item_id="190001", template_id=1010001,
                    category_id=1110549, price=9900, currency="MB", validity=7, name="中文租赁商品", label="租赁 7 天", enabled=True, retired=False),
                dict(key="permanent", product_id=1019002, item_id="190002", template_id=1010001,
                    category_id=1120022, price=500, currency="GB", validity=1, name="O'Hara'; SELECT 1--", label="永久机体", enabled=True, retired=False)]
    sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
    import shop_catalog as model
    data = {"schema_version": 1, "catalog_id": "integration", "products": products}
    receipt = {"database": database, "scope": "new isolated configuration database and synthetic account only", "copied_config_rows": copied,
               "native_procedure_sha256": procedures, "checks": []}
    def run(expected=True):
        model.compile_catalog(data, catalog)
        result = subprocess.run([str(args.exe.resolve()), "--host-ini", str(native_ini.resolve()), "--catalog", str(catalog.resolve()), "--output", str(args.output.resolve())],
                                capture_output=True, text=True, encoding="utf-8", timeout=45)
        if (result.returncode == 0) != expected:
            raise AssertionError(result.stdout + result.stderr)
        if password in result.stdout + result.stderr: raise AssertionError("Credential leak")
        return result
    def check(value, name):
        if not value: raise AssertionError(name)
        receipt["checks"].append(name)
        print("PASS", name, flush=True)
    run()
    t.execute("SELECT p.f_productid,p.f_payment,p.f_price,s.f_itemtblidx,s.f_payment,s.f_price,s.f_validity,s.f_itemNm FROM dbo.tbls_StoreItems p JOIN dbo.tbls_SubItems s ON p.f_itemid=s.f_itemid WHERE p.f_productid IN (1019001,1019002) ORDER BY p.f_productid")
    managed = rows(t)
    check(len(managed) == 2 and managed[0]["f_validity"] == 7, "actual_32bit_native_engine_applies_definitions")
    check(managed[0]["f_itemNm"] == "租赁 7 天", "Unicode_survives_real_SQL_storage")
    t.execute("SELECT f_imgIitemid FROM dbo.tbls_SubItems WHERE f_reProductID IN (1019001,1019002)")
    check(all(str(r[0]).strip() == "0" for r in t.fetchall()), "new_options_use_native_category_image_default")
    ids = [r["f_itemtblidx"] for r in managed]
    run()
    t.execute("SELECT OptionId FROM dbo.sdgo_ShopCatalog ORDER BY ProductId")
    check([r[0] for r in t.fetchall()] == ids, "idempotent_apply_preserves_native_option_ids")
    t.execute("DECLARE @total int;EXEC dbo.sp_StoreSubItemsList 1,1,8,-1,-1,1,'',@total OUTPUT;")
    listed = results(t)
    check(any(any(r.get("f_productid") == 1019002 and str(r.get("f_payment")).strip() == "1" for r in table) for table in listed), "native_GB_filter_returns_managed_product")
    t.execute("DECLARE @total int;EXEC dbo.sp_StoreSubItemsList 1,1,8,-1,-1,2,'',@total OUTPUT;")
    check(any(any(r.get("f_productid") == 1019001 and str(r.get("f_payment")).strip() == "2" for r in table) for table in results(t)), "native_MB_filter_returns_managed_product")
    for pid, expected_currency in [(1019001, "2"), (1019002, "1")]:
        t.execute(f"DECLARE @p int,@pkg char(1),@gift char(1),@n int;EXEC dbo.sp_GetItemBillingCode {pid},0,'2',@p OUTPUT,@pkg OUTPUT,@gift OUTPUT,@n OUTPUT;SELECT @p price;")
        billing = results(t)
        check(any(str(r.get("f_payment", "")).strip() == expected_currency for table in billing for r in table), f"native_billing_uses_correct_currency_{pid}")
    t.execute("DECLARE @r int,@b int;EXEC dbo.sp_PocketGP_IO 3,900000001,500,2,0,@r OUTPUT,@b OUTPUT;SELECT @r result,@b balance;")
    debit = results(t)[-1][0]
    check(debit["result"] == 0 and debit["balance"] == 19500, "native_GP_debit_changes_only_synthetic_GP")
    t.execute("SELECT f_GC FROM dbo.tbls_AccountPocket WHERE f_idx=900000001")
    check(t.fetchone()[0] == 30000, "GB_transaction_leaves_synthetic_MB_unchanged")
    t.execute("DECLARE @r int,@b int;EXEC dbo.sp_PocketGC_IO 3,900000001,9900,2,1019001,@r OUTPUT,@b OUTPUT;SELECT @r result,@b balance;")
    debit = results(t)[-1][0]
    check(debit["result"] == 0 and debit["balance"] == 20100, "native_GC_debit_changes_only_synthetic_GC")
    t.execute("SELECT f_GP FROM dbo.tbls_AccountPocket WHERE f_idx=900000001")
    check(t.fetchone()[0] == 19500, "MB_transaction_leaves_synthetic_GB_unchanged")
    products[0]["price"] = 1234
    run()
    t.execute("SELECT p.f_price,s.f_price FROM dbo.tbls_StoreItems p JOIN dbo.tbls_SubItems s ON p.f_itemid=s.f_itemid WHERE p.f_productid=1019001")
    check(tuple(t.fetchone()) == (1234,1234), "price_update_is_consistent_in_native_tables")
    t.execute("SELECT COUNT(*) FROM dbo.tbls_StoreItems")
    before = t.fetchone()[0]
    collision = dict(products[1]); collision.update(key="collision",product_id=1010001,item_id="199999")
    data["products"] = products + [collision]
    # The model catches self-template ids, so use another valid template for this
    # deliberate native id collision, leaving the production test engine to refuse.
    collision["template_id"] = 1010002
    products[0]["price"] = 4321
    result = run(False)
    t.execute("SELECT COUNT(*) FROM dbo.tbls_StoreItems")
    check(t.fetchone()[0] == before, "native_id_collision_rolls_back_entire_batch")
    t.execute("SELECT p.f_price,s.f_price FROM dbo.tbls_StoreItems p JOIN dbo.tbls_SubItems s ON p.f_itemid=s.f_itemid WHERE p.f_productid=1019001")
    check(tuple(t.fetchone()) == (1234,1234), "collision_rolls_back_earlier_product_price_update")
    check(not list(args.output.glob("*.pending")), "rejected_apply_removes_own_pending_profile")
    data["products"] = [products[1]]
    run()
    t.execute("SELECT f_display FROM dbo.tbls_StoreItems WHERE f_productid=1019001")
    check(str(t.fetchone()[0]).strip() == "0", "removed_definition_is_retired_without_erasing_history")
    t.execute("DECLARE @r int;EXEC dbo.sp_ShopDisplayChk 1019001,@r OUTPUT;SELECT @r result;")
    check(results(t)[-1][0]["result"] == 1, "native_purchase_visibility_rejects_retired_product")
    # Relative paths are accepted by the same engine used by the actual DLL.
    relative = subprocess.run([str(args.exe.resolve()), "--host-ini", native_ini.name, "--catalog", catalog.name, "--output", "."],
                              cwd=args.output.resolve(), capture_output=True, timeout=45)
    check(relative.returncode == 0, "runtime_accepts_relative_paths")
    # Emergency retirement only requires a catalog identity and billing access.
    # Product errors and an unavailable game DB must not trap a host in a sale.
    del private["DB Game"]
    billing_ini = args.output / "private-billing-only.ini"
    with billing_ini.open("w", encoding="utf-16") as f: private.write(f)
    catalog.write_text("[shop]\nSchema=1\nCatalogId=integration\nRevision=disable-test\n[product:broken]\nPrice=bad\n", encoding="utf-16")
    disabled = subprocess.run([str(args.exe.resolve()), "--host-ini", str(billing_ini.resolve()), "--catalog", str(catalog.resolve()),
                               "--output", str(args.output.resolve()), "--disable"], capture_output=True, timeout=45)
    check(disabled.returncode == 0, "disable_works_with_invalid_products_and_no_game_connection")
    t.execute("SELECT COUNT(*) FROM dbo.tbls_StoreItems p JOIN dbo.sdgo_ShopCatalog s ON s.ProductId=p.f_productid WHERE s.CatalogId=N'integration' AND p.f_display='1'")
    check(t.fetchone()[0] == 0, "disable_retires_all_owned_products")
    # Restore a valid profile so the separate DLL ABI harness can also run.
    model.compile_catalog(data, catalog)
    if args.dll:
        harness = args.exe.resolve().with_name("shop_catalog_mod_test.exe")
        dll = subprocess.run([str(harness), str(args.dll.resolve()), str(native_ini.resolve()), str(args.output.resolve())],
                             capture_output=True, timeout=45)
        check(dll.returncode == 0, "actual_DLL_registers_without_startup_application_and_rejects_unknown_host")
        t.execute("SELECT COUNT(*) FROM dbo.tbls_StoreItems p JOIN dbo.sdgo_ShopCatalog s ON s.ProductId=p.f_productid WHERE s.CatalogId=N'integration' AND p.f_display='1'")
        check(t.fetchone()[0] == 0, "host_startup_never_republishes_disabled_shop_offers")
    if args.package_root:
        # Inert World PE, real installer/helper and restricted test DB only.
        import shop_install_test as install_test
        install = args.output.resolve() / "installed-world"
        install_test.fixture(install / "WorldServer.exe", "Catalog")
        (install / "WorldServer.ini").write_bytes(native_ini.read_bytes())
        script = args.package_root.resolve() / "Install.ps1"
        installed = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                                    "-Target", str(install), "-ConfigFile", str(catalog.resolve())], capture_output=True, timeout=45)
        check(installed.returncode == 0, "catalog_package_installs_to_isolated_World_fixture")
        mod = install / "sdgo/mods/shop_catalog"
        applied = subprocess.run([str(mod / "shop_catalog_apply.exe"), "--host-ini", str(install / "WorldServer.ini"),
                                  "--catalog", str(mod / "catalog.ini"), "--output", str(mod)], capture_output=True, timeout=45)
        check(applied.returncode == 0, "packaged_native_helper_applies_using_host_owned_credentials")
        removed = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(script),
                                  "-Target", str(install), "-Uninstall"], capture_output=True, timeout=45)
        t.execute("SELECT COUNT(*) FROM dbo.tbls_StoreItems p JOIN dbo.sdgo_ShopCatalog s ON s.ProductId=p.f_productid WHERE s.CatalogId=N'integration' AND p.f_display='1'")
        check(removed.returncode == 0 and not mod.exists() and t.fetchone()[0] == 0,
              "catalog_uninstaller_retires_SQL_offers_and_retains_shared_framework")
    t.execute("SELECT p.* FROM dbo.tbls_StoreItems p WHERE NOT EXISTS(SELECT 1 FROM dbo.sdgo_ShopCatalog s WHERE s.ProductId=p.f_productid) ORDER BY p.f_idx")
    check(json.dumps(rows(t), ensure_ascii=False, default=str, sort_keys=True) == baseline_native_products, "unowned_native_product_rows_preserved_exactly")
    receipt["runtime_installed_in_production"] = False
    receipt["result"] = "PASS"
    (args.output / "result.json").write_text(json.dumps(receipt, ensure_ascii=False, indent=2), encoding="utf-8")
    print("ISOLATED SHOP SQL CHECKS PASSED", flush=True)
    game.close(); native.close(); target.close(); admin.close()


if __name__ == "__main__":
    main()
