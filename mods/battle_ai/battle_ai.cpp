// Installs Lua decorators before native CNPC::Think. Native movement, weapons,
// damage and networking remain the executor. Never reads another process.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <intrin.h>
#include "sdgo/sdgo_api.h"
#include "combat_math.h"
#include "native_capabilities.h"
#include "native_navigation.h"
#include "difficulty.h"

namespace {
struct lua_State;
using LuaFn = int(__cdecl*)(lua_State*);
using Reader = const char*(__cdecl*)(lua_State*, void*, size_t*);
struct Lua {
    lua_State*(__cdecl* state)(lua_State*);
    int(__cdecl* gettop)(lua_State*);
    int(__cdecl* type)(lua_State*,int);
    double(__cdecl* tonumber)(lua_State*,int);
    void(__cdecl* settop)(lua_State*, int);
    const char*(__cdecl* tostring)(lua_State*, int);
    void(__cdecl* pushnumber)(lua_State*, double);
    void(__cdecl* pushstring)(lua_State*, const char*);
    void(__cdecl* pushcclosure)(lua_State*, LuaFn, int);
    void(__cdecl* gettable)(lua_State*, int);
    void(__cdecl* settable)(lua_State*, int);
    void(__cdecl* newtable)(lua_State*);
    int(__cdecl* pcall)(lua_State*, int, int, int);
    int(__cdecl* load)(lua_State*, Reader, void*, const char*);
} g_lua = {};
using ThinkFn = bool(__thiscall*)(uint8_t*);
using InitFn = void(__thiscall*)(uint8_t*, void*);
ThinkFn g_think = nullptr;
InitFn g_init = nullptr;
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
constexpr int kGlobals = -10001;
thread_local uint8_t* g_current = nullptr;
std::unordered_map<uint8_t*, uint64_t> g_generations;
uint64_t g_serial = 0;
lua_State* g_loaded = nullptr;
std::wstring g_script, g_config;
std::wstring g_universalScript;
ULONGLONG g_checkAt = 0;
FILETIME g_scriptTime = {}, g_configTime = {};
FILETIME g_universalTime = {};
bool g_loading = false;
bool g_preparationFault = false;

using battle_ai::Vec3;
using battle_ai::native::Ray3;
using DispatchFn=int(__cdecl*)(const char*,uint8_t*);
using RayFn=Ray3*(__thiscall*)(uint8_t*,Ray3*);
using GoalFn=int(__thiscall*)(uint8_t*,const Vec3*,int);
using IntentFn=void(__thiscall*)(uint8_t*);
using SetIntentFn=int(__thiscall*)(uint8_t*,char);
using ShotFn=uint32_t(__thiscall*)(uint8_t*,uint32_t,char,char);
using ProjectileFn=bool(__thiscall*)(uint8_t*,void*);
using StateEventFn=void(__thiscall*)(uint8_t*,void*);
using RotateFn=void(__thiscall*)(uint8_t*,float,int);
using MessageFn=bool(__thiscall*)(uint8_t*,uint32_t,void*,uint32_t);
using RouteFn=Vec3*(__thiscall*)(uint8_t*,uint32_t,Vec3,Vec3,Vec3*,uint32_t*);
using DeleteFn=void(__cdecl*)(void*);
DispatchFn g_dispatch=nullptr;
RayFn g_ray=nullptr;
IntentFn g_intent=nullptr;
IntentFn g_stop=nullptr;
SetIntentFn g_setIntent=nullptr;
ShotFn g_shot=nullptr;
ProjectileFn g_projectile=nullptr;
StateEventFn g_trackingEntry=nullptr,g_trackingUpdate=nullptr,g_swingEntry=nullptr,g_fireReceipts=nullptr;
StateEventFn g_swingUpdate=nullptr;
MessageFn g_message=nullptr;
void* g_contactReturn=nullptr;
IntentFn g_queueSwing=nullptr,g_faceTarget=nullptr;
IntentFn g_attackAdmission=nullptr;
RotateFn g_rotate=nullptr;
thread_local uint8_t* g_receiptFsm=nullptr;
using AppFn=uint8_t*(__cdecl*)();
using SkillGateFn=bool(__thiscall*)(uint8_t*);
AppFn g_app=nullptr;
SkillGateFn g_playerSkillGate=nullptr;
int g_appliedRoomDifficulty=-1;
void* g_plainProjectileEntry=nullptr;
GoalFn g_setGoal=nullptr;
RouteFn g_route=nullptr;
DeleteFn g_delete=nullptr;
uint8_t** g_document=nullptr;
battle_ai::native::NavigationApi g_navigation={};
struct RuntimeSettings { bool generic=false,enabled=true,aim=true,lead=true; int maxLeadMs=3000,difficulty=1,quickSwing=80; } g_runtime;
struct Actor {
    uint64_t generation=0;
    ULONGLONG seen=0,aimAt=0,nextPulse=0;
    uint32_t aimTarget=0;
    int intervalMs=120;
    bool ownsIntent=false,ownsAim=false,controlled=false;
    int savedWait=0;
    float savedYawError=0,savedPitchError=0;
    std::vector<Vec3> path;
    size_t waypoint=0;
    int lastHp=0;
    ULONGLONG hurtAt=0;
    uint32_t rng=0;
    bool pathRanged=false,quickSwing=false,sideCue=false,suppressMelee=false;
    ULONGLONG startupAt=0;
    uint32_t startupTarget=0;
    float cueAngle=0;
    int specialSlotLease=-1;
};
std::unordered_map<uint8_t*,Actor> g_actors;
void ClearStartup(uint8_t*,Actor&,bool);
struct Motion { uint8_t* unit=nullptr; Vec3 point,velocity; ULONGLONG at=0; };
std::unordered_map<uint32_t,Motion> g_motion;
struct RangedActivity {ULONGLONG time=0,primaryAt=0;uint32_t target=0,weapon=0,primaryTarget=0;uint64_t calls=0,shots=0;};
std::unordered_map<uint8_t*,RangedActivity> g_rangedShots;
struct Contact {uint32_t target=0,action=0;int combo=0;ULONGLONG time=0;uint8_t* victim=nullptr;};
std::unordered_map<uint8_t*,Contact> g_contacts;
struct LearnedSpeed { float perUpdate=0;int modifier=0;ULONGLONG seen=0;bool nonlinear=false; };
struct FlightRun { uint64_t key=0;float speed=0;int stable=0;ULONGLONG seen=0;Vec3 end; };
std::unordered_map<uint64_t,LearnedSpeed> g_speeds;
std::unordered_map<uint8_t*,FlightRun> g_flights;
thread_local const char* g_nativeCallback=nullptr;
thread_local bool g_nativeFallback=false;
std::atomic<DWORD> g_gameThread{0};
std::mutex g_pendingMutex;
std::vector<uint8_t*> g_pendingInit;
int g_observationFaults=0;

uint64_t SpeedKey(uint32_t unitId,uint32_t weaponId,bool alternate) {
    return (uint64_t(unitId)<<32)|(uint64_t(weaponId)<<1)|(alternate?1:0);
}
int SpeedModifier(uint8_t* unit,uint8_t* weapon) {
    uint8_t range=weapon[0x6A];
    if(range==2) return *reinterpret_cast<int*>(unit+0xCEC);
    if(range==3) return *reinterpret_cast<int*>(unit+0xCF0);
    return 0;
}
float MeasuredSpeed(uint8_t* unit,const battle_ai::native::Capabilities& caps,uint32_t weaponId) {
    auto found=g_speeds.find(SpeedKey(caps.unitId,weaponId,caps.alternate));
    if(found==g_speeds.end()||found->second.nonlinear||GetTickCount64()-found->second.seen>30000) return 0;
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
    if(!record || *reinterpret_cast<uint32_t*>(record)!=weaponId ||
       found->second.modifier!=SpeedModifier(unit,record)) return 0;
    return found->second.perUpdate*60.0f;
}
float PredictionSpeed(uint8_t* unit,const battle_ai::native::Capabilities& caps,const battle_ai::native::Weapon& w) {
    auto observed=g_speeds.find(SpeedKey(caps.unitId,w.id,caps.alternate));
    if(observed!=g_speeds.end() && observed->second.nonlinear) return 0;
    return w.projectileSpeed>0?w.projectileSpeed:MeasuredSpeed(unit,caps,w.id);
}

Actor& ActorFor(uint8_t* unit) {
    auto& a=g_actors[unit];
    auto gen=g_generations.find(unit);
    uint64_t generation=gen==g_generations.end()?0:gen->second;
    if(a.generation!=generation) { a=Actor{}; a.generation=generation; }
    a.seen=GetTickCount64();
    if(g_actors.size()>1024) {
        for(auto it=g_actors.begin();it!=g_actors.end();) {
            if(it->first!=unit && a.seen-it->second.seen>10000) it=g_actors.erase(it); else ++it;
        }
    }
    return a;
}
bool Owned(uint8_t* unit) {
    __try {
        return unit && unit[0x2B65]==0 && unit[0x326C]!=0 &&
            *reinterpret_cast<uint32_t*>(unit+0x3238)>=3604001 &&
            *reinterpret_cast<uint32_t*>(unit+0x3238)<=3607000;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Vec3 Position(uint8_t* unit) { return *reinterpret_cast<Vec3*>(*reinterpret_cast<uint8_t**>(unit+8)+0x88); }
Vec3 AimPoint(uint8_t* unit) {
    Vec3 position=Position(unit);
    auto scene=*reinterpret_cast<uint8_t**>(unit+0x34);
    if(scene) {
        auto bounds=reinterpret_cast<float*>(scene+0xBC);
        Vec3 low={bounds[0],bounds[1],bounds[2]},high={bounds[3],bounds[4],bounds[5]};
        Vec3 center=(low+high)*0.5f;
        if(battle_ai::Finite(center) && low.x<=high.x && low.y<=high.y && low.z<=high.z &&
           battle_ai::Length(center-position)<10000) return center;
    }
    return position;
}
Vec3 Velocity(uint8_t* unit,uint32_t id,Vec3 point,ULONGLONG now) {
    auto& m=g_motion[id];
    if(m.unit!=unit || !m.at || now-m.at>1000) { m={unit,point,{},now}; return {}; }
    if(now-m.at>=20) {
        Vec3 v=(point-m.point)*(1000.0f/static_cast<float>(now-m.at));
        m.velocity=battle_ai::Finite(v) && battle_ai::Length(v)<50000 ? v : Vec3{};
        m.point=point;m.at=now;
    }
    return m.velocity;
}
uint8_t* FindObject(uint32_t id) {
    if(!g_document || !*g_document) return nullptr;
    // The engine's object lookup includes players, NPCs and AI participant list.
    using Fn=uint8_t*(__thiscall*)(uint8_t*,uint32_t);
    auto fn=reinterpret_cast<Fn>(g_api->Symbol(g_mod,"um.find_object"));
    return fn?fn(*g_document,id):nullptr;
}
bool ControlBlocked(uint8_t* unit) {
    uint8_t* movement=nullptr;
    if(!battle_ai::native::detail::ReadPointer(unit,0x9C8,movement)) return true;
    for(size_t offset:{size_t(0xD4),size_t(0xE8),size_t(0xFC),size_t(0x110),size_t(0x138)})
        if(battle_ai::native::detail::InState(movement,offset)) return true;
    return false;
}
bool HurtState(uint8_t* unit) {
    uint8_t* movement=nullptr;
    if(!battle_ai::native::detail::ReadPointer(unit,0x9C8,movement)) return false;
    return battle_ai::native::detail::InState(movement,0xE8)||battle_ai::native::detail::InState(movement,0xFC)||battle_ai::native::detail::InState(movement,0x110);
}
bool FairSpecialResource(uint8_t* fsm,uint8_t* owner) {
    __try {
        uint8_t* controller=nullptr;int restriction=1;
        return owner&&*reinterpret_cast<int*>(owner+0xBC8)>=30000&&owner[0x31C4]!=1&&
            battle_ai::native::detail::ReadPointer(fsm,0xB30,controller)&&
            battle_ai::native::detail::Read(controller,0x2C0,restriction)&&restriction==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void RestoreIntent(uint8_t* unit,Actor& a) {
    __try {
        ClearStartup(unit,a,!ControlBlocked(unit));
        if(a.controlled) { g_setIntent(unit,0);g_stop(unit); }
        if(a.ownsIntent) *reinterpret_cast<int*>(unit+0x31F4)=a.savedWait;
        if(a.ownsAim) {
            *reinterpret_cast<float*>(unit+0x3204)=a.savedYawError;
            *reinterpret_cast<float*>(unit+0x3208)=a.savedPitchError;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    a.controlled=false;a.ownsIntent=false;a.ownsAim=false;a.aimTarget=0;a.suppressMelee=false;a.path.clear();
}
void FollowPath(uint8_t* unit,Actor& a) {
    if(a.path.empty()) return;
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    bool legalRanged=false;
    if(fsm && a.pathRanged && *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x34 &&
       (!*reinterpret_cast<uint8_t**>(fsm+8)||*reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x34)) {
        auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
        legalRanged=record&&(record[0x8F]==1||fsm[0xB38]==1);
    }
    if(!fsm || ControlBlocked(unit) || (!battle_ai::native::detail::Neutral(fsm)&&!legalRanged)) {
        // Native attacks/hurt/guard own the motor now; drop our route without
        // cancelling the movement belonging to their active state.
        a.path.clear();return;
    }
    Vec3 here=Position(unit);
    while(a.waypoint<a.path.size() && battle_ai::Length(a.path[a.waypoint]-here)<(a.pathRanged?25:100)) ++a.waypoint;
    if(a.waypoint>=a.path.size()) { a.path.clear(); return; }
    Vec3 current=*reinterpret_cast<Vec3*>(unit+0x3078);
    if(battle_ai::Length(current-a.path[a.waypoint])>1) g_setGoal(unit,&a.path[a.waypoint],1);
}
struct TargetObservation {
    uint32_t id=0;
    int hp=0,maxHp=0;
    float distance=0;
    bool visible=false,enemy=false,attackable=false,rangedFiring=false,meleeTracking=false,meleeSwinging=false,hurt=false;
    uint32_t selectedTarget=0;
    Vec3 point,ground,velocity,forward;
};
struct AllyObservation {uint32_t id=0,target=0;Vec3 pos;bool meleeTracking=false,meleeSwinging=false;RangedActivity ranged;Contact contact;};
struct Observation {
    battle_ai::native::Capabilities caps={};
    uint32_t id=0;
    uint64_t generation=0;
    int hp=0;
    Vec3 pos,goal;
    bool moving=false,hurt=false,hurtRecently=false,rangedBusy=false,meleeTracking=false,meleeSwinging=false,comboKnown=false,chainWindow=false,meleePressure=false;
    int comboIndex=0,comboMax=0;
    AllyObservation allies[64];size_t allyCount=0;
    TargetObservation targets[128];
    size_t count=0;
};
bool CaptureUnsafe(Observation& o) {
    if(!Owned(g_current)||!g_document||!*g_document||GetCurrentThreadId()!=g_gameThread) return false;
    if(!battle_ai::native::ReadCapabilities(g_current,o.caps)) return false;
    bool hasWeapon=false;
    for(const auto& w:o.caps.weapons) if(w.valid) hasWeapon=true;
    if(!hasWeapon) return false;
    uint8_t* unit=g_current;
    auto& actor=ActorFor(unit);
    ULONGLONG now=GetTickCount64();
    o.id=*reinterpret_cast<uint32_t*>(unit+4);o.generation=actor.generation;
    o.hp=*reinterpret_cast<int*>(unit+0x38);o.pos=Position(unit);
    o.goal=*reinterpret_cast<Vec3*>(unit+0x3078);
    o.moving=unit[0x30DC]!=0;
    // CheckHurt consumes a latch. Observe actual/queued MovementFSM control
    // states instead; do not steal the original AI's damage notification.
    o.hurt=ControlBlocked(unit);
    if(actor.lastHp>0 && o.hp<actor.lastHp) actor.hurtAt=now;
    actor.lastHp=o.hp;o.hurtRecently=actor.hurtAt && now-actor.hurtAt<1600;
    float perception=1500;
    for(const auto& w:o.caps.weapons) if(w.valid) perception=(std::max)(perception,w.range*1.5f);
    perception=(std::min)(perception,24000.0f);
    int ownTeam=*reinterpret_cast<int*>(unit+0x20);
    using CanAttack=bool(__thiscall*)(uint8_t*,uint8_t*);
    Ray3 ray={};
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    if(!fsm) return false;
    o.rangedBusy=battle_ai::native::detail::InState(fsm,0x34);
    o.meleeTracking=battle_ai::native::detail::InState(fsm,0x48);o.meleeSwinging=battle_ai::native::detail::InState(fsm,0x5C);
    o.comboIndex=*reinterpret_cast<int*>(fsm+0x968);o.comboMax=*reinterpret_cast<int*>(fsm+0x96C);
    auto selectedRecord=*reinterpret_cast<uint8_t**>(fsm+0x248);
    o.comboKnown=selectedRecord&&o.comboMax>=1&&o.comboMax<=32&&o.comboIndex>=0&&o.comboIndex<o.comboMax&&selectedRecord[0x264]==o.comboMax;
    o.chainWindow=fsm[0x218]!=0;
    Ray3* nativeRay=g_ray(fsm,&ray);
    if(!nativeRay || !battle_ai::Finite(nativeRay->origin)) return false;
    if(nativeRay!=&ray) ray=*nativeRay;
    for(size_t offset:{size_t(0x48),size_t(0x70),size_t(0x7C)}) {
        auto head=*reinterpret_cast<uint8_t***>(*g_document+offset);
        if(!head) continue;
        auto node=*reinterpret_cast<uint8_t***>(head);
        unsigned traversed=0;
        while(node!=head && node && traversed++<4096 && o.count<128) {
            auto candidate=reinterpret_cast<uint8_t*>(node[2]);
            node=*reinterpret_cast<uint8_t***>(node);
            if(!candidate||candidate==unit) continue;
            uint32_t id=*reinterpret_cast<uint32_t*>(candidate+4);
            bool duplicate=false;
            for(size_t i=0;i<o.count;++i) if(o.targets[i].id==id) duplicate=true;
            for(size_t i=0;i<o.allyCount;++i) if(o.allies[i].id==id) duplicate=true;
            if(!id||duplicate) continue;
            int team=*reinterpret_cast<int*>(candidate+0x20);
            int hp=*reinterpret_cast<int*>(candidate+0x38);
            if(hp<=0) continue;
            auto candidateFsm=*reinterpret_cast<uint8_t**>(candidate+0x9CC);
            if(team==ownTeam && ownTeam!=0 && ownTeam!=5) {
                if(o.allyCount<64) {
                    auto& buddy=o.allies[o.allyCount++];buddy.id=id;buddy.pos=Position(candidate);
                    buddy.target=candidateFsm?*reinterpret_cast<uint32_t*>(candidateFsm+0x200):0;
                    buddy.meleeTracking=battle_ai::native::detail::InState(candidateFsm,0x48);
                    buddy.meleeSwinging=battle_ai::native::detail::InState(candidateFsm,0x5C);
                    auto shot=g_rangedShots.find(candidate);if(shot!=g_rangedShots.end()) buddy.ranged=shot->second;
                    auto contact=g_contacts.find(candidate);if(contact!=g_contacts.end()) buddy.contact=contact->second;
                }
                continue;
            }
            Vec3 point=AimPoint(candidate);
            float distance=static_cast<float>(battle_ai::Length(Position(candidate)-o.pos));
            if(!battle_ai::Finite(point)||!std::isfinite(distance)||distance>perception) continue;
            auto vtable=*reinterpret_cast<void***>(candidate);
            if(!vtable||!vtable[10]||!reinterpret_cast<CanAttack>(vtable[10])(candidate,unit)) continue;
            auto& t=o.targets[o.count++];
            t.id=id;t.hp=hp;t.maxHp=*reinterpret_cast<int*>(candidate+0x3C);
            t.distance=distance;t.enemy=true;t.attackable=true;t.point=point;t.ground=Position(candidate);
            t.hurt=HurtState(candidate);
            t.forward=*reinterpret_cast<Vec3*>(*reinterpret_cast<uint8_t**>(candidate+8)+0x6C);
            t.selectedTarget=candidateFsm?*reinterpret_cast<uint32_t*>(candidateFsm+0x200):0;
            t.meleeTracking=battle_ai::native::detail::InState(candidateFsm,0x48);t.meleeSwinging=battle_ai::native::detail::InState(candidateFsm,0x5C);
            if((t.meleeTracking||t.meleeSwinging)&&t.selectedTarget==o.id) {
                battle_ai::native::Capabilities attacking={};
                if(battle_ai::native::ReadCapabilities(candidate,attacking))
                    for(const auto& w:attacking.weapons) if(w.valid&&w.slot==attacking.selectedSlot&&w.kind==battle_ai::native::WeaponKind::Melee&&distance<=w.range+150) o.meleePressure=true;
            }
            t.velocity=Velocity(candidate,id,point,now);
            t.visible=battle_ai::native::FreshLineClear(g_navigation,*g_document,unit,id,ray.origin,point);
            // A target's entire loadout/boost does not need decoding just to
            // notice gun input. The event hook below also retains brief shots.
            uint8_t* enemyFsm=nullptr;uint8_t* enemyRecord=nullptr;uint8_t rangeType=0;
            if(battle_ai::native::detail::ReadPointer(candidate,0x9CC,enemyFsm) &&
               battle_ai::native::detail::ReadPointer(enemyFsm,0x248,enemyRecord) &&
               battle_ai::native::detail::Read(enemyRecord,0x6A,rangeType) && rangeType>=2 && rangeType<=7)
                t.rangedFiring=candidate[0xDCD]!=0 || candidate[0xDCE]!=0;
            auto recent=g_rangedShots.find(candidate);
            if(recent!=g_rangedShots.end() && now-recent->second.time<1600) t.rangedFiring=true;
        }
        if(traversed>=4096) return false;
    }
    return battle_ai::Finite(o.pos)&&battle_ai::Finite(o.goal);
}
bool Capture(Observation& o) {
    __try { return CaptureUnsafe(o); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Number(lua_State* L,const char* key,double value) {
    g_lua.pushstring(L,key);g_lua.pushnumber(L,value);g_lua.settable(L,-3);
}
void String(lua_State* L,const char* key,const char* value) {
    g_lua.pushstring(L,key);g_lua.pushstring(L,value);g_lua.settable(L,-3);
}
void Vector(lua_State* L,const char* key,Vec3 v) {
    g_lua.pushstring(L,key);g_lua.newtable(L);
    Number(L,"x",v.x);Number(L,"y",v.y);Number(L,"z",v.z);g_lua.settable(L,-3);
}
int __cdecl Observe(lua_State* L) {
    Observation o;
    bool valid=Capture(o);
    g_lua.newtable(L);Number(L,"valid",valid?1:0);
    if(!valid) {
        if(g_observationFaults++<3) g_api->Log(g_mod,SDGO_LOG_WARN,"native capability observation unavailable; using stock AI");
        return 1;
    }
    if(g_runtime.generic) ActorFor(g_current).controlled=true;
    Number(L,"id",o.id);Number(L,"generation",static_cast<double>(o.generation));
    Number(L,"hp",o.hp);Number(L,"unitId",o.caps.unitId);Number(L,"alternate",o.caps.alternate);Number(L,"selectedSlot",o.caps.selectedSlot);
    Number(L,"moving",o.moving);Number(L,"hurtRecently",o.hurtRecently);
    Number(L,"hurt",o.hurt);
    Number(L,"boostFraction",o.caps.boostFraction);
    // Lua SetJumpEnable writes +3291. This is permission for native automatic
    // jump requests, not a promise that every form can fly; the motor decides.
    Number(L,"canJump",g_current[0x3291]!=0);
    Number(L,"meleeBusy",o.caps.meleeBusy);Number(L,"specialBusy",o.caps.specialBusy);
    Number(L,"rangedBusy",o.rangedBusy);
    auto attackFsm=*reinterpret_cast<uint8_t**>(g_current+0x9CC);
    Number(L,"moveCancelOpen",attackFsm&&attackFsm[0xB38]==1);
    Number(L,"nativeTarget",attackFsm?*reinterpret_cast<uint32_t*>(attackFsm+0x200):0);
    auto selectedRecord=attackFsm?*reinterpret_cast<uint8_t**>(attackFsm+0x248):nullptr;
    Number(L,"specialSelected",o.caps.selectedSlot==3&&selectedRecord&&selectedRecord[0x69]==2);
    Number(L,"specialRange",o.caps.weapons[3].range);
    uint8_t* specialRecord=nullptr;
    bool specialKnown=attackFsm&&battle_ai::native::detail::ReadPointer(attackFsm,(o.caps.alternate?0x56C:0x24C)+3*0xC8+0xC,specialRecord)&&
        battle_ai::native::detail::Readable(specialRecord,0x6A)&&specialRecord[0x69]==2;
    Number(L,"specialAvailable",specialKnown&&o.caps.weapons[3].id!=0&&o.caps.weapons[3].range>0);
    uint8_t* movement=nullptr;int restriction=1;
    bool controllerKnown=attackFsm&&battle_ai::native::detail::ReadPointer(attackFsm,0xB30,movement)&&
        battle_ai::native::detail::Read(movement,0x2C0,restriction);
    // NPC IsA(9) can take the player's unconditional non-player branch too.
    // Enforce the actual player resource/restriction fields before that call.
    Number(L,"spReady",FairSpecialResource(attackFsm,g_current)&&g_playerSkillGate(attackFsm));
    bool specialControl=controllerKnown;
    if(specialControl) for(size_t offset:{size_t(0xD4),size_t(0xFC),size_t(0x110),size_t(0x160),size_t(0x174),size_t(0x188)})
        if(battle_ai::native::detail::InState(movement,offset)) specialControl=false;
    Number(L,"specialControlAllowed",specialControl);
    auto shot=g_rangedShots.find(g_current);
    Number(L,"shots",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.shots));
    Number(L,"shotAt",shot==g_rangedShots.end()?0:static_cast<double>(shot->second.primaryAt));
    auto contact=g_contacts.find(g_current);
    Number(L,"contactTarget",contact==g_contacts.end()?0:contact->second.target);
    Number(L,"contactAt",contact==g_contacts.end()?0:static_cast<double>(contact->second.time));
    Number(L,"meleeTracking",o.meleeTracking);Number(L,"meleeSwinging",o.meleeSwinging);Number(L,"comboKnown",o.comboKnown);
    Number(L,"comboIndex",o.comboIndex);Number(L,"comboMax",o.comboMax);Number(L,"chainWindow",o.chainWindow);Number(L,"meleePressure",o.meleePressure);
    Number(L,"actionHandle",attackFsm?*reinterpret_cast<uint32_t*>(attackFsm+0x224):0);
    Number(L,"contactCombo",contact==g_contacts.end()?-1:contact->second.combo);
    Number(L,"contactAction",contact==g_contacts.end()?0:contact->second.action);
    Vector(L,"pos",o.pos);Vector(L,"goal",o.goal);
    g_lua.pushstring(L,"weapons");g_lua.newtable(L);
    int index=0;
    for(const auto& w:o.caps.weapons) {
        if(!w.valid) continue;
        g_lua.pushnumber(L,++index);g_lua.newtable(L);
        Number(L,"slot",w.slot);Number(L,"valid",1);Number(L,"range",w.range);Number(L,"damage",w.damage);
        Number(L,"cycleMs",w.cycleMs);Number(L,"ammoReady",w.ammoReady);Number(L,"ready",w.ready);
        float speed=PredictionSpeed(g_current,o.caps,w);
        Number(L,"movingShot",w.movingShot);Number(L,"projectileSpeed",speed);Number(L,"id",w.id);
        Number(L,"active",o.rangedBusy && w.slot==o.caps.selectedSlot);
        const char* kind="other";
        switch(w.kind) {
        case battle_ai::native::WeaponKind::Melee:kind="melee";break;
        case battle_ai::native::WeaponKind::Hitscan:kind="hitscan";break;
        case battle_ai::native::WeaponKind::Projectile:kind="projectile";break;
        case battle_ai::native::WeaponKind::Missile:kind="missile";break;
        default:break;
        }
        String(L,"kind",kind);g_lua.settable(L,-3);
    }
    g_lua.settable(L,-3);
    g_lua.pushstring(L,"targets");g_lua.newtable(L);
    for(size_t i=0;i<o.count;++i) {
        const auto& t=o.targets[i];g_lua.pushnumber(L,static_cast<double>(i+1));g_lua.newtable(L);
        Number(L,"id",t.id);Number(L,"hp",t.hp);Number(L,"maxHp",t.maxHp);Number(L,"distance",t.distance);
        Number(L,"enemy",t.enemy);Number(L,"visible",t.visible);Number(L,"attackable",t.attackable);
        Number(L,"rangedFiring",t.rangedFiring);Vector(L,"pos",t.point);Vector(L,"groundPos",t.ground);Vector(L,"velocity",t.velocity);
        Number(L,"hurt",t.hurt);
        Vector(L,"forward",t.forward);Number(L,"selectedTarget",t.selectedTarget);Number(L,"meleeTracking",t.meleeTracking);Number(L,"meleeSwinging",t.meleeSwinging);
        g_lua.settable(L,-3);
    }
    g_lua.settable(L,-3);
    g_lua.pushstring(L,"allies");g_lua.newtable(L);
    for(size_t i=0;i<o.allyCount;++i) {
        const auto& buddy=o.allies[i];g_lua.pushnumber(L,static_cast<double>(i+1));g_lua.newtable(L);
        Number(L,"id",buddy.id);Vector(L,"pos",buddy.pos);Number(L,"selectedTarget",buddy.target);
        Number(L,"meleeTracking",buddy.meleeTracking);Number(L,"meleeSwinging",buddy.meleeSwinging);
        Number(L,"rangedTarget",buddy.ranged.primaryTarget);Number(L,"rangedAt",static_cast<double>(buddy.ranged.primaryAt));
        Number(L,"contactTarget",buddy.contact.target);Number(L,"contactAt",static_cast<double>(buddy.contact.time));
        g_lua.settable(L,-3);
    }
    g_lua.settable(L,-3);
    return 1;
}
int __cdecl NativeFallback(lua_State* L) {
    if(!g_current||!g_nativeCallback||g_nativeFallback) { g_lua.pushnumber(L,0);return 1; }
    auto found=g_actors.find(g_current);
    if(g_lua.tonumber(L,1)!=1 && found!=g_actors.end()) RestoreIntent(g_current,found->second);
    g_nativeFallback=true;
    int result=g_dispatch(g_nativeCallback,g_current);
    g_nativeFallback=false;
    g_lua.pushnumber(L,result);return 1;
}
int __cdecl AimTarget(lua_State* L) {
    if(Owned(g_current)) {
        auto& a=ActorFor(g_current);
        double id=g_lua.tonumber(L,1);
        a.aimTarget=std::isfinite(id)&&id>0&&id<=0xffffffff ? static_cast<uint32_t>(id):0;
        a.aimAt=GetTickCount64();
    }
    return 0;
}
int __cdecl CancelRoute(lua_State*) {
    if(Owned(g_current)) {
        auto& a=ActorFor(g_current);
        if(!a.path.empty()) g_stop(g_current);
        a.path.clear();
    }
    return 0;
}
int __cdecl Cadence(lua_State* L) {
    if(!Owned(g_current)) return 0;
    auto& a=ActorFor(g_current);
    if(!a.ownsIntent) {
        a.savedWait=*reinterpret_cast<int*>(g_current+0x31F4);
        a.ownsIntent=true;
    }
    if(!a.ownsAim) {
        a.savedYawError=*reinterpret_cast<float*>(g_current+0x3204);
        a.savedPitchError=*reinterpret_cast<float*>(g_current+0x3208);
        a.ownsAim=true;
    }
    double interval=g_lua.tonumber(L,1);
    a.intervalMs=std::isfinite(interval)?static_cast<int>((std::max)(0.0,(std::min)(10000.0,interval))):120;
    return 0;
}
int __cdecl MeleeGate(lua_State* L) {
    if(GetCurrentThreadId()==g_gameThread&&Owned(g_current)) ActorFor(g_current).suppressMelee=g_lua.tonumber(L,1)==1;
    return 0;
}
int __cdecl ProfileLease(lua_State* L) {
    if(GetCurrentThreadId()==g_gameThread&&Owned(g_current)) {
        int slot=static_cast<int>(g_lua.tonumber(L,1));
        ActorFor(g_current).specialSlotLease=slot>=0&&slot<3?slot:-1;
    }
    return 0;
}
int __cdecl ProfilePreviousSlot(lua_State* L) {
    auto actor=g_actors.find(g_current);
    g_lua.pushnumber(L,actor==g_actors.end()?-1:actor->second.specialSlotLease);return 1;
}
int __cdecl BoundSlot(lua_State* L) {
    battle_ai::native::Capabilities caps={};
    int slot=Owned(g_current)&&battle_ai::native::ReadCapabilities(g_current,caps)?caps.selectedSlot:-1;
    g_lua.pushnumber(L,slot);return 1;
}
bool QueryRoute(uint8_t* nav,uint32_t id,Vec3 from,Vec3 to,Vec3** points,uint32_t* count) {
    __try { *points=g_route(nav,id,from,to,nullptr,count);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool CopyRoute(const Vec3* points,Vec3* out,size_t bytes) {
    __try { memcpy(out,points,bytes);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool FreeRoute(Vec3* points) {
    __try { g_delete(points);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void FailedRoute() {
    if(GetCurrentThreadId()!=g_gameThread || !Owned(g_current)) return;
    auto& a=ActorFor(g_current);a.path.clear();g_stop(g_current);
}
bool LoadRouteUnsafe(uint32_t target,const Vec3* lastSeen) {
    if(!Owned(g_current)||!g_document||!*g_document||GetCurrentThreadId()!=g_gameThread) return false;
    auto& a=ActorFor(g_current);
    a.path.clear(); // a failed repath must never resurrect its previous goal
    auto other=FindObject(target);
    if(!other||*reinterpret_cast<int*>(other+0x38)<=0) return false;
    auto nav=*reinterpret_cast<uint8_t**>(*g_document+0x8364);
    if(!nav) return false;
    uint32_t count=0;
    Vec3 destination=lastSeen?*lastSeen:Position(other);
    Vec3* points=nullptr;
    if(!QueryRoute(nav,*reinterpret_cast<uint32_t*>(g_current+4),Position(g_current),destination,&points,&count)||!points) return false;
    // Copy native-owned results and release with this EXE's allocator. Never
    // mix the game's CRT heap with this DLL's std::vector allocator.
    std::vector<Vec3> staged;
    bool valid=count>0&&count<=4096;
    if(valid) { try { staged.resize(count); } catch(...) { valid=false; } }
    if(valid) valid=CopyRoute(points,staged.data(),sizeof(Vec3)*count);
    // No allocation or unsafe native read happens between copy and release.
    // Even a failed copy/vector allocation releases the returned native buffer.
    bool released=FreeRoute(points);
    if(!valid||!released) return false;
    for(Vec3 p:staged) if(!battle_ai::Finite(p)) return false;
    a.path.swap(staged);a.waypoint=0;
    FollowPath(g_current,a);
    return true;
}
bool LoadRoute(uint32_t id,const Vec3* lastSeen) {
    __try { return LoadRouteUnsafe(id,lastSeen); }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int __cdecl Route(lua_State* L) {
    double number=g_lua.tonumber(L,1);
    Vec3 frozen={};
    bool provided=g_lua.gettop(L)>=5;
    if(Owned(g_current)) ActorFor(g_current).pathRanged=g_lua.gettop(L)>=6&&g_lua.tonumber(L,6)==1;
    if(provided) frozen={static_cast<float>(g_lua.tonumber(L,3)),static_cast<float>(g_lua.tonumber(L,4)),static_cast<float>(g_lua.tonumber(L,5))};
    bool ok=std::isfinite(number)&&number>0&&number<=0xffffffff &&
            (!provided||battle_ai::Finite(frozen)) && LoadRoute(static_cast<uint32_t>(number),provided?&frozen:nullptr);
    if(!ok) FailedRoute();
    g_lua.pushnumber(L,ok?1:0);return 1;
}
int __cdecl Chance(lua_State* L);
int __cdecl Cover(lua_State* L) {
    g_lua.newtable(L);Number(L,"valid",0);
    if(GetCurrentThreadId()!=g_gameThread||!Owned(g_current)||!g_document||!*g_document) return 1;
    __try {
        double id=g_lua.tonumber(L,1);
        Vec3 target={static_cast<float>(g_lua.tonumber(L,2)),static_cast<float>(g_lua.tonumber(L,3)),static_cast<float>(g_lua.tonumber(L,4))};
        if(!std::isfinite(id)||id<=0||id>0xffffffff||!battle_ai::Finite(target)) return 1;
        auto targetId=static_cast<uint32_t>(id);auto other=FindObject(targetId);
        if(!other||*reinterpret_cast<int*>(other+0x38)<=0) return 1;
        auto self=g_current;Vec3 here=Position(self),center=AimPoint(self);
        Vec3 toward=target-center;toward.y=0;double len=battle_ai::Length(toward);
        if(len<100) return 1;
        Vec3 side={-toward.z/static_cast<float>(len),0,toward.x/static_cast<float>(len)};
        Ray3 ray={};auto fsm=*reinterpret_cast<uint8_t**>(self+0x9CC);
        auto native=g_ray(fsm,&ray);if(!native||!battle_ai::Finite(native->origin)) return 1;
        Vec3 muzzle=native->origin-here;
        float halfWidth=60,halfHeight=60;
        auto scene=*reinterpret_cast<uint8_t**>(self+0x34);
        if(scene) {
            auto bounds=reinterpret_cast<battle_ai::native::Aabb*>(scene+0xBC);
            if(battle_ai::native::detail::ValidAabb(*bounds)) {
                halfWidth=(std::max)(30.0f,(std::min)(160.0f,(std::max)(bounds->max.x-bounds->min.x,bounds->max.z-bounds->min.z)*0.5f));
                halfHeight=(std::max)(30.0f,(std::min)(160.0f,(bounds->max.y-bounds->min.y)*0.4f));
            }
        }
        auto clear=[&](Vec3 from,Vec3 to) {return battle_ai::native::FreshLineClear(g_navigation,*g_document,self,targetId,from,to);};
        auto exposed=[&](Vec3 p) {
            Vec3 c=p+(center-here);int count=0;
            for(Vec3 offset:{Vec3{},side*halfWidth,side*(-halfWidth),Vec3{0,halfHeight,0},Vec3{0,-halfHeight,0}})
                if(clear(target,c+offset)) ++count;
            return count;
        };
        Vec3 hide={},peek={};float best=1.0e9f;int exposure=0;
        for(int sign:{-1,1}) for(int tucked=0;tucked<=2;++tucked) {
            Vec3 h=here+side*static_cast<float>(sign*tucked*100);
            if(exposed(h)>1) continue;
            for(int step=1;step<=5;++step) {
                Vec3 p=h+side*static_cast<float>(-sign*step*70);
                if(!clear(p+muzzle,target)) continue;
                int n=exposed(p);if(n==0||n>3) continue;
                float score=static_cast<float>(battle_ai::Length(h-here)+battle_ai::Length(p-h))*0.05f+n*100;
                if(g_lua.gettop(L)>=5 && g_lua.tonumber(L,5)!=sign) score+=15;
                if(score<best) {best=score;hide=h;peek=p;exposure=n;}
                break; // nearest fireable edge, not a broad open destination
            }
        }
        if(best<1.0e9f) {Number(L,"valid",1);Vector(L,"hide",hide);Vector(L,"peek",peek);Number(L,"exposure",exposure);}
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return 1;
}
bool WantsGeneric(uint8_t* unit) {
    return g_runtime.enabled&&g_runtime.generic&&Owned(unit)&&!g_preparationFault;
}
bool WantsAim(uint8_t* unit) {
    return g_runtime.enabled&&g_runtime.aim&&(g_runtime.generic||g_runtime.difficulty==1)&&Owned(unit)&&!g_preparationFault;
}
int __cdecl Dispatch(const char* name,uint8_t* unit) {
    if(GetCurrentThreadId()!=g_gameThread) return g_dispatch(name,unit);
    const char* prior=g_nativeCallback;
    g_nativeCallback=name;
    int result=0;
    if(unit==g_current && !g_nativeFallback && WantsGeneric(unit)) {
        result=g_dispatch("SDGO_BattleAI_GenericUpdate",unit);
    } else {
        result=g_dispatch(name,unit);
        if(unit==g_current&&!g_nativeFallback&&g_runtime.enabled&&g_runtime.difficulty==1&&Owned(unit)&&!g_preparationFault)
            g_dispatch("SDGO_BattleAI_ProfileUpdate",unit);
    }
    g_nativeCallback=prior;
    return result;
}
Ray3* __fastcall RangedRay(uint8_t* fsm,void*,Ray3* out) {
    Ray3* ray=g_ray(fsm,out);
    __try {
        if(GetCurrentThreadId()!=g_gameThread) return ray;
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        auto found=g_actors.find(owner);
        if(!ray || !WantsAim(owner) || found==g_actors.end()) return ray;
        uint32_t targetId=0;
        if(g_runtime.generic) {
            if(GetTickCount64()-found->second.aimAt>1000) return ray;
            targetId=found->second.aimTarget;
        } else targetId=*reinterpret_cast<uint32_t*>(fsm+0x200);
        if(!targetId) return ray;
        auto target=FindObject(targetId);
        if(!target || *reinterpret_cast<int*>(target+0x38)<=0) return ray;
        battle_ai::native::Capabilities caps={};
        if(!battle_ai::native::ReadCapabilities(owner,caps)) return ray;
        float speed=0;
        bool knownSelected=false;
        for(const auto& w:caps.weapons) if(w.valid && w.slot==caps.selectedSlot) {
            knownSelected=true;
            if(w.kind==battle_ai::native::WeaponKind::Melee) return ray;
            speed=g_runtime.lead?PredictionSpeed(owner,caps,w):0;
        }
        if(!knownSelected) return ray;
        Vec3 point=AimPoint(target);
        Vec3 velocity=Velocity(target,targetId,point,GetTickCount64());
        auto aim=battle_ai::Aim(ray->origin,point,velocity,speed,g_runtime.maxLeadMs/1000.0);
        if(battle_ai::Finite(aim.direction) && battle_ai::Length(aim.direction)>0.5) ray->direction=aim.direction;
    } __except(EXCEPTION_EXECUTE_HANDLER) { /* retain original native ray */ }
    return ray;
}
void __fastcall AttackIntent(uint8_t* unit,void*) {
    if(GetCurrentThreadId()!=g_gameThread) {g_intent(unit);return;}
    auto found=g_actors.find(unit);
    bool controlled=WantsGeneric(unit)&&found!=g_actors.end()&&found->second.ownsIntent;
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    bool neutral=battle_ai::native::detail::Neutral(fsm);
    if(controlled && neutral) {
        // Remove the NPC's extra script-profile wait; the weapon FSM retains its
        // actual ammo, reload, animation and cooldown checks.
        *reinterpret_cast<int*>(unit+0x31F4)=0;
    }
    g_intent(unit);
    if(WantsAim(unit)) {
        *reinterpret_cast<float*>(unit+0x3204)=0;*reinterpret_cast<float*>(unit+0x3208)=0;
    }
    if(!controlled) return;
    found=g_actors.find(unit);
    if(found==g_actors.end()||!found->second.ownsIntent) return;
    auto& a=found->second;
    auto record=fsm?*reinterpret_cast<uint8_t**>(fsm+0x248):nullptr;
    if(a.suppressMelee&&record&&(record[0x6A]==1||record[0x6A]==8)) unit[0xDCD]=0;
    ULONGLONG now=GetTickCount64();
    if(neutral && unit[0xDCD]) {
        if(now<a.nextPulse) unit[0xDCD]=0;
        else a.nextPulse=now+static_cast<ULONGLONG>(a.intervalMs);
    }
    if(g_runtime.aim) {
        *reinterpret_cast<float*>(unit+0x3204)=0;
        *reinterpret_cast<float*>(unit+0x3208)=0;
    }
}
void RecordRangedShot(uint8_t* fsm,uint32_t event,char chained,char force) {
    __try {
        if(!g_gameThread || GetCurrentThreadId()!=g_gameThread) return;
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        if(!owner || *reinterpret_cast<int*>(owner+0x38)<=0) return;
        auto& shot=g_rangedShots[owner];shot.time=GetTickCount64();shot.target=*reinterpret_cast<uint32_t*>(fsm+0x200);++shot.calls;
        // Only primary kind 1/8 calls inside native 6B41A0 are new attack-cycle
        // fires. Kind 7, multiple muzzles and arbitrary 6B1270 callers are not.
        if(g_receiptFsm==fsm && event==0 && chained==0 && (force==0||force==1) &&
           *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x34) {
            ++shot.shots;shot.primaryAt=shot.time;shot.primaryTarget=shot.target;
        }
        auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);shot.weapon=record?*reinterpret_cast<uint32_t*>(record):0;
        if(g_rangedShots.size()>2048) {
            ULONGLONG now=GetTickCount64();
            for(auto it=g_rangedShots.begin();it!=g_rangedShots.end();) {
                if(now-it->second.time>5000) it=g_rangedShots.erase(it); else ++it;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
uint32_t __fastcall RangedShot(uint8_t* fsm,void*,uint32_t event,char chained,char force) {
    RecordRangedShot(fsm,event,chained,force);
    return g_shot(fsm,event,chained,force);
}
void __fastcall FireReceipts(uint8_t* fsm,void*,void* event) {
    if(GetCurrentThreadId()!=g_gameThread) {g_fireReceipts(fsm,event);return;}
    auto prior=g_receiptFsm;g_receiptFsm=fsm;
    __try {g_fireReceipts(fsm,event);} __finally {g_receiptFsm=prior;}
}
void __fastcall AttackAdmission(uint8_t* fsm,void*) {
    if(GetCurrentThreadId()==g_gameThread&&g_runtime.enabled&&g_runtime.difficulty>=1&&!g_preparationFault) {
        __try {
            auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
            auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
            // This is the shared ordinary/special admission function. Apply
            // the player SP rule only to AttackStyle 2, never normal shots.
            if(Owned(owner)&&record&&record[0x69]==2&&!FairSpecialResource(fsm,owner)) return;
        } __except(EXCEPTION_EXECUTE_HANDLER) {return;}
    }
    g_attackAdmission(fsm);
}
bool Roll(Actor& a,int percent,uint32_t id) {
    if(!a.rng) a.rng=id^static_cast<uint32_t>(a.generation*2654435761u)^0xA341316Cu;
    a.rng^=a.rng<<13;a.rng^=a.rng>>17;a.rng^=a.rng<<5;
    return a.rng%100<static_cast<unsigned>(percent);
}
int __cdecl Chance(lua_State* L) {
    double value=g_lua.tonumber(L,1);bool selected=false;
    if(GetCurrentThreadId()==g_gameThread&&Owned(g_current)&&std::isfinite(value)&&value>=0&&value<=100)
        selected=Roll(ActorFor(g_current),static_cast<int>(value),*reinterpret_cast<uint32_t*>(g_current+4));
    g_lua.pushnumber(L,selected?1:0);return 1;
}
int g_quickSwingPercent=80;
void ClearStartup(uint8_t* unit,Actor& a,bool face) {
    if(a.sideCue) {
        // Zero yaw is an identity increment, not an inverse. Undo the actual
        // owner-only relative cue before restoring combat facing.
        g_rotate(unit,-a.cueAngle,0);g_rotate(unit,0,0);
        if(face) g_faceTarget(*reinterpret_cast<uint8_t**>(unit+0x9CC));
    }
    a.quickSwing=false;a.sideCue=false;a.startupTarget=0;
    a.cueAngle=0;
}
bool QuickSwingSafe(uint8_t* fsm,uint8_t* owner) {
    if(!WantsGeneric(owner)||ControlBlocked(owner)||fsm[0x9B1]||*reinterpret_cast<int*>(owner+0x38)<=0) return false;
    auto target=FindObject(*reinterpret_cast<uint32_t*>(fsm+0x200));
    auto record=*reinterpret_cast<uint8_t**>(fsm+0x248);
    if(!target||*reinterpret_cast<int*>(target+0x38)<=0||!record||record[0x6A]!=1) return false;
    battle_ai::native::Capabilities caps={};
    if(!battle_ai::native::ReadCapabilities(owner,caps)) return false;
    for(const auto& w:caps.weapons) if(w.valid&&w.slot==caps.selectedSlot&&w.kind==battle_ai::native::WeaponKind::Melee)
        return battle_ai::Length(Position(target)-Position(owner))<=w.range;
    return false;
}
void __fastcall TrackingEntry(uint8_t* fsm,void*,void* event) {
    g_trackingEntry(fsm,event);
    if(GetCurrentThreadId()!=g_gameThread) return;
    __try {
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        if(!QuickSwingSafe(fsm,owner)||*reinterpret_cast<int*>(fsm+0x968)!=0) return;
        auto& a=ActorFor(owner);ClearStartup(owner,a,false);
        // One roll per actual tracking Entry, never once per Think/frame.
        a.quickSwing=Roll(a,g_quickSwingPercent,*reinterpret_cast<uint32_t*>(owner+4));
        if(a.quickSwing) {
            a.startupAt=GetTickCount64();a.startupTarget=*reinterpret_cast<uint32_t*>(fsm+0x200);
            a.sideCue=true;a.cueAngle=(a.rng&1)?35.0f:-35.0f;g_rotate(owner,a.cueAngle,0);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
bool TryQuickSwing(uint8_t* fsm) {
    __try {
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        auto it=g_actors.find(owner);if(it==g_actors.end()||!it->second.quickSwing) return false;
        auto& a=it->second;auto elapsed=GetTickCount64()-a.startupAt;
        if(!QuickSwingSafe(fsm,owner)||a.startupTarget!=*reinterpret_cast<uint32_t*>(fsm+0x200)||elapsed>120) {
            ClearStartup(owner,a,true);return false;
        }
        if(*reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x5C) return true;
        if(*reinterpret_cast<uint8_t**>(fsm+8)) {ClearStartup(owner,a,true);return false;}
        if(elapsed<16) return true; // brief owner-only cue, no tracking dash
        g_queueSwing(fsm);
        if(*reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x5C) return true;
        ClearStartup(owner,a,true);return false;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void __fastcall TrackingUpdate(uint8_t* fsm,void*,void* event) {
    if(GetCurrentThreadId()!=g_gameThread||!TryQuickSwing(fsm)) g_trackingUpdate(fsm,event);
}
void __fastcall SwingEntry(uint8_t* fsm,void*,void* event) {
    if(GetCurrentThreadId()==g_gameThread) {
        __try {
            auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);auto it=g_actors.find(owner);
            if(it!=g_actors.end()&&it->second.quickSwing) ClearStartup(owner,it->second,true);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_swingEntry(fsm,event);
}
void __fastcall SwingUpdate(uint8_t* fsm,void*,void* event) {
    bool suppress=false;
    if(GetCurrentThreadId()==g_gameThread) {
        __try {
            auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);auto it=g_actors.find(owner);
            suppress=WantsGeneric(owner)&&it!=g_actors.end()&&it->second.suppressMelee&&event&&
                *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x5C;
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if(!suppress) {g_swingUpdate(fsm,event);return;}
    auto input=static_cast<uint8_t*>(event);uint8_t saved=input[0xD];input[0xD]=0;
    __try {g_swingUpdate(fsm,event);} __finally {input[0xD]=saved;}
}
bool __fastcall DeliverMessage(uint8_t* document,void*,uint32_t victim,void* message,uint32_t tag) {
    void* caller=_ReturnAddress();Contact receipt;uint8_t* attacker=nullptr;
    if(GetCurrentThreadId()==g_gameThread && caller==g_contactReturn && message && tag==0) {
        __try {
            auto m=static_cast<uint8_t*>(message);
            if(*reinterpret_cast<uint16_t*>(m)==0xC && *reinterpret_cast<int*>(m+0x14)>0) {
                attacker=FindObject(*reinterpret_cast<uint32_t*>(m+0x3C));
                auto fsm=attacker?*reinterpret_cast<uint8_t**>(attacker+0x9CC):nullptr;
                if(fsm && *reinterpret_cast<uint8_t**>(fsm+4)==fsm+0x5C) {
                    receipt={victim,*reinterpret_cast<uint32_t*>(fsm+0x224),*reinterpret_cast<int*>(fsm+0x968),GetTickCount64(),FindObject(victim)};
                }
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {attacker=nullptr;}
    }
    bool result=g_message(document,victim,message,tag);
    if(result&&attacker&&receipt.target&&receipt.victim) {
        if(g_contacts.size()>2048) g_contacts.clear();
        g_contacts[attacker]=receipt;
    }
    return result;
}
struct FlightSample {
    uint8_t* owner=nullptr;uint8_t* weapon=nullptr;uint8_t* node=nullptr;
    Vec3 point;uint64_t key=0;int modifier=0;
};
bool BeforeFlight(uint8_t* projectile,FlightSample& s) {
    __try {
        if(!g_gameThread||GetCurrentThreadId()!=g_gameThread) return false;
        if(!g_runtime.lead) return false;
        if(projectile[0x1D]) return false;
        s.owner=*reinterpret_cast<uint8_t**>(projectile+0x9C4);
        s.weapon=*reinterpret_cast<uint8_t**>(projectile+0x9C8);
        s.node=*reinterpret_cast<uint8_t**>(projectile+8);
        if(!WantsAim(s.owner)||!s.weapon||!s.node||g_actors.find(s.owner)==g_actors.end()) return false;
        // SetOwner captures the launch form. A projectile can outlive a form
        // switch; never attribute its old flight to the owner's new loadout.
        if(projectile[0x9ED]!=s.owner[0xE34]) return false;
        auto def=*reinterpret_cast<uint8_t**>(s.owner+0x9C4);
        if(!def) return false;
        uint32_t unitId=*reinterpret_cast<uint32_t*>(def),weaponId=*reinterpret_cast<uint32_t*>(s.weapon);
        if(!unitId||unitId>9999999||!weaponId||weaponId>9999999) return false;
        s.key=SpeedKey(unitId,weaponId,s.owner[0xE34]!=0);
        auto table=*reinterpret_cast<void***>(projectile);
        if(!table || table[0x18/sizeof(void*)]!=g_plainProjectileEntry) {
            // Homing and split/controller wrappers can keep constant speed
            // while turning. Preserve their native guidance, not linear lead.
            g_speeds[s.key]={0,0,GetTickCount64(),true};return false;
        }
        if(projectile[0xA38]!=0 || *reinterpret_cast<float*>(projectile+0xA8C)!=0) {
            g_speeds[s.key]={0,0,GetTickCount64(),true};
            return false;
        }
        s.modifier=SpeedModifier(s.owner,s.weapon);
        s.point=*reinterpret_cast<Vec3*>(s.node+0x88);
        return battle_ai::Finite(s.point);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void AfterFlightUnsafe(uint8_t* projectile,const FlightSample& s) {
    if(projectile[0xA38]!=0 || *reinterpret_cast<float*>(projectile+0xA8C)!=0) {
        g_speeds[s.key]={0,0,GetTickCount64(),true};return;
    }
    if(projectile[0x1D] ||
       *reinterpret_cast<uint8_t**>(projectile+0x9C4)!=s.owner ||
       *reinterpret_cast<uint8_t**>(projectile+0x9C8)!=s.weapon ||
       *reinterpret_cast<uint8_t**>(projectile+8)!=s.node) return;
    if(projectile[0x9ED]!=s.owner[0xE34]) return;
    Vec3 after=*reinterpret_cast<Vec3*>(s.node+0x88);
    float speed=static_cast<float>(battle_ai::Length(after-s.point));
    if(!battle_ai::Finite(after)||!std::isfinite(speed)||speed<0.001f||speed>100000) return;
    ULONGLONG now=GetTickCount64();
    auto& run=g_flights[projectile];
    bool continuous=run.key==s.key && now-run.seen<1000 &&
                    battle_ai::Length(run.end-s.point)<1 &&
                    std::abs(run.speed-speed)<(std::max)(0.1f,speed*0.03f);
    run.stable=continuous?run.stable+1:1;
    run.key=s.key;run.speed=speed;run.seen=now;run.end=after;
    auto known=g_speeds.find(s.key);
    if(run.stable>=3 && (known==g_speeds.end() || !known->second.nonlinear)) g_speeds[s.key]={speed,s.modifier,now,false};
    if(g_flights.size()>2048) {
        for(auto it=g_flights.begin();it!=g_flights.end();) {
            if(now-it->second.seen>2000) it=g_flights.erase(it);else ++it;
        }
    }
}
void AfterFlight(uint8_t* projectile,const FlightSample& s) {
    __try { AfterFlightUnsafe(projectile,s); } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
bool __fastcall ProjectileUpdate(uint8_t* projectile,void*,void* opaque) {
    FlightSample sample;
    bool observe=BeforeFlight(projectile,sample);
    bool result=g_projectile(projectile,opaque);
    if(observe) AfterFlight(projectile,sample);
    return result;
}

template<typename T> bool Bind(T& out, const char* name) {
    out = reinterpret_cast<T>(g_api->Symbol(g_mod, name));
    return out != nullptr;
}
FILETIME Stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d = {};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d) ? d.ftLastWriteTime : FILETIME{};
}
bool Same(FILETIME a, FILETIME b) {
    return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime;
}
bool Read(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size = {};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < 512 * 1024;
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD n = 0;
        ok = ReadFile(f, &out[0], static_cast<DWORD>(out.size()), &n, nullptr) && n == out.size();
    }
    CloseHandle(f);
    return ok;
}
struct Chunk { const char* data; size_t size; };
const char* __cdecl ReadChunk(lua_State*, void* user, size_t* n) {
    auto& c = *static_cast<Chunk*>(user);
    *n = c.size; c.size = 0;
    return *n ? c.data : nullptr;
}
bool Run(lua_State* L, const std::string& code) {
    int top = g_lua.gettop(L);
    Chunk c = {code.data(), code.size()};
    int err = g_lua.load(L, ReadChunk, &c, "=battle_ai");
    if (!err) err = g_lua.pcall(L, 0, 0, 0);
    if (err) g_api->Log(g_mod, SDGO_LOG_ERROR, "Lua reload rejected; previous policy retained: %s",
                        g_lua.tostring(L, -1) ? g_lua.tostring(L, -1) : "unknown error");
    g_lua.settop(L, top);
    return err == 0;
}
void Register(lua_State* L, const char* name, LuaFn fn) {
    g_lua.pushstring(L, name); g_lua.pushcclosure(L, fn, 0); g_lua.settable(L, kGlobals);
}
int __cdecl Now(lua_State* L) { g_lua.pushnumber(L, static_cast<double>(GetTickCount64())); return 1; }
int __cdecl Log(lua_State* L) {
    const char* s = g_lua.tostring(L, 1);
    if (s) g_api->Log(g_mod, SDGO_LOG_INFO, "%s", s);
    return 0;
}
// Layouts belong only to the manifest's verified GOnline build. Remote copies
// (+2B65) must never run an additional decision loop. Lua also checks GetNPCID.
bool Eligible() {
    __try {
        return g_current && g_current[0x2B65] == 0 && g_current[0x326C] != 0 &&
               *reinterpret_cast<uint32_t*>(g_current + 0x3238) >= 3604001 &&
               *reinterpret_cast<uint32_t*>(g_current + 0x3238) <= 3607000;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int __cdecl Authority(lua_State* L) { g_lua.pushnumber(L, Eligible() ? 1 : 0); return 1; }
int __cdecl Generation(lua_State* L) {
    if(GetCurrentThreadId()!=g_gameThread) {g_lua.pushnumber(L,0);return 1;}
    auto it = g_generations.find(g_current);
    g_lua.pushnumber(L, it == g_generations.end() ? 0 : static_cast<double>(it->second));
    return 1;
}
struct Option { const wchar_t* name; int def, low, high; };
const Option kOptions[] = {
    {L"Enabled",1,0,1}, {L"GenericMode",1,0,1}, {L"DecisionMs",100,50,1000}, {L"ReactionMs",180,0,3000},
    {L"StuckMs",1400,300,10000}, {L"ProgressDistance",30,1,300}, {L"ArrivalDistance",100,10,500},
    {L"RecoveryDistance",350,50,1500}, {L"RecoveryMs",650,150,2000}, {L"RecoveryCooldownMs",1000,250,10000},
    {L"RangedMinDistance",450,0,2000}, {L"TacticCooldownMs",900,300,10000}, {L"LogDecisions",0,0,1},
    {L"TargetHoldMs",600,0,5000},{L"FarTargetSlack",900,0,10000},{L"FarTargetRatioPercent",200,100,1000},
    {L"WeaponHoldMs",450,0,5000},{L"AttackIntervalMs",120,0,10000},
    {L"BurstMs",900,0,10000},{L"BurstPauseMs",100,0,5000},
    {L"AirStrafe",1,0,1},{L"StrafeSwitchMs",450,150,3000},{L"StrafeDistance",400,50,1500},
    {L"JumpMs",600,50,1500},{L"JumpCooldownMs",1500,500,10000},{L"BoostReservePercent",20,0,90},
    {L"ThreatMemoryMs",1600,100,10000},{L"RepathMs",700,150,5000},
    {L"TargetMemoryMs",2500,0,10000},{L"PatrolMs",1600,300,10000},
    {L"AimAssist",1,0,1},{L"ProjectileLead",1,0,1},{L"MaxLeadMs",3000,100,10000}
    ,{L"SwingCancelChance",80,0,100},{L"CoverChance",90,0,100},{L"CoverSearchMs",1600,500,10000},
    {L"CoverTimeoutMs",2200,500,10000},{L"OrbwalkStep",120,50,500},{L"OrbwalkMs",180,80,600},{L"FlankMs",750,300,3000}
};
int CurrentRoomDifficulty() {
    wchar_t title[21]={};size_t length=0;
    __try {
        auto app=g_app();if(!app) return -1;
        auto service=*reinterpret_cast<uint8_t**>(app+0x4A4);if(!service) return -1;
        auto mode=*reinterpret_cast<uint8_t**>(service+0x0C);if(!mode) return -1;
        auto room=*reinterpret_cast<uint8_t**>(mode+8);if(!room) return -1;
        length=*reinterpret_cast<uint32_t*>(room+0x10);
        auto capacity=*reinterpret_cast<uint32_t*>(room+0x14);
        if(length>20||capacity<7||capacity<length||capacity>0x1000) return -1;
        auto text=capacity<8?reinterpret_cast<wchar_t*>(room):*reinterpret_cast<wchar_t**>(room);
        if(!text||text[length]!=0) return -1;
        memcpy(title,text,length*sizeof(wchar_t));
    } __except(EXCEPTION_EXECUTE_HANDLER) {return -1;}
    return battle_ai::RoomDifficulty(std::wstring_view(title,length));
}
std::string ConfigChunk(RuntimeSettings& next,int roomDifficulty) {
    wchar_t difficulty[32]={};
    GetPrivateProfileStringW(L"settings",L"Difficulty",L"",difficulty,32,g_config.c_str());
    if(*difficulty) {
        if(!_wcsicmp(difficulty,L"Easy")||!wcscmp(difficulty,L"0")) next.difficulty=0;
        else if(!_wcsicmp(difficulty,L"Medium")||!wcscmp(difficulty,L"1")) next.difficulty=1;
        else if(!_wcsicmp(difficulty,L"Hard")||!wcscmp(difficulty,L"2")) next.difficulty=2;
        else {next.difficulty=1;g_api->Log(g_mod,SDGO_LOG_WARN,"invalid Difficulty; using Medium");}
    }
    if(roomDifficulty>=0&&roomDifficulty<=2) next.difficulty=roomDifficulty;
    std::string code = "local config = { Difficulty="+std::to_string(next.difficulty)+",\n";
    for (const auto& o : kOptions) {
        wchar_t buf[64] = {}, *end = nullptr;
        GetPrivateProfileStringW(L"settings", o.name, L"", buf, 64, g_config.c_str());
        long value = *buf ? wcstol(buf, &end, 10) : o.def;
        while (end && (*end == L' ' || *end == L'\t')) ++end;
        if ((end && *end) || value < o.low || value > o.high) {
            value = o.def;
            g_api->Log(g_mod, SDGO_LOG_WARN, "invalid config.ini option %ls; using %d", o.name, o.def);
        }
        std::string key;
        for (const wchar_t* p = o.name; *p; ++p) key.push_back(static_cast<char>(*p));
        if(key=="GenericMode") value=next.difficulty==2?1:0;
        if(key=="AimAssist"||key=="ProjectileLead") value=next.difficulty>=1?1:0;
        code += key + "=" + std::to_string(value) + ",\n";
        if(key=="Enabled") next.enabled=value!=0;
        if(key=="GenericMode") next.generic=value!=0;
        if(key=="AimAssist") next.aim=value!=0;
        if(key=="ProjectileLead") next.lead=value!=0;
        if(key=="MaxLeadMs") next.maxLeadMs=static_cast<int>(value);
        if(key=="SwingCancelChance") next.quickSwing=static_cast<int>(value);
    }
    return code + "}\n";
}
void Prepare() {
    lua_State* L = g_lua.state(nullptr);
    if (!L) return;
    ULONGLONG now = GetTickCount64();
    if (L == g_loaded && now < g_checkAt) return;
    g_checkAt = now + 2000;
    FILETIME st = Stamp(g_script), ct = Stamp(g_config), ut=Stamp(g_universalScript);
    int roomDifficulty=CurrentRoomDifficulty();
    if (L == g_loaded && roomDifficulty==g_appliedRoomDifficulty && Same(st,g_scriptTime) && Same(ct,g_configTime) && Same(ut,g_universalTime)) {
        Run(L, "if SDGO_BattleAI_Refresh then SDGO_BattleAI_Refresh() end");
        return;
    }
    // Remember a failed attempt too: no compile/error spam each frame. An edit retries.
    g_loaded = L; g_scriptTime = st; g_configTime = ct;g_universalTime=ut;
    std::string script,universal;
    if (!Read(g_script, script) || !Read(g_universalScript,universal)) {
        g_api->Log(g_mod, SDGO_LOG_ERROR, "cannot read AI policy scripts; native AI retained"); return;
    }
    Register(L,"SDGO_BattleAI_Now",Now);
    Register(L,"SDGO_BattleAI_Log",Log);
    Register(L,"SDGO_BattleAI_Authority",Authority);
    Register(L,"SDGO_BattleAI_Generation",Generation);
    Register(L,"SDGO_BattleAI_Observe",Observe);
    Register(L,"SDGO_BattleAI_Native",NativeFallback);
    Register(L,"SDGO_BattleAI_AimTarget",AimTarget);
    Register(L,"SDGO_BattleAI_Cadence",Cadence);
    Register(L,"SDGO_BattleAI_Route",Route);
    Register(L,"SDGO_BattleAI_CancelRoute",CancelRoute);
    Register(L,"SDGO_BattleAI_Chance",Chance);
    Register(L,"SDGO_BattleAI_Cover",Cover);
    Register(L,"SDGO_BattleAI_MeleeGate",MeleeGate);
    Register(L,"SDGO_BattleAI_ProfileLease",ProfileLease);
    Register(L,"SDGO_BattleAI_ProfilePreviousSlot",ProfilePreviousSlot);
    Register(L,"SDGO_BattleAI_BoundSlot",BoundSlot);
    RuntimeSettings next;
    next.generic=true;
    if (Run(L, ConfigChunk(next,roomDifficulty) + script + "\n" + universal)) {
        if(next.difficulty!=g_runtime.difficulty)
            Run(L,"if SDGO_BattleAI_Universal then SDGO_BattleAI_Universal.states={} end");
        g_runtime=next;
        g_appliedRoomDifficulty=roomDifficulty;
        g_quickSwingPercent=next.quickSwing;
        g_api->Log(g_mod, SDGO_LOG_INFO, "policies loaded (config.ini); difficulty %d, generic %d",next.difficulty,next.generic?1:0);
    }
}
void GuardedPrepare() {
    __try { Prepare(); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        g_preparationFault = true;
        g_api->Log(g_mod, SDGO_LOG_ERROR, "policy preparation failed; native Think retained");
    }
}
void ResetLifecycle(uint8_t* unit) {
    if(g_generations.size()>=1024) g_generations.clear();
    g_generations[unit]=++g_serial;
    g_actors.erase(unit);g_rangedShots.erase(unit);g_contacts.erase(unit);
}
void DrainPendingInit() {
    std::vector<uint8_t*> pending;
    {std::lock_guard<std::mutex> lock(g_pendingMutex);pending.swap(g_pendingInit);}
    for(uint8_t* unit:pending) ResetLifecycle(unit);
}
bool __fastcall Think(uint8_t* unit, void*) {
    DWORD thread=GetCurrentThreadId(),expected=0;
    g_gameThread.compare_exchange_strong(expected,thread);
    if(thread!=g_gameThread) return g_think(unit);
    DrainPendingInit();
    if (!g_loading && !g_preparationFault) { g_loading = true; GuardedPrepare(); g_loading = false; }
    uint8_t* prior = g_current; g_current = unit;
    // Init can precede loading in fixtures or a late loader; give that unit an identity.
    if (g_generations.find(unit) == g_generations.end()) {
        if (g_generations.size() >= 1024) g_generations.clear();
        g_generations[unit] = ++g_serial;
    }
    auto actor=g_actors.find(unit);
    if(actor!=g_actors.end()&&actor->second.specialSlotLease>=0&&
       (!g_runtime.enabled||g_runtime.difficulty<1||g_preparationFault)&&Owned(unit)) {
        g_dispatch("SDGO_BattleAI_ProfileRelease",unit);
    }
    if(actor!=g_actors.end()) {
        if(actor->second.sideCue&&!WantsGeneric(unit)) ClearStartup(unit,actor->second,true);
        if((!WantsGeneric(unit)&&(actor->second.controlled||actor->second.ownsIntent)) ||
           (!WantsAim(unit)&&actor->second.ownsAim)) RestoreIntent(unit,actor->second);
    }
    if(WantsAim(unit)) {
        auto& a=ActorFor(unit);
        if(!a.ownsAim) {
            a.savedYawError=*reinterpret_cast<float*>(unit+0x3204);a.savedPitchError=*reinterpret_cast<float*>(unit+0x3208);a.ownsAim=true;
        }
        *reinterpret_cast<float*>(unit+0x3204)=0;*reinterpret_cast<float*>(unit+0x3208)=0;
    }
    bool result=g_think(unit);
    auto startup=g_actors.find(unit);
    if(startup!=g_actors.end()&&startup->second.sideCue) {
        __try {
            auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
            if(!battle_ai::native::detail::InState(fsm,0x48)&&!battle_ai::native::detail::InState(fsm,0x5C))
                ClearStartup(unit,startup->second,!ControlBlocked(unit));
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    if(WantsGeneric(unit)) {
        auto it=g_actors.find(unit);
        if(it!=g_actors.end() && !it->second.path.empty()) {
            __try { FollowPath(unit,it->second); } __except(EXCEPTION_EXECUTE_HANDLER) { it->second.path.clear(); }
        }
    }
    g_current = prior;
    return result;
}
void __fastcall Init(uint8_t* unit, void*, void* data) {
    g_init(unit, data);
    if(GetCurrentThreadId()==g_gameThread) ResetLifecycle(unit);
    else {std::lock_guard<std::mutex> lock(g_pendingMutex);g_pendingInit.push_back(unit);}
}
}

SDGO_MOD_ENTRY {
    g_api = api; g_mod = mod;
    if (!Bind(g_lua.state,"lua_tinker.state") || !Bind(g_lua.gettop,"lua.gettop") ||
        !Bind(g_lua.type,"lua.type") || !Bind(g_lua.tonumber,"lua.tonumber") ||
        !Bind(g_lua.settop,"lua.settop") || !Bind(g_lua.tostring,"lua.tostring") ||
        !Bind(g_lua.pushnumber,"lua.pushnumber") || !Bind(g_lua.pushstring,"lua.pushstring") ||
        !Bind(g_lua.pushcclosure,"lua.pushcclosure") || !Bind(g_lua.gettable,"lua.gettable") ||
        !Bind(g_lua.settable,"lua.settable") || !Bind(g_lua.newtable,"lua.newtable") ||
        !Bind(g_lua.pcall,"lua.pcall") || !Bind(g_lua.load,"lua.load") ||
        !Bind(g_document,"game.document") || !Bind(g_route,"navigation.route") ||
        !Bind(g_setIntent,"npc.set_attack_intent") || !Bind(g_stop,"npc.move_stop") ||
        !Bind(g_setGoal,"npc.set_move_goal") || !Bind(g_delete,"crt.operator_delete") ||
        !Bind(g_navigation.query,"collision.query") || !Bind(g_navigation.segmentAabb,"collision.segment_aabb") ||
        !Bind(g_navigation.terrainRay,"collision.terrain_ray") || !Bind(g_navigation.rayObb,"collision.ray_obb") ||
        !Bind(g_navigation.raySphere,"collision.ray_sphere")) return 0;
    if(!Bind(g_app,"game.app")||!Bind(g_queueSwing,"combat.queue_melee_swing")||!Bind(g_faceTarget,"npc.face_target")||!Bind(g_rotate,"unit.rotate_y")) return 0;
    if(!Bind(g_contactReturn,"combat.melee_contact_return")) return 0;
    if(!Bind(g_playerSkillGate,"combat.player_sp_gate")) return 0;
    std::wstring dir = api->ModDirectory(mod);
    g_script = dir + L"\\battle_ai.lua"; g_config = dir + L"\\config.ini";
    g_universalScript=dir+L"\\universal_ai.lua";
    g_plainProjectileEntry=api->Symbol(mod,"combat.projectile_update");
    std::string check;
    if (!Read(g_script,check)||!Read(g_universalScript,check)) { api->Log(mod,SDGO_LOG_ERROR,"missing AI policy script"); return 0; }
    if (!api->HookCode(mod,"npc.think",reinterpret_cast<void*>(&Think),reinterpret_cast<void**>(&g_think)) ||
        !api->HookCode(mod,"npc.init_ai",reinterpret_cast<void*>(&Init),reinterpret_cast<void**>(&g_init)) ||
        !api->HookCode(mod,"npc.dispatch_ai",reinterpret_cast<void*>(&Dispatch),reinterpret_cast<void**>(&g_dispatch)) ||
        !api->HookCode(mod,"npc.ranged_ray",reinterpret_cast<void*>(&RangedRay),reinterpret_cast<void**>(&g_ray)) ||
        !api->HookCode(mod,"npc.attack_intent",reinterpret_cast<void*>(&AttackIntent),reinterpret_cast<void**>(&g_intent)) ||
        !api->HookCode(mod,"combat.ranged_shot",reinterpret_cast<void*>(&RangedShot),reinterpret_cast<void**>(&g_shot)) ||
        !api->HookCode(mod,"combat.fire_receipts",reinterpret_cast<void*>(&FireReceipts),reinterpret_cast<void**>(&g_fireReceipts)) ||
        !api->HookCode(mod,"combat.attack_admission",reinterpret_cast<void*>(&AttackAdmission),reinterpret_cast<void**>(&g_attackAdmission)) ||
        !api->HookCode(mod,"npc.tracking_entry",reinterpret_cast<void*>(&TrackingEntry),reinterpret_cast<void**>(&g_trackingEntry)) ||
        !api->HookCode(mod,"npc.tracking_update",reinterpret_cast<void*>(&TrackingUpdate),reinterpret_cast<void**>(&g_trackingUpdate)) ||
        !api->HookCode(mod,"npc.swing_entry",reinterpret_cast<void*>(&SwingEntry),reinterpret_cast<void**>(&g_swingEntry)) ||
        !api->HookCode(mod,"npc.swing_update",reinterpret_cast<void*>(&SwingUpdate),reinterpret_cast<void**>(&g_swingUpdate)) ||
        !api->HookCode(mod,"um.deliver_message",reinterpret_cast<void*>(&DeliverMessage),reinterpret_cast<void**>(&g_message)) ||
        !api->HookCode(mod,"combat.projectile_update",reinterpret_cast<void*>(&ProjectileUpdate),reinterpret_cast<void**>(&g_projectile))) return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: capability-driven local battle bots; native projectiles and collisions");
    return 1;
}
