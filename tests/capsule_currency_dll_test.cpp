#include <windows.h>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/game_test_capsule_observer.h"
#include "../mods/capsule_currency_fix/capsule_currency_logic.h"

namespace {
using Load=void(__fastcall*)(void*,void*);
Load hook=nullptr;
std::vector<uint8_t> source,registered,loaded;
bool mismatchLookup=false;
std::array<uint8_t,0x300> object{};
std::vector<std::string> messages;
int nativeLoads=0;
void Put(uint8_t* p,uint32_t n){memcpy(p,&n,4);}
uint32_t Get(const uint8_t* p){uint32_t n;memcpy(&n,p,4);return n;}
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);printf("PASS %s\n",message);}
void __cdecl Log(SdgoMod*,int,const char* format,...)
{
    char text[1024]{};va_list args;va_start(args,format);vsnprintf_s(text,sizeof(text),_TRUNCATE,format,args);va_end(args);messages.emplace_back(text);
}
void __fastcall NativeLoad(void* self,void*)
{
    ++nativeLoads;
    const size_t count=registered.size()/capsule_currency_fix::kRecordSize,pages=(count+7)/8;
    loaded.assign(pages*8*capsule_currency_fix::kRecordSize,0);
    for(size_t i=0;i<count;++i)memcpy(loaded.data()+i*capsule_currency_fix::kRecordSize,registered.data()+(count-i-1)*capsule_currency_fix::kRecordSize,capsule_currency_fix::kRecordSize);
    auto* p=static_cast<uint8_t*>(self);
    *reinterpret_cast<uint8_t**>(p+0x1A4)=loaded.data();*reinterpret_cast<uint8_t**>(p+0x1A8)=loaded.data()+loaded.size();*reinterpret_cast<uint8_t**>(p+0x1AC)=loaded.data()+loaded.size();Put(p+0x1DC,static_cast<uint32_t>(pages));
}
void* GridData(){return object.data();}
bool __fastcall GridFind(void* grid,void*,void** record,int id,int type,int logging)
{
    if(grid!=object.data()||type!=0x3C||logging)return 0;
    for(size_t i=0;i<registered.size();i+=179)if(Get(registered.data()+i)==static_cast<uint32_t>(id)) {
        *record=mismatchLookup?registered.data():registered.data()+i;return 1;
    }
    return 0;
}
void* Symbol(SdgoMod*,const char* name)
{
    if(!strcmp(name,"grid.data"))return reinterpret_cast<void*>(&GridData);
    if(!strcmp(name,"grid.find"))return reinterpret_cast<void*>(&GridFind);
    return nullptr;
}
int Hook(SdgoMod*,const char* name,void* detour,void** original)
{
    if(strcmp(name,"capsule.activity.load_items"))return 0;hook=reinterpret_cast<Load>(detour);*original=reinterpret_cast<void*>(&NativeLoad);return 1;
}
const uint8_t* Find(uint32_t id)
{
    for(size_t at=0;at<loaded.size();at+=capsule_currency_fix::kRecordSize)if(Get(loaded.data()+at+5)==id)return loaded.data()+at;
    return nullptr;
}
}
int wmain(int argc,wchar_t** argv)
{
    if(argc!=2&&argc!=3)return 2;
    try {
        HMODULE dll=LoadLibraryW(argv[1]);auto entry=dll?reinterpret_cast<SdgoModLoadFn>(GetProcAddress(dll,"SdgoModLoad")):nullptr;
        Check(entry!=nullptr,"actual native currency-fix DLL export loaded");
        Check(!entry(nullptr,nullptr),"missing API rejected safely");
        SdgoApi api{};api.version=SDGO_API_VERSION;api.size=sizeof(api);api.Symbol=Symbol;api.HookCode=Hook;api.Log=Log;
        Check(entry(&api,nullptr)&&hook,"actual module registers its activity-load detour");
        if(argc==3){std::ifstream f(argv[2],std::ios::binary);source.assign(std::istreambuf_iterator<char>(f),{});Check(!source.empty()&&source.size()%179==0,"private deployed activity-table replay admitted");}
        else {
            source.assign(94*179,0);
            for(size_t i=0;i<94;++i){Put(source.data()+i*179,145001+static_cast<uint32_t>(i));Put(source.data()+i*179+5,101+static_cast<uint32_t>(i)*100);source[i*179+9]=0;Put(source.data()+i*179+15,1200);}
            Put(source.data()+91*179+5,9201);Put(source.data()+91*179+15,10000);
            Put(source.data()+92*179+5,9301);Put(source.data()+92*179+15,99);
            Put(source.data()+93*179+5,990001);Put(source.data()+93*179+15,100000);
            source[90*179+9]=2;Put(source.data()+90*179+5,9101);Put(source.data()+90*179+15,3);
        }
        const auto before=source;registered=source;NativeLoad(object.data(),nullptr);const auto oldDisplay=loaded;nativeLoads=0;
        hook(object.data(),nullptr);Check(nativeLoads==1,"native load finishes before fix examines the fresh UI vector");
        const auto* target=Find(9301);Check(target&&target[9]==2&&Get(target+15)==99,"94-record collection coexistence: target is 99 token in the actual detour output");
        size_t changed=0;for(size_t i=0;i<loaded.size();++i)changed+=loaded[i]!=oldDisplay[i]?1:0;
        Check(changed==1,"exactly the known currency byte changes in the UI copy");
        Check(source==before,"disk GDT/replay source remains byte-identical");
        changed=0;for(size_t i=0;i<registered.size();++i)changed+=registered[i]!=before[i]?1:0;
        void* confirmationRecord=nullptr;
        Check(changed==1&&GridFind(GridData(),nullptr,&confirmationRecord,145093,0x3C,0)&&
              static_cast<const uint8_t*>(confirmationRecord)[9]==2&&Get(static_cast<const uint8_t*>(confirmationRecord)+15)==99,
              "fresh native registry lookup used by confirmation sees 99 token; exactly one memory currency byte changes");
        Check(Find(990001)&&Find(990001)[9]==0&&Get(Find(990001)+15)==100000,"new collection machine retains 100000 GP");
        Check(Find(9201)&&Find(9201)[9]==0&&Get(Find(9201)+15)==10000,"ordinary 10000 GP activity machine retained");
        Check(Find(9101)&&Find(9101)[9]==2&&Get(Find(9101)+15)==3,"existing 3-token machine retained");
        Check(std::any_of(messages.begin(),messages.end(),[](const std::string& s){return s.find("display currency corrected")!=std::string::npos;}),"correction produces a runtime result receipt rather than only a load message");
        const auto corrected=registered;hook(object.data(),nullptr);Check(registered==corrected&&Find(9301)[9]==2,"second page load is idempotent with corrected registry");
        registered=source;mismatchLookup=true;NativeLoad(object.data(),nullptr);const auto mismatchDisplay=loaded;
        hook(object.data(),nullptr);Check(loaded==mismatchDisplay&&registered==source,"foreign or mismatched registry lookup refuses both writes");mismatchLookup=false;
        for(size_t i=0;i<registered.size();i+=179)if(Get(registered.data()+i+5)==9301)Put(registered.data()+i+15,100);
        const auto wrongSource=registered;
        NativeLoad(object.data(),nullptr);const auto wrongPrice=loaded;hook(object.data(),nullptr);Check(loaded==wrongPrice&&Find(9301)[9]==0,"changed authoritative price is refused without changing any display byte");
        Check(registered==wrongSource,"refused price leaves native registry unchanged");
        auto observe=reinterpret_cast<SdgoCapsuleGameTestGetSnapshotV1Fn>(GetProcAddress(dll,"SdgoCapsuleGameTestGetSnapshotV1"));
        Check(observe!=nullptr,"optional native activity-load observer is exported");
        SdgoCapsuleGameTestSnapshotV1 receipt{};receipt.size=sizeof(receipt);receipt.version=1;
        Check(!observe(&receipt),"observer is disabled outside an explicit GameTest process");
        SetEnvironmentVariableA("SDGO_GAME_TEST_RUN","12345678-1234-1234-1234-123456789abc");SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE","client");
        registered=source;hook(object.data(),nullptr);
        Check(observe(&receipt) && receipt.generation==1 && receipt.run_hash && receipt.process_id==GetCurrentProcessId() &&
            receipt.ui_thread_id==GetCurrentThreadId() && receipt.grid_id==145093 && receipt.machine_id==9301 && receipt.price==99 &&
            receipt.currency==2 && receipt.registry_currency==2,"observer records the genuine post-load UI and registry correction");
        SetEnvironmentVariableA("SDGO_GAME_TEST_RUN","22345678-1234-1234-1234-123456789abc");
        Check(!observe(&receipt),"an old native load cannot prove a different run");
        SetEnvironmentVariableA("SDGO_GAME_TEST_RUN",nullptr);SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE",nullptr);
        puts("ACTUAL CURRENCY DLL AND 94-ROW ACTIVITY VECTOR REPLAY PASS; native UI visual acceptance remains manual");return 0;
    }catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
}
