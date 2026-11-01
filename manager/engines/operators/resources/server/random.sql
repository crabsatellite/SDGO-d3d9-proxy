CREATE PROCEDURE dbo.sdgo_OperatorUniform
    @Bound INT,
    @Value INT OUTPUT
AS
BEGIN
    SET NOCOUNT ON;
    IF @Bound < 1 OR @Bound > 65535 THROW 51010, 'Invalid random bound', 1;
    DECLARE @Sample BIGINT, @Limit BIGINT;
    SET @Limit = 4294967296 - (CAST(4294967296 AS BIGINT) % @Bound);
    SET @Sample = CONVERT(BIGINT, CONVERT(BINARY(4), CRYPT_GEN_RANDOM(4)));
    WHILE @Sample >= @Limit
        SET @Sample = CONVERT(BIGINT, CONVERT(BINARY(4), CRYPT_GEN_RANDOM(4)));
    SET @Value = CONVERT(INT,@Sample % @Bound);
END;
