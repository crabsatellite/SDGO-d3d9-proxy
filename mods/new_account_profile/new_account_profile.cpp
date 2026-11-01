#define DBINITCONSTANTS
#include <windows.h>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"
#include "oledb_session.h"

namespace {
using namespace sdgo::starter;
using LoginFn=int(__thiscall*)(void*,void*,LoginInput*,void*);
using UserFn=int(__thiscall*)(void*,void*,UserInput*,UserOutput*);
LoginFn originalLogin=nullptr;
UserFn originalUser=nullptr;
const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
Settings settings;
std::wstring billing;

template<class T> bool Read(const T* source,T& target) {
    __try { if(!source)return false;memcpy(&target,source,sizeof(T));return true; }
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
template<class T> bool Write(T* target,const T& value) {
    __try { if(!target)return false;memcpy(target,&value,sizeof(T));return true; }
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool Number(const sdgo::ModSettings& config,const char* key,const char* fallback,int low,int high,int& value) {
    char text[32]={};auto n=config.String(key,fallback,text,sizeof(text));
    if(!n||n>=sizeof(text)-1)return false;
    unsigned number=0;
    for(unsigned i=0;i<n;++i){
        if(text[i]<'0'||text[i]>'9')return false;
        const unsigned digit=text[i]-'0';
        if(number>static_cast<unsigned>(high)/10 || (number==static_cast<unsigned>(high)/10&&digit>static_cast<unsigned>(high)%10))return false;
        number=number*10+digit;
    }
    if(number<static_cast<unsigned>(low))return false;value=static_cast<int>(number);return true;
}
int __fastcall LoginHook(void* self,void*,void* connection,LoginInput* raw,void* output) {
    LoginInput input={};
    if(!Read(raw,input))return kDatabaseError;
    if(!memchr(input.id,0,sizeof(input.id))){
        int failure=4;Write(&raw->result,failure);SecureZeroMemory(input.password,sizeof(input.password));return kDatabaseError;
    }
    int result=kDatabaseError;
    try {
        OleDatabase database(billing);
        if(!database.Open(connection)){
            int failure=4;Write(&raw->result,failure);
            g_api->Log(g_mod,SDGO_LOG_ERROR,"cannot enforce new-account defaults: native database session unavailable");
            SecureZeroMemory(input.password,sizeof(input.password));return kDatabaseError;
        }
        result=Login(database,settings,input,[&](LoginInput& in){return originalLogin(self,connection,&in,output);});
        Write(&raw->result,input.result);
        if(result==kDatabaseError && input.result==4)g_api->Log(g_mod,SDGO_LOG_ERROR,"new-account registration policy returned a database error");
    }catch(...){int failure=4;Write(&raw->result,failure);g_api->Log(g_mod,SDGO_LOG_ERROR,"new-account registration policy failed before publishing a result");}
    SecureZeroMemory(input.password,sizeof(input.password));
    return result;
}
int __fastcall UserHook(void* self,void*,void* connection,UserInput* raw,UserOutput* output) {
    UserInput input={};UserOutput row={};
    if(!Read(raw,input)||!Read(output,row)||input.account<=0)return kDatabaseError;
    int result=kDatabaseError;
    try {
        OleDatabase database(billing);
        if(!database.Open(connection)){
            int failure=1;Write(&raw->result,failure);
            g_api->Log(g_mod,SDGO_LOG_ERROR,"cannot enforce first-profile defaults: native database session unavailable");
            return kDatabaseError;
        }
        result=User(database,settings,input,row,[&](UserInput& in,UserOutput& out){return originalUser(self,connection,&in,&out);});
        Write(&raw->tutorial,input.tutorial);Write(&raw->result,input.result);
        if(result==0)Write(output,row);
        else if(result==kDatabaseError)g_api->Log(g_mod,SDGO_LOG_ERROR,"new-player profile policy failed; incomplete creation was rolled back when active");
    }catch(...){int failure=1;Write(&raw->result,failure);g_api->Log(g_mod,SDGO_LOG_ERROR,"new-player profile policy failed before publishing a result");}
    return result;
}
}
SDGO_MOD_ENTRY {
    if(!api || api->version!=SDGO_API_VERSION || api->size<offsetof(SdgoApi,ModDirectory)+sizeof(api->ModDirectory))return 0;
    g_api=api;g_mod=mod;
    sdgo::ModSettings config(api,mod);
    if(!Number(config,"InitialGP","5000",0,INT_MAX,settings.gp)||
       !Number(config,"InitialCash","0",0,INT_MAX,settings.cash)||
       !Number(config,"RankNumber","5",0,20,settings.rank)||
       !Number(config,"RankStep","1",1,5,settings.step)){
        api->Log(mod,SDGO_LOG_ERROR,"invalid starter values: GP/Cash 0..INT_MAX, RankNumber 0..20, RankStep 1..5");return 0;
    }
    wchar_t path[32768]={},name[32]={};
    if(!GetModuleFileNameW(nullptr,path,32768))return 0;
    std::wstring ini=path;const auto dot=ini.find_last_of(L'.');if(dot==std::wstring::npos)return 0;ini.replace(dot,std::wstring::npos,L".ini");
    DWORD length=GetPrivateProfileStringW(L"DB StoreBilling",L"DBName",L"",name,32,ini.c_str());
    if(!length||length>=31){api->Log(mod,SDGO_LOG_ERROR,"need the host INI's [DB StoreBilling] DBName; no credentials are copied or stored by this mod");return 0;}
    billing=name;
    if(!api->Symbol(mod,"starter.connection_command"))return 0;
    if(!api->HookCode(mod,"starter.login",reinterpret_cast<void*>(&LoginHook),reinterpret_cast<void**>(&originalLogin)))return 0;
    if(!api->HookCode(mod,"starter.profile",reinterpret_cast<void*>(&UserHook),reinterpret_cast<void**>(&originalUser)))return 0;
    api->Log(mod,SDGO_LOG_INFO,"new-account defaults active: GP=%d, cash=%d, rank=%d/%d; existing profiles and SQL definitions are preserved",settings.gp,settings.cash,settings.rank,settings.step);
    return 1;
}
