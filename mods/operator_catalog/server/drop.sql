CREATE PROCEDURE dbo.sdgo_OperatorManagedDrop
    @AccountNumber INT,
    @UserOperatorNumber BIGINT,
    @CategoryID INT OUTPUT,
    @Amount INT OUTPUT,
    @RetVal INT OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    DECLARE @OperatorID INT;
    SET @CategoryID = 1070001;
    SET @Amount = 1; -- native no-reward sentinel carries a positive amount
    SET @RetVal = 1;
    SELECT @OperatorID = OperatorNumber
    FROM dbo.tbl_UserOperators
    WHERE AccountNumber=@AccountNumber AND UserOperatorNumber=@UserOperatorNumber
      AND RunOut=0 AND ISNULL(Forbidden,0)=0
      AND (OPType<>1 OR DateEnd>=GETDATE()) AND (OPType<>2 OR RemainCount>0);
    IF @OperatorID IS NULL RETURN;
    IF NOT EXISTS(SELECT 1 FROM dbo.sdgo_OperatorProfiles WHERE OperatorID=@OperatorID AND Enabled=1) RETURN;
    DECLARE @Roll INT,@QuantityRoll INT,@Min INT,@Max INT,@Chosen INT,@RuleIndex INT;
    EXEC dbo.sdgo_OperatorUniform 10000,@Roll OUTPUT;
    SELECT TOP 1 @Chosen=R.CategoryID,@Min=R.MinAmount,@Max=R.MaxAmount,@RuleIndex=R.RuleIndex
    FROM dbo.sdgo_OperatorRules R
    WHERE R.OperatorID=@OperatorID AND
      (SELECT SUM(R2.ChanceBps) FROM dbo.sdgo_OperatorRules R2
       WHERE R2.OperatorID=R.OperatorID AND R2.RuleIndex<=R.RuleIndex)>@Roll
    ORDER BY R.RuleIndex;
    IF @Chosen IS NULL BEGIN SET @RetVal=0; RETURN; END;
    DECLARE @PoolCount INT,@ItemRoll INT;
    SELECT @PoolCount=COUNT(*) FROM dbo.sdgo_OperatorRuleItems
    WHERE OperatorID=@OperatorID AND RuleIndex=@RuleIndex;
    IF @PoolCount>0
    BEGIN
        EXEC dbo.sdgo_OperatorUniform @PoolCount,@ItemRoll OUTPUT;
        SELECT @Chosen=CategoryID FROM dbo.sdgo_OperatorRuleItems
        WHERE OperatorID=@OperatorID AND RuleIndex=@RuleIndex AND ItemIndex=@ItemRoll;
    END;
    DECLARE @Span INT;
    SET @Span=@Max-@Min+1;
    EXEC dbo.sdgo_OperatorUniform @Span,@QuantityRoll OUTPUT;
    SET @CategoryID=@Chosen;
    SET @Amount=@Min+@QuantityRoll;
    INSERT dbo.tbl_UserDropItemBox(AccountNumber,ItemCategory,Amount,DropConditionID)
    VALUES(@AccountNumber,@CategoryID,@Amount,5000001);
    SET @RetVal=0;
END;
