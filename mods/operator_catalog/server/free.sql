CREATE PROCEDURE dbo.sdgo_EnsureFreeOperators
    @AccountNumber INT
AS
BEGIN
    SET NOCOUNT ON;
    SET XACT_ABORT ON;
    IF NOT EXISTS(SELECT 1 FROM dbo.tbl_Users WHERE AccountNumber=@AccountNumber) RETURN;
    DECLARE @OperatorID INT=0,@CategoryID INT,@Existing BIGINT,@Lock INT;
    DECLARE @Slot INT,@UserOp BIGINT,@Serial BIGINT,@RC INT;
    DECLARE @Resource NVARCHAR(255)=N'SDGO:operator-grants:'+CONVERT(NVARCHAR(20),@AccountNumber);
    BEGIN TRY
        BEGIN TRANSACTION;
        EXEC @Lock=sys.sp_getapplock @Resource=@Resource,@LockMode='Exclusive',
            @LockOwner='Transaction',@LockTimeout=2000;
        IF @Lock<0 BEGIN ROLLBACK TRANSACTION; RETURN; END;
        WHILE 1=1
        BEGIN
            SELECT TOP 1 @OperatorID=OperatorID,@CategoryID=CategoryID
            FROM dbo.sdgo_OperatorProfiles
            WHERE OperatorID>@OperatorID AND Enabled=1 AND DeliveryMode='all_players'
            ORDER BY OperatorID;
            IF @@ROWCOUNT=0 BREAK;
            IF EXISTS(SELECT 1 FROM dbo.tbl_UserOperators WHERE AccountNumber=@AccountNumber
                      AND OperatorNumber=@OperatorID AND ISNULL(Forbidden,0)=0 AND OPType=0 AND RunOut=0) CONTINUE;
            -- Existing personal nicknames, active selection and paid records stay
            -- intact. A free permanent entitlement upgrades one matching record.
            SET @Existing=NULL;
            SELECT TOP 1 @Existing=UserOperatorNumber FROM dbo.tbl_UserOperators
            WHERE AccountNumber=@AccountNumber AND OperatorNumber=@OperatorID AND ISNULL(Forbidden,0)=0
            ORDER BY UserOperatorNumber;
            IF @Existing IS NOT NULL
            BEGIN
                UPDATE dbo.tbl_UserOperators SET OPType=0,RunOut=0,DateEnd=NULL
                WHERE UserOperatorNumber=@Existing AND AccountNumber=@AccountNumber;
                CONTINUE;
            END;
            IF EXISTS(SELECT 1 FROM dbo.tbl_UserOperators WHERE AccountNumber=@AccountNumber
                      AND OperatorNumber=@OperatorID AND Forbidden=1) CONTINUE;
            IF NOT EXISTS(SELECT 1 FROM dbo.tbl_UserOperatorSlotNumberList S
                          WHERE S.UserOperatorSlotNumber BETWEEN 1 AND 100
                          AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserOperators U
                                         WHERE U.AccountNumber=@AccountNumber AND U.SlotNumber=S.UserOperatorSlotNumber))
            BEGIN
                UPDATE dbo.sdgo_OperatorGrantIssues SET ReturnCode=1,LastAttempt=GETDATE()
                WHERE AccountNumber=@AccountNumber AND OperatorID=@OperatorID;
                IF @@ROWCOUNT=0 INSERT dbo.sdgo_OperatorGrantIssues VALUES(@AccountNumber,@OperatorID,1,GETDATE());
                CONTINUE;
            END;
            SET @Slot=0; SET @UserOp=0; SET @Serial=0;
            EXEC @RC=dbo.sp_CreateUserOperator @AccountNumber,'',0,@CategoryID,NULL,0,@OperatorID,0,6,
                @Slot OUTPUT,@UserOp OUTPUT,@Serial OUTPUT;
            IF @RC=0 AND (@Slot<1 OR @Slot>100 OR
                (SELECT COUNT(*) FROM dbo.tbl_UserOperators WHERE AccountNumber=@AccountNumber AND SlotNumber=@Slot)<>1)
                THROW 51013,'Native operator slot allocation changed during grant',1;
            IF @RC<>0
            BEGIN
                UPDATE dbo.sdgo_OperatorGrantIssues SET ReturnCode=@RC,LastAttempt=GETDATE()
                WHERE AccountNumber=@AccountNumber AND OperatorID=@OperatorID;
                IF @@ROWCOUNT=0 INSERT dbo.sdgo_OperatorGrantIssues VALUES(@AccountNumber,@OperatorID,@RC,GETDATE());
            END;
        END;
        COMMIT TRANSACTION;
    END TRY
    BEGIN CATCH
        IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;
        -- A full inventory or an unavailable grant must not break native login.
        RETURN;
    END CATCH;
END;
