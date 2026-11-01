CREATE TRIGGER dbo.sdgo_collection_catalog_units
ON dbo.tbl_UserUnits AFTER INSERT,UPDATE,DELETE
AS
BEGIN
 SET NOCOUNT ON;
 DECLARE @tx bigint;
 SELECT @tx=transaction_id FROM sys.dm_tran_current_transaction;
 IF @tx IS NULL RAISERROR('Catalog transaction identity unavailable',16,1);
 -- A later delete keeps a committed unlock. Temporary grants removed inside
 -- their creation transaction never become a catalog unlock.
 INSERT sdgo_collection_capsule.CatalogUnlocks(AccountNumber,UnitID,PendingTransactionID)
 SELECT d.AccountNumber,d.UnitNumber,0 FROM deleted d
 WHERE d.TimeLimit=0 AND d.RunOut=0 AND NOT EXISTS(
   SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks c WITH(UPDLOCK,HOLDLOCK)
   WHERE c.AccountNumber=d.AccountNumber AND c.UnitID=d.UnitNumber)
 GROUP BY d.AccountNumber,d.UnitNumber;
 INSERT sdgo_collection_capsule.CatalogUnlocks(AccountNumber,UnitID,PendingTransactionID)
 SELECT i.AccountNumber,i.UnitNumber,@tx FROM inserted i
 WHERE i.TimeLimit=0 AND i.RunOut=0 AND NOT EXISTS(
   SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks c WITH(UPDLOCK,HOLDLOCK)
   WHERE c.AccountNumber=i.AccountNumber AND c.UnitID=i.UnitNumber)
 GROUP BY i.AccountNumber,i.UnitNumber;
 DELETE c FROM sdgo_collection_capsule.CatalogUnlocks c
 JOIN (SELECT DISTINCT AccountNumber,UnitNumber FROM deleted) d
 ON c.AccountNumber=d.AccountNumber AND c.UnitID=d.UnitNumber
 WHERE c.PendingTransactionID=@tx AND NOT EXISTS(
   SELECT 1 FROM dbo.tbl_UserUnits u WHERE u.AccountNumber=c.AccountNumber
     AND u.UnitNumber=c.UnitID AND u.TimeLimit=0 AND u.RunOut=0)
 AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserDropItemBox b JOIN dbo.tbl_ItemCategory item ON item.CatalogueID=b.ItemCategory
   WHERE b.AccountNumber=c.AccountNumber AND item.CatalogueID/100000=11 AND item.RefIndex=c.UnitID AND item.TimeLimit=0);
END
