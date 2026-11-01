#pragma once
#define DBINITCONSTANTS
#include <windows.h>
#include <oledb.h>
#include <oledberr.h>
#include <transact.h>
#include <bcrypt.h>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>
#include "sql_policy.h"

namespace sdgo::rank_fix {
template<class T> class Com {
    T* p_=nullptr;
public:
    Com()=default;
    Com(const Com&)=delete;
    Com& operator=(const Com&)=delete;
    ~Com(){if(p_)p_->Release();}
    T* get()const{return p_;} T* operator->()const{return p_;}
    T** put(){if(p_){p_->Release();p_=nullptr;}return &p_;}
};
inline std::wstring Hash(const std::wstring& value) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    std::array<UCHAR,32> bytes={};DWORD objectSize=0,got=0;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)throw std::runtime_error("hash provider unavailable");
    const auto close=[&](){if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);};
    if(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&got,0)<0){close();throw std::runtime_error("hash object unavailable");}
    std::vector<UCHAR> object(objectSize);
    if(BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)<0||
       BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(value.data())),static_cast<ULONG>(value.size()*sizeof(wchar_t)),0)<0||
       BCryptFinishHash(hash,bytes.data(),static_cast<ULONG>(bytes.size()),0)<0){close();throw std::runtime_error("hash failed");}
    close();std::wstring out;for(UCHAR b:bytes){out+=L"0123456789abcdef"[b>>4];out+=L"0123456789abcdef"[b&15];}return out;
}
inline bool Borrow(void* connection,ICommandText** out) {
    __try {
        if(!connection||*reinterpret_cast<int*>(static_cast<BYTE*>(connection)+0x240))return false;
        *out=*reinterpret_cast<ICommandText**>(static_cast<BYTE*>(connection)+8);
        if(!*out)return false;(*out)->AddRef();return true;
    }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
class Session {
    Com<IDBCreateCommand> factory_;
    Com<ITransactionLocal> transaction_;
    bool active_=false;
    bool Create(const std::wstring& sql,Com<ICommandText>& command) {
        return factory_.get()&&SUCCEEDED(factory_->CreateCommand(nullptr,__uuidof(ICommandText),reinterpret_cast<IUnknown**>(command.put())))&&
            SUCCEEDED(command->SetCommandText(DBGUID_DBSQL,sql.c_str()));
    }
public:
    ~Session(){Abort();}
    void Abort(){if(active_){transaction_->Abort(nullptr,FALSE,FALSE);active_=false;}}
    bool Open(void* connection) {
        Com<ICommandText> native;
        return Borrow(connection,native.put())&&SUCCEEDED(native->GetDBSession(__uuidof(IDBCreateCommand),reinterpret_cast<IUnknown**>(factory_.put())))&&
            SUCCEEDED(factory_->QueryInterface(__uuidof(ITransactionLocal),reinterpret_cast<void**>(transaction_.put())));
    }
    bool Begin() {
        XACTTRANSINFO current={};if(SUCCEEDED(transaction_->GetTransactionInfo(&current)))return false;
        ULONG level=0;if(FAILED(transaction_->StartTransaction(ISOLATIONLEVEL_READCOMMITTED,0,nullptr,&level)))return false;
        active_=true;return level==1;
    }
    bool Commit(){if(!active_||FAILED(transaction_->Commit(FALSE,0,0)))return false;active_=false;return true;}
    bool Exec(const std::wstring& sql) {
        Com<ICommandText> command;if(!Create(sql,command))return false;
        DBROWCOUNT affected=0;return SUCCEEDED(command->Execute(nullptr,IID_NULL,nullptr,&affected,nullptr));
    }
    bool String(const std::wstring& sql,std::wstring& value) {
        Com<ICommandText> command;if(!Create(sql,command))return false;
        Com<IRowset> rows;Com<IMultipleResults> results;DBROWCOUNT affected=0;
        if(FAILED(command->Execute(nullptr,__uuidof(IMultipleResults),nullptr,&affected,reinterpret_cast<IUnknown**>(results.put())))||!results.get())return false;
        for(;;){
            const HRESULT next=results->GetResult(nullptr,DBRESULTFLAG_DEFAULT,__uuidof(IRowset),&affected,reinterpret_cast<IUnknown**>(rows.put()));
            if(FAILED(next)||next==DB_S_NORESULT)return false;
            if(rows.get())break;
        }
        Com<IAccessor> access;if(FAILED(rows->QueryInterface(__uuidof(IAccessor),reinterpret_cast<void**>(access.put()))))return false;
        struct Cell { DBSTATUS status;DBLENGTH length;wchar_t value[262145]; };
        auto cell=std::make_unique<Cell>();memset(cell.get(),0,sizeof(Cell));
        DBBINDING binding={};binding.iOrdinal=1;binding.obStatus=offsetof(Cell,status);binding.obLength=offsetof(Cell,length);
        binding.obValue=offsetof(Cell,value);binding.dwPart=DBPART_VALUE|DBPART_STATUS|DBPART_LENGTH;
        binding.dwMemOwner=DBMEMOWNER_CLIENTOWNED;binding.wType=DBTYPE_WSTR;binding.cbMaxLen=sizeof(cell->value);
        HACCESSOR handle=DB_NULL_HACCESSOR;DBBINDSTATUS status=DBBINDSTATUS_BADBINDINFO;
        if(FAILED(access->CreateAccessor(DBACCESSOR_ROWDATA,1,&binding,sizeof(Cell),&handle,&status))||status!=DBBINDSTATUS_OK)return false;
        DBCOUNTITEM count=0;HROW* data=nullptr;HRESULT hr=rows->GetNextRows(DB_NULL_HCHAPTER,0,1,&count,&data);
        if(SUCCEEDED(hr)&&count==1)hr=rows->GetData(data[0],handle,cell.get());
        const bool ok=SUCCEEDED(hr)&&count==1&&cell->status==DBSTATUS_S_OK&&cell->length<sizeof(cell->value)&&cell->length%2==0;
        if(ok)value.assign(cell->value,cell->length/sizeof(wchar_t));
        if(count&&data)rows->ReleaseRows(count,data,nullptr,nullptr,nullptr);
        CoTaskMemFree(data);access->ReleaseAccessor(handle,nullptr);return ok;
    }
};
inline bool Need(bool value,const char* operation="rank policy SQL operation failed"){if(!value)throw std::runtime_error(operation);return value;}
inline bool Prepare(Session& db,const std::wstring& expectedDatabase) {
    std::wstring database;
    Need(db.String(L"SELECT CASE WHEN DB_ID("+Literal(expectedDatabase)+L")=DB_ID() THEN N'1' ELSE N'0' END",database),"rank database identity unavailable");
    if(database!=L"1")return false;
    Need(db.Begin(),"rank owned transaction unavailable");
    struct Rollback { Session& db; ~Rollback(){db.Abort();} } rollback{db};
    // SQLOLEDB defers the SQL transaction until a table access. A constant-only
    // SELECT does not start it, and sp_getapplock(Transaction) returns -999.
    std::wstring transactionCount;
    Need(db.String(L"SELECT CONVERT(nvarchar(20),@@TRANCOUNT) FROM sys.objects WHERE object_id=OBJECT_ID(N'dbo.sp_SetBattleExp_V4')",transactionCount),"rank transaction probe unavailable");
    Need(transactionCount==L"1","rank SQL transaction did not start");
    std::wstring lock;
    Need(db.String(L"DECLARE @r int; EXEC @r=sys.sp_getapplock @Resource=N'sdgo.rank_progression_fix.mod.v1',@LockMode='Exclusive',@LockOwner='Transaction',@LockTimeout=2000; SELECT CONVERT(nvarchar(20),@r)",lock),"rank lock result unavailable");
    if(lock.empty()||lock[0]==L'-')throw std::runtime_error("rank policy SQL lock unavailable ("+std::to_string(std::stoi(lock))+")");
    const auto schema=Identifier(kSchema);std::wstring exists;
    Need(db.String(L"SELECT CASE WHEN SCHEMA_ID(N'sdgo_rank_progression_fix') IS NULL THEN N'0' ELSE N'1' END",exists));
    if(exists==L"0") {
        Need(db.Exec(L"CREATE SCHEMA "+schema+L" AUTHORIZATION dbo"));
        Need(db.Exec(L"EXEC sys.sp_addextendedproperty @name=N'sdgo_owner',@value=N'rank_progression_fix/v1',@level0type=N'SCHEMA',@level0name=N'sdgo_rank_progression_fix'"));
        Need(db.Exec(L"CREATE TABLE "+schema+L".state(procedure_name nvarchar(128) NOT NULL PRIMARY KEY,source_hash char(64) NOT NULL,installed_hash char(64) NOT NULL,sql_options int NOT NULL)"));
    }
    std::wstring owner;
    Need(db.String(L"SELECT ISNULL(CONVERT(nvarchar(128),(SELECT value FROM sys.extended_properties WHERE class=3 AND major_id=SCHEMA_ID(N'sdgo_rank_progression_fix') AND name=N'sdgo_owner')),N'')",owner));
    if(owner!=L"rank_progression_fix/v1")throw std::runtime_error("rank policy schema belongs to another owner");
    std::wstring sessionOptions;
    Need(db.String(L"SELECT CONVERT(nvarchar(10),CONVERT(int,SESSIONPROPERTY('ANSI_NULLS'))+2*CONVERT(int,SESSIONPROPERTY('QUOTED_IDENTIFIER')))",sessionOptions));
    const int oldOptions=std::stoi(sessionOptions);
    const auto setOptions=[&](int flags){return db.Exec(std::wstring(L"SET ANSI_NULLS ")+(flags&1?L"ON":L"OFF"))&&db.Exec(std::wstring(L"SET QUOTED_IDENTIFIER ")+(flags&2?L"ON":L"OFF"));};
    try {
        for(const auto* rawName:kNames) {
            const std::wstring name=rawName;
            const auto native=Literal(L"dbo."+name),alias=Literal(std::wstring(kSchema)+L"."+name);
            std::wstring source,current,flags,savedHash,savedOptions,currentOptions;
            Need(db.String(L"SELECT ISNULL(OBJECT_DEFINITION(OBJECT_ID("+native+L",N'P')),N'')",source));
            if(source.empty())throw std::runtime_error("native rank procedure unavailable");
            Need(db.String(L"SELECT CONVERT(nvarchar(10),CONVERT(int,uses_ansi_nulls)+2*CONVERT(int,uses_quoted_identifier)) FROM sys.sql_modules WHERE object_id=OBJECT_ID("+native+L",N'P')",flags));
            Need(db.String(L"SELECT ISNULL(OBJECT_DEFINITION(OBJECT_ID("+alias+L",N'P')),N'')",current));
            Need(db.String(L"SELECT ISNULL((SELECT CONVERT(nvarchar(64),installed_hash) FROM "+schema+L".state WHERE procedure_name="+Literal(name)+L"),N'')",savedHash));
            if((current.empty()!=savedHash.empty())||(!current.empty()&&Hash(Canonical(current,name,kSchema))!=savedHash))
                throw std::runtime_error("rank policy object has an external edit");
            if(!current.empty()) {
                Need(db.String(L"SELECT CONVERT(nvarchar(10),sql_options) FROM "+schema+L".state WHERE procedure_name="+Literal(name),savedOptions));
                Need(db.String(L"SELECT CONVERT(nvarchar(10),CONVERT(int,uses_ansi_nulls)+2*CONVERT(int,uses_quoted_identifier)) FROM sys.sql_modules WHERE object_id=OBJECT_ID("+alias+L",N'P')",currentOptions));
                if(savedOptions!=currentOptions)throw std::runtime_error("rank policy SQL options have an external edit");
            }
            const auto clone=Clone(source,name,!current.empty());
            const auto sourceHash=Hash(Canonical(source,name,L"dbo"));
            const auto targetHash=Hash(Canonical(clone,name,kSchema));
            const int options=std::stoi(flags);
            if(current.empty()||targetHash!=savedHash||savedOptions!=flags) {
                Need(setOptions(options));Need(db.Exec(clone));
                std::wstring installed;Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID("+alias+L",N'P'))",installed));
                if(Hash(Canonical(installed,name,kSchema))!=targetHash)throw std::runtime_error("rank policy readback mismatch");
                Need(db.Exec(L"DELETE FROM "+schema+L".state WHERE procedure_name="+Literal(name)));
                Need(db.Exec(L"INSERT "+schema+L".state(procedure_name,source_hash,installed_hash,sql_options) VALUES("+Literal(name)+L","+Literal(sourceHash)+L","+Literal(targetHash)+L","+std::to_wstring(options)+L")"));
            }
        }
        Need(setOptions(oldOptions));Need(db.Commit());return true;
    }catch(...){db.Abort();Need(setOptions(oldOptions),"rank SQL option restoration failed");throw;}
}
}
