CREATE TRIGGER dbo.sdgo_collection_capture_profiles
ON dbo.tbl_Users AFTER INSERT
AS
BEGIN
    SET NOCOUNT ON;
    INSERT sdgo_collection_capsule.Coverage(AccountNumber,Complete,Source)
    SELECT DISTINCT i.AccountNumber,1,'new_profile'
    FROM inserted i
    WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Coverage c WITH(UPDLOCK,HOLDLOCK)
                     WHERE c.AccountNumber=i.AccountNumber);
END
