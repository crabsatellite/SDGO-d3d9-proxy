-- Installation imports facts, not an assertion that old logs are exhaustive.
INSERT sdgo_collection_capsule.Coverage(AccountNumber,Complete,Source)
SELECT AccountNumber,0,'legacy_incomplete' FROM dbo.tbl_Users u
WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Coverage c WHERE c.AccountNumber=u.AccountNumber);

INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source)
SELECT AccountNumber,UnitNumber,0,'installed_inventory' FROM dbo.tbl_UserUnits u
WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WHERE s.AccountNumber=u.AccountNumber AND s.UnitID=u.UnitNumber)
GROUP BY AccountNumber,UnitNumber;

INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source)
SELECT AccountID,UnitID,0,'native_capsule_log' FROM dbo.tbl_CapsulePicked p
WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WHERE s.AccountNumber=p.AccountID AND s.UnitID=p.UnitID)
GROUP BY AccountID,UnitID;

DECLARE @historyTable sysname,@sql nvarchar(max);
INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source)
SELECT b.AccountNumber,c.RefIndex,0,'installed_unit_gift'
FROM dbo.tbl_UserDropItemBox b JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=b.ItemCategory
WHERE c.CatalogueID/100000=11 AND c.RefIndex>0
  AND NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WHERE s.AccountNumber=b.AccountNumber AND s.UnitID=c.RefIndex)
GROUP BY b.AccountNumber,c.RefIndex;
DECLARE history_tables CURSOR LOCAL FAST_FORWARD FOR
SELECT t.name FROM sys.tables t JOIN sys.schemas n ON t.schema_id=n.schema_id
WHERE n.name=N'dbo' AND (t.name LIKE N'tbl[_]UUDs%' OR t.name IN
      (N'tbl_UserUnitDeletes',N'tbl_UserUnitDeletesNew',N'tbl_UnitRecoveryLog'))
  AND COL_LENGTH(N'dbo.'+QUOTENAME(t.name),N'AccountNumber') IS NOT NULL
  AND COL_LENGTH(N'dbo.'+QUOTENAME(t.name),N'UnitNumber') IS NOT NULL;
OPEN history_tables;
FETCH NEXT FROM history_tables INTO @historyTable;
WHILE @@FETCH_STATUS=0
BEGIN
    SET @sql=N'INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source)
      SELECT AccountNumber,UnitNumber,0,''native_unit_history'' FROM dbo.'+QUOTENAME(@historyTable)+N' h
      WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WHERE s.AccountNumber=h.AccountNumber AND s.UnitID=h.UnitNumber)
      GROUP BY AccountNumber,UnitNumber;';
    EXEC sys.sp_executesql @sql;
    FETCH NEXT FROM history_tables INTO @historyTable;
END;
CLOSE history_tables;
DEALLOCATE history_tables;
