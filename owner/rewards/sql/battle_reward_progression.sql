SET NOCOUNT ON;
SET XACT_ABORT ON;
IF OBJECT_ID(N'dbo.SdgoBattleProgressPeriod', N'U') IS NULL
    CREATE TABLE dbo.SdgoBattleProgressPeriod (
        AccountNumber int NOT NULL,
        PeriodKind char(1) NOT NULL CHECK (PeriodKind IN ('D','W')),
        PeriodStart date NOT NULL,
        MatchCount int NOT NULL CHECK (MatchCount >= 0),
        FirstPaid bit NOT NULL,
        TargetPaid bit NOT NULL,
        PRIMARY KEY (AccountNumber, PeriodKind, PeriodStart)
    );
IF OBJECT_ID(N'dbo.SdgoBattleProgressMatch', N'U') IS NULL
    CREATE TABLE dbo.SdgoBattleProgressMatch (
        MatchId nvarchar(128) NOT NULL,
        AccountNumber int NOT NULL,
        SourceHash char(64) NOT NULL,
        PolicyRevision char(64) NOT NULL,
        CompletedUtc datetime2(0) NOT NULL,
        Qualified bit NOT NULL,
        DayStart date NOT NULL,
        WeekStart date NOT NULL,
        FirstMb int NOT NULL,
        DailyMb int NOT NULL,
        WeeklyMb int NOT NULL,
        BonusMb int NOT NULL CHECK (BonusMb >= 0),
        AppliedUtc datetime2(3) NOT NULL,
        PRIMARY KEY (MatchId, AccountNumber)
    );
GO
IF OBJECT_ID(N'dbo.SdgoBattleProgressApply', N'P') IS NULL
    EXEC(N'CREATE PROCEDURE dbo.SdgoBattleProgressApply AS RETURN 0');
GO
ALTER PROCEDURE dbo.SdgoBattleProgressApply
    @MatchId nvarchar(128), @AccountNumber int, @ExpectedScope uniqueidentifier,
    @SourceHash char(64), @PolicyRevision char(64), @CompletedUtc datetime2(0),
    @Qualified bit, @FirstMb int, @DailyTarget int, @DailyMb int,
    @WeeklyTarget int, @WeeklyMb int
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    IF @MatchId IS NULL OR LEN(@MatchId)=0 OR @AccountNumber IS NULL OR @AccountNumber<=0
       OR @MatchId COLLATE Latin1_General_100_BIN2 LIKE N'%[^-0-9A-Za-z_.:]%'
       OR @ExpectedScope IS NULL OR @SourceHash IS NULL OR LEN(@SourceHash)<>64
       OR @SourceHash COLLATE Latin1_General_100_BIN2 LIKE '%[^0-9a-f]%'
       OR @PolicyRevision IS NULL OR LEN(@PolicyRevision)<>64
       OR @PolicyRevision COLLATE Latin1_General_100_BIN2 LIKE '%[^0-9a-f]%'
       OR @Qualified IS NULL OR @CompletedUtc IS NULL OR @CompletedUtc<'20000101'
       OR @CompletedUtc>DATEADD(minute,5,SYSUTCDATETIME())
       OR @FirstMb IS NULL OR @FirstMb NOT BETWEEN 0 AND 100000
       OR @DailyMb IS NULL OR @DailyMb NOT BETWEEN 0 AND 100000
       OR @WeeklyMb IS NULL OR @WeeklyMb NOT BETWEEN 0 AND 100000
       OR @DailyTarget IS NULL OR @DailyTarget NOT BETWEEN 2 AND 20
       OR @WeeklyTarget IS NULL OR @WeeklyTarget NOT BETWEEN 3 AND 100 OR @WeeklyTarget<@DailyTarget
        THROW 51200, 'Invalid progression request', 1;
    DECLARE @Day date=CONVERT(date,DATEADD(hour,8,@CompletedUtc));
    DECLARE @Week date=DATEADD(day,-(DATEDIFF(day,CONVERT(date,'19000101'),@Day)%7),@Day);
    BEGIN TRANSACTION;
    BEGIN TRY
        IF NOT EXISTS (SELECT 1 FROM dbo.SdgoBattleRewardWalletMeta WITH (HOLDLOCK)
            WHERE Singleton=1 AND ScopeId=@ExpectedScope AND SchemaVersion=1
              AND OriginServer=CONVERT(nvarchar(128),SERVERPROPERTY('ServerName')) AND OriginDatabase=DB_NAME())
            THROW 51201, 'Progression wallet scope changed', 1;
        DECLARE @LockResult int, @LockName nvarchar(255)=N'sdgo-progress:'+CONVERT(nvarchar(16),@AccountNumber);
        EXEC @LockResult=sys.sp_getapplock @Resource=@LockName,@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=10000;
        IF @LockResult<0 THROW 51202, 'Progression lock unavailable', 1;
        IF EXISTS (SELECT 1 FROM dbo.SdgoBattleProgressMatch WHERE MatchId=@MatchId AND AccountNumber=@AccountNumber)
        BEGIN
            IF NOT EXISTS (SELECT 1 FROM dbo.SdgoBattleProgressMatch WHERE MatchId=@MatchId AND AccountNumber=@AccountNumber
                AND SourceHash=@SourceHash AND PolicyRevision=@PolicyRevision)
                THROW 51203, 'Progression retry identity changed', 1;
            SELECT BonusMb FROM dbo.SdgoBattleProgressMatch WHERE MatchId=@MatchId AND AccountNumber=@AccountNumber;
            COMMIT TRANSACTION; RETURN;
        END;
        DECLARE @First int=0,@Daily int=0,@Weekly int=0;
        IF @Qualified=1
        BEGIN
            IF NOT EXISTS (SELECT 1 FROM dbo.SdgoBattleProgressPeriod WHERE AccountNumber=@AccountNumber AND PeriodKind='D' AND PeriodStart=@Day)
                INSERT dbo.SdgoBattleProgressPeriod VALUES(@AccountNumber,'D',@Day,0,0,0);
            IF NOT EXISTS (SELECT 1 FROM dbo.SdgoBattleProgressPeriod WHERE AccountNumber=@AccountNumber AND PeriodKind='W' AND PeriodStart=@Week)
                INSERT dbo.SdgoBattleProgressPeriod VALUES(@AccountNumber,'W',@Week,0,0,0);
            SELECT @First=CASE WHEN FirstPaid=0 THEN @FirstMb ELSE 0 END,
                   @Daily=CASE WHEN TargetPaid=0 AND MatchCount+1>=@DailyTarget THEN @DailyMb ELSE 0 END
              FROM dbo.SdgoBattleProgressPeriod WHERE AccountNumber=@AccountNumber AND PeriodKind='D' AND PeriodStart=@Day;
            SELECT @Weekly=CASE WHEN TargetPaid=0 AND MatchCount+1>=@WeeklyTarget THEN @WeeklyMb ELSE 0 END
              FROM dbo.SdgoBattleProgressPeriod WHERE AccountNumber=@AccountNumber AND PeriodKind='W' AND PeriodStart=@Week;
            UPDATE dbo.SdgoBattleProgressPeriod SET MatchCount=MatchCount+1,FirstPaid=1,
                TargetPaid=CASE WHEN MatchCount+1>=@DailyTarget THEN 1 ELSE TargetPaid END
              WHERE AccountNumber=@AccountNumber AND PeriodKind='D' AND PeriodStart=@Day;
            UPDATE dbo.SdgoBattleProgressPeriod SET MatchCount=MatchCount+1,
                TargetPaid=CASE WHEN MatchCount+1>=@WeeklyTarget THEN 1 ELSE TargetPaid END
              WHERE AccountNumber=@AccountNumber AND PeriodKind='W' AND PeriodStart=@Week;
        END;
        DECLARE @Bonus int=@First+@Daily+@Weekly;
        IF @Bonus>0
        BEGIN
            DECLARE @RetVal int=-1,@After int=-1;
            EXEC dbo.sp_PocketGC_IO @type=2,@s_idx=@AccountNumber,@s_GC=@Bonus,@s_from=2,@s_productID=0,
                @retVal=@RetVal OUTPUT,@retGC=@After OUTPUT;
            IF @RetVal<>0 OR @After<@Bonus THROW 51204, 'Native progression wallet credit rejected', 1;
        END;
        INSERT dbo.SdgoBattleProgressMatch VALUES(@MatchId,@AccountNumber,@SourceHash,@PolicyRevision,@CompletedUtc,@Qualified,
            @Day,@Week,@First,@Daily,@Weekly,@Bonus,SYSUTCDATETIME());
        SELECT @Bonus AS BonusMb;
        COMMIT TRANSACTION;
    END TRY
    BEGIN CATCH
        IF XACT_STATE()<>0 ROLLBACK TRANSACTION;
        THROW;
    END CATCH;
END;
GO
