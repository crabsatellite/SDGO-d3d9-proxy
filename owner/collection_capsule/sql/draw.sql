CREATE PROCEDURE sdgo_collection_capsule.Draw
    @v_AccountNum int,
    @v_MachineID int
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    -- The stock World path has already charged the registered GP price. This
    -- procedure owns delivery only; failure returns through native compensation.
    IF @@TRANCOUNT=0 RETURN 3002;
    BEGIN TRY
        DECLARE @lock int,@resource nvarchar(128),@dummy int;
        SET @resource=N'sdgo.collection_capsule.account.'+CONVERT(nvarchar(20),@v_AccountNum);
        EXEC @lock=sys.sp_getapplock @Resource=@resource,@LockMode='Exclusive',
             @LockOwner='Transaction',@LockTimeout=5000;
        IF @lock<0 BEGIN RETURN 3002;END;
        IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.State WITH(HOLDLOCK)
                      WHERE Singleton=1 AND SchemaVersion=3 AND Enabled=1 AND MachineID=@v_MachineID AND PriceGP=100000)
        BEGIN RETURN 3002;END;
        -- Existing profiles can draw immediately; no complete-history proof.
        IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Users WITH(HOLDLOCK)
                      WHERE AccountNumber=@v_AccountNum)
        BEGIN RETURN 3002;END;
        -- Native event rewards lock the gift box before inventory/history.
        SELECT @dummy=COUNT(*) FROM dbo.tbl_UserDropItemBox WITH(UPDLOCK,HOLDLOCK)
         WHERE AccountNumber=@v_AccountNum;
        SELECT @dummy=COUNT(*) FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK)
         WHERE AccountNumber=@v_AccountNum;
        -- The current native catalog is the pool. Roots is form metadata, not
        -- a frozen list of prizes: new standalone/ordinary transform families
        -- participate automatically. Reciprocal ownable forms use verified
        -- native/import metadata and only that family's source must still match.
        DECLARE @reach TABLE(StartID int,NodeID int,PRIMARY KEY(StartID,NodeID));
        ;WITH paths AS (
            SELECT UnitNumber AS StartID,UnitNumber AS NodeID,
                   CAST('/'+CONVERT(varchar(11),UnitNumber)+'/' AS varchar(max)) AS Visited
            FROM dbo.tbl_Units WITH(HOLDLOCK)
            UNION ALL
            SELECT p.StartID,n.UnitNumber,
                   CAST(p.Visited+CONVERT(varchar(11),n.UnitNumber)+'/' AS varchar(max))
            FROM paths p JOIN dbo.tbl_Units u ON u.UnitNumber=p.NodeID
            JOIN dbo.tbl_Units n ON n.UnitNumber=u.UnitNumber_Transform
            WHERE CHARINDEX('/'+CONVERT(varchar(11),n.UnitNumber)+'/',p.Visited)=0
        )
        INSERT @reach SELECT StartID,NodeID FROM paths OPTION(MAXRECURSION 32767);
        DECLARE @ownable TABLE(UnitID int PRIMARY KEY,UnitRank tinyint,UnitSubgrade tinyint);
        INSERT @ownable
        SELECT UnitNumber,UnitGradeNumber,UnitGradeNumber2 FROM dbo.tbl_Units
        WHERE SallyReady=1 AND UnitGradeNumber BETWEEN 1 AND 4
          AND UnitGradeNumber2 IN(2,3,4,5)
          AND (NOT(LOWER(LTRIM(RTRIM(UnitModelName)))='name' AND UnitGraph=1010001)
               OR EXISTS(SELECT 1 FROM sdgo_collection_capsule.Roots root
                         JOIN sdgo_collection_capsule.UnitSource source ON source.UnitID=root.UnitID
                         WHERE root.UnitID=dbo.tbl_Units.UnitNumber
                           AND source.TransformTo=ISNULL(dbo.tbl_Units.UnitNumber_Transform,0)
                           AND source.Ready=dbo.tbl_Units.SallyReady
                           AND source.UnitRank=dbo.tbl_Units.UnitGradeNumber
                           AND source.UnitSubgrade=dbo.tbl_Units.UnitGradeNumber2
                           AND source.UnitGraph=dbo.tbl_Units.UnitGraph));
        DECLARE @bases TABLE(UnitID int PRIMARY KEY,UnitRank tinyint);
        INSERT @bases(UnitID,UnitRank)
        SELECT u.UnitID,u.UnitRank FROM @ownable u
        WHERE NOT EXISTS(
            SELECT 1 FROM @reach r JOIN @ownable other ON other.UnitID=r.StartID
            WHERE r.NodeID=u.UnitID AND other.UnitID<>u.UnitID
              AND NOT EXISTS(SELECT 1 FROM @reach back WHERE back.StartID=u.UnitID AND back.NodeID=other.UnitID))
          AND (
            NOT EXISTS(SELECT 1 FROM @reach r JOIN @ownable other ON other.UnitID=r.NodeID
                       WHERE r.StartID=u.UnitID AND other.UnitID<>u.UnitID
                         AND EXISTS(SELECT 1 FROM @reach back WHERE back.StartID=other.UnitID AND back.NodeID=u.UnitID))
            OR (EXISTS(SELECT 1 FROM sdgo_collection_capsule.Roots root WHERE root.UnitID=u.UnitID)
                AND NOT EXISTS(
                    SELECT 1 FROM @reach family JOIN dbo.tbl_Units live ON live.UnitNumber=family.NodeID
                    LEFT JOIN sdgo_collection_capsule.UnitSource source ON source.UnitID=live.UnitNumber
                    WHERE family.StartID=u.UnitID AND (source.UnitID IS NULL
                      OR ISNULL(live.UnitNumber_Transform,0)<>source.TransformTo
                      OR live.SallyReady<>source.Ready OR live.UnitGradeNumber<>source.UnitRank
                      OR live.UnitGradeNumber2<>source.UnitSubgrade OR live.UnitGraph<>source.UnitGraph)))
          )
        ;
        -- A newly imported reciprocal family must register its initial form.
        -- Surface missing/conflicting metadata instead of silently hiding it.
        IF EXISTS(SELECT 1 FROM @ownable u
            WHERE EXISTS(SELECT 1 FROM @reach r JOIN @ownable other ON other.UnitID=r.NodeID
                         WHERE r.StartID=u.UnitID AND other.UnitID<>u.UnitID
                           AND EXISTS(SELECT 1 FROM @reach back WHERE back.StartID=other.UnitID AND back.NodeID=u.UnitID))
              AND NOT EXISTS(SELECT 1 FROM @reach incoming JOIN @ownable prior ON prior.UnitID=incoming.StartID
                             WHERE incoming.NodeID=u.UnitID AND prior.UnitID<>u.UnitID
                               AND NOT EXISTS(SELECT 1 FROM @reach back WHERE back.StartID=u.UnitID AND back.NodeID=prior.UnitID))
              AND (SELECT COUNT(*) FROM @bases chosen JOIN @reach members ON members.NodeID=chosen.UnitID
                   WHERE members.StartID=u.UnitID)<>1)
            RETURN 3008;
        -- Catalog membership is persistent; inventory and pending gifts are
        -- current possession. Prefer missing catalog families, then missing
        -- inventory families only after the entire eligible catalog is unlocked.
        DECLARE @unheld TABLE(UnitID int PRIMARY KEY,UnitRank tinyint);
        INSERT @unheld(UnitID,UnitRank)
        SELECT u.UnitID,u.UnitRank FROM @bases u
        WHERE NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned JOIN @reach family ON family.NodeID=owned.UnitNumber
                         WHERE owned.AccountNumber=@v_AccountNum AND owned.TimeLimit=0 AND owned.RunOut=0 AND family.StartID=u.UnitID)
          AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserDropItemBox b JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=b.ItemCategory
                         JOIN @reach family ON family.NodeID=c.RefIndex
                         WHERE b.AccountNumber=@v_AccountNum AND c.CatalogueID/100000=11 AND c.TimeLimit=0 AND family.StartID=u.UnitID);
        DECLARE @missingCatalog bit=CASE WHEN EXISTS(
            SELECT 1 FROM @bases u WHERE NOT EXISTS(
                SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks unlocked WITH(HOLDLOCK)
                JOIN @reach family ON family.NodeID=unlocked.UnitID
                WHERE unlocked.AccountNumber=@v_AccountNum AND family.StartID=u.UnitID)) THEN 1 ELSE 0 END;
        DECLARE @choices TABLE(Ordinal int IDENTITY(1,1),UnitID int,UnitRank tinyint);
        INSERT @choices(UnitID,UnitRank)
        SELECT u.UnitID,u.UnitRank FROM @unheld u
        WHERE @missingCatalog=0 OR NOT EXISTS(
            SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks unlocked WITH(HOLDLOCK)
            JOIN @reach family ON family.NodeID=unlocked.UnitID
            WHERE unlocked.AccountNumber=@v_AccountNum AND family.StartID=u.UnitID)
        ORDER BY u.UnitID;
        DECLARE @count int,@random bigint,@ceiling bigint,@unit int,@rank tinyint;
        SELECT @count=COUNT(*) FROM @choices;
        IF @count=0 BEGIN RETURN 3002;END;
        IF EXISTS(SELECT 1 FROM @choices u WHERE NOT EXISTS(
            SELECT 1 FROM dbo.tbl_ItemCategory c WHERE c.RefIndex=u.UnitID AND c.CatalogueID/100000=11
              AND c.TimeLimit=0 AND c.Option1=1 AND c.Option2=2)) RETURN 3008;
        IF EXISTS(SELECT 1 FROM sdgo_collection_capsule.OwnedUnitItems o LEFT JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=o.CategoryID
            WHERE c.CatalogueID IS NULL OR c.RefIndex<>o.UnitID OR c.TimeLimit<>0 OR c.Option1<>1 OR c.Option2<>2) RETURN 3008;
        IF EXISTS(SELECT 1 FROM @choices u JOIN sdgo_collection_capsule.UnitDelivery d ON d.UnitID=u.UnitID
            LEFT JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=d.CategoryID
            WHERE c.CatalogueID IS NULL OR c.RefIndex<>u.UnitID OR c.TimeLimit<>0 OR c.Option1<>1 OR c.Option2<>2) RETURN 3008;
        -- Grade probabilities follow sp_DailyLoginReward: C35/B30/A20/S15.
        -- Exhausted grades leave the lottery; unit counts never multiply rank odds.
        DECLARE @grades TABLE(UnitRank tinyint PRIMARY KEY,Weight int);
        INSERT @grades SELECT w.UnitRank,w.Weight FROM (VALUES(1,35),(2,30),(3,20),(4,15)) w(UnitRank,Weight)
         WHERE EXISTS(SELECT 1 FROM @choices c WHERE c.UnitRank=w.UnitRank);
        DECLARE @total int,@ticket int;
        SELECT @total=SUM(Weight) FROM @grades;
        SET @ceiling=CONVERT(bigint,4294967296)-(CONVERT(bigint,4294967296)%@total);
        WHILE 1=1
        BEGIN
            SET @random=CONVERT(bigint,CRYPT_GEN_RANDOM(4));
            IF @random<@ceiling BREAK;
        END;
        SET @ticket=CONVERT(int,@random%@total);
        SELECT TOP 1 @rank=g.UnitRank FROM @grades g
         WHERE @ticket<(SELECT SUM(p.Weight) FROM @grades p WHERE p.UnitRank<=g.UnitRank) ORDER BY g.UnitRank;
        SELECT TOP 1 @unit=UnitID FROM @choices WHERE UnitRank=@rank ORDER BY NEWID();
        DECLARE @category int,@instance bigint;
        SELECT @category=MIN(CatalogueID) FROM dbo.tbl_ItemCategory
         WHERE RefIndex=@unit AND CatalogueID/100000=11 AND TimeLimit=0 AND Option1=1 AND Option2=2;
        -- Prefer the category proven against both client and server at build.
        -- A smaller server-only category must not override that proof.
        SELECT @category=CategoryID FROM sdgo_collection_capsule.UnitDelivery WHERE UnitID=@unit;
        IF @category IS NULL RETURN 3008;
        INSERT dbo.tbl_UserDropItemBox(AccountNumber,ItemCategory,Amount,DropConditionID)
        VALUES(@v_AccountNum,@category,1,@v_MachineID);
        SET @instance=CONVERT(bigint,SCOPE_IDENTITY());
        IF @instance IS NULL OR NOT EXISTS(SELECT 1 FROM dbo.tbl_UserDropItemBox
                    WHERE AccountNumber=@v_AccountNum AND UserDropItemIdx=@instance AND ItemCategory=@category AND Amount=1)
        BEGIN RETURN 3002;END;
        IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.CatalogUnlocks
                      WHERE AccountNumber=@v_AccountNum AND UnitID=@unit)
        BEGIN RETURN 3002;END;
        INSERT dbo.tbl_CapsulePicked(AccountID,MachineID,UnitGradeNumber,UnitID)
        VALUES(@v_AccountNum,@v_MachineID,@rank,@unit);
        SELECT @instance AS DropBoxSerial,@category AS ItemCategory,CONVERT(int,1) AS Amount;
        RETURN 0;
    END TRY
    BEGIN CATCH
        RETURN 3002;
    END CATCH;
END
