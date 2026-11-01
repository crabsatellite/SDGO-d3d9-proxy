CREATE TRIGGER dbo.sdgo_collection_capture_drop_items
ON dbo.tbl_UserDropItemBox AFTER INSERT,UPDATE,DELETE
AS
BEGIN
    SET NOCOUNT ON;
    IF NOT EXISTS(SELECT 1 FROM inserted i JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=i.ItemCategory WHERE c.CatalogueID/100000=11)
       AND NOT EXISTS(SELECT 1 FROM deleted d JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=d.ItemCategory WHERE c.CatalogueID/100000=11)
        RETURN;
    DECLARE @tx bigint,@dummy int;
    SELECT @tx=transaction_id FROM sys.dm_tran_current_transaction;
    IF @tx IS NULL RAISERROR('Collection transaction identity unavailable',16,1);
    -- Gift rows are locked before inventory/history, as in draw and claim.
    SELECT @dummy=COUNT(*) FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
    WHERE AccountNumber IN(SELECT AccountNumber FROM inserted UNION SELECT AccountNumber FROM deleted);
    INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source)
    SELECT d.AccountNumber,c.RefIndex,0,'prior_unit_gift'
    FROM deleted d JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=d.ItemCategory
    WHERE c.CatalogueID/100000=11 AND c.RefIndex>0
      AND NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WITH(UPDLOCK,HOLDLOCK)
                     WHERE s.AccountNumber=d.AccountNumber AND s.UnitID=c.RefIndex)
    GROUP BY d.AccountNumber,c.RefIndex;
    INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source)
    SELECT i.AccountNumber,c.RefIndex,@tx,'unit_gift_acquisition'
    FROM inserted i JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=i.ItemCategory
    WHERE c.CatalogueID/100000=11 AND c.RefIndex>0
      AND NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WITH(UPDLOCK,HOLDLOCK)
                     WHERE s.AccountNumber=i.AccountNumber AND s.UnitID=c.RefIndex)
    GROUP BY i.AccountNumber,c.RefIndex;
    DELETE s FROM sdgo_collection_capsule.Seen s
    WHERE s.FirstTransactionID=@tx
      AND EXISTS(SELECT 1 FROM deleted d JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=d.ItemCategory
                  WHERE c.CatalogueID/100000=11 AND d.AccountNumber=s.AccountNumber AND c.RefIndex=s.UnitID)
      AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits u WHERE u.AccountNumber=s.AccountNumber AND u.UnitNumber=s.UnitID)
      AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserDropItemBox b JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=b.ItemCategory
                      WHERE c.CatalogueID/100000=11 AND b.AccountNumber=s.AccountNumber AND c.RefIndex=s.UnitID);
END
