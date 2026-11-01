#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/operator_model.h"

namespace {
using namespace sdgo::operators;
struct Box { uint32_t type; void* data; };
struct Pair { uint32_t id; Box* box; };
struct InsertResult { void* node; uint8_t inserted; uint8_t pad[3]; };
struct Row { uint8_t* data; size_t size; Box* box; void* node; bool owned; };
std::map<std::pair<int,int>,Row> g_rows;
std::array<uint8_t,kOperatorSize> g_a, g_b, g_unrelated;
std::array<uint8_t,kCategorySize> g_category, g_existing;
std::wstring g_directory;
void* g_hook=nullptr;
int g_originalCalls=0;
bool g_legacy=false;
void Require(bool ok) { if (!ok) ExitProcess(90); }
using ConvertFn=int(WINAPI*)(UINT,DWORD,LPCCH,int,LPWSTR,int);
ConvertFn g_import=&MultiByteToWideChar;
__declspec(noinline) int NativeDecode(UINT page,const char* text,wchar_t* output,int capacity)
{
    volatile int count=g_import(page,0,text,-1,output,capacity);
    return count;
}
int Import(SdgoMod*,const char* dll,const char* function,void* replacement,void** original)
{
    Require(!_stricmp(dll,"kernel32.dll") && !strcmp(function,"MultiByteToWideChar"));
    *original=reinterpret_cast<void*>(g_import);
    g_import=reinterpret_cast<ConvertFn>(replacement);
    return 1;
}
void* __cdecl Grid() { return reinterpret_cast<void*>(0x12345678); }
uint8_t __fastcall Find(void* self,void*,void** out,int id,int type,char)
{
    Require(self==Grid());
    const auto p=g_rows.find({id,type});
    if (p==g_rows.end()) return 0;
    *out=p->second.data;
    return 1;
}
void* __cdecl Allocate(size_t size) { return HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,size); }
void __cdecl Free(void* value) { Require(HeapFree(GetProcessHeap(),0,value)!=0); }
void* __fastcall Make(void* self,void*,const Pair* pair)
{
    Require(self==Grid());
    auto* node=static_cast<uint8_t*>(Allocate(24));
    std::memcpy(node+12,pair,8);
    return node;
}
void __fastcall Insert(void* self,void*,InsertResult* result,void* node,char duplicate)
{
    Require(self==Grid() && duplicate==0);
    Pair pair={};std::memcpy(&pair,static_cast<uint8_t*>(node)+12,8);
    const auto key=std::make_pair(static_cast<int>(pair.id),static_cast<int>(pair.box->type));
    if (g_rows.count(key)) { Free(node);result->inserted=0;return; }
    const size_t size=pair.box->type==0x2C?kOperatorSize:kCategorySize;
    g_rows.emplace(key,Row{static_cast<uint8_t*>(pair.box->data),size,pair.box,node,true});
    result->node=node;result->inserted=1;
}
uint8_t __fastcall Original(void* self,void*,const wchar_t*,int)
{ Require(self==Grid());++g_originalCalls;return 1; }
void* Symbol(SdgoMod*,const char* name)
{
    if (!strcmp(name,"operator.native_name_decode")) return g_legacy?nullptr:reinterpret_cast<void*>(&NativeDecode);
    if (!strcmp(name,"text.ansi_to_wide")) return reinterpret_cast<void*>(&NativeDecode);
    if (!strcmp(name,"operator.grid_data")) return reinterpret_cast<void*>(&Grid);
    if (!strcmp(name,"operator.grid_find")) return reinterpret_cast<void*>(&Find);
    if (!strcmp(name,"operator.grid_make_node")) return reinterpret_cast<void*>(&Make);
    if (!strcmp(name,"operator.grid_insert")) return reinterpret_cast<void*>(&Insert);
    if (!strcmp(name,"operator.native_new")) return reinterpret_cast<void*>(&Allocate);
    if (!strcmp(name,"operator.native_delete")) return reinterpret_cast<void*>(&Free);
    return nullptr;
}
int Hook(SdgoMod*,const char* name,void* detour,void** original)
{
    Require(!strcmp(name,"operator.load_data"));g_hook=detour;*original=reinterpret_cast<void*>(&Original);return 1;
}
void Log(SdgoMod*,int,const char*,...) {}
int Setting(SdgoMod*,const char*,int def) { return def; }
const wchar_t* Directory(SdgoMod*) { return g_directory.c_str(); }
void Put(const std::wstring& file,const wchar_t* section,const wchar_t* key,const wchar_t* value)
{ Require(WritePrivateProfileStringW(section,key,value,file.c_str())!=0); }
void NativeRow(std::array<uint8_t,kOperatorSize>& row,uint32_t id,const wchar_t* marker)
{
    row.fill(0x3A);WriteId(row.data(),id);row[4]=0;Text(row.data()+5,100,marker);
    for(size_t i=0;i<7;++i) Text(row.data()+kAvatarOffsets[i],kAvatarCapacities[i],std::wstring(marker)+std::to_wstring(i));
}
}

int wmain(int argc,wchar_t** argv)
{
    Require(argc==4);
    const int mode=_wtoi(argv[3]);
    g_legacy=mode==3;
    g_directory=argv[2];CreateDirectoryW(g_directory.c_str(),nullptr);
    const std::wstring file=g_directory+L"\\display.ini";
    Put(file,L"catalog",L"Version",L"1");
    Put(file,L"operator:60003",L"Category",L"1210002");
    Put(file,L"operator:60003",L"Template",mode==2?L"69999":L"60003");
    Put(file,L"operator:60003",L"AvatarProfile",L"60013");
    Put(file,L"operator:60003",L"Name",L"display");
    Put(file,L"operator:60003",L"FixedName",g_legacy?nullptr:L"1");
    // UTF16LE hex supports quotes and non-ASCII names without INI quote stripping.
    Put(file,L"operator:60003",L"NameHex",mode==1?L"GG00":L"0C54F3892200");
    Put(file,L"operator:60047",L"Category",L"1219047");
    Put(file,L"operator:60047",L"Template",L"60003");
    Put(file,L"operator:60047",L"AvatarProfile",L"60013");
    Put(file,L"operator:60047",L"Name",L"new operator");
    Put(file,L"operator:60047",L"AvatarSprite",L"sdgo\\mods\\operator_catalog\\assets\\60047.spr");
    for (uint32_t id:{60013u,60018u,60025u,60041u}) {
        const std::wstring section=L"operator:"+std::to_wstring(id);
        Put(file,section.c_str(),L"Category",std::to_wstring(1219000+id-60000).c_str());
        Put(file,section.c_str(),L"Template",L"60003");
        Put(file,section.c_str(),L"AvatarProfile",L"60013");
        Put(file,section.c_str(),L"Name",L"default support");
    }
    NativeRow(g_a,60003,L"avatarA");NativeRow(g_b,60013,L"avatarB");NativeRow(g_unrelated,60001,L"unrelated");
    g_category.fill(0);WriteId(g_category.data(),1210001);g_category[4]=1;WriteId(g_category.data()+105,60001);
    g_existing.fill(0);WriteId(g_existing.data(),1210002);WriteId(g_existing.data()+105,60003);
    g_rows[{60003,0x2C}]={g_a.data(),g_a.size(),nullptr,nullptr,false};
    g_rows[{60013,0x2C}]={g_b.data(),g_b.size(),nullptr,nullptr,false};
    g_rows[{60001,0x2C}]={g_unrelated.data(),g_unrelated.size(),nullptr,nullptr,false};
    g_rows[{1210001,0x22}]={g_category.data(),g_category.size(),nullptr,nullptr,false};
    g_rows[{1210002,0x22}]={g_existing.data(),g_existing.size(),nullptr,nullptr,false};
    const auto before=g_a,otherBefore=g_unrelated;
    const auto categoryBefore=g_existing;
    HMODULE module=LoadLibraryW(argv[1]);Require(module!=nullptr);
    auto entry=reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module,"SdgoModLoad"));Require(entry!=nullptr);
    SdgoApi api={};api.size=sizeof(api);api.version=1;api.Log=&Log;api.Symbol=&Symbol;api.HookCode=&Hook;
    api.SettingInt=&Setting;api.ModDirectory=&Directory;api.HookImport=&Import;
    const int loaded=entry(&api,reinterpret_cast<SdgoMod*>(1));
    if(mode==1 || mode==2) { Require(loaded==0 && !g_hook && g_a==before);FreeLibrary(module);return 0; }
    Require(loaded && g_hook);
    wchar_t rendered[64]={};
    Require(NativeDecode(874,"@SDGO:60003",rendered,64)>0 && !wcscmp(rendered,L"\x540c\x89f3\""));
    Require(NativeDecode(874,"@SDGO:60003",nullptr,0)==4);
    Require(NativeDecode(874,"@SDGO:60003",rendered,2)==0 && GetLastError()==ERROR_INSUFFICIENT_BUFFER);
    Require(NativeDecode(874,"personal-name",rendered,64)>0 && !wcscmp(rendered,L"personal-name"));
    Require(NativeDecode(874,"@SDGO:60047",rendered,64)>0 && !wcscmp(rendered,L"@SDGO:60047"));
    Require(NativeDecode(65001,"@SDGO:60003",rendered,64)>0 && !wcscmp(rendered,L"@SDGO:60003"));
    Require(g_import(874,0,"@SDGO:60003",-1,rendered,64)>0 && !wcscmp(rendered,L"@SDGO:60003"));
    using HookFn=uint8_t(__fastcall*)(void*,void*,const wchar_t*,int);
    auto invoke=reinterpret_cast<HookFn>(g_hook);
    Require(invoke(Grid(),nullptr,L"other.gdt",2)==1 && g_a==before && g_originalCalls==1);
    Require(invoke(Grid(),nullptr,L"gdrs\\0255.gdt",0x2C)==1 && g_originalCalls==2);
    Require(g_unrelated==otherBefore && g_existing==categoryBefore);
    Require(g_a[1200]==before[1200]);
    Require(g_a[kMissionPartsOffset]==1);
    for (uint32_t id:{60013u,60018u,60025u,60041u}) Require(g_rows.at({static_cast<int>(id),0x2C}).data[kMissionPartsOffset]==1);
    for(size_t i=0;i<7;++i) Require(!memcmp(g_a.data()+kAvatarOffsets[i],g_b.data()+kAvatarOffsets[i],kAvatarCapacities[i]*2));
    wchar_t name[100]={};memcpy(name,g_a.data()+5,200);Require(name[0]==0x540C && name[1]==0x89F3 && name[2]==L'"');
    Require(g_rows.count({60047,0x2C}) && g_rows.count({1219047,0x22}));
    const auto& created=g_rows.at({60047,0x2C});Require(ReadId(created.data)==60047 && created.data[1200]==before[1200]);
    Require(created.data[kMissionPartsOffset]==before[kMissionPartsOffset]);
    for(size_t i=0;i<7;++i) {
        wchar_t path[54]={};memcpy(path,created.data+kAvatarOffsets[i],kAvatarCapacities[i]*2);
        Require(std::wstring(path)==L"sdgo\\mods\\operator_catalog\\assets\\60047_"+std::to_wstring(i)+L".spr");
    }
    for(auto& pair:g_rows) if(pair.second.owned) { Free(pair.second.data);Free(pair.second.box);Free(pair.second.node); }
    FreeLibrary(module);
    return 0;
}
