#pragma once
#include "cache_sync.h"

namespace room_random_client {
using RoomInfoFn=uint8_t*(__thiscall*)(uint8_t*);
struct Bindings {
    uint8_t** profile=nullptr;
    uint8_t** app=nullptr;
    const void* waitRoomVtable=nullptr;
    RoomInfoFn roomInfo=nullptr;
    SlotAccess normal,mission;
};
// The 3D room does not publish CAccountData+EC. Its current manager owns a
// CBaseRoomData object, whose verified native getter exposes the packed info.
// Never retain the manager/model pointer across scene transitions.
inline Context ResolveModel(const Bindings& b,uint8_t* model) {
    Context c;
    c.profile=b.profile ? *b.profile : nullptr;
    if(!c.profile)return c;
    c.local=Read<uint32_t>(c.profile,0x138);
    auto* vtable=model ? Read<uint8_t*>(model,0) : nullptr;
    if(!vtable || !b.roomInfo || Read<RoomInfoFn>(vtable,0x1C)!=b.roomInfo)return c;
    const auto ownerGetter=Read<SlotAccess::Owner>(vtable,0x20);
    const auto& slots=ownerGetter==b.normal.owner ? b.normal : b.mission;
    if(!ownerGetter || ownerGetter!=slots.owner || Read<SlotAccess::Get>(vtable,0x88)!=slots.active ||
       Read<SlotAccess::Get>(vtable,0x90)!=slots.ship || Read<SlotAccess::Set>(vtable,0x84)!=slots.setActive ||
       Read<SlotAccess::Set>(vtable,0x8C)!=slots.setShip) return c;
    c.room=b.roomInfo(model);c.source=2;c.model=model;c.slots=slots;return c;
}
inline Context Resolve(const Bindings& b) {
    Context c;
    c.profile=b.profile ? *b.profile : nullptr;
    if(!c.profile)return c;
    c.local=Read<uint32_t>(c.profile,0x138);
    c.room=Read<uint8_t*>(c.profile,0xEC);
    if(c.room){c.source=1;return c;}
    auto* app=b.app ? *b.app : nullptr;
    if(!app || Read<uint32_t>(app,0x4A0)!=18)return c;
    auto* owner=Read<uint8_t*>(app,0x4A4);
    if(!owner || Read<const void*>(owner,0)!=b.waitRoomVtable)return c;
    auto* manager=Read<uint8_t*>(owner,4);
    return ResolveModel(b,manager ? Read<uint8_t*>(manager,0x38) : nullptr);
}
}
