#define DBINITCONSTANTS
#include <windows.h>
#include <msdasc.h>
#include <oledb.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "sdgo/sdgo_api.h"
#include "../mods/collection_capsule/native.h"

using sdgo::collection_capsule::ActivityPickRecord;

namespace {
template<class T>
class ComPtr {
    T* value_ = nullptr;

public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (value_) value_->Release(); }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }
    T** put() {
        if (value_) value_->Release();
        value_ = nullptr;
        return &value_;
    }
};

int failures = 0;
volatile LONG delegated = 0;
std::uint32_t connectionCommandOffset = sdgo::collection_capsule::kNativeCommandOffset;

void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", label);
    if (!condition) ++failures;
}

struct NativeConnection {
    BYTE prefix[8] = {};
    ICommandText* command = nullptr;
    BYTE body[0x240 - 12] = {};
    LONG state = 0;
};
static_assert(offsetof(NativeConnection, command) ==
              sdgo::collection_capsule::kNativeCommandOffset);
static_assert(offsetof(NativeConnection, state) ==
              sdgo::collection_capsule::kNativeErrorOffset);

class Session {
    ComPtr<IDBInitialize> source_;
    ComPtr<IDBCreateCommand> commands_;
    ComPtr<ICommandText> nativeCommand_;

public:
    NativeConnection native;

    bool Open() {
        wchar_t connection[4096] = {};
        const DWORD length =
            GetEnvironmentVariableW(L"SDGO_COLLECTION_GAME", connection, 4096);
        if (!length || length >= 4096) return false;
        ComPtr<IDataInitialize> initialize;
        if (FAILED(CoCreateInstance(
                CLSID_MSDAINITIALIZE, nullptr, CLSCTX_INPROC_SERVER,
                __uuidof(IDataInitialize),
                reinterpret_cast<void**>(initialize.put())))) return false;
        if (FAILED(initialize->GetDataSource(
                nullptr, CLSCTX_INPROC_SERVER, connection,
                __uuidof(IDBInitialize),
                reinterpret_cast<IUnknown**>(source_.put()))) ||
            FAILED(source_->Initialize())) return false;
        ComPtr<IDBCreateSession> sessions;
        if (FAILED(source_->QueryInterface(
                __uuidof(IDBCreateSession),
                reinterpret_cast<void**>(sessions.put()))) ||
            FAILED(sessions->CreateSession(
                nullptr, __uuidof(IDBCreateCommand),
                reinterpret_cast<IUnknown**>(commands_.put()))) ||
            FAILED(commands_->CreateCommand(
                nullptr, __uuidof(ICommandText),
                reinterpret_cast<IUnknown**>(nativeCommand_.put())))) return false;
        native.command = nativeCommand_.get();
        return true;
    }

    bool Execute(const std::wstring& sql) {
        ComPtr<ICommandText> command;
        if (!commands_ ||
            FAILED(commands_->CreateCommand(
                nullptr, __uuidof(ICommandText),
                reinterpret_cast<IUnknown**>(command.put()))) ||
            FAILED(command->SetCommandText(DBGUID_DBSQL, sql.c_str()))) return false;
        DBROWCOUNT affected = 0;
        return SUCCEEDED(command->Execute(
            nullptr, IID_NULL, nullptr, &affected, nullptr));
    }
};

struct Native {};

// ECX plus two stack parameters is the shipped __thiscall ABI. The ignored
// EDX argument lets this standalone function expose that exact machine shape
// without relying on compiler-specific pointer-to-member encodings.
__declspec(noinline) int __fastcall FixturePick(
    void*, void*, void*, ActivityPickRecord* record) {
    InterlockedIncrement(&delegated);
    record->result = 77;
    record->dropBoxSerial = 4343434343LL;
    record->itemCategory = 424242;
    record->amount = 42;
    return 77;
}

PVOID volatile fixtureEntry = reinterpret_cast<PVOID>(&FixturePick);

int Invoke(
    Native& native, void* connection, ActivityPickRecord* record) {
    auto* entry = InterlockedCompareExchangePointer(
        &fixtureEntry, nullptr, nullptr);
    return reinterpret_cast<sdgo::collection_capsule::ActivityPickFunction>(entry)(
        &native, connection, record);
}

std::wstring Root() {
    wchar_t path[32768] = {};
    GetModuleFileNameW(nullptr, path, 32768);
    std::wstring result = path;
    return result.substr(0, result.find_last_of(L"\\/"));
}

std::wstring Stem() {
    wchar_t path[32768] = {};
    GetModuleFileNameW(nullptr, path, 32768);
    std::wstring result = path;
    result = result.substr(result.find_last_of(L"\\/") + 1);
    return result.substr(0, result.find_last_of(L'.'));
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool result =
        ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()),
                    &written, nullptr) &&
        written == text.size();
    CloseHandle(file);
    return result;
}

bool WriteSymbols(const std::wstring& root) {
    auto* base = reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    std::string stem;
    for (wchar_t character : Stem()) stem += static_cast<char>(character);
    char key[160] = {};
    sprintf_s(key, "%s-%08X-%08X", stem.c_str(),
              nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    char header[256] = {};
    sprintf_s(header,
              "[host]\ntimestamp=0x%08X\nsize_of_image=0x%08X\n",
              nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    std::string content = header;
    const std::uintptr_t address =
        reinterpret_cast<std::uintptr_t>(&FixturePick);
    char line[256] = {};
    sprintf_s(line,
              "\n[collection.activity_pick]\nkind=code\nrva=0x%08X\nexpect=",
              static_cast<unsigned>(address - reinterpret_cast<std::uintptr_t>(base)));
    content += line;
    for (unsigned index = 0; index < 12; ++index) {
        sprintf_s(line, "%02X ", reinterpret_cast<const BYTE*>(address)[index]);
        content += line;
    }
    content += '\n';
    const auto layout =
        reinterpret_cast<std::uintptr_t>(&connectionCommandOffset);
    sprintf_s(line,
              "\n[collection.connection_command]\n"
              "kind=data\nrva=0x%08X\nexpect=",
              static_cast<unsigned>(
                  layout - reinterpret_cast<std::uintptr_t>(base)));
    content += line;
    for (unsigned index = 0; index < sizeof(connectionCommandOffset); ++index) {
        sprintf_s(line, "%02X ",
                  reinterpret_cast<const BYTE*>(layout)[index]);
        content += line;
    }
    content += '\n';
    const std::wstring wideKey(key, key + std::strlen(key));
    return WriteTextFile(
        root + L"\\sdgo\\symbols\\" + wideKey + L".ini", content);
}

bool Configure(const std::wstring& root) {
    const std::wstring directory = root + L"\\sdgo\\mods\\collection_capsule";
    CreateDirectoryW(directory.c_str(), nullptr);
    return WriteTextFile(
        directory + L"\\config.ini", "[settings]\nMachineID=990001\n");
}

bool SameReward(const ActivityPickRecord& left, const ActivityPickRecord& right) {
    return left.dropBoxSerial == right.dropBoxSerial &&
           left.itemCategory == right.itemCategory &&
           left.amount == right.amount;
}

ActivityPickRecord Pattern(std::int32_t account, std::int32_t machine) {
    ActivityPickRecord record = {};
    std::memset(&record, 0xa5, sizeof(record));
    record.result = -1;
    record.account = account;
    record.machine = machine;
    return record;
}

void DiagnosePick(
    const char* name, int code, const ActivityPickRecord& record,
    LONG delegateCount) {
    std::printf(
        "diag %s code=%d result=%d serial=%lld category=%d amount=%d delegated=%ld\n",
        name, code, record.result,
        static_cast<long long>(record.dropBoxSerial), record.itemCategory,
        record.amount, delegateCount);
}

class ProcedureSwap {
    Session* session_ = nullptr;
    bool renamed_ = false;

public:
    explicit ProcedureSwap(Session& session) : session_(&session) {}
    ProcedureSwap(const ProcedureSwap&) = delete;
    ProcedureSwap& operator=(const ProcedureSwap&) = delete;
    ~ProcedureSwap() { Restore(); }

    bool Install(const std::wstring& definition) {
        if (!session_->Execute(
                L"IF OBJECT_ID(N'sdgo_collection_capsule.Draw_CoreFixtureActual',N'P') IS NOT NULL "
                L"RAISERROR('Stale collection fixture procedure',16,1);"
                L"EXEC sys.sp_rename N'sdgo_collection_capsule.Draw',"
                L"N'Draw_CoreFixtureActual',N'OBJECT';")) return false;
        renamed_ = true;
        if (session_->Execute(definition)) return true;
        Restore();
        return false;
    }

    bool Restore() {
        if (!renamed_) return true;
        const bool result = session_->Execute(
            L"IF OBJECT_ID(N'sdgo_collection_capsule.Draw',N'P') IS NOT NULL "
            L"DROP PROCEDURE sdgo_collection_capsule.Draw;"
            L"EXEC sys.sp_rename "
            L"N'sdgo_collection_capsule.Draw_CoreFixtureActual',N'Draw',N'OBJECT';");
        if (result) renamed_ = false;
        return result;
    }
};

int Run() {
    Session game;
    Check(game.Open(), "open isolated game OLE DB provider");
    if (failures) return 2;

    const std::wstring root = Root();
    Check(WriteSymbols(root), "write dynamic self-host symbol");
    Check(Configure(root), "write isolated collection machine setting");
    HMODULE core = LoadLibraryW((root + L"\\sdgo\\core.dll").c_str());
    auto initialize = core ? reinterpret_cast<SdgoCoreInitFn>(
                                 GetProcAddress(core, "SdgoCoreInit"))
                           : nullptr;
    Check(initialize != nullptr, "actual core export exists");
    if (!initialize) return 2;
    SdgoBoot boot = {sizeof(boot), "collection_capsule_sql_test",
                     GetModuleHandleW(nullptr)};
    Check(initialize(&boot) != 0,
          "load collection capsule Mod through the actual core");
    if (failures) return 2;

    Native native;
    ActivityPickRecord other = Pattern(1900000001, 12345);
    const LONG before = delegated;
    const int otherCode = Invoke(native, nullptr, &other);
    Check(otherCode == 77 && other.result == 77 &&
              other.dropBoxSerial == 4343434343LL &&
              other.itemCategory == 424242 && other.amount == 42 &&
              delegated == before + 1,
          "another machine delegates to the unmodified native path without a database session");

    constexpr int account = 1900000001;
    Check(game.Execute(
              L"IF EXISTS(SELECT 1 FROM dbo.tbl_Users WHERE AccountNumber=1900000001) "
              L"RAISERROR('Collection C++ fixture account collision',16,1);"
              L"INSERT dbo.tbl_Users(AccountNumber,ExpandSlotCount) VALUES(1900000001,0);"),
          "create isolated account without any history import");

    ActivityPickRecord success = Pattern(account, 990001);
    const int successCode = Invoke(native, &game.native, &success);
    const bool successValid =
        successCode == 0 && success.result == 0 &&
        success.dropBoxSerial > INT32_MAX &&
        success.itemCategory >= 1100000 && success.itemCategory < 1200000 &&
        success.amount == 1;
    if (!successValid)
        DiagnosePick(
            "success", successCode, success, delegated);
    Check(successValid,
          "own machine publishes the exact packed 28-byte activity result");
    Check(game.Execute(
              L"IF NOT EXISTS(SELECT 1 FROM dbo.tbl_UserDropItemBox b "
              L"JOIN dbo.tbl_ItemCategory c ON c.CatalogueID=b.ItemCategory "
              L"JOIN sdgo_collection_capsule.CatalogUnlocks s ON s.AccountNumber=b.AccountNumber "
              L" AND s.UnitID=c.RefIndex "
              L"WHERE b.AccountNumber=1900000001 AND b.UserDropItemIdx=" +
              std::to_wstring(success.dropBoxSerial) +
              L" AND b.ItemCategory=" + std::to_wstring(success.itemCategory) +
              L" AND b.Amount=1 AND b.DropConditionID=990001 "
              L" AND c.CatalogueID/100000=11 AND c.TimeLimit=0 "
              L" AND c.Option1=1 AND c.Option2=2) "
              L"OR EXISTS(SELECT 1 FROM dbo.tbl_UserUnits "
              L"WHERE AccountNumber=1900000001) "
              L"RAISERROR('Committed activity reward or reservation missing',16,1);"),
          "drop-box reward and durable acquisition reservation commit before result publication");

    {
        ProcedureSwap swap(game);
        Check(swap.Install(
                  L"CREATE PROCEDURE sdgo_collection_capsule.Draw "
                  L"@v_AccountNum int,@v_MachineID int AS "
                  L"BEGIN SET NOCOUNT ON;RETURN 3007;END"),
              "install isolated backend-error procedure");
        ActivityPickRecord failed = Pattern(account, 990001);
        const ActivityPickRecord untouched = failed;
        const int code = Invoke(native, &game.native, &failed);
        const bool valid = code == 3007 && failed.result == 3007 &&
                           SameReward(failed, untouched);
        if (!valid)
            DiagnosePick(
                "backend", code, failed, delegated);
        Check(valid,
              "backend return code propagates and leaves caller output untouched");
        Check(swap.Restore(), "restore production draw after backend-error case");
    }

    NativeConnection unavailable = {};
    ActivityPickRecord missing = Pattern(account, 990001);
    const ActivityPickRecord missingPattern = missing;
    const int missingCode = Invoke(native, &unavailable, &missing);
    const bool missingValid =
        missingCode == 1 && missing.result == 1 &&
        SameReward(missing, missingPattern);
    if (!missingValid)
        DiagnosePick(
            "missing", missingCode, missing, delegated);
    Check(missingValid,
          "missing native command fails closed without publishing output");

    Check(game.Execute(
              L"IF OBJECT_ID(N'dbo.collection_cpp_marker',N'U') IS NULL "
              L"CREATE TABLE dbo.collection_cpp_marker(AccountNumber int NOT NULL);"
              L"DELETE dbo.collection_cpp_marker;"),
          "prepare isolated rollback marker");
    {
        ProcedureSwap swap(game);
        Check(swap.Install(
                  L"CREATE PROCEDURE sdgo_collection_capsule.Draw "
                  L"@v_AccountNum int,@v_MachineID int AS BEGIN SET NOCOUNT ON;"
                  L"INSERT dbo.collection_cpp_marker VALUES(@v_AccountNum);"
                  L"SELECT CONVERT(bigint,2147483648) AS WrongSerial,"
                  L"CONVERT(int,1180000) AS ItemCategory,"
                  L"CONVERT(int,1) AS Amount;RETURN 0;END"),
              "install isolated malformed-row procedure");
        ActivityPickRecord malformed = Pattern(account, 990001);
        const ActivityPickRecord malformedPattern = malformed;
        const int code = Invoke(native, &game.native, &malformed);
        const bool valid =
            code == 1 && malformed.result == 1 &&
            SameReward(malformed, malformedPattern);
        if (!valid)
            DiagnosePick(
                "malformed", code, malformed, delegated);
        Check(valid,
              "malformed row fails closed without publishing partial output");
        Check(game.Execute(
                  L"IF EXISTS(SELECT 1 FROM dbo.collection_cpp_marker) "
                  L"RAISERROR('Malformed result escaped transaction rollback',16,1);"),
              "malformed result aborts its independent OLE transaction");
        Check(swap.Restore(), "restore production draw after malformed-row case");
    }

    {
        ProcedureSwap swap(game);
        Check(swap.Install(
                  L"CREATE PROCEDURE sdgo_collection_capsule.Draw "
                  L"@v_AccountNum int,@v_MachineID int AS BEGIN SET NOCOUNT ON;"
                  L"INSERT dbo.collection_cpp_marker VALUES(@v_AccountNum);"
                  L"SELECT CONVERT(bigint,2147483648) AS DropBoxSerial,"
                  L"CONVERT(int,1180000) AS ItemCategory,"
                  L"CONVERT(int,2) AS Amount;RETURN 0;END"),
              "install isolated invalid-amount procedure");
        ActivityPickRecord invalidAmount = Pattern(account, 990001);
        const ActivityPickRecord invalidPattern = invalidAmount;
        const int code = Invoke(native, &game.native, &invalidAmount);
        Check(code == 1 && invalidAmount.result == 1 &&
                  SameReward(invalidAmount, invalidPattern),
              "non-single activity amount fails closed without publishing output");
        Check(game.Execute(
                  L"IF EXISTS(SELECT 1 FROM dbo.collection_cpp_marker) "
                  L"RAISERROR('Invalid amount escaped transaction rollback',16,1);"),
              "invalid activity amount aborts its independent OLE transaction");
        Check(swap.Restore(), "restore production draw after invalid-amount case");
    }

    {
        ProcedureSwap swap(game);
        Check(swap.Install(
                  L"CREATE PROCEDURE sdgo_collection_capsule.Draw "
                  L"@v_AccountNum int,@v_MachineID int AS BEGIN SET NOCOUNT ON;"
                  L"INSERT dbo.collection_cpp_marker VALUES(@v_AccountNum);"
                  L"SELECT CONVERT(bigint,2147483648) AS DropBoxSerial,"
                  L"CONVERT(int,1070001) AS ItemCategory,"
                  L"CONVERT(int,1) AS Amount;RETURN 0;END"),
              "install isolated non-unit-category procedure");
        ActivityPickRecord nonUnit = Pattern(account, 990001);
        const ActivityPickRecord nonUnitPattern = nonUnit;
        const int code = Invoke(native, &game.native, &nonUnit);
        Check(code == 1 && nonUnit.result == 1 &&
                  SameReward(nonUnit, nonUnitPattern),
              "non-unit activity category fails closed without publishing output");
        Check(game.Execute(
                  L"IF EXISTS(SELECT 1 FROM dbo.collection_cpp_marker) "
                  L"RAISERROR('Non-unit category escaped transaction rollback',16,1);"),
              "non-unit activity category aborts its independent OLE transaction");
        Check(swap.Restore(), "restore production draw after non-unit-category case");
    }

    return failures ? 1 : 0;
}
} // namespace

int wmain() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 3;
    const int result = Run();
    CoUninitialize();
    std::puts(result ? "COLLECTION CAPSULE SQL CHECKS FAILED"
                     : "ALL COLLECTION CAPSULE SQL CHECKS PASSED");
    return result;
}
