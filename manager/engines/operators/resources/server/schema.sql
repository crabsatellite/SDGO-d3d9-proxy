IF OBJECT_ID(N'dbo.sdgo_OperatorProfiles', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorProfiles (
    OperatorID INT NOT NULL PRIMARY KEY,
    CategoryID INT NOT NULL,
    Enabled BIT NOT NULL,
    DeliveryMode VARCHAR(16) NOT NULL,
    DisplayName NVARCHAR(100) NOT NULL,
    Revision CHAR(64) NOT NULL,
    CONSTRAINT CK_sdgo_OperatorDelivery CHECK (DeliveryMode IN ('all_players','product'))
);
IF OBJECT_ID(N'dbo.sdgo_OperatorRules', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorRules (
    OperatorID INT NOT NULL,
    RuleIndex INT NOT NULL,
    CategoryID INT NOT NULL,
    ChanceBps INT NOT NULL CHECK (ChanceBps BETWEEN 0 AND 10000),
    MinAmount INT NOT NULL,
    MaxAmount INT NOT NULL,
    PRIMARY KEY (OperatorID,RuleIndex),
    CHECK (MinAmount BETWEEN 1 AND 65535 AND MaxAmount BETWEEN MinAmount AND 65535)
);
IF COL_LENGTH(N'dbo.sdgo_OperatorProfiles',N'FixedName') IS NULL
    ALTER TABLE dbo.sdgo_OperatorProfiles ADD FixedName BIT NOT NULL
        CONSTRAINT DF_sdgo_OperatorFixedName DEFAULT 0;
IF OBJECT_ID(N'dbo.sdgo_OperatorFixedNameBackup',N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorFixedNameBackup (
    UserOperatorNumber BIGINT NOT NULL PRIMARY KEY,
    AccountNumber INT NOT NULL,
    OperatorNumber INT NOT NULL,
    OperatorNickname VARCHAR(16) NOT NULL,
    IsSetName TINYINT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorProcedureBackup', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorProcedureBackup (
    ProcedureName SYSNAME NOT NULL PRIMARY KEY,
    OriginalDefinition NVARCHAR(MAX) NOT NULL,
    InstalledHash CHAR(64) NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorRuleItems', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorRuleItems (
    OperatorID INT NOT NULL,
    RuleIndex INT NOT NULL,
    ItemIndex INT NOT NULL,
    CategoryID INT NOT NULL,
    PRIMARY KEY(OperatorID,RuleIndex,ItemIndex),
    UNIQUE(OperatorID,RuleIndex,CategoryID)
);
IF OBJECT_ID(N'dbo.sdgo_OperatorLegacyNameBackup', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorLegacyNameBackup (
    UserOperatorNumber BIGINT NOT NULL PRIMARY KEY,
    AccountNumber INT NOT NULL,
    OperatorNickname VARCHAR(16) NOT NULL,
    IsSetName TINYINT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorLegacyEntitlementBackup', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorLegacyEntitlementBackup (
    UserOperatorNumber BIGINT NOT NULL PRIMARY KEY,
    AccountNumber INT NOT NULL,
    OperatorNumber INT NOT NULL,
    OPType TINYINT NOT NULL,
    RunOut TINYINT NOT NULL,
    DateEnd DATETIME NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorDataBackup', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorDataBackup (
    OperatorID INT NOT NULL PRIMARY KEY,
    WasPresent BIT NOT NULL,
    OriginalDropType TINYINT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorItemRegistry', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorItemRegistry (
    CategoryID INT NOT NULL PRIMARY KEY,
    OperatorID INT NOT NULL,
    OwnedCategory BIT NOT NULL
);
IF OBJECT_ID(N'dbo.sdgo_OperatorGrantIssues', N'U') IS NULL
CREATE TABLE dbo.sdgo_OperatorGrantIssues (
    AccountNumber INT NOT NULL,
    OperatorID INT NOT NULL,
    ReturnCode INT NOT NULL,
    LastAttempt DATETIME NOT NULL,
    PRIMARY KEY(AccountNumber,OperatorID)
);
