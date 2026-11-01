CREATE TRIGGER dbo.sdgo_collection_capture_units
ON dbo.tbl_UserUnits AFTER INSERT,UPDATE,DELETE
AS
BEGIN
    SET NOCOUNT ON;
    DECLARE @tx bigint;
    SELECT @tx=transaction_id FROM sys.dm_tran_current_transaction;
    IF @tx IS NULL RAISERROR('Collection transaction identity unavailable',16,1);

    -- A deleted row not previously recorded was owned before this statement.
    INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source)
    SELECT d.AccountNumber,d.UnitNumber,0,'prior_unit'
    FROM deleted d
    WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WITH(UPDLOCK,HOLDLOCK)
                     WHERE s.AccountNumber=d.AccountNumber AND s.UnitID=d.UnitNumber)
    GROUP BY d.AccountNumber,d.UnitNumber;

    INSERT sdgo_collection_capsule.Seen(AccountNumber,UnitID,FirstTransactionID,Source)
    SELECT i.AccountNumber,i.UnitNumber,@tx,'unit_acquisition'
    FROM inserted i
    WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WITH(UPDLOCK,HOLDLOCK)
                     WHERE s.AccountNumber=i.AccountNumber AND s.UnitID=i.UnitNumber)
    GROUP BY i.AccountNumber,i.UnitNumber;

    -- Do not count an uncommitted bulk gift removed by new_account_profile as
    -- something the player received. Previously committed ownership stays seen.
    DELETE s FROM sdgo_collection_capsule.Seen s
    JOIN (SELECT DISTINCT AccountNumber,UnitNumber FROM deleted) d
      ON d.AccountNumber=s.AccountNumber AND d.UnitNumber=s.UnitID
    WHERE s.FirstTransactionID=@tx
      AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits u
                     WHERE u.AccountNumber=s.AccountNumber AND u.UnitNumber=s.UnitID);
END
