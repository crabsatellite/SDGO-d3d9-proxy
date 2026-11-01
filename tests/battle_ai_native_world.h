#pragma once
#include <array>
#include <vector>
#include <cstring>
#include <atomic>
#include "../mods/battle_ai/combat_math.h"
#include "../mods/battle_ai/native_navigation.h"
#include "../mods/battle_ai/defence_math.h"
// Private native ABI world. No code from GOnline is executed by this fixture.
namespace native_world {
using battle_ai::Vec3;
using battle_ai::native::Ray3;
using battle_ai::native::Aabb;
inline lua_State* state=nullptr;
inline uint8_t* source=nullptr;
inline std::array<uint8_t,0x8500> document{};
inline uint8_t* documentPointer=document.data();
inline std::array<uint8_t,0x500> app{};
inline std::array<uint8_t,0x40> service{},mode{};
inline std::array<uint8_t,0xA4> room{};
inline wchar_t title[32]={};
inline std::array<uint8_t,0x3300> enemy1{},enemy2{};
inline std::array<std::array<uint8_t,0x1400>,3> definitions{},fsms{};
inline std::array<std::array<uint8_t,0x1400>,3> movements{};
inline std::array<std::array<uint8_t,0x300>,3> nodes{},scenes{};
inline std::array<std::array<uint8_t,0x800>,4> weapons{};
inline std::array<void*,16> unitVtable{},sceneVtable{};
inline bool blocked=false;
inline bool coverWall=false;
inline bool nullRay=false;
inline int pathMode=0;
inline void* protectedPath=nullptr;
inline int jumps=0,moves=0,frees=0,queries=0;
inline int lastWait=0;
inline int trackEntries=0,trackUpdates=0,swingEntries=0,rotations=0,faces=0;
inline int swingInput=-1;
inline int attackAdmissions=0;
inline int levelApplications=0,partApplications=0;
inline uint32_t lastPart=0;
inline int ordinaryNotices=0;
inline std::atomic<bool> stallOrdinary{false},ordinaryEntered{false},releaseOrdinary{false};
inline bool deferSwitch=false;
inline int requestedSlot=-1,switchRequests=0;
inline void* messageEntry=nullptr;
inline void* contactReturn=nullptr;
inline std::array<uint8_t,0x300> wall{};
template<class T> inline void Put(uint8_t* p,size_t off,T value) { memcpy(p+off,&value,sizeof(value)); }
__declspec(noinline) inline uint8_t* __cdecl App() {return app.data();}
inline void SetRoomTitle(const wchar_t* value,bool small=false) {
    Put(app.data(),0x4A4,service.data());Put(service.data(),0x0C,mode.data());Put(mode.data(),8,room.data());
    memset(room.data(),0,room.size());
    auto length=wcslen(value);
    if(small&&length<8) memcpy(room.data(),value,(length+1)*sizeof(wchar_t));
    else {wcscpy_s(title,value);Put(room.data(),0,title);}
    Put(room.data(),0x10,static_cast<uint32_t>(length));Put(room.data(),0x14,uint32_t(small?7:31));
}
template<class T> inline void* Address(T method) {
    static_assert(sizeof(T)==sizeof(void*));void* result;memcpy(&result,&method,sizeof(result));return result;
}
struct Callbacks {
    __declspec(noinline) void SetLevel(int level) {++levelApplications;Put(reinterpret_cast<uint8_t*>(this),0xB0C,level);}
    __declspec(noinline) void ApplyPart(uint32_t part) {++partApplications;lastPart=part;}
    __declspec(noinline) int SpecialNotice(const uint8_t*) {return 0x51A7;}
    __declspec(noinline) int MeleeNotice(const uint8_t*) {return 0x51B1;}
    __declspec(noinline) int RangedNotice(const uint8_t*) {return 0x51B2;}
    __declspec(noinline) int SkillNotice(const uint8_t* packet) {Put(source,0xBC8,*reinterpret_cast<const int32_t*>(packet+0x1F));return 0x51D1;}
    __declspec(noinline) int SpStateNotice(const uint8_t* packet) {
        ++ordinaryNotices;
        if(stallOrdinary) {ordinaryEntered=true;while(!releaseOrdinary) Sleep(1);}
        if(source&&*reinterpret_cast<const uint32_t*>(packet+6)==*reinterpret_cast<uint32_t*>(source+0x2B68))
            Put(source,0xBC8,*reinterpret_cast<const int32_t*>(packet+0xB));
        return 0x51D0;
    }
    __declspec(noinline) uint32_t Canonical(uint32_t pilot) {return source&&*reinterpret_cast<uint32_t*>(source+0x2B68)==pilot?20:0;}
    __declspec(noinline) void Receive(const uint8_t* message) {
        auto unit=reinterpret_cast<uint8_t*>(this);
        if(*reinterpret_cast<const uint16_t*>(message)==0x18) Put(unit,0xBC8,*reinterpret_cast<const int32_t*>(message+0x10));
    }
    __declspec(noinline) void FacingUpdate() {
        auto unit=reinterpret_cast<uint8_t*>(this);auto node=*reinterpret_cast<uint8_t**>(unit+8);
        auto cache=*reinterpret_cast<Vec3*>(unit+0x3090);
        Put(node,0x6C,battle_ai::Horizontal(cache));Put(unit,0xD98,0.0f);
    }
    __declspec(noinline) void AttackAdmission() {++attackAdmissions;}
    __declspec(noinline) bool PlayerSpGate() {
        auto owner=*reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(this)+0xB2C);
        return *reinterpret_cast<int*>(owner+0xBC8)>29999;
    }
    __declspec(noinline) void TrackingEntry(void*) { ++trackEntries; }
    __declspec(noinline) void TrackingUpdate(void*) { ++trackUpdates; }
    __declspec(noinline) void SwingEntry(void*) { ++swingEntries; }
    __declspec(noinline) void SwingUpdate(void* event) {swingInput=static_cast<uint8_t*>(event)[0xD];}
    __declspec(noinline) bool Message(uint32_t id,void*,uint32_t) {return id==1||id==2||id==20;}
    __declspec(noinline) void QueueSwing() {auto fsm=reinterpret_cast<uint8_t*>(this);Put(fsm,8,fsm+0x5C);}
    __declspec(noinline) void Face() {++faces;}
    __declspec(noinline) void Rotate(float degrees,int dispatch) {
        ++rotations;auto unit=reinterpret_cast<uint8_t*>(this);auto cache=*reinterpret_cast<Vec3*>(unit+0x3090);
        double radians=degrees*battle_ai::kPi/180;
        Vec3 rotated={static_cast<float>(cache.x*std::cos(radians)+cache.z*std::sin(radians)),cache.y,static_cast<float>(cache.z*std::cos(radians)-cache.x*std::sin(radians))};
        Put(unit,0x3090,rotated);Put(unit,0xD98,static_cast<float>(radians));Put(unit,0x100,dispatch);
    }
    __declspec(noinline) void Receipts(void*) {Shot(0,0,0);Shot(0,1,0);Shot(0,0,1);}
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
        ++queries;out[0]=wall.data();*count=(blocked||coverWall)?1:0;
    }
    __declspec(noinline) bool Terrain(const Ray3*,float*) { return false; }
    __declspec(noinline) int Goal(const Vec3* point,int) {
        auto unit=reinterpret_cast<uint8_t*>(this);Put(unit,0x3078,*point);unit[0x30DC]=1;++moves;return 1;
    }
    __declspec(noinline) uint8_t* Find(uint32_t id) {
        if(id==20)return source;if(id==1)return enemy1.data();if(id==2)return enemy2.data();return nullptr;
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
// The caller anchor comes from the actual instruction following the fixture's
// native-dispatch call. This exercises strict caller filtering, not a test flag.
__declspec(naked) inline bool __cdecl EmitContact(uint8_t*,uint32_t,void*,uint32_t) {
    __asm {
        mov ecx, dword ptr [esp+4]
        lea eax, contactAfter
        mov contactReturn, eax
        push dword ptr [esp+16]
        push dword ptr [esp+16]
        push dword ptr [esp+16]
        call dword ptr [messageEntry]
    contactAfter:
        ret
    }
}
__declspec(noinline) inline void __cdecl Free(void* p) {
    ++frees;
    if(p==protectedPath) {VirtualFree(p,0,MEM_RELEASE);protectedPath=nullptr;}
    else HeapFree(GetProcessHeap(),0,p);
}
__declspec(noinline) inline bool __cdecl Segment(const Vec3*,const Vec3*,const Aabb*) { return true; }
__declspec(noinline) inline bool __cdecl Obb(const Ray3* ray,const void*,float* t) {
    if(coverWall) {
        if(std::fabs(ray->direction.x)<0.01f) return false;
        *t=(500-ray->origin.x)/ray->direction.x;
        return *t>=0&&ray->origin.z+*t*ray->direction.z<=50;
    }
    *t=200;return ray->direction.z>0.02f;
}
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
    auto movement=movements[index].data();Put(unit,0x9C8,movement);Put(movement,4,movement+0x20);Put(movement,8,uint32_t(0));
    Put(node,0x88,Vec3{x,0,z});Put(scene,0,sceneVtable.data());Put(scene,0x0C,unit);
    Put(node,0x6C,Vec3{0,0,1});Put(unit,0x3090,Vec3{0,0,1});
    Put(scene,0xBC,Aabb{{x-40,50,z-40},{x+40,150,z+40}});
    Put(def,0,uint32_t(987654+index));def[0x247]=3;
    for(int i=0;i<3;++i) {
        Put(def,0x249+i*4,uint32_t(101+i));
        Put(fsm,0x24C+i*0xC8,int16_t(30));Put(fsm,0x254+i*0xC8,int16_t(0));
        Put(fsm,0x258+i*0xC8,weapons[i].data());
    }
    Put(fsm,4,fsm+0x20);Put(fsm,8,uint32_t(0));Put(fsm,0x240,int32_t(1));Put(fsm,0x248,weapons[1].data());Put(fsm,0xB2C,unit);Put(fsm,0xB30,movement);
    Encode(unit,0xBE1,0xBE5,0xBF5,100);Encode(unit,0xBCC,0xBD0,0xBE0,100);
    unit[0x326C]=1;Put(unit,0x3238,uint32_t(3604001));
    unit[0x3291]=1;
}
struct ListNode { ListNode* next;ListNode* prev;uint8_t* unit; };
inline ListNode head{},a{},b{};
inline int SetFire(lua_State* L){source[0x312A]=lua_toboolean(L,2)?1:0;return 0;}
inline int SetTarget(lua_State* L){Put(fsms[0].data(),0x200,uint32_t(lua_tonumber(L,2)));return 0;}
inline int Stop(lua_State*){source[0x30DC]=0;return 0;}
inline int Change(lua_State* L){int i=static_cast<int>(lua_tonumber(L,2));requestedSlot=i;++switchRequests;if(!deferSwitch&&i>=0&&i<4){Put(fsms[0].data(),0x240,i);Put(fsms[0].data(),0x248,weapons[i].data());}return 0;}
inline int Direction(lua_State*){++moves;source[0x30DC]=1;return 0;}
inline int Mode(lua_State*){return 0;}
inline int Jump(lua_State*){++jumps;return 0;}
inline void Initialize(uint8_t* unit,lua_State* L) {
    state=L;source=unit;blocked=false;jumps=moves=frees=queries=0;
    unitVtable[10]=Address(&Callbacks::CanAttack);sceneVtable[5]=Address(&Callbacks::IsTerrain);
    unitVtable[7]=Address(&Callbacks::Receive);
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
    symbol(f,"loadout.set_unit_level",Address(&Callbacks::SetLevel),false);
    symbol(f,"loadout.apply_skill_part",Address(&Callbacks::ApplyPart),false);
    symbol(f,"game.sp_state_notice",Address(&Callbacks::SpStateNotice),false);
    symbol(f,"game.special_notice",Address(&Callbacks::SpecialNotice),false);
    symbol(f,"game.special_melee_notice",Address(&Callbacks::MeleeNotice),false);
    symbol(f,"game.special_ranged_notice",Address(&Callbacks::RangedNotice),false);
    symbol(f,"game.skill_state_notice",Address(&Callbacks::SkillNotice),false);
    symbol(f,"um.object_id_from_pilot",Address(&Callbacks::Canonical),false);
    symbol(f,"npc.facing_update",Address(&Callbacks::FacingUpdate),false);
    symbol(f,"combat.attack_admission",Address(&Callbacks::AttackAdmission),false);
    symbol(f,"combat.player_sp_gate",Address(&Callbacks::PlayerSpGate),false);
    SetRoomTitle(L"",true);symbol(f,"game.app",reinterpret_cast<void*>(&App),false);
    symbol(f,"npc.tracking_entry",Address(&Callbacks::TrackingEntry),false);
    symbol(f,"npc.tracking_update",Address(&Callbacks::TrackingUpdate),false);
    symbol(f,"npc.swing_entry",Address(&Callbacks::SwingEntry),false);
    symbol(f,"npc.swing_update",Address(&Callbacks::SwingUpdate),false);
    messageEntry=Address(&Callbacks::Message);
    EmitContact(document.data(),0,nullptr,0);
    symbol(f,"um.deliver_message",messageEntry,false);
    symbol(f,"combat.melee_contact_return",contactReturn,false);
    symbol(f,"combat.queue_melee_swing",Address(&Callbacks::QueueSwing),false);
    symbol(f,"npc.face_target",Address(&Callbacks::Face),false);
    symbol(f,"unit.rotate_y",Address(&Callbacks::Rotate),false);
    symbol(f,"combat.fire_receipts",Address(&Callbacks::Receipts),false);
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
