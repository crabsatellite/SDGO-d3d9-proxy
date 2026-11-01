#include <array>
#include <cstdio>
#include <cstring>
#include "../mods/room_random/loadout_sync.h"
#include "../mods/room_random_client/cache_sync.h"
namespace {
int checks=0,failures=0;
void Check(bool ok,const char* text){++checks;if(!ok){++failures;std::printf("FAIL %s\n",text);}}
template<class T> void Put(uint8_t* p,size_t n,T v){std::memcpy(p+n,&v,sizeof(v));}
}
int main() {
    using namespace room_random_client;
    std::array<uint8_t,0x5DA8> profile{};std::array<uint8_t,0x4000> room{};
    Context c{profile.data(),room.data(),1001};CacheSync sync;
    Put<uint32_t>(room.data(),0,7);room[4]=2;room[5]=8;
    Put<uint32_t>(room.data(),0x95C,1001);Put<uint32_t>(room.data(),0x57C,15019);
    std::array<uint8_t,0x99> unit{};Put<uint32_t>(unit.data(),0,11032);Put<uint64_t>(unit.data(),8,0x123456789ABCuLL);
    auto notice=room_random_sync::Packet(0x0861,1001,0,7,unit);
    const auto before=room;
    auto wrong=notice;Put<uint32_t>(wrong.data(),6,1002);
    Check(sync.Process(c,0x0861,wrong.data()).reason==4 && room==before,"wrong owner is ignored without touching another participant");
    wrong=notice;Put<uint32_t>(wrong.data(),10,12);
    Check(sync.Process(c,0x0861,wrong.data()).reason==3 && room==before,"out-of-range slot is rejected");
    wrong=notice;Put<uint32_t>(wrong.data(),6,0x80000001u);
    Check(sync.Process(c,0x0861,wrong.data()).reason==3 && room==before,"AI is not rewritten as a human");
    Check(sync.Process(c,0x0861,notice.data()).applied==1,"valid full notification applies immediately");
    Check(Read<uint32_t>(profile.data(),0x1A9)==11032 && Read<uint64_t>(profile.data(),0x1B1)==0x123456789ABCuLL,"own profile model and 64-bit UID agree");
    std::array<uint8_t,0xCD4> start{};Put<uint16_t>(start.data(),0,0x06C3);
    Put<uint32_t>(start.data(),0x20,1001);Put<uint32_t>(start.data(),0x2E,11032);
    Put<uint32_t>(room.data(),0x57C,15019); // a pending old UI command ran late
    auto result=sync.Process(c,0x06C3,start.data());
    Check(result.applied==1 && !result.mismatched && Read<uint32_t>(room.data(),0x57C)==11032,"start boundary reapplies the full pending record before native construction");
    bool expected=false;Check(sync.ObserveFactory(11032,&expected)&&expected,"native factory can report the authoritative model");
    Check(sync.ObserveFactory(15019,&expected)&&!expected,"native factory can detect an old cached model");
    room[5]=7;std::array<uint8_t,0x2BC> ship{};
    Put<uint64_t>(ship.data(),0x1C,0x123456789ABCuLL);Put<uint64_t>(ship.data(),0x34,0x123456789ABCuLL);
    Put<uint32_t>(ship.data(),0x58,11032);
    auto deck=room_random_sync::Packet(0x098B,1001,0,7,ship);
    Check(sync.Process(c,0x098B,deck.data()).applied==1,"complete temporary ship applies");
    Check(std::memcmp(room.data()+0x679,ship.data(),ship.size())==0 && std::memcmp(profile.data()+0x2A6,ship.data(),ship.size())==0,"room and own profile retain the full ship header and records");
    Put<uint32_t>(room.data(),0,8);Put<uint32_t>(room.data(),0x57C,15019);
    result=sync.Process(c,0x06C3,start.data());
    Check(result.applied==0 && result.mismatched==1 && Read<uint32_t>(room.data(),0x57C)==15019,"a new room never reuses an old pending choice or fabricates a partial record");
    Put<uint32_t>(unit.data(),0,15019);Put<uint64_t>(unit.data(),8,777);
    auto restore=room_random_sync::Packet(0x0861,1001,0,8,unit);
    Check(sync.Process(c,0x0861,restore.data()).applied==1 && Read<uint32_t>(profile.data(),0x1A9)==15019,"restore notification returns the original selection");
    std::printf("%d CLIENT CACHE POLICY CHECKS, %d failures\n",checks,failures);return failures?1:0;
}
