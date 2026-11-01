#include "../mods/hangar_capacity/capacity_policy.h"

#include <cstdio>
#include <limits>
#include <string>

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

bool Contains(const std::wstring& value, std::wstring_view expected) {
    return value.find(expected) != std::wstring::npos;
}

bool Excludes(const std::wstring& value, std::wstring_view rejected) {
    return value.find(rejected) == std::wstring::npos;
}

} // namespace

int main() {
    using namespace sdgo::hangar_capacity;

    CHECK(kBaseSlots == 24);
    CHECK(kTargetTotal == 1200);
    CHECK(kTargetExtra == 1176);
    CHECK(kBaseSlots + kTargetExtra == kTargetTotal);

    CHECK(SelectTarget(0) == kTargetExtra);
    CHECK(SelectTarget(420) == kTargetExtra);
    CHECK(SelectTarget(1175) == kTargetExtra);
    CHECK(SelectTarget(1176) == 1176);
    CHECK(SelectTarget(1200) == 1200);
    CHECK(SelectTarget(std::numeric_limits<std::int16_t>::max()) ==
          std::numeric_limits<std::int16_t>::max());
    CHECK(!SelectTarget(-1));
    CHECK(!SelectTarget(
        static_cast<std::int64_t>(std::numeric_limits<std::int16_t>::max()) + 1));

    CHECK(!MakeEnsureSql(L"", 1, 1000));
    CHECK(!MakeEnsureSql(std::wstring(129, L'x'), 1, 1000));
    const wchar_t embeddedNull[] = {L'G', L'\0', L'D'};
    CHECK(!MakeEnsureSql(std::wstring_view(embeddedNull, 3), 1, 1000));
    CHECK(!MakeEnsureSql(L"GOnlineGame", 0, 1000));
    CHECK(!MakeEnsureSql(L"GOnlineGame", -1, 1000));
    CHECK(!MakeEnsureSql(
        L"GOnlineGame",
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1,
        1000));
    CHECK(!MakeEnsureSql(L"GOnlineGame", 1, -1));
    CHECK(!MakeEnsureSql(L"GOnlineGame", 1, kMaxLockTimeoutMs + 1));

    const auto generated = MakeEnsureSql(L"Game's \u30C6\u30B9\u30C8", 2147483647, 1000);
    CHECK(generated.has_value());
    if (generated) {
        const auto& sql = *generated;
        CHECK(Contains(sql, L"DB_ID(N'Game''s \u30C6\u30B9\u30C8')"));
        CHECK(Contains(sql, L"WHERE AccountNumber = 2147483647"));
        CHECK(Contains(sql, L"@LockTimeout = 1000"));

        CHECK(Contains(sql, L"IF @@TRANCOUNT <> 0"));
        CHECK(Contains(sql, L"(@hc_saved_options & 2) <> 0"));
        CHECK(Contains(
            sql,
            L"IF DB_ID() <= 4 OR DB_ID(N'Game''s \u30C6\u30B9\u30C8') IS NULL OR "
            L"DB_ID() <> DB_ID(N'Game''s \u30C6\u30B9\u30C8')"));
        CHECK(Contains(sql, L"BEGIN TRANSACTION"));
        CHECK(Contains(sql, L"sp_getapplock"));
        CHECK(Contains(sql, L"@LockOwner = N'Transaction'"));
        CHECK(Contains(sql, L"COMMIT TRANSACTION"));
        CHECK(Contains(sql, L"ROLLBACK TRANSACTION"));
        CHECK(Contains(sql, L"SET XACT_ABORT ON"));
        CHECK(Contains(sql, L"SET XACT_ABORT OFF"));
        CHECK(Contains(sql, L"SET NOCOUNT ON"));
        CHECK(Contains(sql, L"SET NOCOUNT OFF"));

        CHECK(Contains(sql, L"OBJECT_ID(N'dbo.sp_GetCountUserUnits', N'P')"));
        CHECK(Contains(sql, L"parameter_id = 1"));
        CHECK(Contains(sql, L"parameter_id = 2"));
        CHECK(Contains(sql, L"system_type_id = 52"));
        CHECK(Contains(sql, L"max_length = 2"));
        CHECK(Contains(sql, L"is_output = 1"));
        CHECK(Contains(sql, L"parameter_id = 3"));
        CHECK(Contains(sql, L"@hc_raw < 0 OR @hc_raw > 32767"));
        CHECK(Contains(sql, L"CASE WHEN @hc_raw < 1176 THEN 1176 ELSE @hc_raw END"));
        CHECK(Contains(sql, L"INSERT dbo.tbl_UserUnitSlotNumberList"));
        CHECK(Contains(sql, L"WHERE UserUnitSlotNumber < 1200"));
        CHECK(Contains(sql, L"UPDATE dbo.tbl_Users"));
        CHECK(Contains(sql, L"AND ExpandSlotCount < CONVERT(int, @hc_target)"));
        CHECK(Contains(sql, L"COUNT(DISTINCT UserUnitSlotNumber)"));
        CHECK(Contains(sql, L"ExpandSlotCount = CONVERT(int, @hc_target)"));

        CHECK(Excludes(sql, L"DELETE "));
        CHECK(Excludes(sql, L"MERGE "));
        CHECK(Excludes(sql, L"CREATE TRIGGER"));
        CHECK(Excludes(sql, L"CREATE PROCEDURE"));
        CHECK(Excludes(sql, L"ALTER TABLE"));
        CHECK(Excludes(sql, L"tbl_UserUnits"));
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
