#include <windows.h>
#include <memory>
#include <atomic>
#include <string>
#include "sdgo/sdgo_api.h"
#include "oledb_policy.h"

namespace {
// Both verified native entry points return a 32-bit result in EAX (ret 4).
// The hook must preserve that result, including on unrelated startup queries.
using Execute=int(__thiscall*)(void*,void*);
Execute originals[2]={};
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
std::wstring database;
const wchar_t* commands[2]={};
std::atomic<unsigned> active{0},errors{0};
bool PreparePolicy(void* connection) {
    sdgo::rank_fix::Session session;
    return session.Open(connection)&&sdgo::rank_fix::Prepare(session,database);
}
bool (*preparePolicy)(void*)=&PreparePolicy;

bool ReadCommand(void* info,const wchar_t** command) {
    __try {if(!info)return false;memcpy(command,static_cast<BYTE*>(info)+0xc,sizeof(*command));return *command!=nullptr;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
int Match(const wchar_t* command) {
    __try {
        for(int i=0;i<2;++i){const auto length=wcslen(commands[i]);if(wcsncmp(command,commands[i],length+1)==0)return i;}
    }__except(EXCEPTION_EXECUTE_HANDLER){return -1;}return -1;
}
int Redirected(Execute original,void* connection,void* info,const wchar_t* from,const wchar_t* to) {
    int result=0;
    __try {
        memcpy(static_cast<BYTE*>(info)+0xc,&to,sizeof(to));
        __try {result=original(connection,info);}
        __finally {memcpy(static_cast<BYTE*>(info)+0xc,&from,sizeof(from));}
        return result;
    }__except(EXCEPTION_CONTINUE_SEARCH){return 0;}
}
int Run(int variant,void* connection,void* info) {
    const wchar_t* command=nullptr;int selected=-1;
    if(ReadCommand(info,&command))selected=Match(command);
    if(selected<0)return originals[variant](connection,info);
    std::wstring replacement;
    try {
        if(!preparePolicy(connection))throw std::runtime_error("rank policy session unavailable");
        replacement=sdgo::rank_fix::Redirect(command,sdgo::rank_fix::kNames[selected]);
    }catch(const std::exception& error){
        if(errors.fetch_add(1)<5)g_api->Log(g_mod,SDGO_LOG_ERROR,"rank policy unavailable; native command retained: %s",error.what());
        return originals[variant](connection,info);
    }
    const unsigned bit=1u<<selected;
    if(!(active.fetch_or(bit)&bit))g_api->Log(g_mod,SDGO_LOG_INFO,"rank policy active for %s; native SQL definition unchanged",selected?"Grid/Tag":"normal battle");
    return Redirected(originals[variant],connection,info,command,replacement.c_str());
}
int __fastcall Execute0(void* connection,void*,void* info){return Run(0,connection,info);}
int __fastcall Execute1(void* connection,void*,void* info){return Run(1,connection,info);}
}

SDGO_MOD_ENTRY {
    if(!api||api->version!=SDGO_API_VERSION||api->size<offsetof(SdgoApi,ModDirectory)+sizeof(api->ModDirectory))return 0;
    g_api=api;g_mod=mod;
    wchar_t path[32768]={},name[128]={};
    if(!GetModuleFileNameW(nullptr,path,32768))return 0;
    std::wstring ini=path;auto dot=ini.find_last_of(L'.');if(dot==std::wstring::npos)return 0;ini.replace(dot,std::wstring::npos,L".ini");
    DWORD length=GetPrivateProfileStringW(L"DB Game",L"DBName",L"",name,128,ini.c_str());
    if(!length||length>=127){api->Log(mod,SDGO_LOG_ERROR,"host INI needs [DB Game] DBName; no credentials are read by this mod");return 0;}
    database=name;
    commands[0]=static_cast<const wchar_t*>(api->Symbol(mod,"rank.command.battle"));
    commands[1]=static_cast<const wchar_t*>(api->Symbol(mod,"rank.command.grid"));
    if(!commands[0]||!commands[1]||!api->Symbol(mod,"rank.connection_layout"))return 0;
    if(!api->HookCode(mod,"rank.execute",reinterpret_cast<void*>(&Execute0),reinterpret_cast<void**>(&originals[0]))||
       !api->HookCode(mod,"rank.execute_rows",reinterpret_cast<void*>(&Execute1),reinterpret_cast<void**>(&originals[1])))return 0;
    api->Log(mod,SDGO_LOG_INFO,"rank progression routing installed; policy initializes on the first matching battle DB call");
    return 1;
}
