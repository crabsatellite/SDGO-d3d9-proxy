#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace sdgo::hangar_capacity {

inline constexpr int kBaseSlots = 24;
inline constexpr int kTargetTotal = 1200;
inline constexpr int kTargetExtra = kTargetTotal - kBaseSlots;
inline constexpr int kMaxLockTimeoutMs = 30000;

inline std::optional<std::int16_t> SelectTarget(std::int64_t raw) {
    if (raw < 0 || raw > std::numeric_limits<std::int16_t>::max()) {
        return std::nullopt;
    }
    const auto target = raw < kTargetExtra ? kTargetExtra : raw;
    return static_cast<std::int16_t>(target);
}

namespace detail {

inline std::optional<std::wstring> SqlLiteral(std::wstring_view value) {
    if (value.empty() || value.size() > 128) {
        return std::nullopt;
    }

    std::wstring result = L"N'";
    result.reserve(value.size() + 3);
    for (const wchar_t character : value) {
        if (character == L'\0') {
            return std::nullopt;
        }
        result += character;
        if (character == L'\'') {
            result += L'\'';
        }
    }
    result += L'\'';
    return result;
}

} // namespace detail

inline std::optional<std::wstring> MakeEnsureSql(
    std::wstring_view expectedDatabase,
    std::int64_t account,
    int lockTimeoutMs) {
    const auto database = detail::SqlLiteral(expectedDatabase);
    if (!database ||
        account <= 0 ||
        account > std::numeric_limits<std::int32_t>::max() ||
        lockTimeoutMs < 0 ||
        lockTimeoutMs > kMaxLockTimeoutMs) {
        return std::nullopt;
    }

    const std::wstring accountText = std::to_wstring(account);
    const std::wstring timeoutText = std::to_wstring(lockTimeoutMs);
    std::wstring sql;
    sql.reserve(7000);
    sql += LR"SQL(DECLARE @hc_saved_options int = @@OPTIONS;
DECLARE @hc_own_transaction bit = 0;

IF @@TRANCOUNT <> 0
BEGIN
    RAISERROR(N'hangar capacity requires no ambient transaction', 16, 1);
    RETURN;
END;

IF (@hc_saved_options & 2) <> 0
BEGIN
    RAISERROR(N'hangar capacity does not support IMPLICIT_TRANSACTIONS', 16, 1);
    RETURN;
END;

IF DB_ID() <= 4 OR DB_ID()SQL";
    sql += *database;
    sql += LR"SQL() IS NULL OR DB_ID() <> DB_ID()SQL";
    sql += *database;
    sql += LR"SQL()
BEGIN
    RAISERROR(N'hangar capacity database identity mismatch', 16, 1);
    RETURN;
END;

BEGIN TRY
    SET NOCOUNT ON;
    SET XACT_ABORT ON;

    BEGIN TRANSACTION;
    SET @hc_own_transaction = 1;

    DECLARE @hc_lock_result int;
    EXEC @hc_lock_result = sys.sp_getapplock
        @Resource = N'sdgo.hangar_capacity.ensure.v1',
        @LockMode = N'Exclusive',
        @LockOwner = N'Transaction',
        @LockTimeout = )SQL";
    sql += timeoutText;
    sql += LR"SQL(,
        @DbPrincipal = N'public';
    IF @hc_lock_result < 0
        RAISERROR(N'hangar capacity application lock unavailable (%d)', 16, 1, @hc_lock_result);

    IF OBJECT_ID(N'dbo.tbl_Users', N'U') IS NULL
        RAISERROR(N'hangar capacity requires dbo.tbl_Users', 16, 1);
    IF OBJECT_ID(N'dbo.tbl_UserUnitSlotNumberList', N'U') IS NULL
        RAISERROR(N'hangar capacity requires dbo.tbl_UserUnitSlotNumberList', 16, 1);

    IF NOT EXISTS (
        SELECT 1
        FROM sys.columns
        WHERE object_id = OBJECT_ID(N'dbo.tbl_Users', N'U')
          AND name = N'AccountNumber'
          AND system_type_id = 56
          AND user_type_id = 56
          AND max_length = 4
          AND is_computed = 0)
        RAISERROR(N'hangar capacity requires dbo.tbl_Users.AccountNumber int', 16, 1);

    IF NOT EXISTS (
        SELECT 1
        FROM sys.columns
        WHERE object_id = OBJECT_ID(N'dbo.tbl_Users', N'U')
          AND name = N'ExpandSlotCount'
          AND system_type_id = 56
          AND user_type_id = 56
          AND max_length = 4
          AND is_computed = 0 AND is_identity = 0)
        RAISERROR(N'hangar capacity requires dbo.tbl_Users.ExpandSlotCount int', 16, 1);

    IF NOT EXISTS (
        SELECT 1
        FROM sys.columns
        WHERE object_id = OBJECT_ID(N'dbo.tbl_UserUnitSlotNumberList', N'U')
          AND name = N'UserUnitSlotNumber'
          AND system_type_id = 56
          AND user_type_id = 56
          AND max_length = 4
          AND is_computed = 0 AND is_identity = 0)
        RAISERROR(N'hangar capacity requires dbo.tbl_UserUnitSlotNumberList.UserUnitSlotNumber int', 16, 1);

    IF EXISTS (
        SELECT 1
        FROM sys.triggers AS capacity_trigger
        JOIN sys.trigger_events AS trigger_event
          ON trigger_event.object_id = capacity_trigger.object_id
        WHERE capacity_trigger.is_disabled = 0
          AND ((capacity_trigger.parent_id = OBJECT_ID(N'dbo.tbl_Users')
                AND trigger_event.type_desc = N'UPDATE')
            OR (capacity_trigger.parent_id = OBJECT_ID(N'dbo.tbl_UserUnitSlotNumberList')
                AND trigger_event.type_desc = N'INSERT')))
        RAISERROR(N'hangar capacity does not support enabled triggers on capacity tables', 16, 1);

    DECLARE @hc_getter_id int;
    SET @hc_getter_id = OBJECT_ID(N'dbo.sp_GetCountUserUnits', N'P');
    IF @hc_getter_id IS NULL
        RAISERROR(N'hangar capacity requires dbo.sp_GetCountUserUnits', 16, 1);
    IF (SELECT COUNT(*) FROM sys.parameters WHERE object_id = @hc_getter_id) <> 3
        RAISERROR(N'hangar capacity getter requires three parameters', 16, 1);
    IF NOT EXISTS (
        SELECT 1
        FROM sys.parameters
        WHERE object_id = @hc_getter_id
          AND parameter_id = 1
          AND system_type_id = 56
          AND user_type_id = 56
          AND max_length = 4
          AND is_output = 0)
        RAISERROR(N'hangar capacity getter parameter 1 must be int input', 16, 1);
    IF NOT EXISTS (
        SELECT 1
        FROM sys.parameters
        WHERE object_id = @hc_getter_id
          AND parameter_id = 2
          AND system_type_id = 52
          AND user_type_id = 52
          AND max_length = 2
          AND is_output = 1)
        RAISERROR(N'hangar capacity getter parameter 2 must be smallint output', 16, 1);
    IF NOT EXISTS (
        SELECT 1
        FROM sys.parameters
        WHERE object_id = @hc_getter_id
          AND parameter_id = 3
          AND system_type_id = 56
          AND user_type_id = 56
          AND max_length = 4
          AND is_output = 1)
        RAISERROR(N'hangar capacity getter parameter 3 must be int output', 16, 1);

    DECLARE @hc_account_rows bigint;
    DECLARE @hc_raw int;
    SELECT @hc_account_rows = COUNT_BIG(*)
    FROM dbo.tbl_Users WITH (UPDLOCK, HOLDLOCK)
    WHERE AccountNumber = )SQL";
    sql += accountText;
    sql += LR"SQL(;
    IF @hc_account_rows <> 1
        RAISERROR(N'hangar capacity account must exist exactly once', 16, 1);

    SELECT @hc_raw = ExpandSlotCount
    FROM dbo.tbl_Users WITH (UPDLOCK, HOLDLOCK)
    WHERE AccountNumber = )SQL";
    sql += accountText;
    sql += LR"SQL(;
    IF @hc_raw IS NULL OR @hc_raw < 0 OR @hc_raw > 32767
        RAISERROR(N'hangar capacity raw value is outside SMALLINT range', 16, 1);

    IF EXISTS (
        SELECT 1
        FROM dbo.tbl_UserUnitSlotNumberList WITH (UPDLOCK, HOLDLOCK)
        WHERE UserUnitSlotNumber IS NULL
           OR UserUnitSlotNumber < 1
           OR UserUnitSlotNumber > 32767)
        RAISERROR(N'hangar capacity slot-number pool is invalid', 16, 1);

    DECLARE @hc_target smallint;
    SET @hc_target = CASE WHEN @hc_raw < 1176 THEN 1176 ELSE @hc_raw END;

    ;WITH hc_slots(UserUnitSlotNumber) AS (
        SELECT 1
        UNION ALL
        SELECT UserUnitSlotNumber + 1
        FROM hc_slots
        WHERE UserUnitSlotNumber < 1200)
    INSERT dbo.tbl_UserUnitSlotNumberList(UserUnitSlotNumber)
    SELECT slots.UserUnitSlotNumber
    FROM hc_slots AS slots
    WHERE NOT EXISTS (
        SELECT 1
        FROM dbo.tbl_UserUnitSlotNumberList AS existing WITH (UPDLOCK, HOLDLOCK)
        WHERE existing.UserUnitSlotNumber = slots.UserUnitSlotNumber)
    OPTION (MAXRECURSION 1200);

    UPDATE dbo.tbl_Users
    SET ExpandSlotCount = CONVERT(int, @hc_target)
    WHERE AccountNumber = )SQL";
    sql += accountText;
    sql += LR"SQL(
      AND ExpandSlotCount < CONVERT(int, @hc_target);

    IF (SELECT COUNT(DISTINCT UserUnitSlotNumber)
        FROM dbo.tbl_UserUnitSlotNumberList
        WHERE UserUnitSlotNumber BETWEEN 1 AND 1200) <> 1200
        RAISERROR(N'hangar capacity slot-number fill readback failed', 16, 1);

    IF NOT EXISTS (
        SELECT 1
        FROM dbo.tbl_Users
        WHERE AccountNumber = )SQL";
    sql += accountText;
    sql += LR"SQL(
          AND ExpandSlotCount = CONVERT(int, @hc_target))
        RAISERROR(N'hangar capacity account readback failed', 16, 1);

    COMMIT TRANSACTION;
    SET @hc_own_transaction = 0;

    IF (@hc_saved_options & 16384) = 0 SET XACT_ABORT OFF;
    IF (@hc_saved_options & 512) = 0 SET NOCOUNT OFF;
END TRY
BEGIN CATCH
    DECLARE @hc_error nvarchar(2048);
    SET @hc_error = ERROR_MESSAGE();
    IF @hc_own_transaction = 1 AND XACT_STATE() <> 0
        ROLLBACK TRANSACTION;
    SET @hc_own_transaction = 0;
    IF (@hc_saved_options & 16384) = 0 SET XACT_ABORT OFF;
    IF (@hc_saved_options & 512) = 0 SET NOCOUNT OFF;
    RAISERROR(N'hangar capacity policy failed: %s', 16, 1, @hc_error);
    RETURN;
END CATCH;
)SQL";
    return sql;
}

} // namespace sdgo::hangar_capacity
