#pragma once
#include <array>
#include <vector>
#include <cstring>
#include "../mods/battle_ai/combat_math.h"
#include "../mods/battle_ai/native_navigation.h"
// Private native ABI world. No code from GOnline is executed by this fixture.
namespace native_world {
using battle_ai::Vec3;
using battle_ai::native::Ray3;
using battle_ai::native::Aabb;
inline lua_State* state=nullptr;
inline uint8_t* source=nullptr;
inline std::array<uint8_t,0x8500> document{};
inline uint8_t* documentPointer=document.data();
inline std::array<uint8_t,0x3300> enemy1{},enemy2{};
inline std::array<std::array<uint8_t,0x1400>,3> definitions{},fsms{};
inline std::array<std::array<uint8_t,0x300>,3> nodes{},scenes{};
inline std::array<std::array<uint8_t,0x800>,3> weapons{};
inline std::array<void*,16> unitVtable{},sceneVtable{};
inline bool blocked=false;
inline bool nullRay=false;
inline int pathMode=0;
inline void* protectedPath=nullptr;
inline int jumps=0,moves=0,frees=0,queries=0;
inline int lastWait=0;
inline std::array<uint8_t,0x300> wall{};
template<class T> inline void Put(uint8_t* p,size_t off,T value) { memcpy(p+off,&value,sizeof(value)); }
template<class T> inline void* Address(T method) {
    static_assert(sizeof(T)==sizeof(void*));void* result;memcpy(&result,&method,sizeof(result));return result;
}
struct Callbacks {
    __declspec(noinline) bool CanAttack(uint8_t*) { return true; }
    __declspec(noinline) bool Projectile(void* opaque) {
        auto p=reinterpret_cast<uint8_t*>(this);
        auto node=*reinterpret_cast<uint8_t**>(p+8);
        auto pos=reinterpret_cast<Vec3*>(node+0x88);
        pos->x+=45;
        Put(p,0x100,reinterpret_cast<uintptr_t>(opaque));
        return true;
    }
    __declspec(noinline) bool Homing(void* opaque) { return Projectile(opaque); }
    __declspec(noinline) int AttackIntent(char on) { reinterpret_cast<uint8_t*>(this)[0x312A]=on?1:0;return 1; }
    __declspec(noinline) void StopMotor() { reinterpret_cast<uint8_t*>(this)[0x30DC]=0; }
    __declspec(noinline) uint32_t Shot(uint32_t event,char chain,char force) {
        Put(reinterpret_cast<uint8_t*>(this),0x100,event+static_cast<uint32_t>(chain)+static_cast<uint32_t>(force));
        return 0xBEEFF00D;
    }
    __declspec(noinline) void Intent() {
        auto unit=reinterpret_cast<uint8_t*>(this);
        lastWait=*reinterpret_cast<int*>(unit+0x31F4);
        unit[0xDCD]=unit[0x312A]?1:0;
        Put(unit,0x3204,0.5f);Put(unit,0x31F4,60);
    }
    __declspec(noinline) Ray3* Ray(Ray3* out) {
        if(nullRay) return nullptr;
        out->origin={12,100,13};out->direction={0,0,1};return out;
    }
    __declspec(noinline) bool IsTerrain(int) { return false; }
    __declspec(noinline) void Query(void** out,int* count,int,const Aabb*) {
        ++queries;out[0]=wall.data();*count=blocked?1:0;
    }
    __declspec(noinline) bool Terrain(const Ray3*,float*) { return false; }
    __declspec(noinline) int Goal(const Vec3* point,int) {
        auto unit=reinterpret_cast<uint8_t*>(this);Put(unit,0x3078,*point);unit[0x30DC]=1;++moves;return 1;
    }
    __declspec(noinline) uint8_t* Find(uint32_t id) {
        if(id==1)return enemy1.data();if(id==2)return enemy2.data();return nullptr;
    }
    __declspec(noinline) Vec3* Path(uint32_t,Vec3,Vec3 end,Vec3*,uint32_t* count) {
        if(pathMode==1) return nullptr;
        if(pathMode==3) {
            protectedPath=VirtualAlloc(nullptr,4096,MEM_COMMIT|MEM_RESERVE,PAGE_NOACCESS);
            *count=2;return static_cast<Vec3*>(protectedPath);
        }
        auto p=static_cast<Vec3*>(HeapAlloc(GetProcessHeap(),0,sizeof(Vec3)*2));
        p[0]={200,0,200};p[1]=end;*count=2;return p;
    }
};
__declspec(noinline) inline void __cdecl Free(void* p) {
    ++frees;
    if(p==protectedPath) {VirtualFree(p,0,MEM_RELEASE);protectedPath=nullptr;}
    else HeapFree(GetProcessHeap(),0,p);
}
__declspec(noinline) inline bool __cdecl Segment(const Vec3*,const Vec3*,const Aabb*) { return true; }
__declspec(noinline) inline bool __cdecl Obb(const Ray3* ray,const void*,float* t) { *t=200;return ray->direction.z>0.02f; }
__declspec(noinline) inline bool __cdecl Sphere(const Ray3*,const void*,float*) { return false; }
__declspec(noinline) inline int __cdecl Dispatch(const char* name,uint8_t*) {
    int top=lua_gettop(state);lua_getglobal(state,name);lua_getglobal(state,"npc");
    int err=lua_pcall(state,1,1,0);
    if(err) std::printf("DISPATCH ERROR: %s\n",lua_tostring(state,-1));
    int value=err?0:static_cast<int>(lua_tonumber(state,-1));lua_settop(state,top);return value;
}
inline void Encode(uint8_t* unit,size_t perm,size_t bytes,size_t key,float value) {
    uint8_t raw[4];memcpy(raw,&value,4);
    for(unsigned i=0;i<4;++i){unit[perm+i]=static_cast<uint8_t>(i);unit[bytes+i]=raw[i]^0x5A;}
    unit[key]=0x5A;
}
inline void Configure(uint8_t* unit,size_t index,uint32_t id,int team,int hp,float x,float z) {
    memset(unit,0,0x3300);
    auto def=definitions[index].data(),fsm=fsms[index].data(),node=nodes[index].data(),scene=scenes[index].data();
    Put(unit,0,unitVtable.data());Put(unit,4,id);Put(unit,0x20,team);Put(unit,0x38,hp);Put(unit,0x3C,1000);
    Put(unit,8,node);Put(unit,0x34,scene);Put(unit,0x9C4,def);Put(unit,0x9CC,fsm);
    Put(node,0x88,Vec3{x,0,z});Put(scene,0,sceneVtable.data());Put(scene,0x0C,unit);
    Put(scene,0xBC,Aabb{{x-40,50,z-40},{x+40,150,z+40}});
    Put(def,0,uint32_t(987654+index));def[0x247]=3;
    for(int i=0;i<3;++i) {
        Put(def,0x249+i*4,uint32_t(101+i));
        Put(fsm,0x24C+i*0xC8,int16_t(30));Put(fsm,0x254+i*0xC8,int16_t(0));
        Put(fsm,0x258+i*0xC8,weapons[i].data());
    }
    Put(fsm,8,fsm+0x20);Put(fsm,0x240,int32_t(1));Put(fsm,0x248,weapons[1].data());Put(fsm,0xB2C,unit);
    Encode(unit,0xBE1,0xBE5,0xBF5,100);Encode(unit,0xBCC,0xBD0,0xBE0,100);
    unit[0x326C]=1;Put(unit,0x3238,uint32_t(3604001));
    unit[0x3291]=1;
}
struct ListNode { ListNode* next;ListNode* prev;uint8_t* unit; };
inline ListNode head{},a{},b{};
inline int SetFire(lua_State* L){source[0x312A]=lua_toboolean(L,2)?1:0;return 0;}
inline int SetTarget(lua_State* L){Put(fsms[0].data(),0x200,uint32_t(lua_tonumber(L,2)));return 0;}
inline int Stop(lua_State*){source[0x30DC]=0;return 0;}
inline int Change(lua_State* L){int i=static_cast<int>(lua_tonumber(L,2));if(i>=0&&i<3){Put(fsms[0].data(),0x240,i);Put(fsms[0].data(),0x248,weapons[i].data());}return 0;}
inline int Direction(lua_State*){++moves;source[0x30DC]=1;return 0;}
inline int Mode(lua_State*){return 0;}
inline int Jump(lua_State*){++jumps;return 0;}
inline void Initialize(uint8_t* unit,lua_State* L) {
    state=L;source=unit;blocked=false;jumps=moves=frees=queries=0;
    unitVtable[10]=Address(&Callbacks::CanAttack);sceneVtable[5]=Address(&Callbacks::IsTerrain);
    for(int i=0;i<3;++i) {
        auto w=weapons[i].data();Put(w,0,uint32_t(101+i));w[0x69]=1;w[0x6A]=i==0?1:3;
        Put(w,0x6C,int32_t(i==0?800:500));Put(w,0x70,uint16_t(i==0?0:20));
        Put(w,0x72,uint32_t(i==0?200:2000));Put(w,0x7C,uint16_t(i==0?0:30));
        Put(w,0x82,uint16_t(i==0?30:6));w[0x8F]=1;
    }
    Configure(unit,0,20,2,1000,0,0);Configure(enemy1.data(),1,1,1,1000,1000,0);
    Configure(enemy2.data(),2,2,1,100,1400,100);enemy2[0xDCD]=1;
    head={&a,&b,nullptr};a={&b,&head,enemy1.data()};b={&head,&a,enemy2.data()};
    Put(document.data(),0x48,&head);Put(document.data(),0x82C4,document.data());Put(document.data(),0x8364,document.data());
    Put(wall.data(),0,sceneVtable.data());Put(wall.data(),0xD4,int(1));Put(wall.data(),0xD8,wall.data()+0x180);
    lua_getglobal(L,"npc");
    auto method=[&](const char* name,lua_CFunction fn){lua_pushstring(L,name);lua_pushcfunction(L,fn);lua_settable(L,-3);};
    method("SetAttackOnOff",SetFire);method("SetAttackTarget",SetTarget);method("MoveStop",Stop);
    method("ChangeWeapon",Change);method("SetMoveTargetPos_Dir",Direction);method("SetMoveType",Mode);method("SetJumpTime",Jump);
    lua_pop(L,1);
}
inline void Symbols(FILE* f,void(*symbol)(FILE*,const char*,void*,bool)) {
    symbol(f,"npc.dispatch_ai",reinterpret_cast<void*>(&Dispatch),false);
    symbol(f,"npc.ranged_ray",Address(&Callbacks::Ray),false);
    symbol(f,"npc.attack_intent",Address(&Callbacks::Intent),false);
    symbol(f,"combat.ranged_shot",Address(&Callbacks::Shot),false);
    symbol(f,"combat.projectile_update",Address(&Callbacks::Projectile),false);
    symbol(f,"npc.set_move_goal",Address(&Callbacks::Goal),false);
    symbol(f,"npc.set_attack_intent",Address(&Callbacks::AttackIntent),false);
    symbol(f,"npc.move_stop",Address(&Callbacks::StopMotor),false);
    symbol(f,"navigation.route",Address(&Callbacks::Path),false);
    symbol(f,"crt.operator_delete",reinterpret_cast<void*>(&Free),false);
    symbol(f,"um.find_object",Address(&Callbacks::Find),false);
    symbol(f,"collision.query",Address(&Callbacks::Query),false);
    symbol(f,"collision.segment_aabb",reinterpret_cast<void*>(&Segment),false);
    symbol(f,"collision.terrain_ray",Address(&Callbacks::Terrain),false);
    symbol(f,"collision.ray_obb",reinterpret_cast<void*>(&Obb),false);
    symbol(f,"collision.ray_sphere",reinterpret_cast<void*>(&Sphere),false);
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(f,"[game.document]\nkind=data\nrva=0x%08lX\n\n",static_cast<unsigned long>(reinterpret_cast<uintptr_t>(&documentPointer)-base));
}
}
