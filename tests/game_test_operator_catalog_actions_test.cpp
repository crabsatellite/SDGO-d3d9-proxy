// Offline contract checks only; these fixtures are not native-game evidence.
#include "../mods/game_test_client/operator_catalog_actions.h"
#include <cstdio>
#include <cwchar>

namespace {
int failures=0;bool missing=false;
std::array<uint8_t,sdgo::operators::kOperatorSize> op{};
std::array<uint8_t,sdgo::operators::kCategorySize> item{};
void* __cdecl Data(){return &op;}
uint8_t __fastcall Find(void*,void*,const uint8_t** out,uint32_t id,int type,char) {
    if (missing)return 0;
    if (id==60003 && type==0x2C){*out=op.data();return 1;}
    if (id==1210002 && type==0x22){*out=item.data();return 1;}
    return 0;
}
void* __cdecl Symbol(SdgoMod*,const char* name) {
    if (!strcmp(name,"grid.data"))return reinterpret_cast<void*>(&Data);
    if (!strcmp(name,"grid.find"))return reinterpret_cast<void*>(&Find);
    return nullptr;
}
void Check(bool value,const char* message){if(!value){++failures;printf("FAIL: %s\n",message);}}
}

int main() {
    wchar_t temporary[MAX_PATH]{};GetTempPathW(MAX_PATH,temporary);
    wchar_t filename[MAX_PATH]{};swprintf_s(filename,L"%lsSDGO-GameTest-operator-%lu.ini",temporary,GetCurrentProcessId());
    // Only a bounded temporary fixture is created and removed by this test.
    WritePrivateProfileStringW(L"catalog",L"Version",L"1",filename);
    WritePrivateProfileStringW(L"operator:60003",L"Category",L"1210002",filename);
    WritePrivateProfileStringW(L"operator:60003",L"Name",L"CustomUnique",filename);
    SdgoApi api{};api.Symbol=&Symbol;
    OperatorCatalogActions actions;Check(actions.Bind(&api,nullptr),"native read bindings");
    sdgo::operators::WriteId(op.data(),60003);sdgo::operators::WriteId(item.data(),1210002);
    sdgo::operators::WriteId(item.data()+105,60003);
    const wchar_t name[]=L"CustomUnique";memcpy(op.data()+5,name,sizeof(name));
    Check(actions.Check(filename) && actions.State().verified==1,"actual post-load native records agree with configured values");
    const auto original=op;const auto category=item;
    Check(actions.Check(filename) && op==original && item==category,"readback never writes the native catalog");
    item[4]=1;Check(!actions.Check(filename) && actions.State().error==7,"filtered security row cannot stand in for an eligible item");item=category;
    sdgo::operators::WriteId(item.data()+105,60004);
    Check(!actions.Check(filename) && actions.State().error==7,"category must reference the same operator");item=category;
    missing=true;Check(!actions.Check(filename) && actions.State().error==6,"DLL loading alone cannot substitute for missing native rows");missing=false;
    op[5]='X';Check(!actions.Check(filename) && actions.State().error==8,"unapplied original name is not an override pass");op=original;
    WritePrivateProfileStringW(L"operator:60003",L"NameHex",L"2D4E",filename);
    const wchar_t unicodeName[]={0x4E2D,0};memcpy(op.data()+5,unicodeName,sizeof(unicodeName));
    Check(actions.Check(filename),"Unicode override readback is independent of the INI ANSI code page");
    WritePrivateProfileStringW(L"operator:60003",L"NameHex",L"ZZZZ",filename);
    Check(!actions.Check(filename) && actions.State().error==5,"malformed expected name fails closed");
    WritePrivateProfileStringW(L"operator:60003",nullptr,nullptr,filename);
    Check(!actions.Check(filename) && actions.State().error==9,"empty configuration is not a positive acceptance case");
    DeleteFileW(filename);
    printf("Native operator catalog readback contract: %d failures\n",failures);
    return failures ? 1:0;
}
