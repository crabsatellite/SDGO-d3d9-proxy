#pragma once
#include <array>
#include <cstdint>
#include <cstring>

namespace room_random_client {
constexpr size_t kSlots=12,kStride=0x48E,kSlotBase=0x533,kOwner=0x429;
template<class T> T Read(const uint8_t* p,size_t at) {T v;std::memcpy(&v,p+at,sizeof(v));return v;}
struct SlotAccess {
    using Owner=uint32_t(__thiscall*)(uint8_t*,int);
    using Get=uint8_t*(__thiscall*)(uint8_t*,int);
    using Set=void(__thiscall*)(uint8_t*,int,const uint8_t*);
    Owner owner=nullptr;Get active=nullptr,ship=nullptr;Set setActive=nullptr,setShip=nullptr;
};
struct Context {
    uint8_t* profile=nullptr;uint8_t* room=nullptr;uint32_t local=0;unsigned source=0;
    uint8_t* model=nullptr;SlotAccess slots;
    uint32_t Owner(size_t slot) const {return model ? slots.owner(model,static_cast<int>(slot)) : Read<uint32_t>(room,kSlotBase+slot*kStride+kOwner);}
    uint8_t* Active(size_t slot) const {return model ? slots.active(model,static_cast<int>(slot)) : room+kSlotBase+slot*kStride+0x49;}
};
struct Result {unsigned applied=0,mismatched=0,reason=0,source=0;uint32_t owner=0,model=0,cached=0;};
class CacheSync {
    struct Pending {
        uint32_t owner=0,oldModel=0;
        bool unit=false,ship=false;
        std::array<uint8_t,0x99> active{};
        std::array<uint8_t,0x2BC> deck{};
    };
    uint8_t* room_=nullptr;
    uint32_t key_=0;
    std::array<Pending,kSlots> pending_{};
    std::array<uint32_t,kSlots> expected_{},previous_{};
    void Unit(const Context& c,size_t slot,Pending& p) {
        if(c.model) c.slots.setActive(c.model,static_cast<int>(slot),p.active.data());
        else std::memcpy(c.room+kSlotBase+slot*kStride+0x49,p.active.data(),p.active.size());
        if(p.owner==c.local) std::memcpy(c.profile+0x1A9,p.active.data(),p.active.size());
    }
    void Ship(const Context& c,size_t slot,Pending& p) {
        if(c.model) c.slots.setShip(c.model,static_cast<int>(slot),p.deck.data());
        else std::memcpy(c.room+kSlotBase+slot*kStride+0x146,p.deck.data(),p.deck.size());
        if(p.owner==c.local) std::memcpy(c.profile+0x2A6,p.deck.data(),p.deck.size());
    }
public:
    Result Process(const Context& c,uint16_t opcode,const uint8_t* packet) {
        Result result;result.source=c.source;
        if(!c.profile || !c.room || !c.local || !packet) {result.reason=1;return result;}
        if(Read<uint16_t>(packet,0)!=opcode) {result.reason=2;return result;}
        const uint32_t key=Read<uint32_t>(c.room,0);
        if(c.room!=room_ || key!=key_) {room_=c.room;key_=key;pending_={};expected_={};previous_={};}
        if(opcode==0x0861 || opcode==0x098B) {
            const uint32_t slot=Read<uint32_t>(packet,10),owner=Read<uint32_t>(packet,6);
            if(slot>=kSlots || !owner || (owner&0x80000000u)) {result.reason=3;return result;}
            if(c.Owner(slot)!=owner || !c.Active(slot)) {result.reason=4;return result;}
            auto& p=pending_[slot];
            if(p.owner!=owner) p={};
            p.owner=owner;
            if(opcode==0x0861) {
                p.oldModel=Read<uint32_t>(c.Active(slot),0);
                std::memcpy(p.active.data(),packet+0x12,p.active.size());p.unit=true;
                Unit(c,slot,p);result.model=Read<uint32_t>(p.active.data(),0);
            } else {
                std::memcpy(p.deck.data(),packet+0x12,p.deck.size());p.ship=true;
                Ship(c,slot,p);result.model=Read<uint32_t>(p.deck.data(),0x58);
            }
            result.applied=1;result.owner=owner;
        } else if(opcode==0x06C3) {
            expected_={};previous_={};
            for(size_t slot=0;slot<kSlots;++slot) {
                const auto* entry=packet+0x20+slot*0x10F;
                const uint32_t owner=Read<uint32_t>(entry,0),model=Read<uint32_t>(entry,0x0E);
                if(!owner || (owner&0x80000000u) || !model ||
                   c.Owner(slot)!=owner || !c.Active(slot)) continue;
                auto& p=pending_[slot];
                if(p.owner==owner && p.unit && Read<uint32_t>(p.active.data(),0)==model) {
                    if(p.ship && (c.room[5]==6 || c.room[5]==7)) Ship(c,slot,p);
                    Unit(c,slot,p);++result.applied;
                }
                const auto cached=Read<uint32_t>(c.Active(slot),0);
                if(cached!=model) {++result.mismatched;result.owner=owner;result.model=model;result.cached=cached;}
                else if(!result.mismatched) {result.owner=owner;result.model=model;result.cached=cached;}
                expected_[slot]=model;previous_[slot]=p.oldModel;
            }
        }
        return result;
    }
    bool ObserveFactory(uint32_t model,bool* expected) const {
        for(size_t i=0;i<kSlots;++i) if(expected_[i] && model==expected_[i]) {*expected=true;return true;}
        for(size_t i=0;i<kSlots;++i) if(expected_[i] && model==previous_[i]) {*expected=false;return true;}
        return false;
    }
};
}
