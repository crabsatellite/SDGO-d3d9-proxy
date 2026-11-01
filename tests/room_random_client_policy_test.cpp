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
    // A frozen pre-start RoomPilot is independent of the visible cache. This
    // was the missing regression: the visible cache alone already passed.
    std::array<uint8_t,kStride> frozen{};
    for(size_t i=0;i<frozen.size();++i)frozen[i]=static_cast<uint8_t>(i*17);
    Put<uint32_t>(frozen.data(),kOwner,1001);Put<uint32_t>(frozen.data(),0x49,15019);
    const auto original=frozen;
    CacheSync construction;
    Put<uint32_t>(unit.data(),0,11032);Put<uint64_t>(unit.data(),8,0x100000001ULL);
    Put<uint32_t>(unit.data(),0x52,765432); // full-width optional skill part
    notice=room_random_sync::Packet(0x0861,1001,0,8,unit);room[5]=8;
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,0).applied,"no notice/start never rewrites a construction source");
    construction.Process(c,0x0861,notice.data());
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,0).applied,"a notice alone cannot authorize a later construction");
    construction.Process(c,0x06C3,start.data());
    auto prepared=construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,0);
    Check(prepared.applied && prepared.before==15019 && prepared.after==11032,"frozen human source receives the authoritative model");
    auto expectedPilot=original;std::memcpy(expectedPilot.data()+0x49,unit.data(),unit.size());
    Check(prepared.pilot==expectedPilot,"whole active record including UID, skills and appearance is preserved; all other pilot bytes stay unchanged");
    Check(frozen==original,"frozen native source is never mutated");
    auto wrongEntry=start;Put<uint32_t>(wrongEntry.data(),0x20,1002);
    Check(!construction.PrepareConstruction(c,frozen.data(),wrongEntry.data()+0x20,0).applied,"another owner's source is rejected");
    wrongEntry=start;Put<uint32_t>(wrongEntry.data(),0x20,0x80000001u);
    Check(!construction.PrepareConstruction(c,frozen.data(),wrongEntry.data()+0x20,0).applied,"AI construction is unchanged");
    wrongEntry=start;wrongEntry[0x20+5]^=1;
    Check(!construction.PrepareConstruction(c,frozen.data(),wrongEntry.data()+0x20,0).applied,"changed start-entry metadata cannot reuse the previous authority");
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1).applied,"deck semantics must match the room mode");
    // A second human has their own pending full record and UID.
    Put<uint32_t>(room.data(),kSlotBase+kStride+kOwner,1002);
    Put<uint32_t>(room.data(),kSlotBase+kStride+0x49,15019);
    auto secondUnit=unit;Put<uint64_t>(secondUnit.data(),8,0x200000002ULL);
    auto second=room_random_sync::Packet(0x0861,1002,1,8,secondUnit);
    construction.Process(c,0x0861,second.data());
    Put<uint32_t>(start.data(),0x20+0x10F,1002);Put<uint32_t>(start.data(),0x2E+0x10F,11032);
    construction.Process(c,0x06C3,start.data());
    Put<uint32_t>(frozen.data(),kOwner,1002);
    prepared=construction.PrepareConstruction(c,frozen.data(),start.data()+0x20+0x10F,0);
    Check(prepared.applied && Read<uint64_t>(prepared.pilot.data(),0x51)==0x200000002ULL,"two humans using the same model retain distinct inventory UIDs");
    Put<uint32_t>(frozen.data(),kOwner,1001);
    room[5]=7;ship={};
    for(size_t i=0;i<4;++i) {
        const auto id=static_cast<uint32_t>(11032+i);
        Put<uint32_t>(ship.data(),0x58+i*0x99,id);
        Put<uint64_t>(ship.data(),0x60+i*0x99,0x300000000ULL+i);
        Put<uint64_t>(ship.data(),0x34+i*8,0x300000000ULL+i);
        Put<uint32_t>(start.data(),0x2E+i*0x32,id);
    }
    Put<uint64_t>(ship.data(),0x1C,0x300000000ULL);
    construction.Process(c,0x06C3,start.data());
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1).applied,"deck construction requires the complete ship notice");
    deck=room_random_sync::Packet(0x098B,1001,0,8,ship);
    construction.Process(c,0x098B,deck.data());construction.Process(c,0x06C3,start.data());
    prepared=construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1);
    expectedPilot=frozen;std::memcpy(expectedPilot.data()+0x49,unit.data(),unit.size());std::memcpy(expectedPilot.data()+0x146,ship.data(),ship.size());
    Check(prepared.applied && prepared.pilot==expectedPilot,"complete deck keeps leader, four UIDs and all per-unit data");
    wrongEntry=start;Put<uint32_t>(wrongEntry.data(),0x2E+0x32,99999);
    construction.Process(c,0x06C3,wrongEntry.data());
    Check(!construction.PrepareConstruction(c,frozen.data(),wrongEntry.data()+0x20,1).applied,"deck model mismatch is not corrected with a partial record");
    construction.Process(c,0x06C3,start.data());construction.Process(c,0x0861,restore.data());
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1).applied,"restoration invalidates the previous construction authorization");
    construction.Process(c,0x06C3,start.data());Put<uint32_t>(room.data(),0,9);
    Check(!construction.PrepareConstruction(c,frozen.data(),start.data()+0x20,1).applied,"room identity change rejects a previous construction snapshot");
    std::printf("%d CLIENT CACHE POLICY CHECKS, %d failures\n",checks,failures);return failures?1:0;
}
