// Real core + real battle_ai DLL + official Lua 5.0 VM. Game entry points are
// private fixtures: no game, account, network socket, or existing process used.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include "sdgo/sdgo_api.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
#include "battle_ai_native_world.h"
namespace {
lua_State* g_L = nullptr;
int g_failures = 0;
std::wstring g_root;
void Check(bool ok,const char* what) {
    std::printf("%s %s\n",ok?"PASS":"FAIL",what); if(!ok) ++g_failures;
}
bool Eval(const char* s) {
    int top=lua_gettop(g_L);
    bool ok=luaL_loadbuffer(g_L,s,strlen(s),"=native fixture")==0 && lua_pcall(g_L,0,0,0)==0;
    if(!ok) {std::printf("LUA ERROR %s\n",lua_tostring(g_L,-1));++g_failures;}
    lua_settop(g_L,top); return ok;
}
lua_State* __cdecl State(lua_State* value) { if(value) g_L=value; return g_L; }
struct Unit {
    unsigned char bytes[0x3300] = {};
    __declspec(noinline) bool Think() {
        if(!bytes[0x326C]) return true;
        int result=native_world::Dispatch("VSAI_Gunfighter_01_Update",bytes);
        Check(result==123,"native dispatcher callback preserves its return value");
        reinterpret_cast<native_world::Callbacks*>(this)->Intent();
        return true;
    }
    __declspec(noinline) void Init(void* data) { bytes[0x100]=static_cast<unsigned char>(reinterpret_cast<uintptr_t>(data)); }
};
template<typename T> void* Address(T method) {
    static_assert(sizeof(T)==sizeof(void*),"single inheritance fixture ABI");
    void* p; memcpy(&p,&method,sizeof(p)); return p;
}
void Symbol(FILE* f,const char* name,void* addr,bool bad=false) {
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(f,"[%s]\nkind=code\nrva=0x%08lX\nexpect=",name,static_cast<unsigned long>(reinterpret_cast<uintptr_t>(addr)-base));
    auto p=static_cast<unsigned char*>(addr);
    for(int i=0;i<8;++i) std::fprintf(f,"%02X ",bad?0:p[i]);
    std::fprintf(f,"\n\n");
}
void Symbols(bool bad) {
    auto base=reinterpret_cast<unsigned char*>(GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    wchar_t name[128];
    swprintf_s(name,L"\\sdgo\\symbols\\battle_ai_native_test-%08lX-%08lX.ini",
               nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    FILE* f=nullptr; _wfopen_s(&f,(g_root+name).c_str(),L"wb");
    if(!f) { Check(false,"open fixture symbols"); return; }
    std::fprintf(f,"[host]\nexe=battle_ai_native_test.exe\ntimestamp=0x%08lX\nsize_of_image=0x%08lX\n\n",
                 nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    Symbol(f,"npc.think",Address(&Unit::Think),bad); Symbol(f,"npc.init_ai",Address(&Unit::Init));
    Symbol(f,"lua_tinker.state",reinterpret_cast<void*>(&State));
    Symbol(f,"lua.gettop",reinterpret_cast<void*>(&lua_gettop));
    Symbol(f,"lua.settop",reinterpret_cast<void*>(&lua_settop));
    Symbol(f,"lua.tostring",reinterpret_cast<void*>(&lua_tostring));
    Symbol(f,"lua.pushnumber",reinterpret_cast<void*>(&lua_pushnumber));
    Symbol(f,"lua.pushstring",reinterpret_cast<void*>(&lua_pushstring));
    Symbol(f,"lua.pushcclosure",reinterpret_cast<void*>(&lua_pushcclosure));
    Symbol(f,"lua.gettable",reinterpret_cast<void*>(&lua_gettable));
    Symbol(f,"lua.settable",reinterpret_cast<void*>(&lua_settable));
    Symbol(f,"lua.pcall",reinterpret_cast<void*>(&lua_pcall));
    Symbol(f,"lua.load",reinterpret_cast<void*>(&lua_load));
    Symbol(f,"lua.type",reinterpret_cast<void*>(&lua_type));
    Symbol(f,"lua.tonumber",reinterpret_cast<void*>(&lua_tonumber));
    Symbol(f,"lua.newtable",reinterpret_cast<void*>(&lua_newtable));
    native_world::Symbols(f,Symbol);
    fclose(f);
}
void Config(const char* value) {
    FILE* f=nullptr;
    _wfopen_s(&f,(g_root+L"\\sdgo\\mods\\battle_ai\\config.ini").c_str(),L"wb");
    if(f) { fputs(value,f); fclose(f); }
}
}
int wmain(int argc,wchar_t** argv) {
    bool disabled=argc>1 && !wcscmp(argv[1],L"disabled");
    bool bad=argc>1 && !wcscmp(argv[1],L"bad-signature");
    bool missing=argc>1 && !wcscmp(argv[1],L"missing-script");
    bool generic=argc>1 && !wcscmp(argv[1],L"generic");
    bool easy=argc>1 && !wcscmp(argv[1],L"easy");
    bool medium=argc>1 && (!wcscmp(argv[1],L"medium")||!wcscmp(argv[1],L"enabled"));
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr,exe,MAX_PATH); *wcsrchr(exe,L'\\')=0; g_root=exe;
    Symbols(bad);
    g_L=lua_open(); luaopen_base(g_L); lua_settop(g_L,0); luaopen_string(g_L); lua_settop(g_L,0);
    native_world::state=g_L;
    Check(Eval(R"(
      calls=0; moves=0
      function VSAI_Gunfighter_01_Update(n) calls=calls+1; return 123 end
      function CheckObejctAlive(id) return true end
      function GetDistance(a,b) return 0 end
      function DocumentFind(id) return nil end
      npc={GetNPCID=function() return 3604001 end,GetID=function() return 20 end,
        GetHp=function() return 1000 end,CheckHurt=function() return false end,
        GetNowPos=function() return {} end,GetLuaStep=function() return 3 end,
        GetAttackTarget=function() return 0 end,GetMoveTargetFlag=function() return false end}
      _ACT_VSAI_GF01_NONE=3
    )"),"fixture Lua bindings initialize");
    HMODULE core=LoadLibraryW((g_root+L"\\sdgo\\core.dll").c_str());
    auto init=core?reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core,"SdgoCoreInit")):nullptr;
    Check(init!=nullptr,"real core export present");
    if(!init) return 1;
    SdgoBoot boot={sizeof(boot),"battle_ai_native_test",GetModuleHandleW(nullptr)};
    Check(init(&boot)!=0,"real core initialization");
    Unit unit; unit.bytes[0x326C]=1; *reinterpret_cast<uint32_t*>(unit.bytes+0x3238)=3604001;
    if(generic||easy||medium) {
        native_world::Initialize(unit.bytes,g_L);
        native_world::Put(native_world::fsms[0].data(),0x200,uint32_t(2));
        native_world::enemy2[0xDCD]=0;
        uint32_t shotResult=reinterpret_cast<native_world::Callbacks*>(native_world::fsms[2].data())->Shot(111,2,3);
        Check(shotResult==0xBEEFF00D,"ranged shot hook forwards the original 32-bit returned object value");
        Check(*reinterpret_cast<uint32_t*>(native_world::fsms[2].data()+0x100)==116,"ranged shot observation forwards exact original arguments");
    }
    lua_pushnumber(g_L,987);
    unit.Init(reinterpret_cast<void*>(1)); unit.Think();
    Check(lua_gettop(g_L)==1 && lua_tonumber(g_L,1)==987,"adapter preserves incoming Lua stack");
    if(easy||medium) {
        native_world::Put(unit.bytes,0x31F4,77);unit.Think();
        Check(Eval("assert(calls==2 and SDGO_BattleAI_Universal.states[20]==nil and SDGO_BattleAI.states[20]==nil)"),
              "Easy/Medium keep original decisions without generic or legacy tactics");
        Check(native_world::lastWait==77 && native_world::jumps==0 && native_world::moves==0,
              "Easy/Medium leave native cadence and locomotion unchanged");
        native_world::Ray3 profileRay;
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
        if(easy) {
            Check(profileRay.direction.x==0 && profileRay.direction.z==1 && *reinterpret_cast<float*>(unit.bytes+0x3204)==0.5f,
                  "Easy leaves original aiming ray and NPC error untouched");
        } else {
            Check(profileRay.direction.x>0.9f && *reinterpret_cast<float*>(unit.bytes+0x3204)==0,
                  "Medium corrects native shot aim while retaining original AI");
            auto fsm=native_world::fsms[0].data();auto special=native_world::weapons[3].data();
            memcpy(special,native_world::weapons[1].data(),native_world::weapons[3].size());
            native_world::Put(special,0,uint32_t(104));special[0x69]=2;
            native_world::Put(native_world::definitions[0].data(),0x249+3*4,uint32_t(104));
            native_world::Put(fsm,0x24C+3*0xC8,int16_t(1));native_world::Put(fsm,0x258+3*0xC8,special);
            using RequestFn=void(__thiscall*)(uint8_t*);
            RequestFn volatile nativeRequest=reinterpret_cast<RequestFn>(native_world::Address(&native_world::Callbacks::AttackAdmission));
            int beforeRequests=native_world::attackAdmissions;
            nativeRequest(fsm);unit.Think();
            Check(native_world::attackAdmissions==beforeRequests+1&&*reinterpret_cast<int*>(fsm+0x240)==1,
                  "shared attack admission forwards ordinary shots even without SP and Medium does not select special");
            ++beforeRequests;
            native_world::Put(fsm,0x248,special);nativeRequest(fsm);
            Check(native_world::attackAdmissions==beforeRequests,"final attack admission rejects the special branch without player SP");
            native_world::Put(fsm,0x248,native_world::weapons[1].data());
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));unit.Think();
            Check(*reinterpret_cast<int*>(fsm+0x240)==3&&unit.bytes[0x312A]==0,
                  "Medium requests special slot through the native weapon-change API at full SP");
            unit.Think();Check(unit.bytes[0x312A]!=0,"Medium waits for special selection commit then requests native attack");
            nativeRequest(fsm);Check(native_world::attackAdmissions==beforeRequests+1,"legal native special request forwards to original authorization path");
            unit.bytes[0x31C4]=1;nativeRequest(fsm);unit.bytes[0x31C4]=0;
            Check(native_world::attackAdmissions==beforeRequests+1,"native special gate rejects explicit unit restriction");
            native_world::Put(native_world::movements[0].data(),0x2C0,int32_t(1));nativeRequest(fsm);
            Check(native_world::attackAdmissions==beforeRequests+1,"native special gate rejects controller override");
            native_world::Put(native_world::movements[0].data(),0x2C0,int32_t(0));
            native_world::Put(unit.bytes,0xBC8,int32_t(0));nativeRequest(fsm);
            Check(native_world::attackAdmissions==beforeRequests+1,"final native special gate enforces SP, restriction and controller permissions despite NPC overrides");
            native_world::deferSwitch=true;
            Config("[settings]\nDifficulty=Easy\n");Sleep(2100);unit.Think();
            Check(native_world::requestedSlot==1&&*reinterpret_cast<int*>(fsm+0x240)==3,
                  "profile disable retains a pending restore while the native weapon switch is deferred");
            native_world::deferSwitch=false;int priorSwitches=native_world::switchRequests;unit.Think();
            Check(native_world::switchRequests==priorSwitches+1&&*reinterpret_cast<int*>(fsm+0x240)==1,
                  "Easy handoff retries the previous weapon until the native selection commits");
            priorSwitches=native_world::switchRequests;unit.Think();
            Check(native_world::switchRequests==priorSwitches,"confirmed ordinary slot releases pending restore without repeated commands");
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
            Check(profileRay.direction.x==0 && *reinterpret_cast<float*>(unit.bytes+0x3204)==0.5f,
                  "Medium to Easy reload restores original aim behavior");
            Config("[settings]\n");native_world::SetRoomTitle(L"!ai3 hard");Sleep(2100);unit.Think();
            Check(Eval("assert(SDGO_BattleAI_Universal.states[20] and SDGO_BattleAI_Universal.states[20].target==2)"),
                  "heap room title selects Hard through the real app/service/mode/room pointer chain");
            Config("[settings]\nDifficulty=Hard\n");native_world::SetRoomTitle(L"!ai easy");Sleep(2100);unit.Think();
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
            Check(profileRay.direction.x==0&&profileRay.direction.z==1&&*reinterpret_cast<float*>(unit.bytes+0x3204)==0.5f,
                  "room difficulty overrides local config and hands Easy back to original aim");
            Config("[settings]\n");native_world::SetRoomTitle(L"!ai3",true);Sleep(2100);unit.Think();
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
            Check(profileRay.direction.x>0.9f&&Eval("assert(SDGO_BattleAI_Universal.states[20]==nil)"),
                  "unmarked inline room title returns to Medium with no manual client settings");
            native_world::SetRoomTitle(L"!ai hard");native_world::Put(native_world::room.data(),0x10,uint32_t(21));Sleep(2100);unit.Think();
            Check(Eval("assert(SDGO_BattleAI_Universal.states[20]==nil)"),"malformed room title fails closed to the default profile");
        }
    } else if(generic) {
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2 and calls==1)"),"generic dispatch chooses real observed lower HP and initializes stock once");
        unit.Think();
        Check(unit.bytes[0xDCD]==0,"native attack-intent hook enforces real-time interval between pulses");
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[2].data())->Shot(111,2,3);
        Sleep(130);unit.Think();
        Check(native_world::jumps==1 && native_world::moves>0,"brief ranged shot event enables native air strafe without gauge refill");
        Check(unit.bytes[0xDCD]!=0 && *reinterpret_cast<float*>(unit.bytes+0x3204)==0,"cadence resumes while NPC aiming error is removed");
        native_world::Put(native_world::fsms[0].data(),4,native_world::fsms[0].data()+0x34);
        native_world::Put(unit.bytes,0x31F4,77);unit.Think();
        Check(unit.bytes[0xDCD]!=0 && native_world::lastWait==77,"active charge/beam state retains native held input and extra timer");
        native_world::Put(native_world::fsms[0].data(),4,native_world::fsms[0].data()+0x20);
        native_world::Ray3 ray;
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&ray);
        Check(ray.origin.x==12 && ray.origin.y==100 && ray.origin.z==13 && ray.direction.x>0.9f,
              "native ranged-ray hook keeps original origin and corrects direction");
        native_world::Put(native_world::enemy1.data(),0x38,50);
        Sleep(110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==1 and calls==1)"),"new lower HP is selected without rerunning stock initialization");
        native_world::Put(native_world::enemy1.data(),0x38,1000);
        native_world::blocked=true;Sleep(110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==1)"),"fresh native collision query excludes lower HP behind an obstacle");
        native_world::blocked=false;Sleep(110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),"next fresh query sees newly clear target without a two-second shared cache");
        native_world::Put(native_world::nodes[2].data(),0x90,120.0f);
        native_world::Put(native_world::scenes[2].data(),0xBC,native_world::Aabb{{1360,50,80},{1440,150,160}});
        Sleep(100);unit.Think();
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&ray);
        Check(ray.direction.z>0.15f,"projectile ray leads a laterally moving observed target using native projectile speed");
        native_world::weapons[1][0x6E9]=1; // opaque initializer: static reader must not guess speed
        std::array<uint8_t,0xC00> projectile={};std::array<uint8_t,0x300> projectileNode={};
        std::array<void*,8> projectileVtable={};projectileVtable[0x18/sizeof(void*)]=native_world::Address(&native_world::Callbacks::Projectile);
        native_world::Put(projectile.data(),0,projectileVtable.data());
        native_world::Put(projectile.data(),8,projectileNode.data());
        native_world::Put(projectile.data(),0x9C4,unit.bytes);
        native_world::Put(projectile.data(),0x9C8,native_world::weapons[1].data());
        for(int i=0;i<3;++i) {
            bool result=reinterpret_cast<native_world::Callbacks*>(projectile.data())->Projectile(reinterpret_cast<void*>(0x1234));
            Check(result && *reinterpret_cast<uintptr_t*>(projectile.data()+0x100)==0x1234,"projectile observer preserves AL result and opaque argument");
        }
        Check(Eval("savedGeneric=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.weapons[2].projectileSpeed>2699 and o.weapons[2].projectileSpeed<2701); return 123 end"),"measured-speed probe installs");
        unit.Think();
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        memcpy(native_world::fsms[0].data()+0x56C,native_world::fsms[0].data()+0x24C,4*0xC8);
        unit.bytes[0xE34]=1;
        for(int i=0;i<3;++i) reinterpret_cast<native_world::Callbacks*>(projectile.data())->Projectile(nullptr);
        Eval("SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.valid==1 and o.weapons[2].projectileSpeed==0); return 123 end");
        unit.Think();
        Check(true,"old-form projectile samples are not learned as the new form's speed");
        unit.bytes[0xE34]=0;Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        Check(Eval("SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.weapons[2].projectileSpeed==0); return 123 end"),"nonlinear-speed probe installs");
        projectile[0xA38]=1;
        reinterpret_cast<native_world::Callbacks*>(projectile.data())->Projectile(nullptr);
        unit.Think();
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        projectile[0xA38]=0;projectileVtable[0x18/sizeof(void*)]=native_world::Address(&native_world::Callbacks::Homing);
        native_world::Put(native_world::definitions[0].data(),0,uint32_t(987650));
        for(int i=0;i<3;++i) reinterpret_cast<native_world::Callbacks*>(projectile.data())->Homing(nullptr);
        Eval("SDGO_BattleAI_GenericUpdate=function(n) local o=SDGO_BattleAI_Observe(); assert(o.weapons[2].projectileSpeed==0); return 123 end");
        unit.Think();
        Check(true,"constant-speed homing wrapper is not misclassified as a straight projectile");
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        native_world::weapons[1][0x6E9]=0;
        // Invoke the real C callback during a generic dispatch, while its actor
        // context is valid, to exercise route ownership and native deallocation.
        Check(Eval("savedGeneric=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_Route(1,1)==1); return 123 end"),"route probe installs");
        unit.Think();
        Check(native_world::frees>0,"native path buffer copied and released through the EXE allocator");
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        Check(Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_Route(1,1)==1); assert(SDGO_BattleAI_Route(999,1)==0); return 123 end"),"failed-repath probe installs");
        unit.Think();
        Check(unit.bytes[0x30DC]==0,"failed repath clears the old route and does not reissue its previous motor goal");
        native_world::pathMode=3;int beforeFree=native_world::frees;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_Route(1,1)==0); return 123 end");
        unit.Think();
        Check(native_world::frees==beforeFree+1 && native_world::protectedPath==nullptr && unit.bytes[0x30DC]==0,
              "unreadable native path copy still releases allocation and cancels route");
        native_world::pathMode=0;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) assert(SDGO_BattleAI_Route(1,1)==1); SDGO_BattleAI_CancelRoute(); return 123 end");
        unit.Think();Check(unit.bytes[0x30DC]==0,"cancelling an owned route releases its motor goal");
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        native_world::nullRay=true;unit.Think();native_world::nullRay=false;
        Check(Eval("assert(calls>=2)"),"failed native ray observation falls back rather than querying from a zero origin");
        native_world::Ray3 offThreadRay={};
        std::thread worker([&] {
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&offThreadRay);
            reinterpret_cast<native_world::Callbacks*>(unit.bytes)->Intent();
        });worker.join();
        Check(offThreadRay.direction.x==0 && offThreadRay.direction.z==1,"off-thread native ray retains original behavior without shared policy maps");
        Eval("priorGenericGeneration=SDGO_BattleAI_Universal.states[20].generation");
        std::thread initWorker([&] {unit.Init(reinterpret_cast<void*>(3));});initWorker.join();
        Sleep(110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].generation>priorGenericGeneration)"),"off-thread spawn reset is queued and applied by the established game thread");
        Eval("savedGeneric=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) sample=SDGO_BattleAI_Observe(); return 123 end");
        auto fsm=native_world::fsms[0].data();auto callbacks=reinterpret_cast<native_world::Callbacks*>(fsm);
        native_world::Put(fsm,4,fsm+0x34);native_world::Put(fsm,8,uint32_t(0));
        unit.Think();Check(Eval("assert(sample.shots==0)"),"raw projectile/muzzle calls are not primary attack-cycle fires");
        callbacks->Receipts(nullptr);unit.Think();
        Check(Eval("assert(sample.shots==2)"),"native receipt scope counts primary kinds 1 and 8, excludes chained kind 7");
        native_world::Put(fsm,4,fsm+0x20);callbacks->Receipts(nullptr);unit.Think();
        Check(Eval("assert(sample.shots==2)"),"non-ranged state cannot manufacture a counted primary fire");
        native_world::coverWall=true;auto beforeCoverMoves=native_world::moves;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) coverSample=SDGO_BattleAI_Cover(2,1400,100,0); count80=0; for i=1,1000 do count80=count80+SDGO_BattleAI_Chance(80) end; return 123 end");unit.Think();
        Check(Eval("assert(coverSample.valid==1 and coverSample.exposure<=3 and coverSample.peek.z>0)"),"real native bridge samples a minimally exposed fireable cover edge using private collision queries");
        Check(native_world::moves==beforeCoverMoves&&Eval("assert(count80>=750 and count80<=850)"),"cover sampling is read-only and mod-owned probability does not touch native RNG");
        native_world::coverWall=false;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) sample=SDGO_BattleAI_Observe(); return 123 end");
        native_world::Put(fsm,4,fsm+0x5C);native_world::Put(fsm,8,fsm+0x20);unit.Think();
        Check(Eval("assert(sample.meleeSwinging==1 and sample.rangedBusy==0)"),"actual swing survives a pending neutral observation");
        native_world::Put(fsm,8,uint32_t(0));native_world::Put(fsm,0x240,int32_t(0));
        native_world::Put(fsm,0x248,native_world::weapons[0].data());
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_MeleeGate(1); return 123 end");unit.Think();
        uint8_t input[0x20]={};input[0xD]=1;callbacks->SwingUpdate(input);
        Check(native_world::swingInput==0&&input[0xD]==1,"melee continuation gate preserves native update and restores event input afterward");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_MeleeGate(0); sample=SDGO_BattleAI_Observe(); return 123 end");
        native_world::Put(fsm,0x200,uint32_t(2));native_world::Put(fsm,0x224,uint32_t(1234));
        uint8_t message[0x50]={};native_world::Put(message,0,uint16_t(0xC));native_world::Put(message,0x14,int32_t(100));native_world::Put(message,0x3C,uint32_t(20));
        bool delivered=native_world::EmitContact(native_world::document.data(),2,message,0);unit.Think();
        Check(delivered&&Eval("assert(sample.contactTarget==2)"),"only the exact immediate-contact caller records delivered native melee contact");
        reinterpret_cast<native_world::Callbacks*>(native_world::document.data())->Message(1,message,0);unit.Think();
        Check(Eval("assert(sample.contactTarget==2)"),"another hurt-message caller cannot be mistaken for immediate melee contact");
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{100,0,0});
        native_world::Put(fsm,4,fsm+0x48);native_world::Put(fsm,0x968,int32_t(0));
        auto beforeRotations=native_world::rotations,beforeUpdates=native_world::trackUpdates;
        callbacks->TrackingEntry(input);callbacks->TrackingUpdate(input);Sleep(20);callbacks->TrackingUpdate(input);
        Check(native_world::rotations==beforeRotations+1&&native_world::trackUpdates==beforeUpdates&&
              *reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x5C,
              "selected opener cues only the NPC and skips tracking dash through the native queued swing");
        native_world::Put(fsm,4,fsm+0x5C);native_world::Put(fsm,8,uint32_t(0));callbacks->SwingEntry(input);
        Check(native_world::faces>0&&*reinterpret_cast<float*>(unit.bytes+0xD98)==0&&native_world::swingEntries>0,
              "actual swing Entry restores native target facing and runs original windup");
        fsm[0x9B1]=1;native_world::Put(fsm,4,fsm+0x48);beforeUpdates=native_world::trackUpdates;
        callbacks->TrackingEntry(input);callbacks->TrackingUpdate(input);
        Check(native_world::trackUpdates==beforeUpdates+1,"unsupported parallel tracking path falls back to original update");
        fsm[0x9B1]=0;
        auto movement=native_world::movements[0].data();native_world::Put(movement,4,movement+0xE8);
        beforeUpdates=native_world::trackUpdates;callbacks->TrackingEntry(input);callbacks->TrackingUpdate(input);unit.Think();
        Check(native_world::trackUpdates==beforeUpdates+1&&Eval("assert(sample.hurt==1)"),"pure MovementFSM hurt blocks quick swing without consuming CheckHurt latch");
        native_world::Put(movement,4,movement+0x20);native_world::Put(fsm,4,fsm+0x20);
        Eval("SDGO_BattleAI_GenericUpdate=savedGeneric");
        Config("[settings]\nGenericMode=1\nEnabled=0\n");Sleep(2100);unit.Think();
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&ray);
        Check(unit.bytes[0x312A]==0 && unit.bytes[0x30DC]==0 && ray.direction.z==1 && ray.direction.x==0,
              "disabling generic mode releases owned motor and shooting and retains stock ray");
    } else if(disabled||bad||missing) {
        Check(Eval("assert(SDGO_BattleAI==nil and calls==1)"),"disabled or rejected mod leaves native AI intact");
    } else {
        Check(Eval("assert(SDGO_BattleAI and SDGO_BattleAI.states[20] and SDGO_BattleAI.faults==0); firstGeneration=SDGO_BattleAI.states[20].generation"),"DLL injects policy on first native Think");
        unit.Init(reinterpret_cast<void*>(2)); unit.Think();
        Check(Eval("assert(SDGO_BattleAI.states[20].generation>firstGeneration and calls==2)"),"native respawn hook changes per-NPC generation");
        unit.bytes[0x2B65]=1; Eval("SDGO_BattleAI.states={}"); unit.Think();
        Check(Eval("assert(SDGO_BattleAI.states[20]==nil and calls==3)"),"remote native flag rejects enhanced decisions");
        unit.bytes[0x2B65]=0; *reinterpret_cast<uint32_t*>(unit.bytes+0x3238)=100;
        unit.Think();
        Check(Eval("assert(SDGO_BattleAI.states[20]==nil and calls==4)"),"unrelated native NPC id is excluded");
        *reinterpret_cast<uint32_t*>(unit.bytes+0x3238)=3604001;
        Config("[settings]\nGenericMode=0\nEnabled=0\nDecisionMs=invalid\n"); Sleep(2100); unit.Think();
        Check(Eval("assert(calls==5 and SDGO_BattleAI.faults==0)"),"separate config.ini reloads and invalid value is bounded");
        Config("[settings]\nGenericMode=0\nEnabled=1\n"); Sleep(2100); unit.Think();
        Check(Eval("assert(calls==6 and SDGO_BattleAI.states[20])"),"configuration can re-enable policy");
        Eval("function VSAI_Gunfighter_01_Update(n) calls=calls+1; return 123 end");
        Sleep(2100); unit.Think();
        Check(Eval("assert(calls==7 and VSAI_Gunfighter_01_Update==SDGO_BattleAI.wrappers.VSAI_Gunfighter_01_Update)"),"late native callback replacement is recaptured once");
        Eval("savedPolicy=SDGO_BattleAI.after");
        FILE* f=nullptr;
        _wfopen_s(&f,(g_root+L"\\sdgo\\mods\\battle_ai\\battle_ai.lua").c_str(),L"ab");
        if(f) { fputs("\nthis is invalid lua [",f); fclose(f); }
        Sleep(2100); unit.Think();
        Check(Eval("assert(calls==8 and SDGO_BattleAI.after==savedPolicy and SDGO_BattleAI.faults==0)"),"syntax error retains last working policy");
    }
    std::printf("NATIVE ADAPTER CHECKS %s\n",g_failures?"FAILED":"PASSED");
    // Mods live for this process; do not unload an installed trampoline.
    return g_failures?1:0;
}
