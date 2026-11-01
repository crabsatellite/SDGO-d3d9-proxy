// Real core + real server rules DLL. Private x86 branch fixtures, no server,
// database, socket or game is started. Raw server PE contracts are separate.
#include <windows.h>
#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/ai_catalog_format.h"
namespace {
int failures=0;
uint8_t skillRegistry[0x20]={},skillDefinition[0x40]={},weapons[2][0x80]={};
int npcManager=1,unitManager=2;
std::array<uint8_t,0x30> nativeNpc{};
std::array<uint8_t,0x7c> nativeUnit{};
uint32_t lastSkill=0;
uint8_t lastBroadcast[0x100]={};uint32_t lastLength=0;
void Check(bool ok,const char* name) {std::printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok) ++failures;}
template<class T> void Put(uint8_t* data,size_t at,T value) {memcpy(data+at,&value,sizeof(value));}
__declspec(noinline) void __fastcall FindSkill(void*,void*,uint32_t id,void** out) {lastSkill=id;*out=(id==75002||id==75003)?skillDefinition:nullptr;}
__declspec(noinline) int __fastcall FindNpc(void*,void*,uint32_t id,uint8_t** out) {
    if(!out||id!=3604001) return 0;
    *out=nativeNpc.data();return 1;
}
__declspec(noinline) uint8_t* __fastcall FindUnit(void*,void*,uint32_t id) {
    return id==10001?nativeUnit.data():nullptr;
}
__declspec(noinline) int __fastcall BuildNative(uint8_t*,void*,uint8_t* pilot,uint8_t* unit,uint32_t npc) {
    uint8_t* npcDefinition=nullptr;
    if(!FindNpc(&npcManager,nullptr,npc,&npcDefinition)) return 0;
    uint8_t* definition=FindUnit(&unitManager,nullptr,*reinterpret_cast<uint32_t*>(npcDefinition+4));
    if(!definition) return 0;
    Put(pilot,0,uint32_t(0x80000001));Put(unit,0,npc);Put(unit,0x0C,uint8_t(1));Put(unit,0x54,uint32_t(0));
    Put(unit,0x64,*reinterpret_cast<uint32_t*>(npcDefinition+4));
    Put(unit,0x6C,definition);Put(unit,0x70,weapons[0]);Put(unit,0x70+0x94,weapons[1]);return 0x7D;
}
__declspec(noinline) int __fastcall BroadcastNative(uint8_t*,void*,uint8_t* packet,uint32_t length,uint32_t* sent) {
    lastLength=length;Put(packet,2,uint32_t(0xABCD));memcpy(lastBroadcast,packet,length);*sent=1;return 0x64B1;
}
__declspec(naked) int GatePlayer() {__asm {
        mov ecx,[ebp-328DCh]
        cmp dword ptr [ecx+5],30000
        setae al
        movzx eax,al
        ret
    }
}
__declspec(naked) int GateSite() {__asm {
        mov eax,[ebp-328D8h]
        and eax,80000000h
        jnz bypass
        jmp GatePlayer
        bypass:
        mov eax,2
        ret
    }
}
__declspec(naked) int StartPlayer() {__asm {
        mov ecx,[ebp-328DCh]
        sub dword ptr [ecx+5],10000
        mov eax,1
        ret
    }
}
__declspec(naked) int StartSite() {__asm {
        mov eax,[ebp-328D8h]
        and eax,80000000h
        jnz bypass
        jmp StartPlayer
        bypass:
        xor eax,eax
        ret
    }
}
__declspec(naked) int MeleePlayer() {__asm {
        mov ecx,[ebp-32994h]
        sub dword ptr [ecx+5],20000
        mov eax,1
        ret
    }
}
__declspec(naked) int MeleeSite() {__asm {
        mov eax,[ebp-32950h]
        and eax,80000000h
        jnz bypass
        jmp MeleePlayer
        bypass:
        xor eax,eax
        ret
    }
}
__declspec(naked) int RangedPlayer() {__asm {
        mov ecx,[ebp-1510h]
        sub dword ptr [ecx+5],20000
        mov eax,1
        ret
    }
}
__declspec(naked) int RangedSite() {__asm {
        mov edx,[ebp-14F8h]
        and edx,80000000h
        jnz bypass
        jmp RangedPlayer
        bypass:
        xor eax,eax
        ret
    }
}
__declspec(naked) int MapPlayer() {__asm {
        mov ecx,[ebp-33CD4h]
        sub dword ptr [ecx+5],20000
        mov eax,1
        ret
    }
}
__declspec(naked) int MapSite() {__asm {
        mov eax,[ebp-33CD0h]
        and eax,80000000h
        jnz bypass
        jmp MapPlayer
        bypass:
        xor eax,eax
        ret
    }
}
__declspec(naked) int __cdecl Invoke(uint8_t*,void*) {
    __asm {
        push ebp
        mov eax,[esp+0Ch]
        mov ebp,[esp+8]
        call eax
        pop ebp
        ret}
}
__declspec(naked) int RushSite() {__asm {
    nop
    mov eax,[ebp-33CD0h]
    and eax,80000000h
    jnz bypass
    jmp MapPlayer
bypass:
    xor eax,eax
    ret
}}
void Symbol(FILE* f,const char* name,void* function) {
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));auto bytes=static_cast<uint8_t*>(function);
    std::fprintf(f,"[%s]\nkind=code\nrva=0x%08lX\nexpect=",name,static_cast<unsigned long>(reinterpret_cast<uintptr_t>(function)-base));
    for(int i=0;i<8;++i) std::fprintf(f,"%02X ",bytes[i]);std::fprintf(f,"\n\n");
}
void DataSymbol(FILE* f,const char* name,void* data) {
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(f,"[%s]\nkind=data\nrva=0x%08lX\n\n",name,
                 static_cast<unsigned long>(reinterpret_cast<uintptr_t>(data)-base));
}
void Catalog(const std::wstring& root) {
    using namespace sdgo::catalog;
    Record row{};
    row.npcId=3605001;row.unitId=10001;row.level=4;row.templateNpcId=3604001;row.preferredWeapon=1;
    row.clientUnitHash=1;
    memcpy(row.nativeUnitPrefix.data(),nativeUnit.data(),kNativeUnitPrefixBytes);
    Put32(row.nativeNpc.data(),row.npcId);Put32(row.nativeNpc.data()+4,row.unitId);
    Put32(row.nativeNpc.data()+8,12000);Put32(row.nativeNpc.data()+12,4);
    Put32(row.clientNpc.data(),row.npcId);Put32(row.clientNpc.data()+0x69,row.unitId);
    Put32(row.clientNpc.data()+0xe5,12000);
    Header header{};memcpy(header.magic,"SDGOAI1",8);header.version=1;
    header.recordBytes=sizeof(Record);header.count=1;header.payloadHash=Hash32(&row,sizeof(row));
    const std::filesystem::path directory=std::filesystem::path(root)/L"sdgo/mods/ai_catalog";
    std::filesystem::create_directories(directory);
    std::ofstream catalog(directory/L"catalog.bin",std::ios::binary);
    catalog.write(reinterpret_cast<const char*>(&header),sizeof(header));
    catalog.write(reinterpret_cast<const char*>(&row),sizeof(row));
    std::ofstream manifest(directory/L"mod.ini");
    manifest<<"[mod]\nid=ai_catalog\ncategory=gameplay\nfeature=ai-catalog\napi=1\n"
              "dll=ai_catalog.dll\nenabled=1\norder=50\nhosts=*\n"
              "requires=catalog.npc_manager,catalog.unit_manager,catalog.find_ai_npc,catalog.find_unit,battle.build_ai_pilot\n";
}
void Symbols(const std::wstring& root) {
    auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);wchar_t file[128];
    swprintf_s(file,L"\\sdgo\\symbols\\battle_ai_server_native_test-%08lX-%08lX.ini",nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    FILE* f=nullptr;_wfopen_s(&f,(root+file).c_str(),L"wb");if(!f) {Check(false,"fixture symbols file");return;}
    std::fprintf(f,"[host]\ntimestamp=0x%08lX\nsize_of_image=0x%08lX\n\n",nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    Symbol(f,"ai_sp.request_gate",reinterpret_cast<void*>(&GateSite));Symbol(f,"ai_sp.request_player_gate",reinterpret_cast<void*>(&GatePlayer));
    Symbol(f,"ai_sp.request_cost",reinterpret_cast<void*>(&StartSite));Symbol(f,"ai_sp.request_player_cost",reinterpret_cast<void*>(&StartPlayer));
    Symbol(f,"ai_sp.melee_cost",reinterpret_cast<void*>(&MeleeSite));Symbol(f,"ai_sp.melee_player_cost",reinterpret_cast<void*>(&MeleePlayer));
    Symbol(f,"ai_sp.ranged_cost",reinterpret_cast<void*>(&RangedSite));Symbol(f,"ai_sp.ranged_player_cost",reinterpret_cast<void*>(&RangedPlayer));
    Symbol(f,"ai_sp.map_cost",reinterpret_cast<void*>(&MapSite));Symbol(f,"ai_sp.map_player_cost",reinterpret_cast<void*>(&MapPlayer));
    Symbol(f,"ai_sp.rush_cost",reinterpret_cast<void*>(&RushSite));Symbol(f,"ai_sp.rush_player_cost",reinterpret_cast<void*>(&MapPlayer));
    Symbol(f,"battle.build_ai_pilot",reinterpret_cast<void*>(&BuildNative));Symbol(f,"catalog.find_ai_npc",reinterpret_cast<void*>(&FindNpc));
    Symbol(f,"catalog.find_unit",reinterpret_cast<void*>(&FindUnit));Symbol(f,"loadout.find_skill",reinterpret_cast<void*>(&FindSkill));
    Symbol(f,"ai_sp.broadcast",reinterpret_cast<void*>(&BroadcastNative));
    DataSymbol(f,"catalog.npc_manager",&npcManager);DataSymbol(f,"catalog.unit_manager",&unitManager);
    DataSymbol(f,"loadout.skill_registry",skillRegistry);fclose(f);
}
}
int wmain() {
    wchar_t path[MAX_PATH];GetModuleFileNameW(nullptr,path,MAX_PATH);*wcsrchr(path,L'\\')=0;std::wstring root=path;
    Put(nativeUnit.data(),0,uint32_t(10001));Put(nativeUnit.data(),0x45,uint32_t(1));
    Put(nativeUnit.data(),0x49,uint32_t(101));
    Put(nativeNpc.data(),0,uint32_t(3604001));Put(nativeNpc.data(),4,uint32_t(10001));Put(nativeNpc.data(),8,uint32_t(12000));
    const auto originalNativeUnit=nativeUnit;
    Catalog(root);Symbols(root);
    uint8_t originalBuild[12]={};memcpy(originalBuild,reinterpret_cast<void*>(&BuildNative),sizeof(originalBuild));
    auto core=LoadLibraryW((root+L"\\sdgo\\core.dll").c_str());
    auto init=core?reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core,"SdgoCoreInit")):nullptr;
    Check(init!=nullptr,"real core export");if(!init)
        return 1;
    SdgoBoot boot={sizeof(boot),"battle_ai_server_native_test",GetModuleHandleW(nullptr)};Check(init(&boot)!=0,"real server rules DLL loaded into private fixture");
    const std::wstring serverPath=root+L"\\sdgo\\mods\\battle_ai_server\\battle_ai_server.dll";
    HMODULE server=GetModuleHandleW(serverPath.c_str());
    using ApplyLoadoutFn=int(__cdecl*)(uint8_t*,uint8_t*,uint8_t*,uint32_t);
    auto apply=server?reinterpret_cast<ApplyLoadoutFn>(GetProcAddress(server,"SdgoBattleAiServerApplyLoadoutV1")):nullptr;
    Check(server!=nullptr&&apply!=nullptr,"post-build server loadout provider export");
    const std::wstring catalogPath=root+L"\\sdgo\\mods\\ai_catalog\\ai_catalog.dll";
    Check(GetModuleHandleW(catalogPath.c_str())!=nullptr,"AI catalog remains the active build provider beside server rules");
    Check(memcmp(originalBuild,reinterpret_cast<void*>(&BuildNative),sizeof(originalBuild))!=0,
          "AI catalog exclusively owns the build hook");
    std::vector<uint8_t> room(0x3CE4+12*0x1F54),frameStorage(0x34000);
    auto frame=frameStorage.data()+0x33D00;auto pilot=room.data()+0x3CE4;auto unit=pilot+0x340;
    room[5]=1;Put(pilot,0,uint32_t(0x80000001));Put(unit,0,uint32_t(3604001));
    auto title=[&](const char* text){room[0x79]=static_cast<uint8_t>(strlen(text));memcpy(room.data()+0x7A,text,strlen(text)+1);};
    auto bind=[&](int site){
        size_t h[]={0x328D8,0x328D8,0x32950,0x14F8,0x33CD0,0x33CD0},p[]={0x328DC,0x328DC,0x32994,0x1510,0x33CD4,0x33CD4},r[]={0x3C,0x3C,0x20,0x20,0x18,0x18};
        Put(frame-h[site],0,*reinterpret_cast<uint32_t*>(pilot));Put(frame-p[site],0,pilot);Put(frame-r[site],0,room.data());
    };
    Put(weapons[0],0,uint32_t(101));Put(weapons[1],0,uint32_t(102));
    weapons[0][0x37]=weapons[1][0x37]=1;weapons[0][0x38]=1;weapons[1][0x38]=2;
    using BuildFn=int(__thiscall*)(uint8_t*,uint8_t*,uint8_t*,uint32_t);
    BuildFn volatile build=reinterpret_cast<BuildFn>(&BuildNative);
    auto buildWithLoadout=[&](uint32_t npc) {
        int result=build(room.data(),pilot,unit,npc);
        if(result&&apply) apply(room.data(),pilot,unit,npc);
        return result;
    };
    title("!ai hard");int built=buildWithLoadout(3604001);
    Check(built==0x7D&&unit[0x0C]==13&&lastSkill==75002&&*reinterpret_cast<uint32_t*>(unit+0x54)==reinterpret_cast<uintptr_t>(skillDefinition),
          "Hard post-build callback preserves native return and resolves full-width legal part plus EX13");
    weapons[1][0x38]=1;buildWithLoadout(3604001);
    Check(lastSkill==75003&&unit[0x0C]==13,"melee-only native weapon definitions choose the legal fighting part on the server");
    title("!ai medium");buildWithLoadout(3604001);
    Check(unit[0x0C]==12&&*reinterpret_cast<uint32_t*>(unit+0x54)==0,"Medium server unit carries authoritative level12 and no extra optional part");
    title("!ai easy");buildWithLoadout(3604001);
    Check(unit[0x0C]==1,"Easy server unit retains its original native level");
    Check(apply&&apply(room.data(),pilot,unit,123)==0&&unit[0x0C]==1,
          "provider refusal is a no-op and never changes native build success");
    title("!ai hard");built=build(room.data(),pilot,unit,3605001);
    auto builtDefinition=*reinterpret_cast<uint8_t**>(unit+0x6C);
    Check(built==0x7D&&*reinterpret_cast<uint32_t*>(unit)==3605001&&
          *reinterpret_cast<uint32_t*>(unit+0x64)==10001&&unit[0x0C]==1&&
          builtDefinition&&builtDefinition!=nativeUnit.data()&&
          *reinterpret_cast<uint32_t*>(builtDefinition)==10001,
          "catalog completes the native build with its durable private unit definition");
    Check(FindUnit(&unitManager,nullptr,10001)==nativeUnit.data()&&nativeUnit==originalNativeUnit,
          "catalog temporary unit lookup is restored before the server callback");
    int applied=apply?apply(room.data(),pilot,unit,3605001):0;
    Check(applied==1&&unit[0x0C]==13&&
          *reinterpret_cast<uint32_t*>(unit+0x54)==reinterpret_cast<uintptr_t>(skillDefinition),
          "post-build callback applies EX13 and a legal part after catalog scope restoration");
    title("!ai hard");bind(0);Put(pilot,5,int32_t(29999));
    Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==0,"Hard NPC without full SP enters the native rejection block");
    Put(pilot,5,int32_t(30000));Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==1,"Hard NPC full SP enters the native allowed block");
    bind(1);Check(Invoke(frame,reinterpret_cast<void*>(&StartSite))==1&&*reinterpret_cast<int32_t*>(pilot+5)==20000,
                  "accepted Hard request enters native -10000 block exactly once");
    void* ends[]={reinterpret_cast<void*>(&MeleeSite),reinterpret_cast<void*>(&RangedSite),reinterpret_cast<void*>(&MapSite),reinterpret_cast<void*>(&RushSite)};
    for(int i=0;i<4;++i) {bind(i+2);Put(pilot,9,uint32_t(3));Put(pilot,5,int32_t(20000));Check(Invoke(frame,ends[i])==1&&*reinterpret_cast<int32_t*>(pilot+5)==0,"native terminal branch charges the remaining 20000 SP");}
    bind(4);Put(pilot,9,uint32_t(3));Put(pilot,5,int32_t(20000));Invoke(frame,reinterpret_cast<void*>(&MapSite));
    uint8_t clear[10]={};Put(clear,0,uint16_t(0x554));Put(clear,6,uint32_t(0x80000001));uint32_t sent=0;
    using BroadcastFn=int(__thiscall*)(uint8_t*,uint8_t*,uint32_t,uint32_t*);
    BroadcastFn volatile broadcast=reinterpret_cast<BroadcastFn>(&BroadcastNative);
    int broadcastResult=broadcast(room.data(),clear,10,&sent);
    Check(broadcastResult==0x64B1&&sent==1&&lastLength==0xF0&&*reinterpret_cast<uint16_t*>(lastBroadcast)==0x779&&lastBroadcast[0xA]==0xFF&&lastBroadcast[0x17]==0&&*reinterpret_cast<int32_t*>(lastBroadcast+0xB)==0,
          "map clear emits a full-size Agent-compatible zero-victim SP state while preserving native clear send ABI");
    Check(*reinterpret_cast<uint32_t*>(clear+2)==0xABCD,"original clear recipient header writeback is preserved without changing its payload length");
    broadcast(room.data(),clear,10,&sent);Check(lastLength==10,"consumed clear context cannot emit a stale SP snapshot on a later broadcast");
    bind(5);
    Put(pilot,9,uint32_t(3));Put(pilot,5,int32_t(20000));Invoke(frame,reinterpret_cast<void*>(&RushSite));Put(clear,0,uint16_t(0x65D));
    broadcast(room.data(),clear,10,&sent);
    Check(lastLength==0xF0&&lastBroadcast[0xA]==0xFF&&*reinterpret_cast<uint32_t*>(lastBroadcast+6)==0x80000001&&*reinterpret_cast<int32_t*>(lastBroadcast+0xB)==0,
          "rush clear emits the same authoritative no-hit SP state through the Agent-compatible carrier");
    Put(pilot,9,uint32_t(1));Put(pilot,5,int32_t(5000));
    Check(Invoke(frame,reinterpret_cast<void*>(&RushSite))==0&&*reinterpret_cast<int32_t*>(pilot+5)==5000,"duplicate clear after completion cannot debit a later NPC resource balance");
    title("!ai medium");bind(0);Put(pilot,5,int32_t(100));Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==0,"Medium has the same player resource gate");
    title("!ai");Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==0,"default Medium requires full SP");
    title("normal room");Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==0,"an unmarked room keeps the explicit Medium default for reserved bot participants");
    title("!ai easy");Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==2,"Easy retains original NPC exemption");
    title("!ai hard");room[5]=11;Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==2,"native mission AI mode is unaffected");room[5]=1;
    Put(unit,0,uint32_t(123));Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==2,"unrelated NPC remains under original rules");Put(unit,0,uint32_t(3604001));
    Put(pilot,0,uint32_t(1));bind(0);Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==0,"human path retains its original player SP gate");
    Put(pilot,0,uint32_t(0x80000001));bind(0);Put(frame-0x328DC,0,pilot+1);
    Check(Invoke(frame,reinterpret_cast<void*>(&GateSite))==2,"misaligned pilot frame fails closed to the original branch");
    std::printf("SERVER NATIVE SP CHECKS %s\n",failures?"FAILED":"PASSED");return failures?1:0;
}
