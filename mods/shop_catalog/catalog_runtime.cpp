#include <windows.h>
#include <sql.h>
#include <sqlext.h>

#include <algorithm>
#include <cwchar>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "catalog_contract.h"
#include "catalog_runtime.h"

namespace sdgo::shop_catalog {
namespace {
bool Success(SQLRETURN value) { return value == SQL_SUCCESS || value == SQL_SUCCESS_WITH_INFO; }
std::wstring Absolute(const std::wstring& path)
{
    std::vector<wchar_t> buffer(32768);
    const DWORD count = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!count || count >= buffer.size()) throw std::runtime_error("invalid catalog path");
    return {buffer.data(), count};
}
std::wstring Value(const std::wstring& file, const wchar_t* section, const wchar_t* key,
                   const wchar_t* fallback = L"")
{
    wchar_t text[2048] = {};
    const DWORD count = GetPrivateProfileStringW(section, key, fallback, text, 2048, file.c_str());
    if (count >= 2047) throw std::runtime_error("INI value too long");
    return text;
}
int Integer(const std::wstring& file, const wchar_t* section, const wchar_t* key, int fallback,
            int minimum, int maximum)
{
    const std::wstring text = Value(file, section, key, std::to_wstring(fallback).c_str());
    wchar_t* end = nullptr;
    const long long value = _wcstoi64(text.c_str(), &end, 10);
    if (!end || end == text.c_str() || *end || value < minimum || value > maximum)
        throw std::runtime_error("invalid numeric catalog value");
    return static_cast<int>(value);
}
bool Key(const std::wstring& value)
{
    return !value.empty() && value.size() <= 64 && std::all_of(value.begin(), value.end(), [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
               (ch >= L'0' && ch <= L'9') || ch == L'-' || ch == L'_' || ch == L'.';
    });
}
void Text(const std::wstring& value)
{
    if (value.empty() || value.size() > 50 || value.find_first_of(L"\r\n") != std::wstring::npos)
        throw std::runtime_error("catalog label must contain 1..50 UTF-16 units on one line");
}
Catalog Load(const std::wstring& file, bool headerOnly)
{
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) throw std::runtime_error("catalog.ini missing");
    if (Integer(file, L"shop", L"Schema", 0, 1, 1) != 1) throw std::runtime_error("unsupported catalog schema");
    Catalog result;
    result.id = Value(file, L"shop", L"CatalogId");
    result.revision = Value(file, L"shop", L"Revision", L"manual");
    if (!Key(result.id) || !Key(result.revision)) throw std::runtime_error("invalid catalog identity");
    if (headerOnly) return result;
    std::vector<wchar_t> sections(65536);
    const DWORD size = GetPrivateProfileSectionNamesW(sections.data(), static_cast<DWORD>(sections.size()), file.c_str());
    if (size >= sections.size() - 2) throw std::runtime_error("catalog section list too long");
    std::set<std::wstring> keys, skus;
    std::set<int> ids;
    for (const wchar_t* section = sections.data(); *section; section += wcslen(section) + 1) {
        if (wcsncmp(section, L"product:", 8) != 0) continue;
        if (result.products.size() >= 256) throw std::runtime_error("catalog exceeds 256 products");
        Product product;
        product.key = section + 8;
        product.id = Integer(file, section, L"ProductId", 0, 1, 2147483647);
        product.templateId = Integer(file, section, L"TemplateId", 0, 1, 2147483647);
        product.sku = Value(file, section, L"ItemId");
        product.category = Integer(file, section, L"Category", 0, 1, 2147483647);
        product.price = Integer(file, section, L"Price", -1, 1, 2147483647);
        product.validity = Integer(file, section, L"Validity", 0, 1, 1000000);
        const auto currency = Value(file, section, L"Currency");
        if (currency != L"GB" && currency != L"MB") throw std::runtime_error("currency must be GB or MB");
        product.payment = currency == L"GB" ? 1 : 2;
        product.enabled = Integer(file, section, L"Enabled", 1, 0, 1) != 0;
        product.name = Value(file, section, L"Name");
        product.label = Value(file, section, L"Label");
        Text(product.name); Text(product.label);
        if (!Key(product.key) || !keys.insert(product.key).second || !ids.insert(product.id).second ||
            product.sku.size() != 6 || !std::all_of(product.sku.begin(), product.sku.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) ||
            !skus.insert(product.sku).second || product.id == product.templateId)
            throw std::runtime_error("duplicate or invalid managed product identity");
        result.products.push_back(std::move(product));
    }
    return result;
}
std::wstring EscapeConnection(const std::wstring& value)
{
    std::wstring result = L"{";
    for (wchar_t c : value) { result += c; if (c == L'}') result += c; }
    return result + L"}";
}
class Database {
    SQLHENV env = SQL_NULL_HENV;
    SQLHDBC connection = SQL_NULL_HDBC;
public:
    SQLHSTMT statement = SQL_NULL_HSTMT;
    Database(const std::wstring& ini, const wchar_t* section)
    {
        try {
        if (!Success(SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env)) ||
            !Success(SQLSetEnvAttr(env, SQL_ATTR_ODBC_VERSION, reinterpret_cast<SQLPOINTER>(SQL_OV_ODBC3), 0)) ||
            !Success(SQLAllocHandle(SQL_HANDLE_DBC, env, &connection)))
            throw std::runtime_error("ODBC initialization failed");
        SQLSetConnectAttrW(connection, SQL_LOGIN_TIMEOUT, reinterpret_cast<SQLPOINTER>(5), 0);
        std::wstring text = L"DRIVER={SQL Server};SERVER=" + EscapeConnection(Value(ini, section, L"DBServerName")) +
            L";DATABASE=" + EscapeConnection(Value(ini, section, L"DBName")) + L";UID=" + EscapeConnection(Value(ini, section, L"ID")) +
            L";PWD=" + EscapeConnection(Value(ini, section, L"PWD")) + L";";
        const SQLRETURN opened = SQLDriverConnectW(connection, nullptr, reinterpret_cast<SQLWCHAR*>(text.data()), SQL_NTS,
                                                   nullptr, 0, nullptr, SQL_DRIVER_NOPROMPT);
        SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
        if (!Success(opened)) throw std::runtime_error("ODBC connection failed; credentials not logged");
        if (!Success(SQLAllocHandle(SQL_HANDLE_STMT, connection, &statement))) throw std::runtime_error("ODBC statement allocation failed");
        SQLSetStmtAttrW(statement, SQL_ATTR_QUERY_TIMEOUT, reinterpret_cast<SQLPOINTER>(20), 0);
        } catch (...) { Close(); throw; }
    }
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database() { Close(); }
    void Close()
    {
        if (statement != SQL_NULL_HSTMT) SQLFreeHandle(SQL_HANDLE_STMT, statement);
        if (connection != SQL_NULL_HDBC) { SQLDisconnect(connection); SQLFreeHandle(SQL_HANDLE_DBC, connection); }
        if (env != SQL_NULL_HENV) SQLFreeHandle(SQL_HANDLE_ENV, env);
        statement = SQL_NULL_HSTMT; connection = SQL_NULL_HDBC; env = SQL_NULL_HENV;
    }
    void Execute(const std::wstring& text)
    {
        SQLFreeStmt(statement, SQL_CLOSE);
        const SQLRETURN result = SQLExecDirectW(statement, reinterpret_cast<SQLWCHAR*>(const_cast<wchar_t*>(text.c_str())), SQL_NTS);
        if (!Success(result)) {
            SQLWCHAR state[6] = {}, message[768] = {}; SQLINTEGER native = 0; SQLSMALLINT length = 0;
            SQLGetDiagRecW(SQL_HANDLE_STMT, statement, 1, state, &native, message, 768, &length);
            // Statement diagnostics contain no connection string or credentials.
            std::wstring description = L"SQL catalog statement failed: " + std::wstring(reinterpret_cast<wchar_t*>(state)) +
                                       L" / " + std::to_wstring(native) + L" / " + reinterpret_cast<wchar_t*>(message);
            std::string utf8(description.size() * 4 + 1, '\0');
            const int size = WideCharToMultiByte(CP_UTF8, 0, description.c_str(), -1, utf8.data(), static_cast<int>(utf8.size()), nullptr, nullptr);
            if (size > 0) utf8.resize(static_cast<size_t>(size - 1));
            throw std::runtime_error(utf8);
        }
    }
    int Number(int column)
    {
        SQLINTEGER number = 0; SQLLEN length = 0;
        if (!Success(SQLGetData(statement, static_cast<SQLUSMALLINT>(column), SQL_C_SLONG, &number, sizeof(number), &length)) || length == SQL_NULL_DATA)
            throw std::runtime_error("invalid SQL catalog numeric result");
        return number;
    }
    std::wstring String(int column)
    {
        wchar_t text[512] = {}; SQLLEN length = 0;
        if (!Success(SQLGetData(statement, static_cast<SQLUSMALLINT>(column), SQL_C_WCHAR, text, sizeof(text), &length)) || length == SQL_NULL_DATA || length >= sizeof(text))
            throw std::runtime_error("invalid SQL Unicode label result");
        return text;
    }
};
void Metadata(Database& game, Product& product)
{
    game.Execute(L"SELECT c.TimeLimit,ISNULL(u.UnitGradeNumber-1,0),ISNULL(u.MasteredWeaponType,0),CASE WHEN c.CatalogueID/100000=11 THEN ISNULL(u.SallyReady,0) ELSE 1 END FROM dbo.tbl_ItemCategory c LEFT JOIN dbo.tbl_Units u ON c.CatalogueID/100000=11 AND u.UnitNumber=c.RefIndex WHERE c.CatalogueID=" + std::to_wstring(product.category));
    if (SQLFetch(game.statement) != SQL_SUCCESS) throw std::runtime_error("unknown game item category");
    product.timeLimit = game.Number(1); product.search1 = game.Number(2); product.search2 = game.Number(3);
    if (game.Number(4) != 1) throw std::runtime_error("unit category is not deployable by the native game");
    if (SQLFetch(game.statement) != SQL_NO_DATA || product.timeLimit < 0 || product.timeLimit > 2)
        throw std::runtime_error("ambiguous or unsupported category metadata");
    if (product.category / 100000 == 11 && product.timeLimit != 0 && product.timeLimit != 1)
        throw std::runtime_error("unsupported unit rental mode");
    if (product.timeLimit == 1 && product.validity > 36500)
        throw std::runtime_error("rental duration exceeds supported display range");
}
void WriteUnicode(const std::wstring& path, const std::wstring& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot write generated Unicode profile");
    const wchar_t bom = 0xFEFF;
    DWORD written = 0;
    const DWORD bytes = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    const bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom) &&
                    WriteFile(file, text.data(), bytes, &written, nullptr) && written == bytes && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) throw std::runtime_error("incomplete generated Unicode profile");
}
} // namespace

bool Apply(const std::wstring& hostIni, const std::wstring& catalogIni,
           const std::wstring& outputDirectory, bool disable, LogFn log)
{
    bool committed = false;
    std::wstring temporary;
    try {
        const std::wstring hostFile = Absolute(hostIni);
        Catalog catalog = Load(Absolute(catalogIni), disable);
        if (disable) catalog.products.clear();
        const std::wstring output = Absolute(outputDirectory);
        temporary = output + L"\\runtime-display." + std::to_wstring(GetCurrentProcessId()) + L".pending";
        // Establish writability before starting a database transaction.
        WriteUnicode(temporary, L"; Preparing shop display configuration\r\n");
        if (!catalog.products.empty()) {
            Database game(hostFile, L"DB Game");
            for (auto& product : catalog.products) Metadata(game, product);
        }
        Database billing(hostFile, L"DB StoreBilling");
        try {
            billing.Execute(ApplySql(catalog));
            bool contract = false;
            std::wstring display = L"; Generated from the committed server definitions. UTF-16LE.\r\n[shop]\r\nRevision=" + catalog.revision + L"\r\n";
            unsigned count = 0;
            for (;;) {
                SQLSMALLINT columns = 0;
                if (!Success(SQLNumResultCols(billing.statement, &columns))) throw std::runtime_error("cannot read catalog result");
                if (columns == 7) {
                    if (contract) throw std::runtime_error("duplicate catalog result");
                    contract = true;
                    SQLRETURN fetched;
                    while ((fetched = SQLFetch(billing.statement)) != SQL_NO_DATA) {
                        if (!Success(fetched)) throw std::runtime_error("catalog row fetch failed");
                        billing.Number(1); // Parent product id, retained in the native DB.
                        const int option = billing.Number(2), category = billing.Number(3), validity = billing.Number(4);
                        const std::wstring payment = billing.String(5);
                        const int price = billing.Number(6);
                        const std::wstring label = billing.String(7);
                        display += L"\r\n[option:" + std::to_wstring(option) + L"]\r\nCategory=" + std::to_wstring(category) +
                            L"\r\nValidity=" + std::to_wstring(validity) + L"\r\nPayment=" + payment +
                            L"\r\nPrice=" + std::to_wstring(price) + L"\r\nText=" + label + L"\r\n";
                        ++count;
                    }
                } else if (columns != 0) throw std::runtime_error("unexpected catalog result layout");
                const SQLRETURN next = SQLMoreResults(billing.statement);
                if (next == SQL_NO_DATA) break;
                if (!Success(next)) throw std::runtime_error("catalog transaction rejected");
            }
            if (!contract) throw std::runtime_error("catalog result contract missing");
            WriteUnicode(temporary, display);
            billing.Execute(L"COMMIT TRANSACTION;");
            committed = true;
            const std::wstring destination = output + L"\\runtime-display.ini";
            if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
                throw std::runtime_error("database applied, but publishing client display profile failed");
            log(false, L"shop catalog applied: " + catalog.id + L", " + std::to_wstring(count) + L" active managed products; native/unowned products preserved");
            return true;
        } catch (...) {
            if (!committed) {
                try { billing.Execute(L"IF @@TRANCOUNT>0 ROLLBACK TRANSACTION;"); } catch (...) {}
            }
            throw;
        }
    } catch (const std::exception& error) {
        // Rejected applications own only this process's staging file. Keep a
        // committed profile on publication failure so the host can recover it.
        if (!committed && !temporary.empty()) DeleteFileW(temporary.c_str());
        wchar_t decoded[2048] = {};
        MultiByteToWideChar(CP_UTF8, 0, error.what(), -1, decoded, 2048);
        const std::wstring text = decoded;
        log(true, text + (committed ? L"; previous client overrides are guarded by exact business fields" : L"; transaction not committed"));
        return false;
    }
}
} // namespace sdgo::shop_catalog
