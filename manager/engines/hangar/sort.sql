ALTER PROCEDURE dbo.sdgo_mod_SortHangar
    @AccountNumber int, @Request uniqueidentifier,
    @Changed int OUTPUT, @Total int OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    IF @AccountNumber <= 0 OR @Request IS NULL BEGIN RAISERROR('Invalid hangar request',16,1); RETURN; END;
    DECLARE @Own bit = CASE WHEN @@TRANCOUNT=0 THEN 1 ELSE 0 END;
    IF @Own=1 BEGIN TRANSACTION;
    ELSE SAVE TRANSACTION SDGO_HangarSort;
    BEGIN TRY
        DECLARE @Lock int, @Resource nvarchar(255)=N'SDGO.Hangar.Sort.'+CONVERT(nvarchar(20),@AccountNumber);
        EXEC @Lock=sys.sp_getapplock @Resource=@Resource,@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000;
        IF @Lock<0 RAISERROR('Hangar busy',16,1);
        SELECT @Changed=ChangedSlots,@Total=UnitCount FROM dbo.sdgo_mod_HangarSortReceipt
          WHERE AccountNumber=@AccountNumber AND RequestId=@Request;
        IF @@ROWCOUNT>0
        BEGIN
            IF @Own=1 COMMIT TRANSACTION;
            RETURN;
        END;
        IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Users WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber=@AccountNumber)
            RAISERROR('Unknown hangar owner',16,1);
        SELECT * INTO #Before FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber=@AccountNumber;
        SELECT @Total=COUNT(*) FROM #Before;
        IF @Total>1200 RAISERROR('Unsupported hangar capacity',16,1);
        IF EXISTS(SELECT UserUnitSlotNumber FROM #Before GROUP BY UserUnitSlotNumber HAVING COUNT(*)>1)
            RAISERROR('Duplicate inventory slots',16,1);
        SELECT UU.UserUnitNumber,UU.UserUnitSlotNumber AS OldSlot,
            CONVERT(int,ROW_NUMBER() OVER(ORDER BY COALESCE(U.UnitGradeNumber,0) DESC,
              COALESCE(U.UnitGradeNumber2,0) DESC,UU.UnitNumber DESC,UU.DateGet DESC,UU.UserUnitNumber DESC)) AS NewSlot
        INTO #Order FROM #Before UU LEFT JOIN dbo.tbl_Units U ON U.UnitNumber=UU.UnitNumber;
        IF (SELECT COUNT(*) FROM #Order)<>@Total RAISERROR('Ambiguous unit catalog',16,1);
        SELECT @Changed=COUNT(*) FROM #Order WHERE OldSlot<>NewSlot;
        DECLARE @Base bigint=ISNULL((SELECT MAX(CONVERT(bigint,UserUnitSlotNumber)) FROM #Before),0)+10000;
        IF @Base+@Total>2147483647 RAISERROR('Temporary slot overflow',16,1);
        -- Move only slots; locks, active stable UID, ships, paint, experience,
        -- currencies, drop boxes and every other inventory column are preserved.
        UPDATE UU SET UserUnitSlotNumber=CONVERT(int,@Base+O.NewSlot)
          FROM dbo.tbl_UserUnits UU JOIN #Order O ON O.UserUnitNumber=UU.UserUnitNumber
          WHERE UU.AccountNumber=@AccountNumber AND O.OldSlot<>O.NewSlot;
        UPDATE UU SET UserUnitSlotNumber=O.NewSlot
          FROM dbo.tbl_UserUnits UU JOIN #Order O ON O.UserUnitNumber=UU.UserUnitNumber
          WHERE UU.AccountNumber=@AccountNumber AND O.OldSlot<>O.NewSlot;
        IF EXISTS(SELECT 1 FROM dbo.tbl_UserUnits UU JOIN #Order O ON O.UserUnitNumber=UU.UserUnitNumber
            WHERE UU.AccountNumber=@AccountNumber AND UU.UserUnitSlotNumber<>O.NewSlot)
            RAISERROR('Hangar order verification failed',16,1);
        IF EXISTS(SELECT __IMMUTABLE_COLUMNS__ FROM #Before EXCEPT
                  SELECT __IMMUTABLE_COLUMNS__ FROM dbo.tbl_UserUnits WHERE AccountNumber=@AccountNumber)
           OR EXISTS(SELECT __IMMUTABLE_COLUMNS__ FROM dbo.tbl_UserUnits WHERE AccountNumber=@AccountNumber EXCEPT
                     SELECT __IMMUTABLE_COLUMNS__ FROM #Before)
            RAISERROR('Inventory content changed during sorting',16,1);
        -- The native client allocates 24 + ExpandSlotCount slots before loading
        -- records. Imported inventories can exceed that metadata and crash the
        -- unguarded native slot writer. Repair only the minimum six-slot blocks.
        IF EXISTS(SELECT 1 FROM dbo.tbl_Users WHERE AccountNumber=@AccountNumber AND 24+ExpandSlotCount<@Total)
            UPDATE dbo.tbl_Users SET ExpandSlotCount=((@Total-24+5)/6)*6 WHERE AccountNumber=@AccountNumber;
        INSERT dbo.sdgo_mod_HangarSortReceipt(AccountNumber,RequestId,ChangedSlots,UnitCount)
            VALUES(@AccountNumber,@Request,@Changed,@Total);
        IF @Own=1 COMMIT TRANSACTION;
    END TRY
    BEGIN CATCH
        DECLARE @Error nvarchar(4000)=ERROR_MESSAGE();
        IF @Own=1 AND @@TRANCOUNT>0 ROLLBACK TRANSACTION;
        ELSE IF @Own=0 AND XACT_STATE()=1 ROLLBACK TRANSACTION SDGO_HangarSort;
        RAISERROR(@Error,16,1);
    END CATCH;
END;
