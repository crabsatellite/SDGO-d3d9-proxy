#pragma once
#include <windows.h>
#include <oledb.h>
#include <transact.h>
#include <cstddef>
#include <cstring>
#include <string>
#include <type_traits>
#include "policy.h"

namespace sdgo::starter {
template<class T> class Com {
    T* p_ = nullptr;
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p_) p_->Release(); }
    T* get() const { return p_; }
    T** put() { if (p_) { p_->Release(); p_=nullptr; } return &p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
};
inline std::wstring Identifier(const std::wstring& name) {
    std::wstring out=L"[";
    for(wchar_t c:name) { out+=c; if(c==L']')out+=c; }
    return out+L"]";
}
inline bool NativeCommand(void* connection, ICommandText** command) {
    __try {
        if (!connection || *reinterpret_cast<int*>(static_cast<BYTE*>(connection)+0x240)) return false;
        *command=*reinterpret_cast<ICommandText**>(static_cast<BYTE*>(connection)+8);
        if (!*command) return false;
        (*command)->AddRef();
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
class OleDatabase final : public Database {
    Com<IDBCreateCommand> factory_;
    Com<ITransactionLocal> transaction_;
    std::wstring billing_;
    bool Create(const std::wstring& sql, Com<ICommandText>& command) {
        if(!factory_ || FAILED(factory_->CreateCommand(nullptr,__uuidof(ICommandText),reinterpret_cast<IUnknown**>(command.put()))))return false;
        return SUCCEEDED(command->SetCommandText(DBGUID_DBSQL,sql.c_str()));
    }
    template<class Integer> bool Scalar(const std::wstring& sql,Integer& value,const char* id=nullptr) {
        static_assert(std::is_integral_v<Integer> && (sizeof(Integer)==4 || sizeof(Integer)==8));
        Com<ICommandText> command;
        if(!Create(sql,command))return false;
        Com<IAccessor> parameters;
        HACCESSOR parameterHandle=DB_NULL_HACCESSOR;
        DBPARAMS params={};
        char login[15]={};
        if(id) {
            const auto length=strnlen_s(id,sizeof(login));
            if(length>=sizeof(login))return false;
            memcpy(login,id,length);
            Com<ICommandWithParameters> info;
            if(FAILED(command->QueryInterface(__uuidof(ICommandWithParameters),reinterpret_cast<void**>(info.put()))) ||
               FAILED(command->QueryInterface(__uuidof(IAccessor),reinterpret_cast<void**>(parameters.put()))))return false;
            DB_UPARAMS ordinal=1;
            DBPARAMBINDINFO definition={};
            definition.pwszDataSourceType=const_cast<wchar_t*>(L"varchar");
            definition.ulParamSize=14;definition.dwFlags=DBPARAMFLAGS_ISINPUT;
            if(FAILED(info->SetParameterInfo(1,&ordinal,&definition)))return false;
            DBBINDING bind={};
            bind.iOrdinal=1;bind.dwPart=DBPART_VALUE;bind.dwMemOwner=DBMEMOWNER_CLIENTOWNED;
            bind.eParamIO=DBPARAMIO_INPUT;bind.cbMaxLen=15;bind.wType=DBTYPE_STR;
            DBBINDSTATUS status=DBBINDSTATUS_BADBINDINFO;
            if(FAILED(parameters->CreateAccessor(DBACCESSOR_PARAMETERDATA,1,&bind,sizeof(login),&parameterHandle,&status)) || status!=DBBINDSTATUS_OK){
                if(parameterHandle!=DB_NULL_HACCESSOR)parameters->ReleaseAccessor(parameterHandle,nullptr);
                return false;
            }
            params.pData=login;params.cParamSets=1;params.hAccessor=parameterHandle;
        }
        Com<IRowset> rows;
        DBROWCOUNT affected=0;
        HRESULT result=command->Execute(nullptr,__uuidof(IRowset),id?&params:nullptr,&affected,reinterpret_cast<IUnknown**>(rows.put()));
        if(parameterHandle!=DB_NULL_HACCESSOR)parameters->ReleaseAccessor(parameterHandle,nullptr);
        if(FAILED(result)||!rows)return false;
        Com<IAccessor> accessor;
        if(FAILED(rows->QueryInterface(__uuidof(IAccessor),reinterpret_cast<void**>(accessor.put()))))return false;
        struct Cell { DBSTATUS status; DBLENGTH length; Integer value; } cell={};
        DBBINDING bind={};
        bind.iOrdinal=1;bind.obStatus=offsetof(Cell,status);bind.obLength=offsetof(Cell,length);bind.obValue=offsetof(Cell,value);
        bind.dwPart=DBPART_VALUE|DBPART_STATUS|DBPART_LENGTH;bind.dwMemOwner=DBMEMOWNER_CLIENTOWNED;
        bind.eParamIO=DBPARAMIO_NOTPARAM;bind.cbMaxLen=sizeof(Integer);bind.wType=sizeof(Integer)==8?DBTYPE_I8:DBTYPE_I4;
        HACCESSOR handle=DB_NULL_HACCESSOR;DBBINDSTATUS status=DBBINDSTATUS_BADBINDINFO;
        if(FAILED(accessor->CreateAccessor(DBACCESSOR_ROWDATA,1,&bind,sizeof(Cell),&handle,&status))||status!=DBBINDSTATUS_OK){
            if(handle!=DB_NULL_HACCESSOR)accessor->ReleaseAccessor(handle,nullptr);
            return false;
        }
        DBCOUNTITEM count=0;HROW* handles=nullptr;
        result=rows->GetNextRows(DB_NULL_HCHAPTER,0,1,&count,&handles);
        if(SUCCEEDED(result)&&count==1)result=rows->GetData(handles[0],handle,&cell);
        const bool ok=SUCCEEDED(result)&&count==1&&cell.status==DBSTATUS_S_OK;
        if(count&&handles)rows->ReleaseRows(count,handles,nullptr,nullptr,nullptr);
        CoTaskMemFree(handles);accessor->ReleaseAccessor(handle,nullptr);
        if(ok)value=cell.value;
        return ok;
    }
    bool Execute(const std::wstring& sql) {
        Com<ICommandText> command;if(!Create(sql,command))return false;
        DBROWCOUNT affected=0;
        return SUCCEEDED(command->Execute(nullptr,IID_NULL,nullptr,&affected,nullptr));
    }
public:
    explicit OleDatabase(std::wstring billing) : billing_(Identifier(billing)) {}
    bool Open(void* connection) {
        Com<ICommandText> native;
        if(!NativeCommand(connection,native.put()))return false;
        if(FAILED(native->GetDBSession(__uuidof(IDBCreateCommand),reinterpret_cast<IUnknown**>(factory_.put()))))return false;
        factory_->QueryInterface(__uuidof(ITransactionLocal),reinterpret_cast<void**>(transaction_.put()));
        return true;
    }
    bool Begin() override {
        if(!transaction_ || Active())return false;
        ULONG level=0;return SUCCEEDED(transaction_->StartTransaction(ISOLATIONLEVEL_READCOMMITTED,0,nullptr,&level));
    }
    bool Active() override { XACTTRANSINFO info={};return transaction_&&SUCCEEDED(transaction_->GetTransactionInfo(&info)); }
    bool Commit() override { return transaction_&&SUCCEEDED(transaction_->Commit(FALSE,0,0)); }
    void Abort() override { if(transaction_)transaction_->Abort(nullptr,FALSE,FALSE); }
    bool Account(const char* id,bool lock,int& value) override {
        return Scalar(L"SELECT ISNULL(MAX(f_idx),0) FROM dbo.tbl_Account "+std::wstring(lock?L"WITH(UPDLOCK,HOLDLOCK) ":L"")+L"WHERE f_loginId=?",value,id);
    }
    bool User(int id,bool lock,bool& exists) override {
        int value=0;const bool ok=Scalar(L"SELECT COUNT(*) FROM dbo.tbl_Users "+std::wstring(lock?L"WITH(UPDLOCK,HOLDLOCK) ":L"")+L"WHERE AccountNumber="+std::to_wstring(id),value);
        exists=value!=0;return ok;
    }
    bool Wallet(int id,const Settings& settings) override {
        return Execute(L"SET NOCOUNT ON; UPDATE "+billing_+L".dbo.tbls_AccountPocket SET f_GP="+std::to_wstring(settings.gp)+L",f_GC="+std::to_wstring(settings.cash)+L" WHERE f_idx="+std::to_wstring(id)+L"; IF @@ROWCOUNT<>1 RAISERROR('Starter wallet row missing',16,1);");
    }
    bool Rank(const Settings& settings,RankStart& rank) override {
        return Scalar(L"SELECT CASE WHEN COUNT(*)=1 THEN MIN(levelExp) ELSE -1 END FROM dbo.tbl_Levelname WHERE levelID="+std::to_wstring(settings.rank)+L" AND LevelStep="+std::to_wstring(settings.step),rank.experience);
    }
    bool Profile(int id,const Settings& settings,const RankStart& rank) override {
        return Execute(L"SET NOCOUNT ON; UPDATE dbo.tbl_Users SET UserLevelNumber="+std::to_wstring(settings.rank)+L",UserLevelStep="+std::to_wstring(settings.step)+L",UserExp="+std::to_wstring(rank.experience)+L" WHERE AccountNumber="+std::to_wstring(id)+L"; IF @@ROWCOUNT<>1 RAISERROR('Starter profile row missing',16,1);");
    }
    bool UnitSnapshot(int id,int64_t& lastUnit) override {
        // Keep pre-existing rows and block concurrent grants until initialization commits.
        return Scalar(L"SELECT ISNULL(MAX(UserUnitNumber),0) FROM dbo.tbl_UserUnits WITH(UPDLOCK,HOLDLOCK) WHERE AccountNumber="+std::to_wstring(id),lastUnit);
    }
    bool LimitUnits(int id,int64_t lastUnit,const Settings& settings) override {
        const auto account=std::to_wstring(id),boundary=std::to_wstring(lastUnit);
        return Execute(L"SET NOCOUNT ON; DELETE FROM dbo.tbl_UserUnits WHERE AccountNumber="+account+
            L" AND UserUnitNumber>"+boundary+L" AND UserUnitNumber NOT IN (SELECT TOP ("+std::to_wstring(settings.unitLimit)+
            L") UserUnitNumber FROM dbo.tbl_UserUnits WHERE AccountNumber="+account+L" AND UserUnitNumber>"+boundary+
            L" ORDER BY UserUnitSlotNumber,UserUnitNumber); "
            L"UPDATE dbo.tbl_Users SET UserUnitKindCnt=(SELECT COUNT(DISTINCT UnitNumber) FROM dbo.tbl_UserUnits WHERE AccountNumber="+account+
            L"),UserUnitTotalCnt=(SELECT COUNT(*) FROM dbo.tbl_UserUnits WHERE AccountNumber="+account+
            L"),GoUserUnitNum=CASE WHEN GoUserUnitNum<>0 AND NOT EXISTS(SELECT 1 FROM dbo.tbl_UserUnits WHERE AccountNumber="+account+
            L" AND UserUnitNumber=GoUserUnitNum) THEN ISNULL((SELECT MIN(UserUnitNumber) FROM dbo.tbl_UserUnits WHERE AccountNumber="+account+
            L"),0) ELSE GoUserUnitNum END WHERE AccountNumber="+account+L"; IF @@ROWCOUNT<>1 RAISERROR('Starter unit owner missing',16,1);");
    }
};
}
