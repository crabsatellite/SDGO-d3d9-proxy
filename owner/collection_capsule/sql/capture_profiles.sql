CREATE TRIGGER dbo.sdgo_collection_capture_profiles
ON dbo.tbl_Users AFTER INSERT
AS
BEGIN
    SET NOCOUNT ON;
    INSERT sdgo_collection_capsule.Coverage(AccountNumber,Complete,Source)
    SELECT DISTINCT i.AccountNumber,
        CASE WHEN EXISTS(SELECT 1 FROM sdgo_collection_capsule.State st
                         WHERE st.Singleton=1 AND st.RegistrationBaseline=1
                           AND i.AccountNumber>st.LegacyAccountMax) THEN 1 ELSE 0 END,
        CASE WHEN EXISTS(SELECT 1 FROM sdgo_collection_capsule.State st
                         WHERE st.Singleton=1 AND st.RegistrationBaseline=1
                           AND i.AccountNumber>st.LegacyAccountMax) THEN 'new_registration' ELSE 'legacy_incomplete' END
    FROM inserted i
    WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Coverage c WITH(UPDLOCK,HOLDLOCK)
                     WHERE c.AccountNumber=i.AccountNumber);
END
