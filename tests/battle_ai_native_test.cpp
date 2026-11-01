// Real core + real battle_ai DLL + official Lua 5.0 VM. Game entry points are
// private fixtures: no game, account, network socket, or existing process used.
#include <windows.h>
#include <algorithm>
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
int QueueFixtureSwing(lua_State*) {
    auto fsm=native_world::fsms[0].data();
    native_world::Put(fsm,0x240,int32_t(0));native_world::Put(fsm,0x248,native_world::weapons[0].data());
    native_world::Put(fsm,8,fsm+0x5C);
    return 0;
}
struct Unit {
    unsigned char bytes[0x3300] = {};
    __declspec(noinline) bool Think() {
        if(!bytes[0x326C]) return true;
        int result=native_world::Dispatch("VSAI_Gunfighter_01_Update",bytes);
        Check(result==123,"native dispatcher callback preserves its return value");
        reinterpret_cast<native_world::Callbacks*>(this)->Intent();
        if(*reinterpret_cast<uint8_t**>(bytes+8)) reinterpret_cast<native_world::Callbacks*>(this)->FacingUpdate();
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
    if(generic||medium) {
        auto fsm=native_world::fsms[0].data();
        native_world::Put(fsm,0x240,int32_t(0));native_world::Put(fsm,0x248,native_world::weapons[0].data());
        native_world::Put(unit.bytes,0xDD0,int32_t(1));
        int prior=native_world::switchRequests;
        Eval("npc:ChangeWeapon(1)");
        Check(native_world::switchRequests==prior&&*reinterpret_cast<int*>(fsm+0x240)==0,
              "stock NPC hint shortcut can suppress a switch that never actually committed");
        Eval("savedWeaponProbe=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) weaponProbeResult=SDGO_BattleAI_RequestWeapon(1); return 123 end");
        unit.Think();
        Check(native_world::switchRequests==prior+1&&native_world::requestedSlot==1&&*reinterpret_cast<int*>(fsm+0x240)==1,
              "native message bridge retries a stale NPC hint and lets the receiver commit the real slot");
        native_world::deferSwitch=true;
        native_world::Put(fsm,0x240,int32_t(0));native_world::Put(fsm,0x248,native_world::weapons[0].data());
        unit.Think();
        Check(*reinterpret_cast<int*>(fsm+0x240)==0,
              "queued switch never fabricates acknowledgement when the native receiver defers it");
        Check(Eval("assert(SDGO_BattleAI_RequestWeapon(1)==0)"),"weapon bridge rejects calls outside the current owned AI callback");
        native_world::deferSwitch=false;
        native_world::Put(fsm,0x240,int32_t(1));native_world::Put(fsm,0x248,native_world::weapons[1].data());
        Eval("SDGO_BattleAI_GenericUpdate=savedWeaponProbe");
    }
    if(generic||medium) {
        Eval("motionProbe=SDGO_BattleAI_GenericUpdate; SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_Observe(); n:SetMoveTargetPos_Dir(2,400,2,false); n:SetMoveType(1); n:SetAttackOnOff(true); return 123 end");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,1000});
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,-1});
        native_world::Put(unit.bytes,0x3078,native_world::Vec3{0,0,-400});
        unit.Think();
        const auto protectedFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(protectedFacing.z>0.99f&&unit.bytes[0x30DC]!=0&&reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078)->z==-400,
              "Medium/Hard backward movement retains its waypoint and front facing despite a reversed path yaw cache");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{0,0,-3000});
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        unit.Think();
        Check(reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C)->z>0.99f,
              "a far idle opponent cannot force side-on defence during an existing front engagement");
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{0,0,-1000});
        native_world::Put(native_world::fsms[1].data(),0x200,uint32_t(20));
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        lua_pushcfunction(g_L,QueueFixtureSwing);lua_setglobal(g_L,"FixtureQueueSwing");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_Observe(); n:SetAttackTarget(2); FixtureQueueSwing(); return 123 end");
        const int beforeMeleeTurn=native_world::rotations;unit.Think();
        Check(native_world::rotations==beforeMeleeTurn&&reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C)->z>0.99f,
              "a melee action queued after observation retains native target-facing instead of defence turning its swing away");
        native_world::Put(native_world::fsms[0].data(),8,uint32_t(0));
        native_world::Put(native_world::fsms[0].data(),0x240,int32_t(1));
        native_world::Put(native_world::fsms[0].data(),0x248,native_world::weapons[1].data());
        native_world::Put(native_world::fsms[1].data(),0x200,uint32_t(0));
        native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{1000,0,0});
        Eval("SDGO_BattleAI_GenericUpdate=motionProbe");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{1400,0,100});
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        unit.bytes[0x30DC]=0;
    }
    if(easy||medium) {
        native_world::Put(unit.bytes,0x31F4,77);unit.Think();
        if(easy) {
            Check(Eval("assert(calls==2 and SDGO_BattleAI_Universal.states[20]==nil and SDGO_BattleAI.states[20]==nil)"),
                  "Easy keeps original decisions without generic or legacy tactics");
            Check(native_world::lastWait==77 && native_world::jumps==0 && native_world::moves==0,
                  "Easy leaves native cadence and locomotion unchanged");
        } else {
            Check(Eval("assert(calls==1 and SDGO_BattleAI_Universal.states[20].target==2 and SDGO_BattleAI.states[20]==nil)"),
                  "Medium uses continuous generic decisions and never reruns stock idle");
            Check(native_world::lastWait==0 && unit.bytes[0xDCD]!=0 && native_world::jumps==0,
                  "Medium clears native NPC idle wait and fires on consecutive ready frames without Hard air tactics");
        }
        native_world::Ray3 profileRay;
        reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
        if(easy) {
            Check(profileRay.direction.x==0 && profileRay.direction.z==1 && *reinterpret_cast<float*>(unit.bytes+0x3204)==0.5f,
                  "Easy leaves original aiming ray and NPC error untouched");
        } else {
            Check(profileRay.direction.x>0.9f && *reinterpret_cast<float*>(unit.bytes+0x3204)==0,
                  "Medium corrects native shot aim with continuous decisions");
            using InitializeFn=void(__cdecl*)(uint8_t*,int,uint32_t);
            auto initializeUnit=reinterpret_cast<InitializeFn>(GetProcAddress(GetModuleHandleW(L"battle_ai.dll"),"SdgoBattleAiInitializeUnit"));
            Check(initializeUnit!=nullptr,"optional match_ai native loadout endpoint is exported by its stable name");
            if(initializeUnit) {
                native_world::Put(unit.bytes,0xBC8,int32_t(18000));initializeUnit(unit.bytes,0x101,1);
                Check(*reinterpret_cast<int*>(unit.bytes+0xB0C)==12&&native_world::lastPart==0&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,
                      "Medium first spawn applies native level12/no part and clears pooled SP to the authoritative match start");
                int priorParts=native_world::partApplications;initializeUnit(unit.bytes,1,1);
                Check(native_world::partApplications==priorParts,"duplicate spawn/deck generation never reconstructs a client part");
                native_world::Put(unit.bytes,0xBC8,int32_t(12000));initializeUnit(unit.bytes,0x101,1);
                Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==12000,"duplicate initial-spawn delivery cannot erase earned SP");
                initializeUnit(unit.bytes,2,2);
                Check(*reinterpret_cast<int*>(unit.bytes+0xB0C)==13&&native_world::lastPart==75002,
                      "Hard applies native EX13 and a full-width legal mobility part for a mixed ranged loadout");
                Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==12000,"respawn/deck generation preserves the match SP resource");
                native_world::Put(unit.bytes,0xBC8,int32_t(0));
                native_world::Put(unit.bytes,0xB0C,int32_t(1));priorParts=native_world::partApplications;initializeUnit(unit.bytes,0,3);
                Check(*reinterpret_cast<int*>(unit.bytes+0xB0C)==1&&native_world::partApplications==priorParts,
                      "Easy spawn keeps the native level and optional part unchanged");
                uint8_t range1=native_world::weapons[1][0x6A],range2=native_world::weapons[2][0x6A];
                native_world::weapons[1][0x6A]=native_world::weapons[2][0x6A]=1;initializeUnit(unit.bytes,2,4);
                Check(native_world::lastPart==75003,"native melee-only weapon properties select the full-width fighting part on the client");
                native_world::weapons[1][0x6A]=range1;native_world::weapons[2][0x6A]=range2;
            }
            native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
            native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,-500});
            native_world::Put(unit.bytes,0x3090,native_world::Vec3{-1,0,0});Sleep(90);unit.Think();
            auto facing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
            Check(facing.z>0.95f&&std::abs(facing.x)>0.01f,"Medium rear defence requests a bounded native scene turn even when old desired cache points elsewhere");
            for(int i=0;i<16;++i) {Sleep(16);unit.Think();}
            facing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
            Check(facing.z<-0.98f,"Medium quickly faces an enemy attacking from behind over visible native frames");
            native_world::Put(native_world::enemy1.data(),0x38,int32_t(1000));
            native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{0,0,1000});
            native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,-1000});
            native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
            int beforeDefenceMoves=native_world::moves;Sleep(90);unit.Think();
            Check(native_world::moves>beforeDefenceMoves&&std::abs(reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078)->x)>1,
                  "Medium proposes a native lateral route to reduce front/rear crossfire");
            native_world::Put(native_world::nodes[1].data(),0x88,native_world::Vec3{1000,0,0});
            native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{1400,0,100});
            native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});Sleep(90);unit.Think();
            auto fsm=native_world::fsms[0].data();auto special=native_world::weapons[3].data();
            memcpy(special,native_world::weapons[1].data(),native_world::weapons[3].size());
            native_world::Put(special,0,uint32_t(104));special[0x69]=2;
            native_world::Put(native_world::definitions[0].data(),0x249+3*4,uint32_t(104));
            native_world::Put(fsm,0x24C+3*0xC8,int16_t(1));native_world::Put(fsm,0x258+3*0xC8,special);
            using RequestFn=void(__thiscall*)(uint8_t*);
            RequestFn volatile nativeRequest=reinterpret_cast<RequestFn>(native_world::Address(&native_world::Callbacks::AttackAdmission));
            auto requestSpecial=[&] {
                native_world::Put(fsm,0x240,int32_t(3));native_world::Put(fsm,0x248,special);
                nativeRequest(fsm);
            };
            int beforeRequests=native_world::attackAdmissions;
            nativeRequest(fsm);unit.Think();
            Check(native_world::attackAdmissions==beforeRequests+1&&*reinterpret_cast<int*>(fsm+0x240)==1,
                  "shared attack admission forwards ordinary shots even without SP and Medium does not select special");
            ++beforeRequests;
            native_world::Put(fsm,0x248,special);requestSpecial();
            Check(native_world::attackAdmissions==beforeRequests,"final attack admission rejects the special branch without player SP");
            native_world::Put(fsm,0x248,native_world::weapons[1].data());
            auto pressureMotor=native_world::movements[0].data();
            native_world::Put(pressureMotor,4,pressureMotor+0xE8);
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));unit.Think();
            Check(*reinterpret_cast<int*>(fsm+0x240)==3&&unit.bytes[0x312A]==0,
                  "Medium uses full SP for a player-permitted hurt counter through the native weapon-change API");
            unit.Think();Check(unit.bytes[0x312A]!=0,"Medium waits for special selection commit then requests native attack");
            native_world::Put(pressureMotor,4,pressureMotor+0x20);
            requestSpecial();Check(native_world::attackAdmissions==beforeRequests+1,"legal native special request forwards to original authorization path");
            native_world::Put(unit.bytes,0x2B68,uint32_t(0x80000014));uint8_t accepted[0x18]={};
            native_world::Put(accepted,6,uint32_t(0x80000014));native_world::Put(accepted,0xA,uint32_t(104));native_world::Put(accepted,0x12,int32_t(20000));
            using NoticeFn=int(__thiscall*)(uint8_t*,const uint8_t*);
            NoticeFn volatile notify=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::SpecialNotice));
            int noticeReturn=notify(native_world::document.data(),accepted);unit.Think();
            Check(noticeReturn==0x51A7&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==20000,
                  "matched server special acceptance preserves native return and applies authoritative SP through native command18");
            native_world::Put(unit.bytes,0xBC8,int32_t(15000));notify(native_world::document.data(),accepted);unit.Think();
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==15000,"duplicate special notice without a pending request cannot refund NPC SP");
            uint8_t terminal[0x20]={};native_world::Put(terminal,6,uint32_t(0x80000014));native_world::Put(terminal,0xE,int32_t(0));
            NoticeFn volatile meleeNotice=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::MeleeNotice));
            int terminalResult=meleeNotice(native_world::document.data(),terminal);unit.Think();
            Check(terminalResult==0x51B1&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,"actual melee completion606 applies authoritative remaining SP through native command18");
            Check(*reinterpret_cast<int*>(fsm+0x240)==1,
                  "authoritative completion immediately restores the ordinary weapon without waiting for a missed busy-state sample");
            native_world::Put(terminal,0xE,int32_t(20000));meleeNotice(native_world::document.data(),terminal);unit.Think();
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,"duplicate terminal notification cannot refund a finished special");
            unit.bytes[0x31C4]=1;requestSpecial();unit.bytes[0x31C4]=0;
            Check(native_world::attackAdmissions==beforeRequests+1,"native special gate rejects explicit unit restriction");
            native_world::Put(native_world::movements[0].data(),0x2C0,int32_t(1));requestSpecial();
            Check(native_world::attackAdmissions==beforeRequests+1,"native special gate rejects controller override");
            native_world::Put(native_world::movements[0].data(),0x2C0,int32_t(0));
            native_world::Put(unit.bytes,0xBC8,int32_t(0));requestSpecial();
            Check(native_world::attackAdmissions==beforeRequests+1,"final native special gate enforces SP, restriction and controller permissions despite NPC overrides");
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));requestSpecial();notify(native_world::document.data(),accepted);unit.Think();
            NoticeFn volatile rangedNotice=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::RangedNotice));
            native_world::Put(terminal,0xA,int32_t(0));
            int offThreadResult=0;
            std::thread noticeWorker([&]{offThreadResult=rangedNotice(native_world::document.data(),terminal);});noticeWorker.join();
            Check(offThreadResult==0x51B2&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==20000,"off-thread ranged completion forwards original and queues SP without touching game-thread actor state");
            unit.Think();Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,"game thread drains ranged completion789 authoritative SP before the next decision");
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));requestSpecial();notify(native_world::document.data(),accepted);unit.Think();
            NoticeFn volatile spState=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::SpStateNotice));
            uint8_t state[0xF0]={};native_world::Put(state,0,uint16_t(0x779));native_world::Put(state,6,uint32_t(0x80000014));
            state[0xA]=0xFF;native_world::Put(state,0xB,int32_t(0));native_world::Put(state,0xF,uint32_t(104));
            int priorDamageNotices=native_world::ordinaryNotices;int oldHp=*reinterpret_cast<int*>(unit.bytes+0x38);
            int stateResult=spState(native_world::document.data(),state);unit.Think();
            Check(stateResult==1&&native_world::ordinaryNotices==priorDamageNotices&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==0&&*reinterpret_cast<int*>(unit.bytes+0x38)==oldHp,
                  "fixed-size zero-victim SP state updates use native command18 without damage, hurt, effects or attack processing");
            state[0xA]=0;native_world::Put(state,0xB,int32_t(1000));int ordinaryResult=spState(native_world::document.data(),state);
            Check(ordinaryResult==0x51D0&&native_world::ordinaryNotices==priorDamageNotices+1,
                  "ordinary0779 notification preserves its original return and processing");
            state[0xA]=0xFF;native_world::Put(state,0xB,int32_t(0));
            std::thread stateWorker([&]{spState(native_world::document.data(),state);});stateWorker.join();
            state[0xA]=0;native_world::Put(state,0xB,int32_t(4000));
            std::thread gainWorker([&]{spState(native_world::document.data(),state);});gainWorker.join();
            unit.Think();Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==4000,
                  "ordered off-thread state and ordinary damage snapshots cannot overwrite a newer earned SP gain");
            state[0xA]=0xFF;native_world::Put(state,0xB,int32_t(0));
            std::thread oldState([&]{spState(native_world::document.data(),state);});oldState.join();
            state[0xA]=0;native_world::Put(state,0xB,int32_t(5000));spState(native_world::document.data(),state);unit.Think();
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==5000,"older worker state cannot overwrite a newer main-thread ordinary SP gain");
            native_world::Put(state,0xB,int32_t(6000));
            std::thread oldGain([&]{spState(native_world::document.data(),state);});oldGain.join();
            state[0xA]=0xFF;native_world::Put(state,0xB,int32_t(0));spState(native_world::document.data(),state);unit.Think();
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,"older worker gain cannot refund a newer main-thread terminal SP state");
            uint8_t lateOrdinary[0xF0]={};memcpy(lateOrdinary,state,sizeof(state));lateOrdinary[0xA]=0;native_world::Put(lateOrdinary,0xB,int32_t(7000));
            native_world::ordinaryEntered=false;native_world::releaseOrdinary=false;native_world::stallOrdinary=true;
            std::thread lateGain([&]{spState(native_world::document.data(),lateOrdinary);});
            for(int i=0;i<1000&&!native_world::ordinaryEntered;++i) Sleep(1);
            Check(native_world::ordinaryEntered,"controlled old ordinary handler has entered before the newer state");
            spState(native_world::document.data(),state);unit.Think();
            native_world::releaseOrdinary=true;lateGain.join();native_world::stallOrdinary=false;
            Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==7000,"controlled legacy worker finishes its old native SP write after the newer state");
            unit.Think();Check(*reinterpret_cast<int*>(unit.bytes+0xBC8)==0,
                  "late old native writes are corrected to the last authoritative sequence before AI decisions");
            std::thread oldBeforeSkill([&]{spState(native_world::document.data(),state);});oldBeforeSkill.join();
            uint8_t skillPacket[40]={};native_world::Put(skillPacket,0,uint16_t(0x6AA));native_world::Put(skillPacket,6,uint32_t(0x80000014));native_world::Put(skillPacket,0x1F,int32_t(9000));
            NoticeFn volatile skillNotice=reinterpret_cast<NoticeFn>(native_world::Address(&native_world::Callbacks::SkillNotice));
            int skillResult=skillNotice(native_world::document.data(),skillPacket);unit.Think();
            Check(skillResult==0x51D1&&*reinterpret_cast<int*>(unit.bytes+0xBC8)==9000,
                  "native skill-state SP snapshots share the same sequence and cannot be rolled back by an older queued clear");
            native_world::Put(pressureMotor,4,pressureMotor+0xE8);
            native_world::Put(unit.bytes,0xBC8,int32_t(30000));unit.Think();unit.Think();
            native_world::Put(pressureMotor,4,pressureMotor+0x20);
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
            native_world::Put(unit.bytes,0xBC8,int32_t(0));
            Config("[settings]\n");native_world::SetRoomTitle(L"!ai3 hard");Sleep(2100);unit.Think();
            Check(Eval("assert(SDGO_BattleAI_Universal.difficulty==2 and SDGO_BattleAI_Universal.states[20].target==2)"),
                  "heap room title selects Hard through the real app/service/mode/room pointer chain");
            Config("[settings]\nDifficulty=Hard\n");native_world::SetRoomTitle(L"!ai easy");Sleep(2100);unit.Think();
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
            Check(profileRay.direction.x==0&&profileRay.direction.z==1&&*reinterpret_cast<float*>(unit.bytes+0x3204)==0.5f,
                  "room difficulty overrides local config and hands Easy back to original aim");
            Config("[settings]\n");native_world::SetRoomTitle(L"!ai3",true);Sleep(2100);unit.Think();
            reinterpret_cast<native_world::Callbacks*>(native_world::fsms[0].data())->Ray(&profileRay);
            Check(profileRay.direction.x>0.9f&&Eval("assert(SDGO_BattleAI_Universal.difficulty==1 and SDGO_BattleAI_Universal.states[20])"),
                  "unmarked inline room title returns to Medium with no manual client settings");
            native_world::SetRoomTitle(L"!ai hard");native_world::Put(native_world::room.data(),0x10,uint32_t(21));Sleep(2100);unit.Think();
            Check(Eval("assert(SDGO_BattleAI_Universal.difficulty==1 and SDGO_BattleAI_Universal.states[20])"),"malformed room title fails closed to the default profile");
        }
    } else if(generic) {
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2 and calls==1)"),"generic dispatch chooses real observed lower HP and initializes stock once");
        unit.Think();
        Check(unit.bytes[0xDCD]!=0,"native attack-intent hook has no additional interval between ready pulses");
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
        Eval("SDGO_BattleAI_GenericUpdate=function(n) coverSample=SDGO_BattleAI_Cover(2,1400,100,0); coverRepeat=SDGO_BattleAI_Cover(2,1400,100,0); count80=0; for i=1,1000 do count80=count80+SDGO_BattleAI_Chance(80) end; return 123 end");
        int maxCoverQueries=0;
        for(int frame=0;frame<6;++frame) {int previousQueries=native_world::queries;unit.Think();maxCoverQueries=(std::max)(maxCoverQueries,native_world::queries-previousQueries);}
        Check(maxCoverQueries<=46&&Eval("assert(coverRepeat.pending==1)"),
              "cover geometry is bounded per Think and repeated same-frame calls do not repeat the search");
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
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(100));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,150});
        auto enemyFsm=native_world::fsms[2].data();native_world::Put(enemyFsm,4,enemyFsm+0x5C);native_world::Put(enemyFsm,0x200,uint32_t(20));
        native_world::Put(enemyFsm,0x240,int32_t(0));native_world::Put(enemyFsm,0x248,native_world::weapons[0].data());
        native_world::Put(unit.bytes,0x2B70,500.0f);native_world::Put(unit.bytes,0x2B74,500.0f);
        native_world::Put(native_world::definitions[0].data(),0x145,int32_t(500));native_world::definitions[0][0x149]=0;
        native_world::definitions[0][0x14E]=4;native_world::definitions[0][0x14F]=4;
        native_world::Put(movement,4,movement+0xE8);unit.bytes[0x30DC]=0;
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        Sleep(16);unit.Think();auto shieldFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(shieldFacing.z>0.95f&&std::abs(shieldFacing.x)>0.01f&&unit.bytes[0x30DC]==0,
              "Hard begins a smooth back-shield turn during native hurt without forcing a movement escape");
        for(int i=0;i<16;++i) {Sleep(16);unit.Think();}
        shieldFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(shieldFacing.z<-0.98f,"Hard presents an intact rear shield to the melee attacker");
        native_world::Put(unit.bytes,0x2B70,0.0f);Sleep(16);unit.Think();
        auto brokenFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(brokenFacing.z>-0.999f&&brokenFacing.z<0,"shield break immediately starts a bounded turn back toward front defence");
        for(int i=0;i<16;++i) {Sleep(16);unit.Think();}
        brokenFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(brokenFacing.z>0.98f,"broken rear shield never leaves the bot intentionally exposing its back");
        native_world::blocked=true;native_world::Put(unit.bytes,0x2B70,500.0f);native_world::definitions[0][0x149]=2;
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        native_world::Put(message,0x3C,uint32_t(2));message[0x19]=1;message[0x1A]=2;
        native_world::EmitContact(native_world::document.data(),20,message,0);
        for(int i=0;i<16;++i) {Sleep(16);unit.Think();}
        shieldFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
        Check(shieldFacing.z<-0.98f,"a recent delivered attacker uses message HitType at +1A, not adjacent RangeType +19, when visibility is lost");
        native_world::blocked=false;native_world::definitions[0][0x149]=0;
        for(uint8_t sideMode:{uint8_t(2),uint8_t(1)}) {
            native_world::definitions[0][0x14E]=sideMode;native_world::definitions[0][0x14F]=sideMode;
            native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
            for(int i=0;i<8;++i) {Sleep(16);unit.Think();}
            auto sideFacing=*reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C);
            Check(sideMode==2?sideFacing.x<-0.98f:sideFacing.x>0.98f,
                  sideMode==2?"class-3 shield presents its numeric side with forward -X to attacker +Z":"class-4 shield uses the opposite native yaw sign with forward +X");
        }
        native_world::Put(movement,4,movement+0x20);
        native_world::Put(unit.bytes,0x2B70,500.0f);
        native_world::definitions[0][0x14E]=native_world::definitions[0][0x14F]=4;
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_Observe(); n:SetMoveTargetPos_Dir(2,400,2,false); n:SetMoveType(1); return 123 end");
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,-1});
        unit.Think();
        Check(reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C)->z>0.99f&&unit.bytes[0x30DC]!=0,
              "Hard legal retreat faces the attacker instead of turning its intact rear shield and back into the movement");
        native_world::Put(unit.bytes,0x2B70,0.0f);
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

    if(generic||medium) {
        native_world::Initialize(unit.bytes,g_L);
        Config(generic?"[settings]\nDifficulty=Hard\nCoverChance=0\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        native_world::Configure(native_world::remoteHuman.data(),3,3,1,200,1200,150);
        native_world::remoteHuman[0x2B65]=1;native_world::Put(native_world::remoteHuman.data(),0x3238,uint32_t(0));
        native_world::Configure(native_world::sameTeamNpc.data(),4,4,2,1,600,50);
        native_world::remoteHead={&native_world::remoteEntry,&native_world::remoteEntry,nullptr};
        native_world::remoteEntry={&native_world::remoteHead,&native_world::remoteHead,native_world::remoteHuman.data()};
        native_world::friendlyHead={&native_world::friendlyEntry,&native_world::friendlyEntry,nullptr};
        native_world::friendlyEntry={&native_world::friendlyHead,&native_world::friendlyHead,native_world::sameTeamNpc.data()};
        native_world::Put(native_world::document.data(),0x70,&native_world::remoteHead);
        native_world::Put(native_world::document.data(),0x7C,&native_world::friendlyHead);
        native_world::Put(native_world::enemy1.data(),0x3238,uint32_t(0));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(50));
        unit.Init(reinterpret_cast<void*>(4));Sleep(2100);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),
              "enemy AI selects the opposing team's AI while excluding its own lower-HP AI teammate");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(2));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "authoritative knockdown protection excludes the previous target from attack selection");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));
        auto protectedMovement=native_world::movements[2].data();native_world::Put(protectedMovement,8,protectedMovement+0x110);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "a pending native get-up phase remains protected before authoritative state synchronization");
        native_world::Put(protectedMovement,8,uint32_t(0));native_world::Put(protectedMovement,4,protectedMovement+0xE8);unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),
              "ordinary hit-stun immediately becomes a legal target after the protected phase clears");
        native_world::Put(protectedMovement,4,protectedMovement+0x20);
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(0));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "the target's native permission state excludes an unattackable AI without excluding remote humans");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(4));native_world::Put(native_world::enemy2.data(),0xEB8,uint32_t(20));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),
              "native restricted permission can admit an AI for this specific attacker");
        native_world::Put(native_world::enemy2.data(),0xEB8,uint32_t(999));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "an AI restricted to another attacker does not bypass native targetability");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(0));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==3)"),
              "enemy AI selects the remote human teammate instead of being limited to the local human");
        native_world::Put(native_world::remoteHuman.data(),0x38,int32_t(0));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==1)"),
              "local human remains a normal eligible opponent after other opposing targets die");
        native_world::Put(unit.bytes,0x20,int32_t(1));
        native_world::Put(native_world::enemy2.data(),0x38,int32_t(50));native_world::Put(native_world::enemy2.data(),0x20,int32_t(2));
        native_world::Put(native_world::sameTeamNpc.data(),0x20,int32_t(1));unit.Think();
        Check(Eval("assert(SDGO_BattleAI_Universal.states[20].target==2)"),
              "host-side allied AI attacks the opposing AI and never the host or its own AI teammate");
        auto protectionFsm=native_world::fsms[0].data();
        native_world::Put(protectionFsm,0x240,int32_t(0));native_world::Put(protectionFsm,0x248,native_world::weapons[0].data());
        native_world::Put(protectionFsm,0x200,uint32_t(2));native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(2));
        auto sourceCallbacks=reinterpret_cast<native_world::Callbacks*>(protectionFsm);
        const int beforeProtectedAdmission=native_world::attackAdmissions;sourceCallbacks->AttackAdmission();
        Check(native_world::attackAdmissions==beforeProtectedAdmission,
              "native melee admission rejects protection even if it changed after the last Lua decision");
        native_world::Put(protectionFsm,4,protectionFsm+0x48);native_world::Put(protectionFsm,8,uint32_t(0));
        uint8_t trackingInput[0x20]={};trackingInput[0xD]=1;
        const int beforeProtectedTracking=native_world::trackUpdates;sourceCallbacks->TrackingUpdate(trackingInput);
        Check(*reinterpret_cast<uint8_t**>(protectionFsm+4)==protectionFsm+0x48&&
              *reinterpret_cast<uint8_t**>(protectionFsm+8)==protectionFsm+0x20&&native_world::trackUpdates==beforeProtectedTracking,
              "protected-target knife tracking queues a normal release instead of chasing or snapping an active swing");
        native_world::Put(protectionFsm,8,uint32_t(0));native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));
        sourceCallbacks->TrackingUpdate(trackingInput);
        Check(native_world::trackUpdates==beforeProtectedTracking+1,
              "ordinary tracking resumes immediately when target protection clears");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(2));native_world::Put(protectionFsm,8,protectionFsm+0x5C);
        sourceCallbacks->TrackingUpdate(trackingInput);
        Check(*reinterpret_cast<uint8_t**>(protectionFsm+8)==protectionFsm+0x5C&&native_world::trackUpdates==beforeProtectedTracking+2,
              "protection cancellation never overwrites an already queued native action");
        native_world::Put(protectionFsm,4,protectionFsm+0x5C);uint8_t protectedInput[0x20]={};protectedInput[0xD]=1;
        sourceCallbacks->SwingUpdate(protectedInput);
        Check(native_world::swingInput==0&&protectedInput[0xD]==1,
              "native continuation suppresses another slash against a protected target without corrupting shared input");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));sourceCallbacks->AttackAdmission();
        sourceCallbacks->SwingUpdate(protectedInput);
        Check(native_world::attackAdmissions==beforeProtectedAdmission+1&&native_world::swingInput==1,
              "ordinary hurt resumes legal native melee admission and continuation immediately");
        native_world::Put(protectionFsm,4,protectionFsm+0x20);native_world::Put(protectionFsm,8,uint32_t(0));native_world::Put(protectionFsm,0x240,int32_t(1));
        native_world::Put(protectionFsm,0x248,native_world::weapons[1].data());
        native_world::Put(native_world::document.data(),0x70,static_cast<native_world::ListNode*>(nullptr));
        native_world::Put(native_world::document.data(),0x7C,static_cast<native_world::ListNode*>(nullptr));
    }
    if(generic||medium) {
        native_world::Initialize(unit.bytes,g_L);
        Config(generic?"[settings]\nDifficulty=Hard\nCoverChance=0\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(2));
        native_world::Put(native_world::nodes[2].data(),0x88,native_world::Vec3{0,0,150});
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,-1});
        unit.Init(reinterpret_cast<void*>(5));Sleep(2100);unit.Think();
        Check(!unit.bytes[0x312A]&&!unit.bytes[0xDCD]&&unit.bytes[0x30DC]&&
              reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x6C)->z>0.99f,
              "a sole knocked-down enemy triggers non-firing repositioning while the actor keeps its front toward the get-up target");
        native_world::Put(native_world::enemy2.data(),0x9D8,int32_t(1));unit.Think();
        Check(unit.bytes[0x312A]&&unit.bytes[0xDCD],
              "the first fully cleared get-up observation resumes a natively ready shot without an extra timer");
    }
    if(generic||medium) {
        native_world::Initialize(unit.bytes,g_L);native_world::directPathFixture=true;native_world::pathMode=0;
        Config(generic?"[settings]\nDifficulty=Hard\n":"[settings]\nDifficulty=Medium\n");
        native_world::SetRoomTitle(generic?L"!ai hard":L"!ai medium");
        unit.Init(reinterpret_cast<void*>(3));Sleep(2100);unit.Think();
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_Observe();return 123 end");
        auto location=[&] {return *reinterpret_cast<native_world::Vec3*>(native_world::nodes[0].data()+0x88);};
        auto setPosition=[&](native_world::Vec3 p){native_world::Put(native_world::nodes[0].data(),0x88,p);};
        auto crossfire=[&] {
            auto p=location();
            native_world::Put(native_world::nodes[1].data(),0x88,p+native_world::Vec3{0,0,-5000});
            native_world::Put(native_world::nodes[2].data(),0x88,p+native_world::Vec3{0,0,5000});
            uint8_t hit[0x50]={};native_world::Put(hit,0,uint16_t(0xC));native_world::Put(hit,0x14,int32_t(1));
            for(uint32_t attacker:{1u,2u}) {native_world::Put(hit,0x3C,attacker);native_world::EmitContact(native_world::document.data(),20,hit,0);}
        };
        crossfire();const auto anchor=location();const double radius=generic?1200:480;
        native_world::pathMode=4;const int priorPaths=native_world::pathCalls;unit.Think();
        const auto alternateGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(unit.bytes[0x30DC]&&alternateGoal.x<anchor.x&&native_world::pathCalls-priorPaths<=3,
              "a blocked best escape point tries a bounded reachable alternative instead of giving up on movement");
        if(unit.bytes[0x30DC]) setPosition(alternateGoal);
        native_world::pathMode=0;
        bool bounded=true;double furthest=0;
        for(int step=0;step<12;++step) {
            unit.Think();
            if(unit.bytes[0x30DC]) {
                auto goal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
                double distance=battle_ai::Length(goal-anchor);furthest=(std::max)(furthest,distance);
                bounded=bounded&&distance<=radius+0.1;setPosition(goal);
            }
        }
        Check(bounded&&furthest>radius-1,
              "a continuous crossfire episode reaches but never exceeds its Medium/Hard defensive destination radius");
        const auto nextAnchor=location();
        native_world::Put(native_world::nodes[1].data(),0x88,nextAnchor+native_world::Vec3{0,0,1000});
        native_world::Put(native_world::nodes[2].data(),0x88,nextAnchor+native_world::Vec3{10,0,1100});
        Sleep(1050);unit.Think();crossfire();unit.Think();
        const auto newGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(unit.bytes[0x30DC]&&battle_ai::Length(newGoal-nextAnchor)>25&&battle_ai::Length(newGoal-nextAnchor)<=radius+0.1,
              "a pressure-free interval expires the old anchor so a new crossfire episode can move again");
        native_world::Put(native_world::enemy1.data(),0x38,int32_t(0));setPosition(newGoal);
        const int beforePointRoute=native_world::pathCalls;unit.Think();
        Check(unit.bytes[0x30DC]&&native_world::pathCalls>beforePointRoute,
              "an explicit defensive destination remains navigable without a living threat object");
        crossfire();
        native_world::Put(native_world::nodes[0].data(),0x6C,native_world::Vec3{0,0,1});
        native_world::Put(unit.bytes,0x3090,native_world::Vec3{0,0,1});
        Eval("SDGO_BattleAI_GenericUpdate=function(n) local before=SDGO_BattleAI_Observe(); SDGO_BattleAI_RecoveryMode(1); SDGO_BattleAI_CancelRoute(); n:SetMoveTargetPos_Dir(3,350,3,false); n:SetMoveType(1); n:SetJumpTime(0); n:SetAttackOnOff(true); local after=SDGO_BattleAI_Observe(); assert(after.movementRecovery==1 and after.defenceMoving==0); return 123 end");
        auto recoveryGoal=location()+native_world::Vec3{177,0,0};native_world::Put(unit.bytes,0x3078,recoveryGoal);
        const int routesBeforeRecovery=native_world::pathCalls,turnsBeforeRecovery=native_world::rotations;
        Sleep(16);unit.Think();
        const auto retainedGoal=*reinterpret_cast<native_world::Vec3*>(unit.bytes+0x3078);
        Check(native_world::pathCalls==routesBeforeRecovery&&battle_ai::Length(retainedGoal-recoveryGoal)<0.01&&unit.bytes[0x30DC],
              "a wall-recovery motor lease prevents defence from restoring the blocked route in the same Think");
        Check(native_world::rotations>turnsBeforeRecovery,
              "wall recovery keeps bounded front-facing defence while it owns the movement goal");
        Eval("SDGO_BattleAI_GenericUpdate=function(n) SDGO_BattleAI_RecoveryMode(0); local o=SDGO_BattleAI_Observe(); assert(o.movementRecovery==0); return 123 end");
        unit.Think();
    }
    std::printf("NATIVE ADAPTER CHECKS %s\n",g_failures?"FAILED":"PASSED");
    // Mods live for this process; do not unload an installed trampoline.
    return g_failures?1:0;
}
