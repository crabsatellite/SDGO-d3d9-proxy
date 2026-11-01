CREATE TRIGGER dbo.sdgo_OperatorFixedNames ON dbo.tbl_UserOperators
AFTER INSERT,UPDATE AS
BEGIN
    -- SDGO_OPERATOR_FIXED_NAMES_V1
    SET NOCOUNT ON;
    IF TRIGGER_NESTLEVEL(OBJECT_ID(N'dbo.sdgo_OperatorFixedNames'))>1 RETURN;
    UPDATE U SET OperatorNickname='@SDGO:'+CONVERT(VARCHAR(5),U.OperatorNumber),IsSetName=1
    FROM dbo.tbl_UserOperators U
    JOIN inserted I ON I.UserOperatorNumber=U.UserOperatorNumber
    JOIN dbo.sdgo_OperatorProfiles P ON P.OperatorID=U.OperatorNumber
    WHERE P.Enabled=1 AND P.FixedName=1 AND ISNULL(U.Forbidden,0)=0
      AND (U.IsSetName<>1 OR U.OperatorNickname COLLATE Latin1_General_100_BIN2<>'@SDGO:'+CONVERT(VARCHAR(5),U.OperatorNumber));
END;
