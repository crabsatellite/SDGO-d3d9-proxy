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
#include "sdgo/sdgo_api.h"
#include "combat_math.h"
#include "native_capabilities.h"
#include "native_navigation.h"

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
using RouteFn=Vec3*(__thiscall*)(uint8_t*,uint32_t,Vec3,Vec3,Vec3*,uint32_t*);
using DeleteFn=void(__cdecl*)(void*);
DispatchFn g_dispatch=nullptr;
RayFn g_ray=nullptr;
IntentFn g_intent=nullptr;
IntentFn g_stop=nullptr;
SetIntentFn g_setIntent=nullptr;
ShotFn g_shot=nullptr;
ProjectileFn g_projectile=nullptr;
void* g_plainProjectileEntry=nullptr;
GoalFn g_setGoal=nullptr;
RouteFn g_route=nullptr;
DeleteFn g_delete=nullptr;
uint8_t** g_document=nullptr;
battle_ai::native::NavigationApi g_navigation={};
struct RuntimeSettings { bool generic=false,enabled=true,aim=true,lead=true; int maxLeadMs=3000; } g_runtime;
struct Actor {
    uint64_t generation=0;
    ULONGLONG seen=0,aimAt=0,nextPulse=0;
    uint32_t aimTarget=0;
    int intervalMs=120;
    bool ownsIntent=false,controlled=false;
    int savedWait=0;
    float savedYawError=0,savedPitchError=0;
    std::vector<Vec3> path;
    size_t waypoint=0;
    int lastHp=0;
    ULONGLONG hurtAt=0;
};
std::unordered_map<uint8_t*,Actor> g_actors;
struct Motion { uint8_t* unit=nullptr; Vec3 point,velocity; ULONGLONG at=0; };
std::unordered_map<uint32_t,Motion> g_motion;
std::unordered_map<uint8_t*,ULONGLONG> g_rangedShots;
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
void RestoreIntent(uint8_t* unit,Actor& a) {
    __try {
        if(a.controlled) { g_setIntent(unit,0);g_stop(unit); }
        if(a.ownsIntent) {
            *reinterpret_cast<int*>(unit+0x31F4)=a.savedWait;
            *reinterpret_cast<float*>(unit+0x3204)=a.savedYawError;
            *reinterpret_cast<float*>(unit+0x3208)=a.savedPitchError;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    a.controlled=false;a.ownsIntent=false;a.aimTarget=0;a.path.clear();
}
void FollowPath(uint8_t* unit,Actor& a) {
    if(a.path.empty()) return;
    auto fsm=*reinterpret_cast<uint8_t**>(unit+0x9CC);
    if(!fsm || *reinterpret_cast<uint8_t**>(fsm+8)!=fsm+0x20) {
        // Native attacks/hurt/guard own the motor now; drop our route without
        // cancelling the movement belonging to their active state.
        a.path.clear();return;
    }
    Vec3 here=Position(unit);
    while(a.waypoint<a.path.size() && battle_ai::Length(a.path[a.waypoint]-here)<100) ++a.waypoint;
    if(a.waypoint>=a.path.size()) { a.path.clear(); return; }
    Vec3 current=*reinterpret_cast<Vec3*>(unit+0x3078);
    if(battle_ai::Length(current-a.path[a.waypoint])>1) g_setGoal(unit,&a.path[a.waypoint],1);
}
struct TargetObservation {
    uint32_t id=0;
    int hp=0,maxHp=0;
    float distance=0;
    bool visible=false,enemy=false,attackable=false,rangedFiring=false;
    Vec3 point,ground,velocity;
};
struct Observation {
    battle_ai::native::Capabilities caps={};
    uint32_t id=0;
    uint64_t generation=0;
    int hp=0;
    Vec3 pos,goal;
    bool moving=false,hurt=false,hurtRecently=false,rangedBusy=false;
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
    // Native CheckHurt is also available through the same Lua NPC object. The
    // bridge fills it in Observe below rather than guessing a damage-state byte.
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
    auto currentState=*reinterpret_cast<uint8_t**>(fsm+8);
    o.rangedBusy=currentState==fsm+0x34 || currentState==fsm+0x48;
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
            if(!id||duplicate) continue;
            int team=*reinterpret_cast<int*>(candidate+0x20);
            if(team==ownTeam && ownTeam!=0 && ownTeam!=5) continue;
            int hp=*reinterpret_cast<int*>(candidate+0x38);
            if(hp<=0) continue;
            Vec3 point=AimPoint(candidate);
            float distance=static_cast<float>(battle_ai::Length(Position(candidate)-o.pos));
            if(!battle_ai::Finite(point)||!std::isfinite(distance)||distance>perception) continue;
            auto vtable=*reinterpret_cast<void***>(candidate);
            if(!vtable||!vtable[10]||!reinterpret_cast<CanAttack>(vtable[10])(candidate,unit)) continue;
            auto& t=o.targets[o.count++];
            t.id=id;t.hp=hp;t.maxHp=*reinterpret_cast<int*>(candidate+0x3C);
            t.distance=distance;t.enemy=true;t.attackable=true;t.point=point;t.ground=Position(candidate);
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
            if(recent!=g_rangedShots.end() && now-recent->second<1600) t.rangedFiring=true;
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
    ActorFor(g_current).controlled=true;
    Number(L,"id",o.id);Number(L,"generation",static_cast<double>(o.generation));
    Number(L,"hp",o.hp);Number(L,"unitId",o.caps.unitId);Number(L,"alternate",o.caps.alternate);Number(L,"selectedSlot",o.caps.selectedSlot);
    Number(L,"moving",o.moving);Number(L,"hurtRecently",o.hurtRecently);
    Number(L,"boostFraction",o.caps.boostFraction);
    // Lua SetJumpEnable writes +3291. This is permission for native automatic
    // jump requests, not a promise that every form can fly; the motor decides.
    Number(L,"canJump",g_current[0x3291]!=0);
    Number(L,"meleeBusy",o.caps.meleeBusy);Number(L,"specialBusy",o.caps.specialBusy);
    Number(L,"rangedBusy",o.rangedBusy);
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
        a.savedYawError=*reinterpret_cast<float*>(g_current+0x3204);
        a.savedPitchError=*reinterpret_cast<float*>(g_current+0x3208);
        a.ownsIntent=true;
    }
    double interval=g_lua.tonumber(L,1);
    a.intervalMs=std::isfinite(interval)?static_cast<int>((std::max)(0.0,(std::min)(10000.0,interval))):120;
    return 0;
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
    if(provided) frozen={static_cast<float>(g_lua.tonumber(L,3)),static_cast<float>(g_lua.tonumber(L,4)),static_cast<float>(g_lua.tonumber(L,5))};
    bool ok=std::isfinite(number)&&number>0&&number<=0xffffffff &&
            (!provided||battle_ai::Finite(frozen)) && LoadRoute(static_cast<uint32_t>(number),provided?&frozen:nullptr);
    if(!ok) FailedRoute();
    g_lua.pushnumber(L,ok?1:0);return 1;
}
bool WantsGeneric(uint8_t* unit) {
    return g_runtime.enabled&&g_runtime.generic&&Owned(unit)&&!g_preparationFault;
}
int __cdecl Dispatch(const char* name,uint8_t* unit) {
    if(GetCurrentThreadId()!=g_gameThread) return g_dispatch(name,unit);
    const char* prior=g_nativeCallback;
    g_nativeCallback=name;
    int result=0;
    if(unit==g_current && !g_nativeFallback && WantsGeneric(unit)) {
        result=g_dispatch("SDGO_BattleAI_GenericUpdate",unit);
    } else result=g_dispatch(name,unit);
    g_nativeCallback=prior;
    return result;
}
Ray3* __fastcall RangedRay(uint8_t* fsm,void*,Ray3* out) {
    Ray3* ray=g_ray(fsm,out);
    __try {
        if(GetCurrentThreadId()!=g_gameThread) return ray;
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        auto found=g_actors.find(owner);
        if(!ray || !g_runtime.aim || !WantsGeneric(owner) ||
           found==g_actors.end() || !found->second.aimTarget ||
           GetTickCount64()-found->second.aimAt>1000) return ray;
        auto target=FindObject(found->second.aimTarget);
        if(!target || *reinterpret_cast<int*>(target+0x38)<=0) return ray;
        battle_ai::native::Capabilities caps={};
        if(!battle_ai::native::ReadCapabilities(owner,caps)) return ray;
        float speed=0;
        for(const auto& w:caps.weapons) if(w.valid && w.slot==caps.selectedSlot) {
            if(w.kind==battle_ai::native::WeaponKind::Melee) return ray;
            speed=g_runtime.lead?PredictionSpeed(owner,caps,w):0;
        }
        Vec3 point=AimPoint(target);
        Vec3 velocity=Velocity(target,found->second.aimTarget,point,GetTickCount64());
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
    bool neutral=fsm && *reinterpret_cast<uint8_t**>(fsm+8)==fsm+0x20;
    if(controlled && neutral) {
        // Remove the NPC's extra script-profile wait; the weapon FSM retains its
        // actual ammo, reload, animation and cooldown checks.
        *reinterpret_cast<int*>(unit+0x31F4)=0;
    }
    g_intent(unit);
    if(!controlled) return;
    found=g_actors.find(unit);
    if(found==g_actors.end()||!found->second.ownsIntent) return;
    auto& a=found->second;
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
void RecordRangedShot(uint8_t* fsm) {
    __try {
        if(!g_gameThread || GetCurrentThreadId()!=g_gameThread) return;
        auto owner=*reinterpret_cast<uint8_t**>(fsm+0xB2C);
        if(!owner || *reinterpret_cast<int*>(owner+0x38)<=0) return;
        g_rangedShots[owner]=GetTickCount64();
        if(g_rangedShots.size()>2048) {
            ULONGLONG now=GetTickCount64();
            for(auto it=g_rangedShots.begin();it!=g_rangedShots.end();) {
                if(now-it->second>5000) it=g_rangedShots.erase(it); else ++it;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}
uint32_t __fastcall RangedShot(uint8_t* fsm,void*,uint32_t event,char chained,char force) {
    RecordRangedShot(fsm);
    return g_shot(fsm,event,chained,force);
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
        if(!WantsGeneric(s.owner)||!s.weapon||!s.node||g_actors.find(s.owner)==g_actors.end()) return false;
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
};
std::string ConfigChunk(RuntimeSettings& next) {
    std::string code = "local config = {\n";
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
        code += key + "=" + std::to_string(value) + ",\n";
        if(key=="Enabled") next.enabled=value!=0;
        if(key=="GenericMode") next.generic=value!=0;
        if(key=="AimAssist") next.aim=value!=0;
        if(key=="ProjectileLead") next.lead=value!=0;
        if(key=="MaxLeadMs") next.maxLeadMs=static_cast<int>(value);
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
    if (L == g_loaded && Same(st,g_scriptTime) && Same(ct,g_configTime) && Same(ut,g_universalTime)) {
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
    RuntimeSettings next;
    next.generic=true;
    if (Run(L, ConfigChunk(next) + script + "\n" + universal)) {
        g_runtime=next;
        g_api->Log(g_mod, SDGO_LOG_INFO, "policies loaded (config.ini); generic mode %d",next.generic?1:0);
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
    g_actors.erase(unit);g_rangedShots.erase(unit);
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
    if(actor!=g_actors.end()) {
        if(!WantsGeneric(unit)) RestoreIntent(unit,actor->second);
        else if(g_runtime.aim && actor->second.ownsIntent) {
            *reinterpret_cast<float*>(unit+0x3204)=0;
            *reinterpret_cast<float*>(unit+0x3208)=0;
        }
    }
    bool result=g_think(unit);
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
        !api->HookCode(mod,"combat.projectile_update",reinterpret_cast<void*>(&ProjectileUpdate),reinterpret_cast<void**>(&g_projectile))) return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: capability-driven local battle bots; native projectiles and collisions");
    return 1;
}
