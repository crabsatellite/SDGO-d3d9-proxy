IF SCHEMA_ID(N'sdgo_collection_capsule') IS NULL
BEGIN
    EXEC(N'CREATE SCHEMA sdgo_collection_capsule AUTHORIZATION dbo');
    EXEC sys.sp_addextendedproperty @name=N'sdgo_owner', @value=N'collection_capsule/v1',
        @level0type=N'SCHEMA', @level0name=N'sdgo_collection_capsule';
END;
IF ISNULL((SELECT CONVERT(nvarchar(128),value) FROM sys.extended_properties
           WHERE class=3 AND major_id=SCHEMA_ID(N'sdgo_collection_capsule') AND name=N'sdgo_owner'),N'')<>N'collection_capsule/v1'
    RAISERROR('Collection schema belongs to another owner',16,1);

IF OBJECT_ID(N'sdgo_collection_capsule.State',N'U') IS NULL
    CREATE TABLE sdgo_collection_capsule.State (
        Singleton int NOT NULL PRIMARY KEY CHECK(Singleton=1),
        SchemaVersion int NOT NULL,
        MachineID int NOT NULL,
        PriceGP int NOT NULL CHECK(PriceGP=100000),
        PoolHash char(64) NOT NULL,
        Enabled bit NOT NULL,
        InstalledAt datetime NOT NULL DEFAULT GETUTCDATE()
    );
IF OBJECT_ID(N'sdgo_collection_capsule.Roots',N'U') IS NULL
    CREATE TABLE sdgo_collection_capsule.Roots (
        UnitID int NOT NULL PRIMARY KEY,
        UnitRank tinyint NOT NULL,
        UnitSubgrade tinyint NOT NULL,
        Origin varchar(16) NOT NULL DEFAULT 'native'
    );
IF OBJECT_ID(N'sdgo_collection_capsule.UnitSource',N'U') IS NULL
    CREATE TABLE sdgo_collection_capsule.UnitSource (
        UnitID int NOT NULL PRIMARY KEY,
        TransformTo int NOT NULL,
        Ready int NOT NULL,
        UnitRank int NOT NULL,
        UnitSubgrade int NOT NULL,
        UnitGraph int NOT NULL,
        Placeholder bit NOT NULL,
        Origin varchar(16) NOT NULL DEFAULT 'native'
    );
IF OBJECT_ID(N'sdgo_collection_capsule.ImportedForms',N'U') IS NULL
    CREATE TABLE sdgo_collection_capsule.ImportedForms (
        UnitID int NOT NULL PRIMARY KEY,
        RootUnitID int NOT NULL,
        PackageID nvarchar(128) NOT NULL,
        RevisionHash char(64) NOT NULL
    );
IF OBJECT_ID(N'sdgo_collection_capsule.OwnedUnitItems',N'U') IS NULL
    CREATE TABLE sdgo_collection_capsule.OwnedUnitItems (
        UnitID int NOT NULL PRIMARY KEY,
        CategoryID int NOT NULL UNIQUE CHECK(CategoryID BETWEEN 1180000 AND 1199999),
        Name nvarchar(48) NOT NULL
    );
IF OBJECT_ID(N'sdgo_collection_capsule.UnitDelivery',N'U') IS NULL
    CREATE TABLE sdgo_collection_capsule.UnitDelivery (
        UnitID int NOT NULL PRIMARY KEY,
        CategoryID int NOT NULL UNIQUE
    );
IF OBJECT_ID(N'sdgo_collection_capsule.Modules',N'U') IS NULL
    CREATE TABLE sdgo_collection_capsule.Modules (
        ObjectName nvarchar(128) NOT NULL PRIMARY KEY,
        InstalledHash char(64) NOT NULL,
        InstalledDefinition nvarchar(max) NOT NULL
    );
IF OBJECT_ID(N'sdgo_collection_capsule.CatalogUnlocks',N'U') IS NULL
    CREATE TABLE sdgo_collection_capsule.CatalogUnlocks (
        AccountNumber int NOT NULL,
        UnitID int NOT NULL,
        PendingTransactionID bigint NOT NULL DEFAULT 0,
        PRIMARY KEY(AccountNumber,UnitID)
    );
