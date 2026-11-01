#pragma once

#include <windows.h>
#include <oledb.h>
#include <transact.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "native.h"

namespace sdgo::collection_capsule {

template<class T>
class ComPtr {
    T* value_ = nullptr;

public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { reset(); }

    T* get() const { return value_; }
    T* operator->() const { return value_; }
    explicit operator bool() const { return value_ != nullptr; }

    T** put() {
        reset();
        return &value_;
    }

    void attach(T* value) {
        reset();
        value_ = value;
    }

    void reset() {
        if (value_) value_->Release();
        value_ = nullptr;
    }
};

inline bool BorrowNativeCommand(void* connection, ICommandText** output) {
    if (!output) return false;
    *output = nullptr;
    __try {
        auto* bytes = static_cast<BYTE*>(connection);
        if (!bytes ||
            *reinterpret_cast<const LONG*>(bytes + kNativeErrorOffset) != 0) return false;
        auto* command =
            *reinterpret_cast<ICommandText**>(bytes + kNativeCommandOffset);
        if (!command) return false;
        command->AddRef();
        *output = command;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

class Transaction {
    ITransactionLocal* transaction_ = nullptr;
    bool active_ = false;

public:
    explicit Transaction(ITransactionLocal* transaction) : transaction_(transaction) {}
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() { Abort(); }

    bool Begin() {
        if (!transaction_ || active_) return false;
        ULONG level = 0;
        if (FAILED(transaction_->StartTransaction(
                ISOLATIONLEVEL_READCOMMITTED, 0, nullptr, &level))) return false;
        active_ = true;
        return level == 1;
    }

    bool Commit() {
        if (!active_) return false;
        if (FAILED(transaction_->Commit(FALSE, 0, 0))) return false;
        active_ = false;
        return true;
    }

    void Abort() {
        if (!active_) return;
        transaction_->Abort(nullptr, FALSE, FALSE);
        active_ = false;
    }
};

struct ActivityReward {
    std::int64_t dropBoxSerial = 0;
    std::int32_t itemCategory = 0;
    std::int32_t amount = 0;
};

struct DrawRow {
    struct I64 {
        DBSTATUS status = DBSTATUS_E_UNAVAILABLE;
        DBLENGTH length = 0;
        LONGLONG value = 0;
    };
    struct I32 {
        DBSTATUS status = DBSTATUS_E_UNAVAILABLE;
        DBLENGTH length = 0;
        LONG value = 0;
    };
    I64 dropBoxSerial;
    I32 itemCategory;
    I32 amount;
};

inline bool ExactColumns(IRowset* rows) {
    if (!rows) return false;
    ComPtr<IColumnsInfo> columns;
    if (FAILED(rows->QueryInterface(
            __uuidof(IColumnsInfo), reinterpret_cast<void**>(columns.put())))) return false;
    DBORDINAL count = 0;
    DBCOLUMNINFO* info = nullptr;
    OLECHAR* strings = nullptr;
    const HRESULT result = columns->GetColumnInfo(&count, &info, &strings);
    bool valid = SUCCEEDED(result) && count == 3 && info;
    constexpr DBTYPE types[] = {DBTYPE_I8, DBTYPE_I4, DBTYPE_I4};
    constexpr DBLENGTH sizes[] = {8, 4, 4};
    constexpr const wchar_t* names[] = {
        L"DropBoxSerial", L"ItemCategory", L"Amount"};
    if (valid) {
        for (DBORDINAL index = 0; index < count; ++index) {
            if (info[index].iOrdinal != index + 1 || info[index].wType != types[index] ||
                info[index].ulColumnSize != sizes[index] || !info[index].pwszName ||
                CompareStringOrdinal(
                    info[index].pwszName, -1, names[index], -1, TRUE) != CSTR_EQUAL) {
                valid = false;
                break;
            }
        }
    }
    CoTaskMemFree(info);
    CoTaskMemFree(strings);
    return valid;
}

inline bool ExactValue(const DrawRow::I64& value) {
    return value.status == DBSTATUS_S_OK && value.length == sizeof(value.value);
}
inline bool ExactValue(const DrawRow::I32& value) {
    return value.status == DBSTATUS_S_OK && value.length == sizeof(value.value);
}
inline bool ReadOneRow(IRowset* rows, ActivityReward& output) {
    if (!ExactColumns(rows)) return false;
    ComPtr<IAccessor> accessors;
    if (FAILED(rows->QueryInterface(
            __uuidof(IAccessor), reinterpret_cast<void**>(accessors.put())))) return false;

    DBBINDING bindings[3] = {};
    auto bind = [&](std::size_t index, DBTYPE type, DBLENGTH size, DBBYTEOFFSET status,
                    DBBYTEOFFSET length, DBBYTEOFFSET value) {
        auto& item = bindings[index];
        item.iOrdinal = static_cast<DBORDINAL>(index + 1);
        item.obStatus = status;
        item.obLength = length;
        item.obValue = value;
        item.dwPart = DBPART_STATUS | DBPART_LENGTH | DBPART_VALUE;
        item.dwMemOwner = DBMEMOWNER_CLIENTOWNED;
        item.eParamIO = DBPARAMIO_NOTPARAM;
        item.cbMaxLen = size;
        item.wType = type;
    };
    bind(0, DBTYPE_I8, 8, offsetof(DrawRow, dropBoxSerial.status),
         offsetof(DrawRow, dropBoxSerial.length), offsetof(DrawRow, dropBoxSerial.value));
    bind(1, DBTYPE_I4, 4, offsetof(DrawRow, itemCategory.status),
         offsetof(DrawRow, itemCategory.length), offsetof(DrawRow, itemCategory.value));
    bind(2, DBTYPE_I4, 4, offsetof(DrawRow, amount.status),
         offsetof(DrawRow, amount.length), offsetof(DrawRow, amount.value));

    DBBINDSTATUS statuses[3] = {};
    HACCESSOR accessor = DB_NULL_HACCESSOR;
    const HRESULT created = accessors->CreateAccessor(
        DBACCESSOR_ROWDATA, 3, bindings, sizeof(DrawRow), &accessor, statuses);
    if (FAILED(created)) {
        if (accessor != DB_NULL_HACCESSOR)
            accessors->ReleaseAccessor(accessor, nullptr);
        return false;
    }
    bool bindingsValid = true;
    for (DBBINDSTATUS status : statuses) {
        if (status != DBBINDSTATUS_OK) bindingsValid = false;
    }
    if (!bindingsValid) {
        if (accessor != DB_NULL_HACCESSOR)
            accessors->ReleaseAccessor(accessor, nullptr);
        return false;
    }

    DrawRow row;
    DBCOUNTITEM count = 0;
    HROW* handles = nullptr;
    HRESULT result = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 1, &count, &handles);
    bool valid = SUCCEEDED(result) && count == 1 && handles;
    if (valid) valid = SUCCEEDED(rows->GetData(handles[0], accessor, &row));
    if (count && handles &&
        FAILED(rows->ReleaseRows(count, handles, nullptr, nullptr, nullptr)))
        valid = false;
    CoTaskMemFree(handles);

    count = 0;
    handles = nullptr;
    result = rows->GetNextRows(DB_NULL_HCHAPTER, 0, 1, &count, &handles);
    if (count && handles &&
        FAILED(rows->ReleaseRows(count, handles, nullptr, nullptr, nullptr)))
        valid = false;
    CoTaskMemFree(handles);
    if (FAILED(result) || count != 0) valid = false;

    if (accessor == DB_NULL_HACCESSOR ||
        FAILED(accessors->ReleaseAccessor(accessor, nullptr)))
        valid = false;
    valid = valid && ExactValue(row.dropBoxSerial) && ExactValue(row.itemCategory) &&
            ExactValue(row.amount);
    if (!valid || row.dropBoxSerial.value <= 0 ||
        row.itemCategory.value < kUnitCategoryBegin ||
        row.itemCategory.value >= kUnitCategoryEnd || row.amount.value != 1) return false;

    output.dropBoxSerial = row.dropBoxSerial.value;
    output.itemCategory = row.itemCategory.value;
    output.amount = row.amount.value;
    return true;
}

class DrawDatabase {
    ComPtr<IDBCreateCommand> commands_;
    ComPtr<ITransactionLocal> transactions_;

    bool CreateCommand(ComPtr<ICommandText>& command) {
        return commands_ &&
               SUCCEEDED(commands_->CreateCommand(
                   nullptr, __uuidof(ICommandText),
                   reinterpret_cast<IUnknown**>(command.put())));
    }

    bool MaterializeTransaction() {
        ComPtr<ICommandText> command;
        if (!CreateCommand(command) ||
            FAILED(command->SetCommandText(
                DBGUID_DBSQL,
                L"SET NOCOUNT ON;"
                L"DECLARE @object int;"
                L"SELECT @object=object_id FROM sys.objects "
                L"WHERE object_id=OBJECT_ID(N'sdgo_collection_capsule.Draw');"
                L"IF @object IS NULL OR @@TRANCOUNT<>1 "
                L"RAISERROR('Collection transaction unavailable',16,1);")))
            return false;
        DBROWCOUNT affected = 0;
        return SUCCEEDED(command->Execute(
            nullptr, IID_NULL, nullptr, &affected, nullptr));
    }

public:
    bool Open(void* connection) {
        ComPtr<ICommandText> native;
        ICommandText* borrowed = nullptr;
        if (!BorrowNativeCommand(connection, &borrowed)) return false;
        native.attach(borrowed);

        ComPtr<IGetDataSource> owner;
        if (FAILED(native->GetDBSession(
                __uuidof(IGetDataSource),
                reinterpret_cast<IUnknown**>(owner.put())))) return false;
        ComPtr<IDBCreateSession> sessions;
        if (FAILED(owner->GetDataSource(
                __uuidof(IDBCreateSession),
                reinterpret_cast<IUnknown**>(sessions.put())))) return false;
        if (FAILED(sessions->CreateSession(
                nullptr, __uuidof(IDBCreateCommand),
                reinterpret_cast<IUnknown**>(commands_.put())))) return false;
        return SUCCEEDED(commands_->QueryInterface(
            __uuidof(ITransactionLocal),
            reinterpret_cast<void**>(transactions_.put())));
    }

    int Draw(std::int32_t account, std::int32_t machine, ActivityReward& output) {
        if (!commands_ || !transactions_ || account <= 0 || machine <= 0) return kDatabaseFailure;
        Transaction transaction(transactions_.get());
        if (!transaction.Begin()) return kDatabaseFailure;
        // SQLOLEDB defers BEGIN TRANSACTION until a statement touches a table.
        // The draw procedure deliberately rejects delivery without an owned
        // outer transaction, so force and verify that boundary first.
        if (!MaterializeTransaction()) return kDatabaseFailure;

        ComPtr<ICommandText> command;
        if (!CreateCommand(command) ||
            FAILED(command->SetCommandText(
                DBGUID_DBSQL,
                L"{?=call sdgo_collection_capsule.Draw(?,?)}"))) return kDatabaseFailure;

        ComPtr<ICommandWithParameters> parameterInfo;
        ComPtr<IAccessor> parameterAccessors;
        if (FAILED(command->QueryInterface(
                __uuidof(ICommandWithParameters),
                reinterpret_cast<void**>(parameterInfo.put()))) ||
            FAILED(command->QueryInterface(
                __uuidof(IAccessor),
                reinterpret_cast<void**>(parameterAccessors.put())))) return kDatabaseFailure;

        DB_UPARAMS ordinals[] = {1, 2, 3};
        DBPARAMBINDINFO definitions[3] = {};
        for (auto& definition : definitions) {
            definition.pwszDataSourceType = const_cast<wchar_t*>(L"int");
            definition.ulParamSize = 4;
            definition.bPrecision = 10;
        }
        definitions[0].dwFlags = DBPARAMFLAGS_ISOUTPUT;
        definitions[1].dwFlags = DBPARAMFLAGS_ISINPUT;
        definitions[2].dwFlags = DBPARAMFLAGS_ISINPUT;
        if (FAILED(parameterInfo->SetParameterInfo(3, ordinals, definitions)))
            return kDatabaseFailure;

        struct DrawParameters {
            std::int32_t result;
            std::int32_t account;
            std::int32_t machine;
        } parameters = {-1, account, machine};
        DBBINDING bindings[3] = {};
        for (std::size_t index = 0; index < 3; ++index) {
            bindings[index].iOrdinal = static_cast<DBORDINAL>(index + 1);
            bindings[index].obValue = static_cast<DBBYTEOFFSET>(index * sizeof(std::int32_t));
            bindings[index].dwPart = DBPART_VALUE;
            bindings[index].dwMemOwner = DBMEMOWNER_CLIENTOWNED;
            bindings[index].eParamIO =
                index == 0 ? DBPARAMIO_OUTPUT : DBPARAMIO_INPUT;
            bindings[index].cbMaxLen = sizeof(std::int32_t);
            bindings[index].wType = DBTYPE_I4;
        }
        DBBINDSTATUS statuses[3] = {};
        HACCESSOR parameterAccessor = DB_NULL_HACCESSOR;
        const HRESULT accessorResult = parameterAccessors->CreateAccessor(
            DBACCESSOR_PARAMETERDATA, 3, bindings, sizeof(parameters),
            &parameterAccessor, statuses);
        if (FAILED(accessorResult)) {
            if (parameterAccessor != DB_NULL_HACCESSOR)
                parameterAccessors->ReleaseAccessor(parameterAccessor, nullptr);
            return kDatabaseFailure;
        }
        bool parametersValid = true;
        for (DBBINDSTATUS status : statuses) {
            if (status != DBBINDSTATUS_OK) parametersValid = false;
        }
        if (!parametersValid) {
            parameterAccessors->ReleaseAccessor(parameterAccessor, nullptr);
            return kDatabaseFailure;
        }

        DBPARAMS values = {&parameters, 1, parameterAccessor};
        DBROWCOUNT affected = 0;
        ComPtr<IRowset> rows;
        const HRESULT executed = command->Execute(
            nullptr, __uuidof(IRowset), &values, &affected,
            reinterpret_cast<IUnknown**>(rows.put()));

        ActivityReward pending = {};
        bool rowValid = false;
        if (SUCCEEDED(executed) && rows) rowValid = ReadOneRow(rows.get(), pending);
        rows.reset(); // SQL Server publishes procedure return parameters after rowset release.
        const bool accessorReleased =
            parameterAccessor != DB_NULL_HACCESSOR &&
            SUCCEEDED(parameterAccessors->ReleaseAccessor(
                parameterAccessor, nullptr));

        if (FAILED(executed)) return kDatabaseFailure;
        if (!accessorReleased) return kDatabaseFailure;
        if (parameters.result != 0)
            return parameters.result > 0 ? parameters.result : kDatabaseFailure;
        if (!rowValid) return kDatabaseFailure;
        if (!transaction.Commit()) return kDatabaseFailure;
        output = pending;
        return 0;
    }
};

} // namespace sdgo::collection_capsule
