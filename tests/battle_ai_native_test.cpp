// Real core + real battle_ai DLL + official Lua 5.0 VM. Game entry points are
// private fixtures: no game, account, network socket, or existing process used.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"
extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}
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
    if(!ok) std::printf("LUA ERROR %s\n",lua_tostring(g_L,-1));
    lua_settop(g_L,top); return ok;
}
lua_State* __cdecl State(lua_State* value) { if(value) g_L=value; return g_L; }
struct Unit {
    unsigned char bytes[0x3300] = {};
    __declspec(noinline) void Think() {
        if(!bytes[0x326C]) return;
        int top=lua_gettop(g_L);
        lua_getglobal(g_L,"VSAI_Gunfighter_01_Update"); lua_getglobal(g_L,"npc");
        Check(lua_pcall(g_L,1,1,0)==0,"native Think can execute decorated callback");
        Check(lua_tonumber(g_L,-1)==123,"decorator preserves native return value");
        lua_settop(g_L,top);
    }
    __declspec(noinline) void Init(void* data) { bytes[0]=static_cast<unsigned char>(reinterpret_cast<uintptr_t>(data)); }
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
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr,exe,MAX_PATH); *wcsrchr(exe,L'\\')=0; g_root=exe;
    Symbols(bad);
    g_L=lua_open(); luaopen_base(g_L); lua_settop(g_L,0); luaopen_string(g_L); lua_settop(g_L,0);
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
    lua_pushnumber(g_L,987);
    unit.Init(reinterpret_cast<void*>(1)); unit.Think();
    Check(lua_gettop(g_L)==1 && lua_tonumber(g_L,1)==987,"adapter preserves incoming Lua stack");
    if(disabled||bad||missing) {
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
        Config("[settings]\nEnabled=0\nDecisionMs=invalid\n"); Sleep(2100); unit.Think();
        Check(Eval("assert(calls==5 and SDGO_BattleAI.faults==0)"),"separate config.ini reloads and invalid value is bounded");
        Config("[settings]\nEnabled=1\n"); Sleep(2100); unit.Think();
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
