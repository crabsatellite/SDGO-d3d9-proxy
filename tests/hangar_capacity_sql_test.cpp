#define DBINITCONSTANTS
#include <windows.h>
#include <msdasc.h>
#include <oledb.h>
#include <transact.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "../mods/hangar_capacity/oledb_session.h"

namespace {
using sdgo::hangar_capacity::ComPtr;
int failures = 0, nativeCalls = 0;
void Check(bool ok, const char* message) { std::printf("%s %s\n", ok ? "ok  " : "FAIL", message); if (!ok) ++failures; }
struct NativeConnection { BYTE pad[8]{}; ICommandText* command = nullptr; BYTE rest[0x240 - 12]{}; int state = 0; };
static_assert(offsetof(NativeConnection, command) == 8 && offsetof(NativeConnection, state) == 0x240);
#pragma pack(push,1)
struct Args { std::int32_t account; std::int16_t extra; std::int32_t result; };
#pragma pack(pop)
static_assert(sizeof(Args) == 10);

bool Execute(IDBCreateCommand* factory, const std::wstring& sql) {
    ComPtr<ICommandText> command;
    if (FAILED(factory->CreateCommand(nullptr, __uuidof(ICommandText), reinterpret_cast<IUnknown**>(command.put()))) ||
        FAILED(command->SetCommandText(DBGUID_DBSQL, sql.c_str()))) return false;
    DBROWCOUNT affected = 0;
    return SUCCEEDED(command->Execute(nullptr, IID_NULL, nullptr, &affected, nullptr));
}
bool Scalar(IDBCreateCommand* factory, const std::wstring& sql, std::wstring& out) {
    ComPtr<ICommandText> command; ComPtr<IMultipleResults> results; ComPtr<IRowset> rows;
    DBROWCOUNT affected = 0;
    if (FAILED(factory->CreateCommand(nullptr, __uuidof(ICommandText), reinterpret_cast<IUnknown**>(command.put()))) ||
        FAILED(command->SetCommandText(DBGUID_DBSQL, sql.c_str())) ||
        FAILED(command->Execute(nullptr, __uuidof(IMultipleResults), nullptr, &affected, reinterpret_cast<IUnknown**>(results.put()))) || !results.get()) return false;
    for (;;) {
        const auto hr = results->GetResult(nullptr, DBRESULTFLAG_DEFAULT, __uuidof(IRowset), &affected, reinterpret_cast<IUnknown**>(rows.put()));
        if (FAILED(hr) || hr == DB_S_NORESULT) return false;
        if (rows.get()) break;
    }
    ComPtr<IAccessor> access;
    if (FAILED(rows->QueryInterface(__uuidof(IAccessor), reinterpret_cast<void**>(access.put())))) return false;
    struct Cell { DBSTATUS status; DBLENGTH length; wchar_t text[256]; } cell{};
    DBBINDING binding{}; binding.iOrdinal = 1; binding.obStatus = offsetof(Cell, status); binding.obLength = offsetof(Cell, length);
    binding.obValue = offsetof(Cell, text); binding.dwPart = DBPART_VALUE | DBPART_STATUS | DBPART_LENGTH;
    binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED; binding.wType = DBTYPE_WSTR; binding.cbMaxLen = sizeof(cell.text);
    HACCESSOR handle = DB_NULL_HACCESSOR; DBBINDSTATUS status = DBBINDSTATUS_BADBINDINFO;
    if (FAILED(access->CreateAccessor(DBACCESSOR_ROWDATA, 1, &binding, sizeof(cell), &handle, &status)) || status != DBBINDSTATUS_OK) return false;
    DBCOUNTITEM count = 0; HROW* data = nullptr;
    auto hr = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 1, &count, &data);
    if (SUCCEEDED(hr) && count == 1) hr = rows->GetData(data[0], handle, &cell);
    const bool ok = SUCCEEDED(hr) && count == 1 && cell.status == DBSTATUS_S_OK && cell.length < sizeof(cell.text);
    if (ok) out.assign(cell.text, cell.length / sizeof(wchar_t));
    if (count && data) rows->ReleaseRows(count, data, nullptr, nullptr, nullptr);
    CoTaskMemFree(data); access->ReleaseAccessor(handle, nullptr);
    return ok;
}
struct Connection {
    ComPtr<IDBInitialize> source; ComPtr<IDBCreateCommand> factory; ComPtr<ICommandText> command; NativeConnection native;
    bool Open() {
        wchar_t setting[4096]{};
        if (!GetEnvironmentVariableW(L"SDGO_HANGAR_TEST_CONNECTION", setting, 4096)) return false;
        ComPtr<IDataInitialize> init; ComPtr<IDBCreateSession> sessions;
        const bool ok = SUCCEEDED(CoCreateInstance(CLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IDataInitialize), reinterpret_cast<void**>(init.put()))) &&
            SUCCEEDED(init->GetDataSource(nullptr, CLSCTX_INPROC_SERVER, setting, __uuidof(IDBInitialize), reinterpret_cast<IUnknown**>(source.put()))) &&
            SUCCEEDED(source->Initialize()) && SUCCEEDED(source->QueryInterface(__uuidof(IDBCreateSession), reinterpret_cast<void**>(sessions.put()))) &&
            SUCCEEDED(sessions->CreateSession(nullptr, __uuidof(IDBCreateCommand), reinterpret_cast<IUnknown**>(factory.put()))) &&
            SUCCEEDED(factory->CreateCommand(nullptr, __uuidof(ICommandText), reinterpret_cast<IUnknown**>(command.put())));
        SecureZeroMemory(setting, sizeof(setting));
        native.command = command.get(); return ok;
    }
};
Connection* live = nullptr;
struct Native {
    __declspec(noinline) int Count(void* connection, Args* args) {
        ++nativeCalls;
        Check(connection == &live->native, "original native connection identity is retained");
        std::wstring row;
        if (!Scalar(live->factory.get(), L"DECLARE @s smallint,@r int; EXEC dbo.sp_GetCountUserUnits " +
            std::to_wstring(args->account) + L",@s OUTPUT,@r OUTPUT; SELECT CONVERT(nvarchar(20),@r)+N'|'+ISNULL(CONVERT(nvarchar(20),@s),N'0')", row)) return 1;
        const auto split = row.find(L'|'); if (split == std::wstring::npos) return 1;
        args->result = std::stoi(row.substr(0, split)); args->extra = static_cast<std::int16_t>(std::stoi(row.substr(split + 1)));
        return args->result;
    }
};
std::uint32_t layout[] = {10, 8};
std::wstring Root() { wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768); std::wstring s(path); return s.substr(0, s.find_last_of(L"\\/")); }
std::wstring Stem() { wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768); std::wstring s(path); s = s.substr(s.find_last_of(L"\\/") + 1); return s.substr(0, s.find_last_of(L'.')); }
void Write(const std::wstring& file, const std::string& text) {
    HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD count = 0; Check(handle != INVALID_HANDLE_VALUE && WriteFile(handle, text.data(), static_cast<DWORD>(text.size()), &count, nullptr) && count == text.size(), "write isolated fixture symbols");
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
}
template<class F> uintptr_t Member(F method) { uintptr_t result = 0; static_assert(sizeof(method) == sizeof(result)); std::memcpy(&result, &method, sizeof(result)); return result; }
void Symbols() {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    char key[160]{}, header[256]{}; const auto stem = Stem(); std::string name;
    for (const auto character : stem) { if (character > 127) { Check(false, "fixture executable name must be ASCII"); return; } name += static_cast<char>(character); }
    sprintf_s(key, "%s-%08X-%08X", name.c_str(), nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    sprintf_s(header, "[host]\ntimestamp=0x%08X\nsize_of_image=0x%08X\n", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    std::string text(header);
    const auto add = [&](const char* id, uintptr_t address, const char* kind, unsigned bytes) {
        char line[256]{}; sprintf_s(line, "\n[%s]\nkind=%s\nrva=0x%08X\nexpect=", id, kind, static_cast<unsigned>(address - reinterpret_cast<uintptr_t>(base))); text += line;
        for (unsigned i = 0; i < bytes; ++i) { sprintf_s(line, "%02X ", reinterpret_cast<BYTE*>(address)[i]); text += line; } text += '\n';
    };
    add("hangar.capacity.get_count", Member(&Native::Count), "code", 12);
    add("hangar.capacity.args_layout", reinterpret_cast<uintptr_t>(&layout[0]), "data", 4);
    add("hangar.capacity.connection_command", reinterpret_cast<uintptr_t>(&layout[1]), "data", 4);
    add("hangar.capacity.args_bindings", reinterpret_cast<uintptr_t>(&layout[0]), "data", 4);
    add("hangar.capacity.request_args", reinterpret_cast<uintptr_t>(&layout[1]), "data", 4);
    std::wstring wideKey(key, key + strlen(key)); Write(Root() + L"\\sdgo\\symbols\\" + wideKey + L".ini", text);
}
bool AssertSql(const std::wstring& condition) { return Execute(live->factory.get(), L"IF NOT (" + condition + L") RAISERROR('Hangar fixture assertion failed',16,1)"); }
int Run(bool enabled) {
    Connection session; live = &session; Check(session.Open(), "open an isolated native SQLOLEDB session"); if (failures) return 2;
    Symbols();
    HMODULE core = LoadLibraryW((Root() + L"\\sdgo\\core.dll").c_str());
    const auto initialize = core ? reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core, "SdgoCoreInit")) : nullptr;
    if (!initialize) return 2;
    SdgoBoot boot{sizeof(boot), "hangar_sql_test", GetModuleHandleW(nullptr)};
    Check(initialize(&boot) != 0, "load actual capacity DLL through the actual core");
    Native target;
    Check(SUCCEEDED(session.command->SetCommandText(DBGUID_DBSQL, L"SELECT N'native bindings retained'")), "prepare native borrowed command marker");
    Args first{1, -11, -99}; int result = target.Count(&session.native, &first);
    Check(result == 0 && first.extra == (enabled ? 1176 : 420) && first.result == 0, "native reply sees exactly the persisted policy capacity");
    Check(AssertSql(L"(SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=2)=900"), "another account is not bulk-updated");
    Check(AssertSql(enabled ? L"(SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)=1200" :
        L"(SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)=1099"), "slot pool is extended only when the Mod is enabled");
    Check(AssertSql(L"EXISTS(SELECT 1 FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber=1300)"), "pre-existing slot numbers are retained");
    wchar_t* text = nullptr; GUID dialect = DBGUID_DBSQL;
    Check(SUCCEEDED(session.command->GetCommandText(&dialect, &text)) && text && wcscmp(text, L"SELECT N'native bindings retained'") == 0, "Mod does not overwrite the leased native command or bindings"); CoTaskMemFree(text);
    if (!enabled) return failures ? 1 : 0;
    Args bigger{3, -11, -99}; Check(target.Count(&session.native, &bigger) == 0 && bigger.extra == 1800, "larger saved capacity is never shrunk");
    Args newcomer{4, -11, -99}; Check(target.Count(&session.native, &newcomer) == 0 && newcomer.extra == 1176, "zero-expansion new account gets native 1200 slots");
    for (int account : {5, 6, 999}) {
        Args invalid{account, -11, -99}; const auto before = invalid; const auto calls = nativeCalls;
        Check(target.Count(&session.native, &invalid) == 1 && nativeCalls == calls && std::memcmp(&before, &invalid, sizeof(invalid)) == 0,
            "invalid/missing persisted account fails before native getter without publishing output");
    }
    ComPtr<ITransactionLocal> transaction;
    Check(SUCCEEDED(session.factory->QueryInterface(__uuidof(ITransactionLocal), reinterpret_cast<void**>(transaction.put()))), "borrow the fixture transaction interface");
    ULONG level = 0; Check(SUCCEEDED(transaction->StartTransaction(ISOLATIONLEVEL_READCOMMITTED, 0, nullptr, &level)), "start deferred caller COM transaction");
    Args ambient{1, -11, -99}; const auto beforeAmbient = ambient; const auto beforeCalls = nativeCalls;
    Check(target.Count(&session.native, &ambient) == 1 && nativeCalls == beforeCalls && std::memcmp(&ambient, &beforeAmbient, sizeof(ambient)) == 0,
        "deferred COM transaction is rejected without running native getter");
    XACTTRANSINFO info{}; Check(SUCCEEDED(transaction->GetTransactionInfo(&info)), "caller COM transaction remains active");
    Check(SUCCEEDED(transaction->Abort(nullptr, FALSE, FALSE)), "caller owns its own transaction rollback");
    Check(Execute(session.factory.get(), L"BEGIN TRANSACTION; UPDATE dbo.fixture_wallet SET amount=amount+1"), "start caller SQL transaction");
    Check(target.Count(&session.native, &ambient) == 1 && AssertSql(L"@@TRANCOUNT=1 AND (SELECT amount FROM dbo.fixture_wallet)=124"), "caller SQL transaction and uncommitted work survive rejection");
    Check(Execute(session.factory.get(), L"ROLLBACK TRANSACTION"), "rollback only the fixture caller transaction");
    {
        Connection blocker;
        Check(blocker.Open(), "open independent fixture row-lock owner");
        Check(Execute(blocker.factory.get(), L"BEGIN TRANSACTION; UPDATE dbo.tbl_Users SET ExpandSlotCount=ExpandSlotCount WHERE AccountNumber=1"), "hold only a fixture account row lock");
        std::wstring initialOptions, restoredOptions;
        const auto options = L"SELECT CONVERT(nvarchar(12),@@TRANCOUNT)+N'|'+CONVERT(nvarchar(12),@@OPTIONS)";
        Check(Scalar(session.factory.get(), options, initialOptions), "capture borrowed session before timeout");
        Args timed{1, -11, -99}; const auto saved = timed; const auto originalCalls = nativeCalls;
        const auto started = GetTickCount64();
        Check(target.Count(&session.native, &timed) == 1 && nativeCalls == originalCalls && std::memcmp(&timed, &saved, sizeof(timed)) == 0,
            "provider command timeout fails before native read without publishing output");
        Check(GetTickCount64() - started < 15000, "provider timeout remains bounded");
        Check(Execute(blocker.factory.get(), L"ROLLBACK TRANSACTION"), "release only the fixture blocking transaction");
        Check(Scalar(session.factory.get(), options, restoredOptions) && initialOptions == restoredOptions,
            "cancelled policy restores caller options and leaves no transaction");
        Check(target.Count(&session.native, &timed) == 0 && timed.extra == 1176,
            "leased native SQL session remains usable after cancellation");
    }
    Check(Execute(session.factory.get(), L"UPDATE dbo.tbl_Users SET ExpandSlotCount=420 WHERE AccountNumber=1; DELETE dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1100 AND 1200"), "prepare policy rollback case");
    Check(Execute(session.factory.get(), L"ALTER TABLE dbo.tbl_UserUnitSlotNumberList ADD CONSTRAINT fixture_fail_slots CHECK(UserUnitSlotNumber<>1180)"), "inject isolated allocation failure");
    Args failed{1, -11, -99}; const auto beforeFailure = failed; const auto calls = nativeCalls;
    Check(target.Count(&session.native, &failed) == 1 && nativeCalls == calls && std::memcmp(&failed, &beforeFailure, sizeof(failed)) == 0,
        "SQL insertion failure uses native error without touching caller output");
    Check(AssertSql(L"@@TRANCOUNT=0 AND (SELECT ExpandSlotCount FROM dbo.tbl_Users WHERE AccountNumber=1)=420 AND (SELECT COUNT(*) FROM dbo.tbl_UserUnitSlotNumberList WHERE UserUnitSlotNumber BETWEEN 1 AND 1200)=1099"), "failed policy has no partial account/pool change or leaked transaction");
    Check(Execute(session.factory.get(), L"ALTER TABLE dbo.tbl_UserUnitSlotNumberList DROP CONSTRAINT fixture_fail_slots"), "remove only fixture fault injection");
    Check(target.Count(&session.native, &failed) == 0 && failed.extra == 1176, "a later native read can retry successfully after failure");
    Check(AssertSql(L"(SELECT COUNT(*) FROM dbo.tbl_UserUnits)=2 AND EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WHERE UserUnitNumber=2147483658 AND paint=77) AND (SELECT amount FROM dbo.fixture_wallet)=123"), "owned units, 64-bit serials, paint and currency are unchanged");
    return failures ? 1 : 0;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    const int result = Run(argc < 2 || wcscmp(argv[1], L"disabled") != 0);
    CoUninitialize(); std::puts(result ? "HANGAR SQL CHECKS FAILED" : "ALL HANGAR SQL CHECKS PASSED"); return result;
}
