#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sdgo::shop_catalog {
struct Product {
    std::wstring key;
    int id = 0;
    std::wstring sku;
    int templateId = 0;
    int category = 0;
    int price = 0;
    int payment = 1; // Native ordinary shop: 1 GB/GP, 2 MB/GC.
    int validity = 1;
    bool enabled = true;
    std::wstring name;
    std::wstring label;
    int timeLimit = -1;
    int search1 = 0;
    int search2 = 0;
};
struct Catalog {
    std::wstring id;
    std::wstring revision;
    std::vector<Product> products;
};
inline std::wstring Quote(const std::wstring& text)
{
    std::wstring result = L"N'";
    for (wchar_t c : text) {
        result += c;
        if (c == L'\'') result += L'\'';
    }
    return result + L"'";
}
inline std::wstring Identifier(const std::wstring& text)
{
    std::wstring result = L"[";
    for (wchar_t c : text) { result += c; if (c == L']') result += L']'; }
    return result + L"]";
}

// The caller owns the connection and commits only after reading the result and
// preparing the client Unicode profile. This batch does not change any native
// procedure, balance, inventory, log, or unowned product.
inline std::wstring ApplySql(const Catalog& catalog)
{
    std::wstring sql = LR"SQL(
SET NOCOUNT ON; SET XACT_ABORT ON;
BEGIN TRANSACTION;
DECLARE @lock int;
EXEC @lock=sys.sp_getapplock @Resource=N'sdgo.shop_catalog',@LockMode='Exclusive',@LockOwner='Session',@LockTimeout=5000;
IF @lock<0 BEGIN RAISERROR('Shop catalog is busy',16,1); RETURN; END;
IF OBJECT_ID(N'dbo.tbls_StoreItems',N'U') IS NULL OR OBJECT_ID(N'dbo.tbls_SubItems',N'U') IS NULL OR OBJECT_ID(N'dbo.tbls_StoreItemsPlus',N'U') IS NULL
BEGIN RAISERROR('Native shop schema missing',16,1); RETURN; END;
IF OBJECT_ID(N'dbo.sdgo_ShopCatalog',N'U') IS NULL
CREATE TABLE dbo.sdgo_ShopCatalog(
 CatalogId nvarchar(64) NOT NULL, ProductKey nvarchar(64) NOT NULL,
 ProductId int NOT NULL UNIQUE, ItemId char(6) NOT NULL UNIQUE, OptionId int NOT NULL UNIQUE,
 Revision nvarchar(64) NOT NULL, PRIMARY KEY(CatalogId,ProductKey));
DECLARE @owner nvarchar(64),@revision nvarchar(64),@key nvarchar(64),@pid int,@sku char(6),@template int,
 @category int,@price int,@payment char(1),@validity int,@visible char(1),@name nvarchar(50),@label nvarchar(50),
 @option int,@tab int,@mode int,@ref int,@kind int,@s1 int,@s2 int,@qty int,@prefix char(1);
)SQL";
    sql += L"SET @owner=" + Quote(catalog.id) + L";SET @revision=" + Quote(catalog.revision) + L";\n";
    for (const Product& p : catalog.products) {
        sql += L"SET @key=" + Quote(p.key) + L";SET @pid=" + std::to_wstring(p.id) +
            L";SET @sku=" + Quote(p.sku) + L";SET @template=" + std::to_wstring(p.templateId) +
            L";SET @category=" + std::to_wstring(p.category) + L";SET @price=" + std::to_wstring(p.price) +
            L";SET @payment=" + Quote(std::to_wstring(p.payment)) + L";SET @validity=" + std::to_wstring(p.validity) +
            L";SET @visible=" + Quote(p.enabled ? L"1" : L"0") + L";SET @name=" + Quote(p.name) + L";SET @label=" + Quote(p.label) + L";\n";
        sql += LR"SQL(
SET @option=NULL; SET @mode=NULL; SET @tab=NULL; SET @s1=0; SET @s2=0;
IF (SELECT COUNT(*) FROM dbo.tbls_StoreItems WHERE f_productid=@template AND ISNULL(f_package,'0')='0')<>1
BEGIN RAISERROR('Native product template is ambiguous or missing',16,1); RETURN; END;
SELECT @tab=f_depth1,@s1=ISNULL(f_search1,0),@s2=ISNULL(f_search2,0),@prefix=LEFT(f_itemid,1) FROM dbo.tbls_StoreItems WHERE f_productid=@template AND ISNULL(f_package,'0')='0';
IF @tab IS NULL OR @tab=20 OR @pid<=(@tab+100)*10000 OR @pid>=(@tab+101)*10000
BEGIN RAISERROR('Invalid native product template or tab range',16,1); RETURN; END;
IF LEFT(@sku,1)<>@prefix BEGIN RAISERROR('SKU kind does not match native template',16,1); RETURN; END;
)SQL";
        sql += L"SET @mode=" + std::to_wstring(p.timeLimit) + L";SET @kind=@category/100000;\n";
        sql += LR"SQL(
IF @mode IS NULL BEGIN RAISERROR('Item category is unknown to the game database',16,1); RETURN; END;
IF @kind=11 AND @mode=0 AND @validity<>1
BEGIN RAISERROR('Permanent unit validity must be one inventory item',16,1); RETURN; END;
)SQL";
        sql += L"IF NOT EXISTS(SELECT 1 FROM dbo.tbls_SubItems t JOIN dbo.tbls_StoreItems p ON p.f_itemid=t.f_itemid WHERE p.f_productid=@template AND t.f_categoryNumber/100000=@kind)\n";
        sql += LR"SQL(
BEGIN RAISERROR('Template uses a different native item kind',16,1); RETURN; END;
)SQL";
        sql += L"IF @kind=11 BEGIN SET @s1=" + std::to_wstring(p.search1) + L";SET @s2=" + std::to_wstring(p.search2) + L"; END;\n";
        sql += LR"SQL(
SELECT @option=OptionId FROM dbo.sdgo_ShopCatalog WHERE CatalogId=@owner AND ProductKey=@key AND ProductId=@pid AND ItemId=@sku;
IF @option IS NULL
BEGIN
 IF EXISTS(SELECT 1 FROM dbo.sdgo_ShopCatalog WHERE CatalogId=@owner AND ProductKey=@key)
  OR EXISTS(SELECT 1 FROM dbo.tbls_StoreItems WHERE f_productid=@pid OR f_itemid=@sku)
  OR EXISTS(SELECT 1 FROM dbo.tbls_SubItems WHERE f_itemid=@sku)
  OR EXISTS(SELECT 1 FROM dbo.sdgo_ShopCatalog WHERE ProductId=@pid OR ItemId=@sku)
 BEGIN RAISERROR('Shop ID collision or immutable identity changed',16,1); RETURN; END;
 INSERT dbo.tbls_StoreItems(f_productid,f_itemid,f_itemNm,f_subitemCnt,f_depth0,f_depth1,f_depth2,f_depth3,f_validity,f_price,f_DC,f_DCrate,f_payment,f_limitedAdition,f_quantity,f_package,f_display,f_search1,f_search2,f_limitedGift)
 VALUES(@pid,@sku,@name,1,3,@tab,CASE WHEN @kind=11 AND @mode=1 THEN 2 ELSE 1 END,0,@validity,@price,0,0,@payment,'2',99999999,'0',@visible,@s1,@s2,'1');
 INSERT dbo.tbls_SubItems(f_itemid,f_menu_id,f_itemNm,f_price,f_validity,f_payment,f_categoryNumber,f_display,f_index,f_reProductID,f_imgIitemid,f_regdate)
 VALUES(@sku,'',@label,@price,@validity,@payment,@category,@visible,'1',@pid,'0',GETDATE());
 SET @option=CONVERT(int,SCOPE_IDENTITY());
 INSERT dbo.sdgo_ShopCatalog(CatalogId,ProductKey,ProductId,ItemId,OptionId,Revision) VALUES(@owner,@key,@pid,@sku,@option,@revision);
END;
IF (SELECT COUNT(*) FROM dbo.tbls_StoreItems WHERE f_productid=@pid AND f_itemid=@sku)<>1
 OR (SELECT COUNT(*) FROM dbo.tbls_SubItems WHERE f_itemtblidx=@option AND f_itemid=@sku)<>1
 OR (SELECT COUNT(*) FROM dbo.tbls_SubItems WHERE f_itemid=@sku)<>1
BEGIN RAISERROR('Managed product identity has drifted',16,1); RETURN; END;
IF NOT EXISTS(SELECT 1 FROM dbo.tbls_StoreItemsPlus WHERE f_productid=@pid)
 INSERT dbo.tbls_StoreItemsPlus(f_productid,f_desc1,f_desc2,f_desc3,f_info1,f_info2,f_info3,f_info4,f_info5,f_info6,f_info7,f_info8,f_info9,f_info10,f_info11,f_regdate)
 VALUES(@pid,N'',N'',N'',0,0,0,0,0,0,0,0,0,0,0,GETDATE());
UPDATE dbo.tbls_StoreItems SET f_itemNm=@name,f_validity=@validity,f_price=@price,f_payment=@payment,f_display=@visible,
 f_depth2=CASE WHEN @kind=11 AND @mode=1 THEN 2 ELSE 1 END,f_search1=@s1,f_search2=@s2 WHERE f_productid=@pid AND f_itemid=@sku;
UPDATE dbo.tbls_SubItems SET f_itemNm=@label,f_price=@price,f_validity=@validity,f_payment=@payment,f_categoryNumber=@category,f_display=@visible,f_imgIitemid='0',f_reProductID=@pid
 WHERE f_itemtblidx=@option AND f_itemid=@sku;
UPDATE dbo.sdgo_ShopCatalog SET Revision=@revision WHERE CatalogId=@owner AND ProductKey=@key;
)SQL";
    }
    std::wstring keys = L"N''";
    for (const auto& p : catalog.products) keys += L"," + Quote(p.key);
    sql += L"UPDATE p SET f_display='0' FROM dbo.tbls_StoreItems p JOIN dbo.sdgo_ShopCatalog s ON s.ProductId=p.f_productid AND s.ItemId=p.f_itemid WHERE s.CatalogId=@owner AND s.ProductKey NOT IN(" + keys + L");\n";
    sql += L"UPDATE p SET f_display='0' FROM dbo.tbls_SubItems p JOIN dbo.sdgo_ShopCatalog s ON s.OptionId=p.f_itemtblidx AND s.ItemId=p.f_itemid WHERE s.CatalogId=@owner AND s.ProductKey NOT IN(" + keys + L");\n";
    sql += LR"SQL(
SELECT s.ProductId,s.OptionId,p.f_categoryNumber,p.f_validity,p.f_payment,p.f_price,p.f_itemNm
 FROM dbo.sdgo_ShopCatalog s JOIN dbo.tbls_SubItems p ON p.f_itemtblidx=s.OptionId AND p.f_itemid=s.ItemId
 WHERE s.CatalogId=@owner AND p.f_display='1' ORDER BY s.ProductId;
)SQL";
    return sql;
}
} // namespace sdgo::shop_catalog
