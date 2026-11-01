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
                      WHERE Singleton=1 AND Enabled=1 AND MachineID=@v_MachineID AND PriceGP=100000)
        BEGIN RETURN 3002;END;
        IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Coverage WITH(HOLDLOCK)
                      WHERE AccountNumber=@v_AccountNum AND Complete=1)
        BEGIN RETURN 3002;END;
        -- Lock inventory before history, matching the acquisition triggers' order.
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
        DECLARE @choices TABLE(Ordinal int IDENTITY(1,1),UnitID int,UnitRank tinyint);
        INSERT @choices(UnitID,UnitRank)
        SELECT u.UnitID,u.UnitRank FROM @bases u
        WHERE NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen s WITH(HOLDLOCK)
                         JOIN @reach family ON family.NodeID=s.UnitID
                         WHERE s.AccountNumber=@v_AccountNum AND family.StartID=u.UnitID)
          AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits owned JOIN @reach family ON family.NodeID=owned.UnitNumber
                         WHERE owned.AccountNumber=@v_AccountNum AND family.StartID=u.UnitID)
        ORDER BY u.UnitID;
        DECLARE @count int,@random bigint,@ceiling bigint,@unit int,@rank tinyint;
        SELECT @count=COUNT(*) FROM @choices;
        IF @count=0 BEGIN RETURN 3002;END;
        SET @ceiling=CONVERT(bigint,4294967296)-(CONVERT(bigint,4294967296)%@count);
        WHILE 1=1
        BEGIN
            SET @random=CONVERT(bigint,CRYPT_GEN_RANDOM(4));
            IF @random<@ceiling BREAK;
        END;
        SELECT @unit=UnitID,@rank=UnitRank FROM @choices
         WHERE Ordinal=CONVERT(int,@random%@count)+1;

        DECLARE @rc int,@slot int,@instance bigint,@serial bigint,@maxLevel tinyint,@end datetime;
        EXEC @rc=dbo.sp_CreateUserUnit @v_AccountNum,'',0,0,@end,0,@unit,1,0,1,
             @slot OUTPUT,@instance OUTPUT,@serial OUTPUT,@maxLevel OUTPUT;
        IF @rc<>0 BEGIN RETURN @rc;END;
        IF @instance IS NULL OR NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits
                    WHERE AccountNumber=@v_AccountNum AND UserUnitNumber=@instance AND UnitNumber=@unit)
        BEGIN RETURN 3002;END;
        IF NOT EXISTS(SELECT 1 FROM sdgo_collection_capsule.Seen
                      WHERE AccountNumber=@v_AccountNum AND UnitID=@unit)
        BEGIN RETURN 3002;END;
        INSERT dbo.tbl_CapsulePicked(AccountID,MachineID,UnitGradeNumber,UnitID)
        VALUES(@v_AccountNum,@v_MachineID,@rank,@unit);
        INSERT sdgo_collection_capsule.DrawLog(AccountNumber,UnitID,UserUnitNumber,MachineID)
        VALUES(@v_AccountNum,@unit,@instance,@v_MachineID);
        SELECT @instance AS UserUnitNum,@unit AS UnitID,@slot AS SlotIndex,
               @serial AS SerialNum,@maxLevel AS nCustomLvMax;
        RETURN 0;
    END TRY
    BEGIN CATCH
        RETURN 3002;
    END CATCH;
END
