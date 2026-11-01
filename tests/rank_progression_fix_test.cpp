#include "ai_test_support.h"
#include "../mods/rank_progression_fix/rank_progression_fix.cpp"
#include <cstdarg>
#include <fstream>

namespace {
const wchar_t* battle=L"{? = call sp_SetBattleExp_V4(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)}";
const wchar_t* grid=L"{? = call sp_SetGridBattleExp_V2(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)}";
int calls=0,prepares=0,nativeResult=0x13572468;bool ready=true;std::wstring captured;
int __fastcall Capture(void*,void*,void* info) {
    const wchar_t* command=nullptr;memcpy(&command,static_cast<BYTE*>(info)+0xc,sizeof(command));captured=command;++calls;
    const uint16_t marker=17;memcpy(static_cast<BYTE*>(info)+0x3a2,&marker,sizeof(marker));
    return nativeResult;
}
bool PrepareFake(void*){++prepares;return ready;}
void Log(SdgoMod*,int,const char*,...){}
void* Symbol(SdgoMod*,const char* name) {
    if(!strcmp(name,"rank.command.battle"))return const_cast<wchar_t*>(battle);
    if(!strcmp(name,"rank.command.grid"))return const_cast<wchar_t*>(grid);
    if(!strcmp(name,"rank.connection_layout"))return reinterpret_cast<void*>(1);
    return nullptr;
}
int Hook(SdgoMod*,const char*,void*,void** original){*original=reinterpret_cast<void*>(&Capture);return 1;}
std::wstring Source(const std::wstring& name) {
    return L"-- native fixture\nCREATE PROCEDURE dbo.["+name+L"] AS\n"
        L"DECLARE @UserLevelNumber int,@newlevel int,@UserLevelStep int,@NewLevelStep int;\n"
        L"IF(@UserLevelNumber < @newlevel OR @UserLevelStep < @NewLevelStep)\nPRINT N'晋级';";
}
template<class Fn> bool Throws(Fn fn){try{fn();return false;}catch(const std::exception&){return true;}}
}

int main() {
    using namespace sdgo::rank_fix;
    SCENARIO("bounded SQL source transformation");
    for(auto* name:kNames) {
        auto original=Source(name);auto updated=Clone(original,name,false);
        CHECK(updated.find(L"CREATE PROCEDURE [sdgo_rank_progression_fix].["+std::wstring(name)+L"]")!=std::wstring::npos);
        CHECK(updated.find(L"@UserLevelNumber = @newlevel AND @UserLevelStep < @NewLevelStep")!=std::wstring::npos);
        CHECK(original.find(L"sdgo_rank")==std::wstring::npos);
        auto alter=Clone(original,name,true);CHECK(Canonical(alter,name,kSchema)==Canonical(updated,name,kSchema));
        CHECK(Throws([&]{Clone(original,L"sp_wrong",false);}));
    }
    auto original=Source(kNames[0]);auto predicate=L"IF(@UserLevelNumber < @newlevel OR @UserLevelStep < @NewLevelStep)";
    auto commented=L"/* nested /* "+std::wstring(predicate)+L" */ comment */\n"+original+L"\nPRINT N'"+predicate+L"';";
    auto updated=Clone(commented,kNames[0],false);CHECK(updated.find(L"PRINT N'"+std::wstring(predicate)+L"'")!=std::wstring::npos);
    CHECK(Throws([&]{Clone(original+L"\n"+predicate,kNames[0],false);}));
    CHECK(Throws([&]{Clone(L"SELECT 1;"+original,kNames[0],false);}));
    CHECK(Throws([&]{Mask(L"/* unterminated");}));CHECK(Throws([&]{Mask(L"SELECT 'unterminated");}));
    CHECK(Literal(L"a'b")==L"N'a''b'");CHECK(Identifier(L"a]b")==L"[a]]b]");

    SCENARIO("framework entry and native command record routing");
    wchar_t path[MAX_PATH]={};GetModuleFileNameW(nullptr,path,MAX_PATH);
    std::wstring ini=path;ini.replace(ini.find_last_of(L'.'),std::wstring::npos,L".ini");
    CHECK(WritePrivateProfileStringW(L"DB Game",L"DBName",L"rank_fixture",ini.c_str())!=0);
    SdgoApi api={};api.size=sizeof(api);api.version=SDGO_API_VERSION;api.Log=&Log;api.Symbol=&Symbol;api.HookCode=&Hook;
    CHECK(SdgoModLoad(&api,reinterpret_cast<SdgoMod*>(1))==1);preparePolicy=&PrepareFake;
    for(int variant=0;variant<2;++variant) {
        for(auto* command:{battle,grid}) {
            std::array<BYTE,0x730> info={};memcpy(info.data()+0xc,&command,sizeof(command));
            CHECK(Run(variant,nullptr,info.data())==nativeResult);
            CHECK(captured==Redirect(command,command==battle?kNames[0]:kNames[1]));
            const wchar_t* restored=nullptr;memcpy(&restored,info.data()+0xc,sizeof(restored));CHECK(restored==command);
            uint16_t marker=0;memcpy(&marker,info.data()+0x3a2,sizeof(marker));CHECK(marker==17);
        }
    }
    auto* other=L"{? = call unrelated(?)}";std::array<BYTE,0x730> info={};memcpy(info.data()+0xc,&other,sizeof(other));
    nativeResult=-7;
    int prior=prepares;CHECK(Execute0(nullptr,nullptr,info.data())==nativeResult);CHECK(prepares==prior);CHECK(captured==other);
    ready=false;memcpy(info.data()+0xc,&battle,sizeof(battle));CHECK(Execute1(nullptr,nullptr,info.data())==nativeResult);CHECK(captured==battle);
    CHECK(calls==6);CHECK(prepares==5);
    CHECK(DeleteFileW(ini.c_str())!=0);
    return ai_test::Finish();
}
