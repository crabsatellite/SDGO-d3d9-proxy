SET NOCOUNT ON;
SET XACT_ABORT ON;

IF OBJECT_ID(N'dbo.sp_PocketGC_IO', N'P') IS NULL
    THROW 51100, 'Native dbo.sp_PocketGC_IO is required', 1;

IF (SELECT COUNT(*) FROM sys.parameters
    WHERE object_id = OBJECT_ID(N'dbo.sp_PocketGC_IO') AND parameter_id > 0) <> 7
   OR NOT EXISTS (
       SELECT 1
       FROM sys.parameters p
       JOIN sys.types t ON t.user_type_id = p.user_type_id
       WHERE p.object_id = OBJECT_ID(N'dbo.sp_PocketGC_IO')
         AND p.parameter_id = 1 AND p.name = N'@type'
         AND t.name = N'smallint' AND p.is_output = 0)
   OR NOT EXISTS (
       SELECT 1
       FROM sys.parameters p
       JOIN sys.types t ON t.user_type_id = p.user_type_id
       WHERE p.object_id = OBJECT_ID(N'dbo.sp_PocketGC_IO')
         AND p.parameter_id = 2 AND p.name = N'@s_idx'
         AND t.name = N'int' AND p.is_output = 0)
   OR NOT EXISTS (
       SELECT 1
       FROM sys.parameters p
       JOIN sys.types t ON t.user_type_id = p.user_type_id
       WHERE p.object_id = OBJECT_ID(N'dbo.sp_PocketGC_IO')
         AND p.parameter_id = 3 AND p.name = N'@s_GC'
         AND t.name = N'int' AND p.is_output = 0)
   OR NOT EXISTS (
       SELECT 1
       FROM sys.parameters p
       JOIN sys.types t ON t.user_type_id = p.user_type_id
       WHERE p.object_id = OBJECT_ID(N'dbo.sp_PocketGC_IO')
         AND p.parameter_id = 4 AND p.name = N'@s_from'
         AND t.name = N'smallint' AND p.is_output = 0)
   OR NOT EXISTS (
       SELECT 1
       FROM sys.parameters p
       JOIN sys.types t ON t.user_type_id = p.user_type_id
       WHERE p.object_id = OBJECT_ID(N'dbo.sp_PocketGC_IO')
         AND p.parameter_id = 5 AND p.name = N'@s_productID'
         AND t.name = N'int' AND p.is_output = 0)
   OR NOT EXISTS (
       SELECT 1
       FROM sys.parameters p
       JOIN sys.types t ON t.user_type_id = p.user_type_id
       WHERE p.object_id = OBJECT_ID(N'dbo.sp_PocketGC_IO')
         AND p.parameter_id = 6 AND p.name = N'@retVal'
         AND t.name = N'int' AND p.is_output = 1)
   OR NOT EXISTS (
       SELECT 1
       FROM sys.parameters p
       JOIN sys.types t ON t.user_type_id = p.user_type_id
       WHERE p.object_id = OBJECT_ID(N'dbo.sp_PocketGC_IO')
         AND p.parameter_id = 7 AND p.name = N'@retGC'
         AND t.name = N'int' AND p.is_output = 1)
    THROW 51101, 'Native dbo.sp_PocketGC_IO signature is unsupported', 1;

IF OBJECT_ID(N'dbo.SdgoBattleRewardWalletMeta', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.SdgoBattleRewardWalletMeta (
        Singleton tinyint NOT NULL,
        ScopeId uniqueidentifier NOT NULL,
        SchemaVersion int NOT NULL,
        OriginServer nvarchar(128) NOT NULL,
        OriginDatabase sysname NOT NULL,
        CONSTRAINT PK_SdgoBattleRewardWalletMeta PRIMARY KEY (Singleton),
        CONSTRAINT CK_SdgoBattleRewardWalletMeta_Singleton CHECK (Singleton = 1)
    );
END;

IF COL_LENGTH(N'dbo.SdgoBattleRewardWalletMeta', N'Singleton') IS NULL
   OR COL_LENGTH(N'dbo.SdgoBattleRewardWalletMeta', N'ScopeId') IS NULL
   OR COL_LENGTH(N'dbo.SdgoBattleRewardWalletMeta', N'SchemaVersion') IS NULL
   OR COL_LENGTH(N'dbo.SdgoBattleRewardWalletMeta', N'OriginServer') IS NULL
   OR COL_LENGTH(N'dbo.SdgoBattleRewardWalletMeta', N'OriginDatabase') IS NULL
    THROW 51102, 'Conflicting dbo.SdgoBattleRewardWalletMeta table', 1;

IF NOT EXISTS (SELECT 1 FROM dbo.SdgoBattleRewardWalletMeta WHERE Singleton = 1)
BEGIN
    INSERT dbo.SdgoBattleRewardWalletMeta
        (Singleton, ScopeId, SchemaVersion, OriginServer, OriginDatabase)
    VALUES
        (1, NEWID(), 1, CONVERT(nvarchar(128), SERVERPROPERTY('ServerName')), DB_NAME());
END;

UPDATE dbo.SdgoBattleRewardWalletMeta
SET ScopeId = CASE
        WHEN OriginServer <> CONVERT(nvarchar(128), SERVERPROPERTY('ServerName'))
          OR OriginDatabase <> DB_NAME()
        THEN NEWID()
        ELSE ScopeId
    END,
    OriginServer = CONVERT(nvarchar(128), SERVERPROPERTY('ServerName')),
    OriginDatabase = DB_NAME()
WHERE Singleton = 1;

IF NOT EXISTS (
    SELECT 1
    FROM dbo.SdgoBattleRewardWalletMeta
    WHERE Singleton = 1
      AND SchemaVersion = 1
      AND OriginServer = CONVERT(nvarchar(128), SERVERPROPERTY('ServerName'))
      AND OriginDatabase = DB_NAME())
    THROW 51103, 'Unsupported battle reward wallet schema version or scope', 1;

IF OBJECT_ID(N'dbo.SdgoBattleRewardMbReceipt', N'U') IS NULL
BEGIN
    CREATE TABLE dbo.SdgoBattleRewardMbReceipt (
        MatchId nvarchar(128) NOT NULL,
        AccountNumber int NOT NULL,
        PlanHash char(64) NOT NULL,
        MbDelta int NOT NULL,
        MbAfter int NOT NULL,
        NativeRetVal int NOT NULL,
        AppliedUtc datetime2(3) NOT NULL,
        CONSTRAINT PK_SdgoBattleRewardMbReceipt
            PRIMARY KEY (MatchId, AccountNumber),
        CONSTRAINT CK_SdgoBattleRewardMbReceipt_Account
            CHECK (AccountNumber > 0),
        CONSTRAINT CK_SdgoBattleRewardMbReceipt_Delta
            CHECK (MbDelta > 0),
        CONSTRAINT CK_SdgoBattleRewardMbReceipt_Balance
            CHECK (MbAfter >= MbDelta),
        CONSTRAINT CK_SdgoBattleRewardMbReceipt_Native
            CHECK (NativeRetVal = 0)
    );
END;

IF COL_LENGTH(N'dbo.SdgoBattleRewardMbReceipt', N'MatchId') <> 256
   OR COL_LENGTH(N'dbo.SdgoBattleRewardMbReceipt', N'AccountNumber') <> 4
   OR COL_LENGTH(N'dbo.SdgoBattleRewardMbReceipt', N'PlanHash') <> 64
   OR COL_LENGTH(N'dbo.SdgoBattleRewardMbReceipt', N'MbDelta') <> 4
   OR COL_LENGTH(N'dbo.SdgoBattleRewardMbReceipt', N'MbAfter') <> 4
   OR COL_LENGTH(N'dbo.SdgoBattleRewardMbReceipt', N'NativeRetVal') <> 4
   OR COL_LENGTH(N'dbo.SdgoBattleRewardMbReceipt', N'AppliedUtc') <> 7
    THROW 51104, 'Conflicting dbo.SdgoBattleRewardMbReceipt table', 1;
GO

IF OBJECT_ID(N'dbo.SdgoBattleRewardApplyMb', N'P') IS NULL
    EXEC(N'CREATE PROCEDURE dbo.SdgoBattleRewardApplyMb AS RETURN 0');
GO

ALTER PROCEDURE dbo.SdgoBattleRewardApplyMb
    @MatchId nvarchar(128),
    @AccountNumber int,
    @ExpectedScope uniqueidentifier,
    @PlanHash char(64),
    @Mb int
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;

    IF @MatchId IS NULL
       OR LEN(@MatchId) = 0
       OR @MatchId COLLATE Latin1_General_100_BIN2 LIKE N'%[^-0-9A-Za-z_.:]%'
       OR @AccountNumber <= 0
       OR @ExpectedScope IS NULL
       OR @PlanHash IS NULL
       OR LEN(@PlanHash) <> 64
       OR @PlanHash COLLATE Latin1_General_100_BIN2 LIKE '%[^0-9A-Fa-f]%'
       OR @Mb <= 0
        THROW 51110, 'Invalid battle reward MB request', 1;

    BEGIN TRANSACTION;
    BEGIN TRY
        IF NOT EXISTS (
            SELECT 1
            FROM dbo.SdgoBattleRewardWalletMeta WITH (UPDLOCK, HOLDLOCK)
            WHERE Singleton = 1
              AND ScopeId = @ExpectedScope
              AND SchemaVersion = 1
              AND OriginServer = CONVERT(nvarchar(128), SERVERPROPERTY('ServerName'))
              AND OriginDatabase = DB_NAME())
            THROW 51111, 'Battle reward wallet scope changed', 1;

        DECLARE @LockResult int;
        DECLARE @LockName nvarchar(255);
        SET @LockName = N'sdgo-battle-mb:'
            + CONVERT(varchar(64), HASHBYTES(
                'SHA2_256',
                CONVERT(varbinary(512), @MatchId + N':' + CONVERT(nvarchar(16), @AccountNumber))
              ), 2);

        EXEC @LockResult = sys.sp_getapplock
            @Resource = @LockName,
            @LockMode = 'Exclusive',
            @LockOwner = 'Transaction',
            @LockTimeout = 10000;
        IF @LockResult < 0
            THROW 51112, 'Battle reward MB lock unavailable', 1;

        DECLARE @ExistingHash char(64);
        DECLARE @ExistingDelta int;
        DECLARE @ExistingAfter int;
        DECLARE @ExistingRetVal int;

        SELECT
            @ExistingHash = PlanHash,
            @ExistingDelta = MbDelta,
            @ExistingAfter = MbAfter,
            @ExistingRetVal = NativeRetVal
        FROM dbo.SdgoBattleRewardMbReceipt WITH (UPDLOCK, HOLDLOCK)
        WHERE MatchId = @MatchId AND AccountNumber = @AccountNumber;

        IF @ExistingHash IS NOT NULL
        BEGIN
            IF @ExistingHash <> @PlanHash OR @ExistingDelta <> @Mb
                THROW 51113, 'Battle reward retry changed plan or MB amount', 1;

            SELECT
                @MatchId AS MatchId,
                @AccountNumber AS AccountNumber,
                @ExistingHash AS PlanHash,
                @ExistingDelta AS MbDelta,
                @ExistingAfter AS MbAfter,
                @ExistingRetVal AS NativeRetVal,
                CAST(1 AS bit) AS AlreadyApplied;
            COMMIT TRANSACTION;
            RETURN;
        END;

        DECLARE @NativeRetVal int = -1;
        DECLARE @NativeMbAfter int = -1;

        EXEC dbo.sp_PocketGC_IO
            @type = 2,
            @s_idx = @AccountNumber,
            @s_GC = @Mb,
            @s_from = 2,
            @s_productID = 0,
            @retVal = @NativeRetVal OUTPUT,
            @retGC = @NativeMbAfter OUTPUT;

        IF @NativeRetVal <> 0
            THROW 51114, 'Native dbo.sp_PocketGC_IO rejected battle reward', 1;
        IF @NativeMbAfter < @Mb
            THROW 51115, 'Native dbo.sp_PocketGC_IO returned an invalid balance', 1;

        INSERT dbo.SdgoBattleRewardMbReceipt
            (MatchId, AccountNumber, PlanHash, MbDelta, MbAfter, NativeRetVal, AppliedUtc)
        VALUES
            (@MatchId, @AccountNumber, @PlanHash, @Mb, @NativeMbAfter, @NativeRetVal,
             SYSUTCDATETIME());

        SELECT
            @MatchId AS MatchId,
            @AccountNumber AS AccountNumber,
            @PlanHash AS PlanHash,
            @Mb AS MbDelta,
            @NativeMbAfter AS MbAfter,
            @NativeRetVal AS NativeRetVal,
            CAST(0 AS bit) AS AlreadyApplied;
        COMMIT TRANSACTION;
    END TRY
    BEGIN CATCH
        IF XACT_STATE() <> 0
            ROLLBACK TRANSACTION;
        THROW;
    END CATCH;
END;
GO
