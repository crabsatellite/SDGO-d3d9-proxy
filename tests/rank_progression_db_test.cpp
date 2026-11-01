#include <windows.h>
#include <msdasc.h>
#include <memory>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include "../mods/rank_progression_fix/oledb_policy.h"

using namespace sdgo::rank_fix;
static std::wstring Read(const wchar_t* path,const wchar_t* key) {
    wchar_t value[2048]={};auto n=GetPrivateProfileStringW(L"DB Game",key,L"",value,2048,path);
    if(!n||n>=2047)throw std::runtime_error("fixture connection setting unavailable");return value;
}
static std::wstring Quote(const std::wstring& value) {
    std::wstring out=L"\"";for(auto c:value){out+=c;if(c==L'"')out+=c;}return out+L"\"";
}
static void Replace(std::wstring& text,const std::wstring& needle,const std::wstring& value) {
    size_t p=0;while((p=text.find(needle,p))!=std::wstring::npos){text.replace(p,needle.size(),value);p+=value.size();}
}
static void BoundCall(IDBCreateCommand* factory,const std::wstring& sql,const std::wstring& data,const std::wstring& database,bool enabled) {
    struct Value { DBSTATUS status=DBSTATUS_S_OK;DBLENGTH length=sizeof(LONGLONG);union {LONGLONG value;char text[4096];};Value():value(0){} };
    std::vector<Value> values;std::vector<DBBINDING> bindings;
    std::vector<DBPARAMBINDINFO> metadata;std::vector<DB_UPARAMS> ordinals;std::vector<std::wstring> types;
    std::wistringstream input(data);std::wstring token;
    while(std::getline(input,token,L',')){
        const auto a=token.find(L':'),b=token.find(L':',a+1);Need(a!=std::wstring::npos&&b!=std::wstring::npos);
        types.push_back(token.substr(0,a));Value value;value.value=std::stoll(token.substr(b+1));values.push_back(value);
        const auto direction=static_cast<DBPARAMIO>(std::stoi(token.substr(a+1,b-a-1)));
        DBBINDING binding={};binding.iOrdinal=values.size();binding.dwPart=DBPART_STATUS|DBPART_LENGTH|DBPART_VALUE;
        binding.obStatus=(values.size()-1)*sizeof(Value)+offsetof(Value,status);
        binding.obLength=(values.size()-1)*sizeof(Value)+offsetof(Value,length);
        binding.obValue=(values.size()-1)*sizeof(Value)+offsetof(Value,value);
        binding.dwMemOwner=DBMEMOWNER_CLIENTOWNED;binding.eParamIO=direction;
        const bool text=types.back()==L"varchar";binding.wType=static_cast<DBTYPE>(text?DBTYPE_STR:DBTYPE_I8);binding.cbMaxLen=text?sizeof(Value::text):sizeof(LONGLONG);
        bindings.push_back(binding);ordinals.push_back(bindings.size());DBPARAMBINDINFO info={};
        info.dwFlags=direction==DBPARAMIO_OUTPUT?DBPARAMFLAGS_ISOUTPUT:DBPARAMFLAGS_ISINPUT;if(text)info.ulParamSize=4095;metadata.push_back(info);
    }
    for(size_t i=0;i<types.size();++i)metadata[i].pwszDataSourceType=types[i].data();
    Com<ICommandText> command;Need(SUCCEEDED(factory->CreateCommand(nullptr,__uuidof(ICommandText),reinterpret_cast<IUnknown**>(command.put()))));
    Need(SUCCEEDED(command->SetCommandText(DBGUID_DBSQL,sql.c_str())));
    Com<ICommandWithParameters> parameters;Need(SUCCEEDED(command->QueryInterface(__uuidof(ICommandWithParameters),reinterpret_cast<void**>(parameters.put()))));
    Need(SUCCEEDED(parameters->SetParameterInfo(ordinals.size(),ordinals.data(),metadata.data())));
    Com<IAccessor> access;Need(SUCCEEDED(command->QueryInterface(__uuidof(IAccessor),reinterpret_cast<void**>(access.put()))));
    std::vector<DBBINDSTATUS> status(bindings.size());HACCESSOR handle=DB_NULL_HACCESSOR;
    Need(SUCCEEDED(access->CreateAccessor(DBACCESSOR_PARAMETERDATA,bindings.size(),bindings.data(),sizeof(Value)*values.size(),&handle,status.data())));
    if(enabled){
        // The live command already owns parameter bindings when its leased
        // session is borrowed. Policy setup must leave those bindings usable.
        std::vector<BYTE> lease(0x248,0);auto pointer=command.get();memcpy(lease.data()+8,&pointer,sizeof(pointer));
        Session policy;Need(policy.Open(lease.data()));Need(Prepare(policy,database));
    }
    DBPARAMS params={};params.pData=values.data();params.cParamSets=1;params.hAccessor=handle;DBROWCOUNT affected=0;
    const auto hr=command->Execute(nullptr,IID_NULL,&params,&affected,nullptr);access->ReleaseAccessor(handle,nullptr);
    Need(SUCCEEDED(hr),"qualified OLE DB bound call failed");
    Need(values[0].status==DBSTATUS_S_OK&&values[0].value==0,"bound native return code differs");
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=4)return 2;
    if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)))return 3;
    int result=1;
    try {
        Com<IDataInitialize> init;Com<IDBInitialize> source;Com<IDBCreateSession> sessionMaker;Com<IDBCreateCommand> factory;
        Need(SUCCEEDED(CoCreateInstance(CLSID_MSDAINITIALIZE,nullptr,CLSCTX_INPROC_SERVER,__uuidof(IDataInitialize),reinterpret_cast<void**>(init.put()))));
        const auto database=Read(argv[1],L"DBName");
        auto connection=L"Provider=SQLOLEDB;OLE DB Services=-2;Data Source="+Quote(Read(argv[1],L"DBServerName"))+L";Initial Catalog="+Quote(database)+L";User ID="+Quote(Read(argv[1],L"ID"))+L";Password="+Quote(Read(argv[1],L"PWD"))+L";";
        Need(SUCCEEDED(init->GetDataSource(nullptr,CLSCTX_INPROC_SERVER,connection.c_str(),__uuidof(IDBInitialize),reinterpret_cast<IUnknown**>(source.put()))));
        Need(SUCCEEDED(source->Initialize()));
        Need(SUCCEEDED(source->QueryInterface(__uuidof(IDBCreateSession),reinterpret_cast<void**>(sessionMaker.put()))));
        Need(SUCCEEDED(sessionMaker->CreateSession(nullptr,__uuidof(IDBCreateCommand),reinterpret_cast<IUnknown**>(factory.put()))));
        Com<ICommandText> native;Need(SUCCEEDED(factory->CreateCommand(nullptr,__uuidof(ICommandText),reinterpret_cast<IUnknown**>(native.put()))));
        std::vector<BYTE> layout(0x248,0);auto pointer=native.get();memcpy(layout.data()+8,&pointer,sizeof(pointer));
        Session db;Need(db.Open(layout.data()));
        std::cerr<<"fixture: borrowed native session open\n";
        std::wstring before[2];for(int i=0;i<2;++i)Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID("+Literal(L"dbo."+std::wstring(kNames[i]))+L"))",before[i]));
        std::cerr<<"fixture: original definitions read\n";
        Need(!Prepare(db,L"not_the_fixture_database"));
        Need(db.Begin());std::wstring count;
        Need(db.String(L"SELECT CONVERT(nvarchar(20),@@TRANCOUNT) FROM sys.objects WHERE object_id=OBJECT_ID(N'dbo.sp_SetBattleExp_V4')",count));
        {
            Session guest;Need(guest.Open(layout.data()));bool refused=false;
            try{Prepare(guest,database);}catch(const std::exception&){refused=true;}
            Need(refused,"policy joined a native transaction");
        }
        Need(db.String(L"SELECT CONVERT(nvarchar(20),@@TRANCOUNT)",count));Need(count==L"1","native transaction was aborted by the mod");db.Abort();
        Need(db.Exec(L"CREATE SCHEMA sdgo_rank_progression_fix AUTHORIZATION dbo"));
        bool refusedOwner=false;try{Prepare(db,database);}catch(const std::exception&){refusedOwner=true;}
        Need(refusedOwner,"foreign schema was accepted");
        Need(db.Exec(L"DROP SCHEMA sdgo_rank_progression_fix"));
        // The second owned procedure fails after the first was created. Neither
        // procedure, the state table nor schema may survive the aborted setup.
        Need(db.Exec(L"CREATE TRIGGER rank_fixture_reject ON DATABASE FOR CREATE_PROCEDURE AS IF EVENTDATA().value('(/EVENT_INSTANCE/SchemaName)[1]','nvarchar(128)')=N'sdgo_rank_progression_fix' AND EVENTDATA().value('(/EVENT_INSTANCE/ObjectName)[1]','nvarchar(128)')=N'sp_SetGridBattleExp_V2' BEGIN RAISERROR('fixture rejection',16,1); ROLLBACK; END"));
        bool rejected=false;try{Prepare(db,database);}catch(const std::exception&){rejected=true;}
        Need(rejected,"second-procedure rejection was ignored");
        std::wstring absent;Need(db.String(L"SELECT CASE WHEN SCHEMA_ID(N'sdgo_rank_progression_fix') IS NULL THEN N'1' ELSE N'0' END",absent));
        Need(absent==L"1","failed initialization left partial objects");
        Need(db.Exec(L"DROP TRIGGER rank_fixture_reject ON DATABASE"));
        Need(Prepare(db,database));std::cerr<<"fixture: first policy preparation passed\n";Need(Prepare(db,database));
        for(int i=0;i<2;++i) {
            std::wstring after;Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID("+Literal(L"dbo."+std::wstring(kNames[i]))+L"))",after));
            Need(before[i]==after);
            std::wstring clone;Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID("+Literal(std::wstring(kSchema)+L"."+kNames[i])+L"))",clone));
            Need(Canonical(clone,kNames[i],kSchema)==Canonical(Clone(before[i],kNames[i],true),kNames[i],kSchema));
        }
        std::ifstream file(argv[2],std::ios::binary);Need(!!file);std::string line;int checks=0;
        while(std::getline(file,line)) {
            if(!line.empty()&&line.back()=='\r')line.pop_back();if(line.empty())continue;
            for(unsigned char c:line)Need(c<128);
            std::vector<std::wstring> fields;size_t start=0;
            for(size_t i=0;i<=line.size();++i)if(i==line.size()||line[i]=='|'){
                fields.emplace_back(line.begin()+static_cast<std::ptrdiff_t>(start),line.begin()+static_cast<std::ptrdiff_t>(i));start=i+1;
            }
            Need(fields.size()==4);Need(db.Exec(fields[2]));auto command=fields[3];
            Replace(command,L"@SCHEMA@",fields[0]==L"1"?Identifier(kSchema):L"dbo");
            Need(db.Exec(command));
            std::wstring state;
            Need(db.String(L"SELECT CONVERT(nvarchar(20),UserLevelNumber)+N','+CONVERT(nvarchar(20),UserLevelStep)+N','+CONVERT(nvarchar(20),UserExp)+N','+CONVERT(nvarchar(20),UserBattle)+N','+CONVERT(nvarchar(20),UserWin) FROM dbo.tbl_Users WHERE AccountNumber=900000001",state));
            Need(state==fields[1],"native rank boundary output differs");++checks;
        }
        Need(checks>=4);
        std::wifstream rpc(argv[3]);Need(!!rpc);std::wstring call;int boundChecks=0;
        while(std::getline(rpc,call)){
            if(!call.empty()&&call.back()==L'\r')call.pop_back();const auto split=call.find(L'|');Need(split!=std::wstring::npos);
            for(int enabled=0;enabled<2;++enabled){
                Need(db.Exec(L"UPDATE dbo.tbl_Users SET UserLevelNumber=15,UserLevelStep=1,UserExp=30090 WHERE AccountNumber=900000001"));
                const auto nativeCall=call.substr(0,split);const auto sql=enabled?Redirect(nativeCall,kNames[boundChecks/2]):nativeCall;
                BoundCall(factory.get(),sql,call.substr(split+1),database,enabled!=0);std::wstring actual;
                Need(db.String(L"SELECT CONVERT(nvarchar(20),UserLevelNumber)+N','+CONVERT(nvarchar(20),UserLevelStep)+N','+CONVERT(nvarchar(20),UserExp) FROM dbo.tbl_Users WHERE AccountNumber=900000001",actual));
                Need(actual==(enabled?L"15,1,30110":L"5,2,30110"),"bound native output differs");++boundChecks;
            }
        }
        Need(boundChecks==4);
        // A manual edit to a Mod-owned procedure is detected, never overwritten.
        std::wstring alias;Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID(N'sdgo_rank_progression_fix.sp_SetBattleExp_V4'))",alias));
        auto h=ParseHeader(alias,kNames[0],kSchema);alias.replace(h.action,h.actionLength,L"ALTER");
        Need(db.Exec(alias+L"\n-- external fixture edit\n"));
        bool refused=false;try{Prepare(db,database);}catch(const std::exception&){refused=true;}
        Need(refused);
        for(int i=0;i<2;++i){std::wstring current;Need(db.String(L"SELECT OBJECT_DEFINITION(OBJECT_ID("+Literal(L"dbo."+std::wstring(kNames[i]))+L"))",current));Need(current==before[i]);}
        Need(db.String(L"SELECT CONVERT(nvarchar(20),@@TRANCOUNT)",absent));Need(absent==L"0");
        std::cout<<"PASS real OLE DB: owned policies, idempotence, native definitions unchanged, enabled/disabled outputs, rank boundaries, atomic failure, foreign edit guard ("<<checks<<" native calls + "<<boundChecks<<" bound RPC calls)\n";
        result=0;
    }catch(const std::exception& e){std::cerr<<"Rank Mod SQL fixture failed: "<<e.what()<<"\n";}
    CoUninitialize();return result;
}
