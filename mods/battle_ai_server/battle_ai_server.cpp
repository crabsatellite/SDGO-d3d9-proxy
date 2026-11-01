// Fair player resource rules for reserved PvP bot participants. Unmarked room
// names use Medium, including participants added by a compatible spawner.
// Detours only
// select existing native checks/cost blocks; identity, locks, admission, debit,
// skill modifiers and notifications continue through the original server code.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "../battle_ai/difficulty.h"
#include "../battle_ai/native_loadout.h"
#include "sdgo/room_titles_lookup.h"

namespace {
const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
bool g_enabled=true;
void* g_gateOriginal=nullptr;void* g_gatePlayer=nullptr;
void* g_startOriginal=nullptr;void* g_startPlayer=nullptr;
void* g_meleeOriginal=nullptr;void* g_meleePlayer=nullptr;
void* g_rangedOriginal=nullptr;void* g_rangedPlayer=nullptr;
void* g_mapOriginal=nullptr;void* g_mapPlayer=nullptr;
void* g_rushOriginal=nullptr;void* g_rushPlayer=nullptr;
using BuildFn=int(__thiscall*)(uint8_t*,uint8_t*,uint8_t*,uint32_t);
BuildFn g_build=nullptr;
battle_ai::loadout::ServerNativeFunctions g_loadoutNative;
using BroadcastFn=int(__thiscall*)(uint8_t*,uint8_t*,uint32_t,uint32_t*);
BroadcastFn g_broadcast=nullptr;
struct ClearContext {uint8_t* pilot=nullptr;uint32_t roomIndex=0,handle=0;uint16_t opcode=0;ULONGLONG at=0;};
thread_local ClearContext g_clear;

int RoomDifficulty(const uint8_t* room) {
    if(g_api&&g_api->ModDirectory) {
        const auto api=sdgo::FindRoomTitleApi(g_api->ModDirectory(g_mod));char full[SDGO_ROOM_TITLE_MAX_UTF8_BYTES+1]={};
        const auto n=api?api->QueryForPilot(*reinterpret_cast<const uint32_t*>(room+0x4CE),full,sizeof(full)):0;
        if(n&&n<=SDGO_ROOM_TITLE_MAX_UTF8_BYTES&&full[n]==0) {
            int value=battle_ai::RoomDifficulty(std::string_view(full,n));return value<0?1:value;
        }
    }
    wchar_t title[21]={};auto length=room[0x79];
    if(length>20) return 1;
    for(unsigned i=0;i<length;++i) title[i]=room[0x7A+i];
    int chosen=battle_ai::RoomDifficulty(std::wstring_view(title,length));
    return chosen<0?1:chosen;
}
bool FairPilot(const uint8_t* room,const uint8_t* pilot,uint32_t handle) {
    if(!g_enabled||!room||!pilot||(handle&0x80000000u)==0) return false;
    uint8_t mode=room[5];
    if(mode==5||mode==8||mode==10||mode==11||mode==12) return false;
    auto first=reinterpret_cast<uintptr_t>(room+0x3CE4),position=reinterpret_cast<uintptr_t>(pilot);
    if(position<first||position>=first+12*0x1F54||(position-first)%0x1F54||*reinterpret_cast<const uint32_t*>(pilot)!=handle) return false;
    unsigned form=pilot[0x33C];if(form>=4) return false;
    auto npc=*reinterpret_cast<const uint32_t*>(pilot+0x340+form*0x658);
    return npc>=3604001&&npc<=3607000&&RoomDifficulty(room)>=1;
}
int __stdcall FairCostDecision(const uint8_t* frame,int site) {
    __try {
        size_t handleOffset=0,pilotOffset=0,roomOffset=0;
        switch(site) {
        case 0:case 1:handleOffset=0x328D8;pilotOffset=0x328DC;roomOffset=0x3C;break;
        case 2:handleOffset=0x32950;pilotOffset=0x32994;roomOffset=0x20;break;
        case 3:handleOffset=0x14F8;pilotOffset=0x1510;roomOffset=0x20;break;
        case 4:case 5:handleOffset=0x33CD0;pilotOffset=0x33CD4;roomOffset=0x18;break;
        default:return 0;
        }
        auto handle=*reinterpret_cast<const uint32_t*>(frame-handleOffset);
        auto pilot=*reinterpret_cast<uint8_t* const*>(frame-pilotOffset);
        auto room=*reinterpret_cast<uint8_t* const*>(frame-roomOffset);
        if(!FairPilot(room,pilot,handle)) return 0;
        // Clear packets have no native replay guard. For our NPCs charge only
        // an active accepted special; its native clear changes state back to 1.
        if(site>=2&&*reinterpret_cast<const uint32_t*>(pilot+9)!=3) return 0;
        if(site==4||site==5) g_clear={pilot,*reinterpret_cast<uint32_t*>(room),handle,static_cast<uint16_t>(site==4?0x554:0x65D),GetTickCount64()};
        return 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return 0;}
}
bool ServerLoadoutProfile(void* context,const void*,battle_ai::loadout::NativeProfile* output) {
    auto unit=static_cast<const uint8_t*>(context);if(!unit||!output) return false;
    battle_ai::loadout::WeaponProfile weapons[3]={};size_t count=0;
    for(size_t slot=0;slot<3;++slot) {
        auto weapon=*reinterpret_cast<uint8_t* const*>(unit+0x70+slot*0x94);
        if(!weapon) continue;
        uint8_t style=weapon[0x37],range=weapon[0x38];
        if(style==2) continue;
        if(style<1||style>3||range<1||range>8) return false;
        auto& facts=weapons[count++];facts.style=style;facts.rangeType=range;
        std::memcpy(&facts.damage,weapon+0x3A,4);std::memcpy(&facts.range,weapon+0x40,4);
        std::memcpy(&facts.magazine,weapon+0x44,2);facts.maxCombo=weapon[0x52];
    }
    *output=battle_ai::loadout::ProfileFromWeapons(weapons,count);return count!=0;
}
void ApplyBuiltLoadout(uint8_t* room,uint8_t* pilot,uint8_t* unit,uint32_t npc) {
    __try {
        if(!g_enabled||!room||!pilot||!unit||npc<3604001||npc>3607000) return;
        auto mode=room[5];if(mode==5||mode==8||mode==10||mode==11||mode==12) return;
        int difficulty=RoomDifficulty(room);
        auto definition=*reinterpret_cast<uint8_t**>(unit+0x6C);
        battle_ai::loadout::NativeProfileSource source{unit,ServerLoadoutProfile};
        battle_ai::loadout::NativeLoadout plan;
        if(!battle_ai::loadout::FromNativeSource(static_cast<battle_ai::Difficulty>(difficulty),source,definition,&plan)) return;
        battle_ai::loadout::ServerApplyState state;
        auto result=battle_ai::loadout::ApplyServer(unit,1,plan,g_loadoutNative,&state);
        if(result!=battle_ai::loadout::ApplyResult::Applied)
            g_api->Log(g_mod,SDGO_LOG_WARN,"native AI level/part unavailable (%d); original unit retained",static_cast<int>(result));
    } __except(EXCEPTION_EXECUTE_HANDLER) {g_api->Log(g_mod,SDGO_LOG_WARN,"native AI loadout layout unavailable; original unit retained");}
}
int __fastcall BuildPilot(uint8_t* room,void*,uint8_t* pilot,uint8_t* unit,uint32_t npc) {
    int result=g_build(room,pilot,unit,npc);
    if(result) ApplyBuiltLoadout(room,pilot,unit,npc);
    return result;
}
bool MakeClearSpState(uint8_t* room,const uint8_t* packet,uint32_t length,uint8_t* output) {
    __try {
        if(!g_clear.pilot||!room||!packet||length!=10||GetTickCount64()-g_clear.at>1000||
           *reinterpret_cast<uint32_t*>(room)!=g_clear.roomIndex||*reinterpret_cast<const uint16_t*>(packet)!=g_clear.opcode||
           *reinterpret_cast<const uint32_t*>(packet+6)!=g_clear.handle||*reinterpret_cast<uint32_t*>(g_clear.pilot)!=g_clear.handle) return false;
        int32_t sp=*reinterpret_cast<int32_t*>(g_clear.pilot+5);if(sp<0||sp>30000) return false;
        unsigned form=g_clear.pilot[0x33C];if(form>=4) return false;
        auto unit=g_clear.pilot+0x340+form*0x658;auto weapon=*reinterpret_cast<uint8_t**>(unit+0x40);
        if(!weapon) weapon=*reinterpret_cast<uint8_t**>(unit+0x70);
        if(!weapon||!*reinterpret_cast<uint32_t*>(weapon)) return false;
        // AgentServer forwards this existing notification at its native fixed
        // 0xF0 size. Native classification is 0..9; FF marks a pure state
        // update with zero victims and no effect object, not an attack/hit.
        const uint16_t opcode=0x779;std::memcpy(output,&opcode,2);
        std::memcpy(output+6,&g_clear.handle,4);output[0xA]=0xFF;
        std::memcpy(output+0xB,&sp,4);std::memcpy(output+0xF,weapon,4);
        g_clear={};return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {g_clear={};return false;}
}
int __fastcall Broadcast(uint8_t* room,void*,uint8_t* packet,uint32_t length,uint32_t* sent) {
    uint8_t state[0xF0]={};bool synchronize=MakeClearSpState(room,packet,length,state);
    int result=g_broadcast(room,packet,length,sent);
    if(synchronize) {uint32_t ignored=0;g_broadcast(room,state,sizeof(state),&ignored);}
    return result;
}
}

// Preserve every original register/flag before asking a source-bound policy.
// The selected native block itself recalculates flags and retains all reject
// branches, including low SP, invulnerability and invalid weapon/target cases.
__declspec(naked) static void GateDetour() {
    __asm {
        pushad
        pushfd
        push 0
        push ebp
        call FairCostDecision
        test eax,eax
        jnz fair
        popfd
        popad
        jmp dword ptr [g_gateOriginal]
    fair:
        popfd
        popad
        jmp dword ptr [g_gatePlayer]
    }
}
__declspec(naked) static void StartDetour() {
    __asm {
        pushad
        pushfd
        push 1
        push ebp
        call FairCostDecision
        test eax,eax
        jnz fair
        popfd
        popad
        jmp dword ptr [g_startOriginal]
    fair:
        popfd
        popad
        jmp dword ptr [g_startPlayer]
    }
}
__declspec(naked) static void MeleeDetour() {
    __asm {
        pushad
        pushfd
        push 2
        push ebp
        call FairCostDecision
        test eax,eax
        jnz fair
        popfd
        popad
        jmp dword ptr [g_meleeOriginal]
    fair:
        popfd
        popad
        jmp dword ptr [g_meleePlayer]
    }
}
__declspec(naked) static void RangedDetour() {
    __asm {
        pushad
        pushfd
        push 3
        push ebp
        call FairCostDecision
        test eax,eax
        jnz fair
        popfd
        popad
        jmp dword ptr [g_rangedOriginal]
    fair:
        popfd
        popad
        jmp dword ptr [g_rangedPlayer]
    }
}
__declspec(naked) static void MapDetour() {
    __asm {
        pushad
        pushfd
        push 4
        push ebp
        call FairCostDecision
        test eax,eax
        jnz fair
        popfd
        popad
        jmp dword ptr [g_mapOriginal]
    fair:
        popfd
        popad
        jmp dword ptr [g_mapPlayer]
    }
}
__declspec(naked) static void RushDetour() {
    __asm {
        pushad
        pushfd
        push 5
        push ebp
        call FairCostDecision
        test eax,eax
        jnz fair
        popfd
        popad
        jmp dword ptr [g_rushOriginal]
    fair:
        popfd
        popad
        jmp dword ptr [g_rushPlayer]
    }
}
SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;g_enabled=api->SettingInt(mod,"Enabled",1)!=0;
    g_gatePlayer=api->Symbol(mod,"ai_sp.request_player_gate");
    g_startPlayer=api->Symbol(mod,"ai_sp.request_player_cost");
    g_meleePlayer=api->Symbol(mod,"ai_sp.melee_player_cost");
    g_rangedPlayer=api->Symbol(mod,"ai_sp.ranged_player_cost");
    g_mapPlayer=api->Symbol(mod,"ai_sp.map_player_cost");
    g_rushPlayer=api->Symbol(mod,"ai_sp.rush_player_cost");
    g_loadoutNative.skillRegistry=api->Symbol(mod,"loadout.skill_registry");
    g_loadoutNative.findSkill=reinterpret_cast<battle_ai::loadout::FindServerSkillFn>(api->Symbol(mod,"loadout.find_skill"));
    if(!g_gatePlayer||!g_startPlayer||!g_meleePlayer||!g_rangedPlayer||!g_mapPlayer||!g_rushPlayer) return 0;
    if(!g_loadoutNative.skillRegistry||!g_loadoutNative.findSkill) return 0;
    if(!api->HookCode(mod,"ai_sp.request_gate",reinterpret_cast<void*>(&GateDetour),&g_gateOriginal)||
       !api->HookCode(mod,"ai_sp.request_cost",reinterpret_cast<void*>(&StartDetour),&g_startOriginal)||
       !api->HookCode(mod,"ai_sp.melee_cost",reinterpret_cast<void*>(&MeleeDetour),&g_meleeOriginal)||
       !api->HookCode(mod,"ai_sp.ranged_cost",reinterpret_cast<void*>(&RangedDetour),&g_rangedOriginal)||
       !api->HookCode(mod,"ai_sp.map_cost",reinterpret_cast<void*>(&MapDetour),&g_mapOriginal)||
       !api->HookCode(mod,"ai_sp.rush_cost",reinterpret_cast<void*>(&RushDetour),&g_rushOriginal)||
       !api->HookCode(mod,"ai_sp.broadcast",reinterpret_cast<void*>(&Broadcast),reinterpret_cast<void**>(&g_broadcast))||
       !api->HookCode(mod,"battle.build_ai_pilot",reinterpret_cast<void*>(&BuildPilot),reinterpret_cast<void**>(&g_build))) return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: Medium/Hard PvP bots use native player SP checks and costs");
    return 1;
}
