ALTER PROCEDURE dbo.sdgo_mod_HangarGBCommand
 @AccountNumber int,@Body varchar(300),@Result int OUTPUT
AS
BEGIN
 SET NOCOUNT ON; SET XACT_ABORT ON; SET @Result=5;
 DECLARE @Id uniqueidentifier,@Action char(1),@Tail varchar(250),@Keep int,@Count int,@Expected bigint,@Sort bit,@Part int,@Data varchar(224);
 BEGIN TRY
   IF LEFT(@Body,12)<>'!sdgo-gb-v1 ' OR LEN(@Body)<50 RETURN;
   SET @Id=CONVERT(uniqueidentifier,SUBSTRING(@Body,13,36));
   IF SUBSTRING(@Body,49,1)<>' ' RETURN;
   SET @Action=SUBSTRING(@Body,50,1);SET @Tail=SUBSTRING(@Body,52,250);
   IF @Action='H'
   BEGIN
     DECLARE @Tokens varchar(250)=REPLACE(@Tail,' ','.');
     SET @Keep=CONVERT(int,PARSENAME(@Tokens,4));SET @Count=CONVERT(int,PARSENAME(@Tokens,3));
     SET @Expected=CONVERT(bigint,PARSENAME(@Tokens,2));SET @Sort=CONVERT(bit,PARSENAME(@Tokens,1));
     IF @Keep IS NULL OR @Keep<1 OR @Keep>20 OR @Count IS NULL OR @Count<1 OR @Count>1200 OR @Expected IS NULL OR @Expected<1 OR @Expected>2147483647 OR @Sort IS NULL RETURN;
   END
   ELSE IF @Action='D'
   BEGIN
     DECLARE @Split int=CHARINDEX(' ',@Tail);IF @Split<2 OR LEN(@Tail)-@Split>224 RETURN;
     SET @Part=CONVERT(int,LEFT(@Tail,@Split-1));SET @Data=SUBSTRING(@Tail,@Split+1,224);
     IF @Part<0 OR @Part>=150 OR LEN(@Data)<28 OR LEN(@Data)>224 OR LEN(@Data)%28<>0 OR @Data LIKE '%[^0-9A-Fa-f]%' RETURN;
   END
   ELSE IF @Action<>'C' OR LEN(@Tail)<>0 RETURN;
 END TRY
 BEGIN CATCH RETURN; END CATCH;
 IF @AccountNumber<=0 OR @Id IS NULL RETURN;
 DECLARE @Own bit=CASE WHEN @@TRANCOUNT=0 THEN 1 ELSE 0 END;
 IF @Own=1 BEGIN TRANSACTION;
 ELSE SAVE TRANSACTION SDGO_HangarGB;
 BEGIN TRY
   DECLARE @Lock int,@Resource nvarchar(255)=N'SDGO.Hangar.Sort.'+CONVERT(nvarchar(20),@AccountNumber);
   EXEC @Lock=sys.sp_getapplock @Resource=@Resource,@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=5000;
   IF @Lock<0 RAISERROR('Hangar busy',16,1);
   IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Users WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber=@AccountNumber) RAISERROR('Unknown owner',16,1);
   IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_mod_HangarGBJob WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber=@AccountNumber AND RequestId=@Id)
     INSERT dbo.sdgo_mod_HangarGBJob(AccountNumber,RequestId) VALUES(@AccountNumber,@Id);
   DECLARE @State varchar(12);
   SELECT @State=State FROM dbo.sdgo_mod_HangarGBJob WHERE AccountNumber=@AccountNumber AND RequestId=@Id;
   IF @State IN ('done','failed')
   BEGIN
     SET @Result=CASE WHEN @State='done' THEN 0 ELSE 5 END;
     IF @Own=1 COMMIT TRANSACTION;
     RETURN;
   END;
   IF @Action='H'
   BEGIN
     IF EXISTS(SELECT 1 FROM dbo.sdgo_mod_HangarGBJob WHERE AccountNumber=@AccountNumber AND RequestId=@Id AND HeaderSet=1
       AND (KeepCopies<>@Keep OR EntryCount<>@Count OR ExpectedGB<>@Expected OR SortAfter<>@Sort)) RAISERROR('Conflicting batch header',16,1);
     UPDATE dbo.sdgo_mod_HangarGBJob SET HeaderSet=1,KeepCopies=@Keep,EntryCount=@Count,ExpectedGB=@Expected,SortAfter=@Sort
       WHERE AccountNumber=@AccountNumber AND RequestId=@Id;
   END
   ELSE IF @Action='D'
   BEGIN
     DECLARE @Raw varbinary(112)=CONVERT(varbinary(112),@Data,2),@Offset int=1,@Position int=@Part*8,@Uid bigint,@Type int,@Slot int;
     WHILE @Offset<=DATALENGTH(@Raw)
     BEGIN
       SET @Uid=CONVERT(bigint,SUBSTRING(@Raw,@Offset,8));SET @Type=CONVERT(int,SUBSTRING(@Raw,@Offset+8,4));SET @Slot=CONVERT(int,SUBSTRING(@Raw,@Offset+12,2));
       IF @Uid<=0 OR @Type<=0 OR @Slot<1 OR @Slot>1200 RAISERROR('Invalid selected unit',16,1);
       IF EXISTS(SELECT 1 FROM dbo.sdgo_mod_HangarGBSelection WHERE AccountNumber=@AccountNumber AND RequestId=@Id AND Position=@Position)
       BEGIN
         IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_mod_HangarGBSelection WHERE AccountNumber=@AccountNumber AND RequestId=@Id AND Position=@Position AND UserUnitNumber=@Uid AND UnitNumber=@Type AND SlotNumber=@Slot)
           RAISERROR('Conflicting selection fragment',16,1);
       END
       ELSE INSERT dbo.sdgo_mod_HangarGBSelection VALUES(@AccountNumber,@Id,@Position,@Uid,@Type,@Slot);
       SET @Offset=@Offset+14;SET @Position=@Position+1;
     END;
   END
   ELSE UPDATE dbo.sdgo_mod_HangarGBJob SET CommitRequested=1 WHERE AccountNumber=@AccountNumber AND RequestId=@Id;
   SELECT @Keep=KeepCopies,@Count=EntryCount,@Expected=ExpectedGB,@Sort=SortAfter
     FROM dbo.sdgo_mod_HangarGBJob WHERE AccountNumber=@AccountNumber AND RequestId=@Id AND HeaderSet=1 AND CommitRequested=1;
   IF @@ROWCOUNT>0 AND (SELECT COUNT(*) FROM dbo.sdgo_mod_HangarGBSelection WHERE AccountNumber=@AccountNumber AND RequestId=@Id)>=@Count
   BEGIN
     IF (SELECT COUNT(*) FROM dbo.sdgo_mod_HangarGBSelection WHERE AccountNumber=@AccountNumber AND RequestId=@Id)<>@Count
       OR EXISTS(SELECT 1 FROM dbo.sdgo_mod_HangarGBSelection WHERE AccountNumber=@AccountNumber AND RequestId=@Id AND Position>=@Count)
       RAISERROR('Incomplete selection',16,1);
     DECLARE @Active bigint;SELECT @Active=GoUserUnitNum FROM dbo.tbl_Users WHERE AccountNumber=@AccountNumber;
     SELECT * INTO #BeforeGB FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber=@AccountNumber;
     SELECT S.UserUnitNumber,S.UnitNumber,S.SlotNumber INTO #SelectedGB FROM dbo.sdgo_mod_HangarGBSelection S WHERE S.AccountNumber=@AccountNumber AND S.RequestId=@Id;
     IF (SELECT COUNT(*) FROM #SelectedGB S JOIN #BeforeGB U ON U.UserUnitNumber=S.UserUnitNumber AND U.UnitNumber=S.UnitNumber AND U.UserUnitSlotNumber=S.SlotNumber)<>@Count
       RAISERROR('Inventory changed since preview',16,1);
     IF EXISTS(SELECT 1 FROM #BeforeGB U JOIN #SelectedGB S ON S.UserUnitNumber=U.UserUnitNumber
       WHERE U.UserUnitNumber=@Active OR U.LockState<>0 OR U.BattleShipCount<>0 OR U.UnitLevelNumber>1 OR U.TimeLimit<>0 OR U.RunOut<>0 OR U.Forbidden<>0)
       RAISERROR('Selected unit is protected',16,1);
     IF EXISTS(SELECT S.UnitNumber FROM #SelectedGB S GROUP BY S.UnitNumber
       HAVING (SELECT COUNT(*) FROM #BeforeGB U WHERE U.UnitNumber=S.UnitNumber)-COUNT(*)<@Keep)
       RAISERROR('KeepCopies would be violated',16,1);
     DECLARE @GB bigint;
     SELECT @GB=SUM(CONVERT(bigint,G.DeleteUnitPoint)) FROM #SelectedGB S JOIN dbo.tbl_Units U ON U.UnitNumber=S.UnitNumber
       JOIN dbo.tbl_UnitGradeInfo G ON G.UnitGradeNumber=U.UnitGradeNumber AND G.UnitGradeNumber2=U.UnitGradeNumber2;
     IF (SELECT COUNT(*) FROM #SelectedGB S JOIN dbo.tbl_Units U ON U.UnitNumber=S.UnitNumber
       JOIN dbo.tbl_UnitGradeInfo G ON G.UnitGradeNumber=U.UnitGradeNumber AND G.UnitGradeNumber2=U.UnitGradeNumber2)<>@Count
       RAISERROR('Ambiguous reward catalog',16,1);
     IF @GB IS NULL OR @GB<>@Expected OR EXISTS(SELECT 1 FROM #SelectedGB S LEFT JOIN dbo.tbl_Units U ON U.UnitNumber=S.UnitNumber
       LEFT JOIN dbo.tbl_UnitGradeInfo G ON G.UnitGradeNumber=U.UnitGradeNumber AND G.UnitGradeNumber2=U.UnitGradeNumber2
       WHERE G.DeleteUnitPoint IS NULL OR G.DeleteUnitPoint<=0 OR U.UnitGradeNumber2=1)
       RAISERROR('Reward changed since preview',16,1);
     DECLARE @DeleteUid bigint,@DeleteType int,@OutType int,@Time tinyint,@Shop tinyint,@Run tinyint,@Serial bigint,@Category int,@OutSlot int,@Ret int;
     DECLARE SelectedUnits CURSOR LOCAL STATIC READ_ONLY FOR SELECT UserUnitNumber,UnitNumber FROM #SelectedGB ORDER BY UserUnitNumber;
     OPEN SelectedUnits;FETCH NEXT FROM SelectedUnits INTO @DeleteUid,@DeleteType;
     WHILE @@FETCH_STATUS=0
     BEGIN
       SET @Ret=0;
       EXEC dbo.sp_DeleteUserUnit @AccountNumber,@DeleteUid,@DeleteType,0,@OutType OUTPUT,@Time OUTPUT,@Shop OUTPUT,@Run OUTPUT,@Serial OUTPUT,@Category OUTPUT,@OutSlot OUTPUT,@Ret OUTPUT;
       IF @Ret<>0 RAISERROR('Native dismantle rejected batch',16,1);
       FETCH NEXT FROM SelectedUnits INTO @DeleteUid,@DeleteType;
     END;
     CLOSE SelectedUnits;DEALLOCATE SelectedUnits;
     IF EXISTS(SELECT 1 FROM dbo.tbl_UserUnits U JOIN #SelectedGB S ON S.UserUnitNumber=U.UserUnitNumber WHERE U.AccountNumber=@AccountNumber)
       RAISERROR('Selected unit remained after dismantle',16,1);
     IF EXISTS(SELECT __ALL_COLUMNS__ FROM #BeforeGB WHERE UserUnitNumber NOT IN(SELECT UserUnitNumber FROM #SelectedGB)
       EXCEPT SELECT __ALL_COLUMNS__ FROM dbo.tbl_UserUnits WHERE AccountNumber=@AccountNumber)
       OR EXISTS(SELECT __ALL_COLUMNS__ FROM dbo.tbl_UserUnits WHERE AccountNumber=@AccountNumber
       EXCEPT SELECT __ALL_COLUMNS__ FROM #BeforeGB WHERE UserUnitNumber NOT IN(SELECT UserUnitNumber FROM #SelectedGB))
       RAISERROR('Unselected inventory changed',16,1);
     UPDATE __BILLING_DB__.dbo.tbls_AccountPocket WITH(UPDLOCK,HOLDLOCK) SET f_GP=f_GP+CONVERT(int,@GB)
       WHERE f_idx=@AccountNumber AND @GB<=2147483647 AND f_GP<=2147483647-@GB;
     IF @@ROWCOUNT<>1 RAISERROR('GB wallet unavailable or overflow',16,1);
     IF @Sort=1
     BEGIN
       DECLARE @Changed int,@Total int;
       EXEC dbo.sdgo_mod_SortHangar @AccountNumber,@Id,@Changed OUTPUT,@Total OUTPUT;
     END;
     UPDATE dbo.sdgo_mod_HangarGBJob SET State='done',RemovedUnits=@Count,AwardedGB=@GB,CompletedUtc=SYSUTCDATETIME() WHERE AccountNumber=@AccountNumber AND RequestId=@Id;
   END;
   SET @Result=0;
   IF @Own=1 COMMIT TRANSACTION;
 END TRY
 BEGIN CATCH
   DECLARE @Error nvarchar(200)=LEFT(ERROR_MESSAGE(),200);
   IF XACT_STATE()<>0 ROLLBACK TRANSACTION;
   UPDATE dbo.sdgo_mod_HangarGBJob SET State='failed',LastError=@Error,CompletedUtc=SYSUTCDATETIME() WHERE AccountNumber=@AccountNumber AND RequestId=@Id AND State<>'done';
   SET @Result=5;
 END CATCH;
END;
