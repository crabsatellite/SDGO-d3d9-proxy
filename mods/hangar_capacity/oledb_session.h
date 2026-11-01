#pragma once
#define DBINITCONSTANTS
#include <windows.h>
#include <oledb.h>
#include <oledberr.h>
#include <transact.h>
#include <cstddef>
#include <cstring>
#include <string>

namespace sdgo::hangar_capacity {
template<class T> class ComPtr {
    T* value_ = nullptr;
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { if (value_) value_->Release(); }
    T* get() const { return value_; }
    T* operator->() const { return value_; }
    T** put() { if (value_) value_->Release(); value_ = nullptr; return &value_; }
};

// Same verified World connection layout as the native command adapters. The
// original command and its parameter bindings are never changed by this Mod.
inline bool BorrowCommand(void* connection, ICommandText** command) {
    __try {
        if (!connection || *reinterpret_cast<int*>(static_cast<BYTE*>(connection) + 0x240)) return false;
        *command = *reinterpret_cast<ICommandText**>(static_cast<BYTE*>(connection) + 8);
        if (!*command) return false;
        (*command)->AddRef();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class PolicySession {
    ComPtr<IDBCreateCommand> factory_;
    unsigned timeout_;
    bool Create(const std::wstring& sql, ComPtr<ICommandText>& command) {
        if (!factory_.get() || FAILED(factory_->CreateCommand(nullptr, __uuidof(ICommandText),
                reinterpret_cast<IUnknown**>(command.put()))) || !command.get()) return false;
        ComPtr<ICommandProperties> properties;
        if (FAILED(command->QueryInterface(__uuidof(ICommandProperties), reinterpret_cast<void**>(properties.put())))) return false;
        DBPROP property{};
        property.dwPropertyID = DBPROP_COMMANDTIMEOUT;
        property.dwOptions = DBPROPOPTIONS_REQUIRED;
        property.colid = DB_NULLID;
        property.vValue.vt = VT_I4;
        property.vValue.lVal = static_cast<LONG>(timeout_);
        DBPROPSET set{&property, 1, DBPROPSET_ROWSET};
        if (FAILED(properties->SetProperties(1, &set)) || property.dwStatus != DBPROPSTATUS_OK) return false;
        return SUCCEEDED(command->SetCommandText(DBGUID_DBSQL, sql.c_str()));
    }
    bool Scalar(const std::wstring& sql, std::wstring& value) {
        ComPtr<ICommandText> command;
        if (!Create(sql, command)) return false;
        ComPtr<IMultipleResults> results;
        ComPtr<IRowset> rows;
        DBROWCOUNT affected = 0;
        if (FAILED(command->Execute(nullptr, __uuidof(IMultipleResults), nullptr, &affected,
                reinterpret_cast<IUnknown**>(results.put()))) || !results.get()) return false;
        for (;;) {
            const HRESULT hr = results->GetResult(nullptr, DBRESULTFLAG_DEFAULT, __uuidof(IRowset),
                &affected, reinterpret_cast<IUnknown**>(rows.put()));
            if (FAILED(hr) || hr == DB_S_NORESULT) return false;
            if (rows.get()) break;
        }
        ComPtr<IAccessor> access;
        if (FAILED(rows->QueryInterface(__uuidof(IAccessor), reinterpret_cast<void**>(access.put())))) return false;
        struct Cell { DBSTATUS status; DBLENGTH length; wchar_t text[96]; } cell{};
        DBBINDING binding{};
        binding.iOrdinal = 1; binding.obStatus = offsetof(Cell, status);
        binding.obLength = offsetof(Cell, length); binding.obValue = offsetof(Cell, text);
        binding.dwPart = DBPART_VALUE | DBPART_STATUS | DBPART_LENGTH;
        binding.dwMemOwner = DBMEMOWNER_CLIENTOWNED; binding.wType = DBTYPE_WSTR;
        binding.cbMaxLen = sizeof(cell.text);
        HACCESSOR handle = DB_NULL_HACCESSOR;
        DBBINDSTATUS status = DBBINDSTATUS_BADBINDINFO;
        if (FAILED(access->CreateAccessor(DBACCESSOR_ROWDATA, 1, &binding, sizeof(cell), &handle, &status)) ||
            status != DBBINDSTATUS_OK) return false;
        DBCOUNTITEM count = 0; HROW* data = nullptr;
        HRESULT hr = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 1, &count, &data);
        if (SUCCEEDED(hr) && count == 1) hr = rows->GetData(data[0], handle, &cell);
        const bool ok = SUCCEEDED(hr) && count == 1 && cell.status == DBSTATUS_S_OK &&
            cell.length < sizeof(cell.text) && cell.length % sizeof(wchar_t) == 0;
        if (ok) value.assign(cell.text, cell.length / sizeof(wchar_t));
        if (count && data) rows->ReleaseRows(count, data, nullptr, nullptr, nullptr);
        CoTaskMemFree(data); access->ReleaseAccessor(handle, nullptr);
        return ok;
    }
    bool Exec(const std::wstring& sql) {
        ComPtr<ICommandText> command;
        if (!Create(sql, command)) return false;
        ComPtr<IMultipleResults> results;
        DBROWCOUNT affected = 0;
        if (FAILED(command->Execute(nullptr, __uuidof(IMultipleResults), nullptr, &affected,
                reinterpret_cast<IUnknown**>(results.put()))) || !results.get()) return false;
        // Some provider errors are delivered after an earlier row-count token.
        // Consume the whole policy batch before declaring its commit successful.
        ComPtr<IRowset> row;
        for (;;) {
            const HRESULT hr = results->GetResult(nullptr, DBRESULTFLAG_DEFAULT, __uuidof(IRowset),
                &affected, reinterpret_cast<IUnknown**>(row.put()));
            if (hr == DB_S_NORESULT) return true;
            if (FAILED(hr)) return false;
        }
    }
public:
    explicit PolicySession(unsigned timeout) : timeout_(timeout) {}
    bool Open(void* connection) {
        ComPtr<ICommandText> original;
        return BorrowCommand(connection, original.put()) &&
            SUCCEEDED(original->GetDBSession(__uuidof(IDBCreateCommand), reinterpret_cast<IUnknown**>(factory_.put()))) &&
            factory_.get();
    }
    bool Ensure(const std::wstring& sql) {
        if (!factory_.get()) return false;
        // SQLOLEDB can defer a COM transaction before the first table access;
        // @@TRANCOUNT alone is therefore not enough to identify caller work.
        ComPtr<ITransactionLocal> transaction;
        XACTTRANSINFO active{};
        if (FAILED(factory_->QueryInterface(__uuidof(ITransactionLocal), reinterpret_cast<void**>(transaction.put()))) ||
            SUCCEEDED(transaction->GetTransactionInfo(&active))) return false;
        std::wstring initial;
        if (!Scalar(L"SELECT CONVERT(nvarchar(12),@@TRANCOUNT)+N'|'+CONVERT(nvarchar(12),@@OPTIONS)", initial)) return false;
        const auto separator = initial.find(L'|');
        if (separator == std::wstring::npos || initial.substr(0, separator) != L"0") return false;
        const std::wstring optionText = initial.substr(separator + 1);
        if (optionText.empty() || optionText.find_first_not_of(L"0123456789") != std::wstring::npos || optionText.size() > 10) return false;
        const auto options = std::stoul(optionText);
        if (options & 2u) return false; // implicit transaction is owned by another policy
        const auto restore = [&]() {
            const std::wstring cleanup = std::wstring(L"IF @@TRANCOUNT>0 ROLLBACK TRANSACTION; SET NOCOUNT ") +
                ((options & 512u) ? L"ON;" : L"OFF;") + L"SET XACT_ABORT " +
                ((options & 16384u) ? L"ON;" : L"OFF;");
            return Exec(cleanup);
        };
        // An OLE DB command timeout/attention can bypass T-SQL CATCH. Since the
        // leased session had no caller transaction, rollback is ours alone.
        try {
            if (!Exec(sql)) { restore(); return false; }
            std::wstring final;
            if (!Scalar(L"SELECT CONVERT(nvarchar(12),@@TRANCOUNT)+N'|'+CONVERT(nvarchar(12),@@OPTIONS)", final) || final != initial) {
                restore(); return false;
            }
            return true;
        } catch (...) {
            try { restore(); } catch (...) {}
            return false;
        }
    }
};
} // namespace sdgo::hangar_capacity
