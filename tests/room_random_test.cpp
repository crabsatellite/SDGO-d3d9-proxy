#include "ai_test_support.h"
#include "../mods/room_random/room_random.cpp"
#include "sdgo/owned_unit_record.h"
#include "native_title_image.h"
#include "room_random_native_copy.h"
#include <functional>
#include <set>

namespace room_random_inventory {
bool Read(const Settings&,const std::vector<uint32_t>&,
          std::vector<sdgo::random_loadout::Candidate>&,std::string&) { return false; }
}

namespace fixture {
std::vector<uint8_t> room(0x33000),preview(0x33000),lobby(0x2218),lobbyProc(0x100);
std::vector<uint8_t> client(0x4000);
std::vector<std::vector<uint8_t>> notices;
int noticeCalls=0,failNotice=0;
using CopyNoticeFn=std::function<void(std::vector<uint8_t>&,const uint8_t*,uint16_t)>;
CopyNoticeFn nativeCopyNotice;
std::vector<policy::Candidate> inventory;
std::map<uint32_t,std::array<uint8_t,64>> metadata;
std::vector<std::string> hooks;
std::vector<uint32_t> requestedOwners;
std::map<uint32_t,std::vector<uint32_t>> built;
int reads=0,builds=0,releases=0,failAfter=0,validationError=0;
bool readFailure=false,changeOwner=false;
bool changeReady=false;
bool addAiAfterValidation=false;
bool nativeOverrideSecond=false;
std::string fullTitle;
uint32_t __cdecl FullTitle(uint32_t owner,char* out,uint32_t capacity) {
    if(owner!=1001 || fullTitle.empty() || capacity<=fullTitle.size()) return 0;
    std::memcpy(out,fullTitle.c_str(),fullTitle.size()+1);
    return static_cast<uint32_t>(fullTitle.size());
}
const SdgoRoomTitleApi fullTitleApi{sizeof(SdgoRoomTitleApi),1,&FullTitle};

uint8_t* Info(int slot) { return room.data()+kSlotBase+slot*kSlotStride; }
void Title(std::vector<uint8_t>& dst,const char* text,size_t lengthOffset=kTitleLength,size_t textOffset=kTitle) {
    const size_t n=(std::min)(std::strlen(text),kTitleMax);
    dst[lengthOffset]=static_cast<uint8_t>(n);
    std::memset(dst.data()+textOffset,0,kTitleMax);
    std::memcpy(dst.data()+textOffset,text,n);
}
policy::Candidate Unit(uint32_t owner,uint32_t id,uint8_t rank,uint8_t points) {
    policy::Candidate u; u.owner=owner; u.unit=id; u.rank=rank; u.points=points; u.eligible=true;
    u.serial=(uint64_t{1}<<40)+owner*100+id;
    sdgo::owned_unit_record::Row raw;
    raw.unit=id; raw.serial=u.serial; raw.slot=static_cast<uint16_t>(id); raw.level=13;
    raw.normalMax=4; raw.energy=2000; raw.colours[0]=static_cast<uint16_t>(owner);
    raw.customSkills[0]=0x7730; raw.locked=1;
    u.data=sdgo::owned_unit_record::Pack(raw);
    metadata[id][6]=rank; metadata[id][0xB]=points;
    return u;
}
bool Inventory(const room_random_inventory::Settings&,const std::vector<uint32_t>& owners,
               std::vector<policy::Candidate>& out,std::string& error) {
    ++reads; requestedOwners=owners;
    if(readFailure) { error="synthetic query timeout"; return false; }
    out=inventory;
    if(changeOwner) Put<uint32_t>(Info(1),kOwner,9090);
    if(changeReady) Info(1)[0x42F]=0;
    return true;
}
uint8_t* __fastcall Lookup(void*,void*,uint32_t id) {
    auto it=metadata.find(id); return it==metadata.end()?nullptr:it->second.data();
}
int __fastcall ValidateNative(void*,void*,uint8_t* snapshot,int slot) {
    if(validationError) return validationError;
    if(!UsesDeck(snapshot[5])) {
        float energy=Get<float>(snapshot+kSlotBase+slot*kSlotStride,kActive+74);
        if(energy<=0) return 0xFB4;
    }
    return 0;
}
int __fastcall BuildNative(void*,void*,uint8_t*,uint8_t* info,uint8_t*,uint8_t,uint8_t mode) {
    ++builds;
    const uint32_t owner=Get<uint32_t>(info,kOwner);
    std::vector<uint32_t> ids;
    if(UsesDeck(mode)) {
        unsigned points=0;
        for(size_t i=0;i<4;++i) {
            const uint8_t* p=info+kDeck+i*0x99;
            uint32_t id=Get<uint32_t>(p,0);
            if(!id) continue;
            CHECK(metadata.find(id)!=metadata.end());
            points+=metadata[id][0xB]; ids.push_back(id);
            CHECK(Get<uint64_t>(p,8)>UINT32_MAX);
        }
        CHECK(points==10); CHECK(ids.size()<=4);
        CHECK(Get<uint64_t>(info,kActiveSerial)==Get<uint64_t>(info+kDeck,8));
    } else ids.push_back(Get<uint32_t>(info+kActive,0));
    for(uint32_t id:ids)
        CHECK(std::any_of(inventory.begin(),inventory.end(),[&](const auto& u){return u.owner==owner && u.unit==id;}));
    built[owner]=ids;
    return 1;
}
int __fastcall BroadcastNative(uint8_t* actual,void*,uint8_t* packet,uint32_t length,uint32_t* sent) {
    ++noticeCalls;*sent=0;
    if(failNotice==noticeCalls) return 0;
    const uint16_t opcode=Get<uint16_t>(packet,0);
    CHECK(opcode==0x0861 || opcode==0x098B);
    CHECK(length==(opcode==0x0861 ? 0xABu : 0x2CEu));
    const uint32_t slot=Get<uint32_t>(packet,10),owner=Get<uint32_t>(packet,6);
    CHECK(slot<12);CHECK((owner&kAiBit)==0);
    CHECK(Get<uint32_t>(actual+kSlotBase+slot*kSlotStride,kOwner)==owner);
    CHECK(Get<uint32_t>(packet,14)==Get<uint32_t>(actual,0));
    notices.emplace_back(packet,packet+length);
    if(nativeCopyNotice) nativeCopyNotice(client,packet,opcode);
    else std::memcpy(client.data()+0x5D7+slot*0x48E+(opcode==0x0861 ? 0x49 : 0x146),
                     packet+0x12,opcode==0x0861 ? 0x99 : 0x2BC);
    for(unsigned i=0;i<12;++i) {
        const uint32_t id=Get<uint32_t>(actual,kSlotBase+i*kSlotStride+kOwner);
        if(id && !(id&kAiBit)) ++*sent;
    }
    Put<uint32_t>(packet,2,1001); // the native broadcaster rewrites recipient routing
    return 1;
}
void __fastcall ReleaseNative(uint8_t*,void*) { ++releases; }
int __fastcall DispatchNative(void* proc,void*,int,void*,uint32_t*,uint32_t*) {
    preview=room;
    for(int slot=0;slot<12;++slot) {
        const uint32_t owner=Get<uint32_t>(preview.data()+kSlotBase+slot*kSlotStride,kOwner);
        if(!owner || (owner&kAiBit)) continue;
        const int valid=Validate(proc,nullptr,preview.data(),slot);
        if(valid) return valid;
    }
    if(addAiAfterValidation) Put<uint32_t>(room.data(),kSlotBase+2*kSlotStride+kOwner,kAiBit|123);
    for(int slot=0;slot<12;++slot) {
        const uint32_t owner=Get<uint32_t>(Info(slot),kOwner);
        if(!owner || (owner&kAiBit)) continue;
        if(nativeOverrideSecond && slot==1) {
            Put<uint32_t>(Info(slot),kActive,99999);
            Put<uint32_t>(Info(slot),kDeck,99999);
        }
        if(!Build(proc,nullptr,room.data(),Info(slot),room.data()+0x3CE4+slot*0x1F54,room[4],room[5])) return kStartError;
        if(failAfter && builds>=failAfter) return kStartError;
    }
    return 0;
}
int __fastcall SpecialNative(void* proc,void*,void*,uintptr_t) {
    return DispatchNative(proc,nullptr,0,nullptr,nullptr,nullptr);
}
int __fastcall LobbyFind(void*,void*,const uint32_t*,int* index) { *index=1;return 1; }
uint8_t* __fastcall LobbyRoom(void*,void*,int,uint16_t) { return lobby.data(); }
void __fastcall LobbyUnlock(void*,void*,int) {}

void Reset(uint8_t mode=1,const char* title="!rand") {
    g_titleApi=nullptr;fullTitle.clear();
    g_saved.clear();g_history.clear();g_attempt=nullptr;g_token=0;
    std::fill(room.begin(),room.end(),uint8_t{0});std::fill(lobby.begin(),lobby.end(),uint8_t{0});
    inventory.clear();metadata.clear();built.clear();requestedOwners.clear();ai_test::logs.clear();
    notices.clear();noticeCalls=failNotice=0;
    std::fill(client.begin(),client.end(),uint8_t{0xCC});
    reads=builds=releases=failAfter=validationError=0;readFailure=changeOwner=changeReady=addAiAfterValidation=nativeOverrideSecond=false;
    Put<uint32_t>(room.data(),0,0x10077); room[4]=1;room[5]=mode;
    Title(room,title);
    for(int s=0;s<2;++s) {
        Put<uint32_t>(Info(s),kOwner,1001u+s);
        Put<uint32_t>(Info(s),kActive,880u+s);
        Put<uint64_t>(Info(s),kActive+8,10000u+s);
        Put<float>(Info(s),kActive+74,0.0f); // random preflight must replace the empty selected unit
        Info(s)[0x42F]=1; // readiness must remain untouched
    }
    const std::array<uint8_t,7> ranks{4,4,3,2,2,1,1},points{5,5,4,3,3,2,2};
    for(uint32_t owner:{1001u,1002u})
        for(size_t i=0;i<ranks.size();++i)
            inventory.push_back(Unit(owner,(owner-1000)*100+static_cast<uint32_t>(i+1),ranks[i],points[i]));
    g_api=&ai_test::api;g_mod=nullptr;g_log=false;
    g_inventory=&Inventory;g_dispatch=reinterpret_cast<DispatchFn>(&DispatchNative);
    g_special=reinterpret_cast<SpecialFn>(&SpecialNative);
    g_validate=reinterpret_cast<ValidateFn>(&ValidateNative);g_build=reinterpret_cast<BuildFn>(&BuildNative);
    g_release=reinterpret_cast<ReleaseFn>(&ReleaseNative);g_lookup=reinterpret_cast<UnitLookupFn>(&Lookup);
    g_broadcast=reinterpret_cast<BroadcastFn>(&BroadcastNative);
    g_unitTable=reinterpret_cast<void*>(1);
    g_lobby=nullptr;g_lobbyFind=nullptr;g_lobbyRoom=nullptr;g_lobbyUnlock=nullptr;
    for(unsigned i=0;i<2;++i) std::memcpy(client.data()+0x5D7+i*0x48E,Info(i),0x48E);
}
int Start() {
    if(room[5]==13 || room[5]==14) return SpecialStart(nullptr,nullptr,room.data(),0);
    uint32_t key=0,index=0;return Dispatch(nullptr,nullptr,0,nullptr,&key,&index);
}
const wchar_t* Directory(SdgoMod*) { return L"Z:\\sdgo-offline-no-config\\room_random"; }
uint32_t Setting(SdgoMod*,const char*,const char* def,char* out,uint32_t size) {
    strcpy_s(out,size,def); return static_cast<uint32_t>(std::strlen(def));
}
void* Symbol(SdgoMod*,const char* key) {
    if(std::strcmp(key,"battle.room_random_broadcast")==0) return reinterpret_cast<void*>(&BroadcastNative);
    if(std::strcmp(key,"battle.unit_lookup")==0) return reinterpret_cast<void*>(&Lookup);
    if(std::strcmp(key,"battle.unit_table")==0) return reinterpret_cast<void*>(1);
    if(std::strcmp(key,"battle.lobby_proc")==0) return lobbyProc.data();
    if(std::strcmp(key,"battle.lobby_map_find")==0) return reinterpret_cast<void*>(&fixture::LobbyFind);
    if(std::strcmp(key,"battle.lobby_room")==0) return reinterpret_cast<void*>(&fixture::LobbyRoom);
    if(std::strcmp(key,"battle.lobby_release")==0) return reinterpret_cast<void*>(&fixture::LobbyUnlock);
    return nullptr;
}
int Hook(SdgoMod*,const char* key,void*,void** original) {
    hooks.emplace_back(key);
    if(std::strcmp(key,"battle.start_dispatch")==0) *original=reinterpret_cast<void*>(&DispatchNative);
    else if(std::strcmp(key,"battle.start_special")==0) *original=reinterpret_cast<void*>(&SpecialNative);
    else if(std::strcmp(key,"battle.validate_unit")==0) *original=reinterpret_cast<void*>(&ValidateNative);
    else if(std::strcmp(key,"battle.build_human_unit")==0) *original=reinterpret_cast<void*>(&BuildNative);
    else if(std::strcmp(key,"battle.release_pilots")==0) *original=reinterpret_cast<void*>(&ReleaseNative);
    else return 0;
    return 1;
}
std::vector<uint8_t> Sources() {
    std::vector<uint8_t> result;
    for(int i=0;i<2;++i) result.insert(result.end(),Info(i),Info(i)+kSlotStride);
    return result;
}
} // namespace fixture

// Execute only the native start-packet's model-selection branch in this test
// process. No game entry point, imports, networking or database is invoked.
__declspec(naked) void __cdecl InvokeAnnounceModel(void*,void*) {
    __asm {
        push ebp
        mov eax,[esp+0Ch]
        mov ebp,[esp+8]
        call eax
        pop ebp
        ret
    }
}
int wmain(int argc,wchar_t** argv) {
    using namespace fixture;
    std::unique_ptr<room_random_native_test::ClientCopies> nativeCopies;
    if(argc>=3) {
        nativeCopies=std::make_unique<room_random_native_test::ClientCopies>(argv[2]);
        nativeCopyNotice=[&](std::vector<uint8_t>& service,const uint8_t* packet,uint16_t opcode) {
            nativeCopies->Apply(service,packet,opcode);
        };
    }
    SCENARIO("exact room token and byte boundaries");
    CHECK(policy::Command("room !RaNd !ai3",16));
    CHECK(policy::Command("!random",7));
    CHECK(!policy::Command("!randomize",10));
    CHECK(!policy::Command("room!rand",9));
    CHECK(!policy::Command(nullptr,10));
    CHECK(!policy::Command("!rand",4));

    SCENARIO("all start modes synchronize native client caches and restore the saved selection");
    for(uint8_t mode=1;mode<=14;++mode) {
        Reset(mode,"!ai !rand");const auto originalClient=client;
        CHECK(Start()==0);
        CHECK(notices.size()==(UsesDeck(mode)?4u:2u));
        for(unsigned i=0;i<2;++i) {
            const uint8_t* cached=client.data()+0x5D7+i*0x48E;
            CHECK(Get<uint32_t>(cached,0x49)==built.at(1001+i).front());
            CHECK(std::memcmp(cached+0x49,Info(i)+kActive,0x99)==0);
            if(UsesDeck(mode)) {
                CHECK(std::memcmp(cached+0x146,Info(i)+kShip,0x2BC)==0);
                for(size_t n=0;n<built.at(1001+i).size();++n)
                    CHECK(Get<uint64_t>(cached,0x17A+n*8)==Get<uint64_t>(cached,0x19E+n*0x99+8));
            }
        }
        Release(room.data(),nullptr);CHECK(client==originalClient);
    }

    SCENARIO("partial notification and failure after the final build restore every client");
    for(int failure:{1,2,3,4}) {
        Reset(7);const auto originalClient=client,originalSource=Sources();failNotice=failure;
        CHECK(Start()==kStartError);CHECK(client==originalClient);CHECK(Sources()==originalSource);
        CHECK(g_saved.empty());CHECK(g_history.empty());
    }
    Reset(5);const auto lateClient=client,lateSource=Sources();failAfter=2;
    CHECK(Start()==kStartError);CHECK(client==lateClient);CHECK(Sources()==lateSource);
    CHECK(g_saved.empty());CHECK(g_history.empty());

    SCENARIO("no random command emits no temporary loadout notification");
    Reset(5,"!ai");CHECK(Start()==0xFB4);CHECK(notices.empty());

    if(argc>=2) {
        SCENARIO("native start packet announces the randomized model in every room type");
        room_title_native::KnownBattleServerImage image(argv[1]);
        constexpr size_t codeSize=0xA3; // 004B6466 through 004B6508, all branches local
        auto source=image.Function<const uint8_t*>(0x000B6466);
        CHECK(room_title_native::detail::Sha256(source,codeSize)==
              "57232cde326829e9176de6988642bc8eef6d60ad0a4cec619e37e2f422c377af");
        auto code=static_cast<uint8_t*>(VirtualAlloc(nullptr,codeSize+1,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        CHECK(code!=nullptr);
        if(code) {
            std::memcpy(code,source,codeSize);code[codeSize]=0xC3;
            DWORD previousProtection=0;
            CHECK(VirtualProtect(code,codeSize+1,PAGE_EXECUTE_READ,&previousProtection)!=FALSE);
            FlushInstructionCache(GetCurrentProcess(),code,codeSize+1);
            for(uint8_t type:{uint8_t{1},uint8_t{2},uint8_t{3}})
                for(uint8_t mode:{uint8_t{1},uint8_t{5},uint8_t{8},uint8_t{10}}) {
                    Reset(mode,"!ai !rand");room[4]=type;
                    constexpr uint32_t originalAnnouncedModel=21025;
                    Put<uint32_t>(Info(0),0x450,originalAnnouncedModel);
                    CHECK(Start()==0);
                    const uint32_t chosen=Get<uint32_t>(Info(0),kActive);
                    std::vector<uint8_t> storage(0x3000);
                    uint8_t* frame=storage.data()+0x2600;
                    Put<void*>(frame,8,room.data());
                    Put<int>(frame-0xCF0,0,0); // native participant slot index
                    InvokeAnnounceModel(frame,code);
                    CHECK(Get<uint32_t>(frame-0xCC5,0)==chosen);
                    Release(room.data(),nullptr);
                    CHECK(Get<uint32_t>(Info(0),0x450)==originalAnnouncedModel);
                }
            VirtualFree(code,0,MEM_RELEASE);
        }
    }

    SCENARIO("exact decks and repeat exclusion across every feasible set");
    Reset(7);std::mt19937_64 rng(771);
    std::set<std::vector<uint32_t>> expected,seen;
    std::vector<policy::Candidate> own;
    for(auto u:inventory) if(u.owner==1001) own.push_back(u);
    for(unsigned mask=1;mask<(1u<<own.size());++mask) {
        unsigned sum=0,count=0;std::vector<uint32_t> ids;
        for(size_t i=0;i<own.size();++i) if(mask&(1u<<i)) {sum+=own[i].points;++count;ids.push_back(own[i].unit);}
        if(sum==10 && count<=4) expected.insert(ids);
    }
    std::vector<uint32_t> previous;
    for(int i=0;i<3000;++i) {
        auto selected=policy::Choose(inventory,1001,{},true,previous,rng);
        CHECK(selected.error==policy::Error::None);
        auto identity=policy::Identity(selected.units);
        CHECK(expected.find(identity)!=expected.end());CHECK(identity!=previous);
        seen.insert(identity);previous=identity;
    }
    CHECK(seen==expected);

    SCENARIO("owned-only, rank limits, distinct models and exact budget failures");
    auto selected=policy::Choose(inventory,9999,{},false,{},rng);
    CHECK(selected.error==policy::Error::NoEligibleUnit);
    selected=policy::Choose(inventory,1001,{1,1,true},true,{},rng);
    CHECK(selected.error==policy::Error::NoExactDeck);
    selected=policy::Choose(inventory,1001,{4,4,false},true,{},rng);
    CHECK(selected.units.size()==2);CHECK(selected.units[0].points+selected.units[1].points==10);
    const auto only=policy::Identity(selected.units);
    selected=policy::Choose(inventory,1001,{4,4,false},true,only,rng);
    CHECK(!selected.alternativeToPrevious);

    SCENARIO("same model copies are legal but the same serial cannot occupy two slots");
    auto copy=Unit(1001,101,4,5);copy.serial+=123;
    auto copy2=copy;copy2.serial+=123;
    std::vector<policy::Candidate> duplicates{Unit(1001,101,4,5),copy,copy2};
    selected=policy::Choose(duplicates,1001,{},true,{},rng);
    CHECK(selected.units.size()==2);CHECK(selected.units[0].serial!=selected.units[1].serial);
    CHECK(policy::Identity(selected.units)==std::vector<uint32_t>({101,101}));
    selected=policy::Choose(duplicates,1001,{},true,{101,101},rng);
    CHECK(!selected.alternativeToPrevious);
    duplicates.push_back(Unit(1001,102,4,5));
    for(int i=0;i<100;++i) {
        selected=policy::Choose(duplicates,1001,{},true,{101,101},rng);
        CHECK(policy::Identity(selected.units)==std::vector<uint32_t>({101,102}));
        CHECK(selected.units[0].serial!=selected.units[1].serial);
    }
    std::vector<policy::Candidate> mixed=own;
    for(size_t index:{size_t{0},size_t{2},size_t{5}}) {
        auto extra=own[index];extra.serial+=1000000;mixed.push_back(extra);
    }
    std::set<std::vector<uint32_t>> logical;
    for(unsigned mask=1;mask<(1u<<mixed.size());++mask) {
        unsigned sum=0,count=0;std::vector<uint32_t> ids;
        for(size_t i=0;i<mixed.size();++i) if(mask&(1u<<i)) {sum+=mixed[i].points;++count;ids.push_back(mixed[i].unit);}
        if(sum==10 && count<=4) {std::sort(ids.begin(),ids.end());logical.insert(ids);}
    }
    for(const auto& old:logical) for(int i=0;i<50;++i) {
        auto answer=policy::Choose(mixed,1001,{},true,old,rng);
        auto identity=policy::Identity(answer.units);
        CHECK(logical.find(identity)!=logical.end());CHECK(identity!=old);
        std::set<uint64_t> used;
        for(const auto& item:answer.units) CHECK(used.insert(item.serial).second);
    }

    SCENARIO("all fourteen native mode values share the start contract");
    for(uint8_t mode=1;mode<=14;++mode) {
        Reset(mode);const auto original=Sources();
        CHECK(Start()==0);CHECK(reads==1);CHECK(builds==2);CHECK(g_history.size()==2);
        CHECK(requestedOwners==std::vector<uint32_t>({1001,1002}));
        CHECK(Sources()!=original);CHECK(g_saved.size()==1);
        const auto stable=built;
        // A native rebuild outside the start context must keep the held loadout.
        CHECK(Build(nullptr,nullptr,room.data(),Info(0),room.data()+0x3CE4,room[4],mode)==1);
        CHECK(built==stable);CHECK(reads==1);CHECK(g_attempt==nullptr);
        Release(room.data(),nullptr);CHECK(releases==1);CHECK(Sources()==original);CHECK(g_saved.empty());
    }

    SCENARIO("type-three mission and special starts update the client model and restore it");
    for(uint8_t mode:{uint8_t{1},uint8_t{5},uint8_t{8},uint8_t{10},uint8_t{13},uint8_t{14}}) {
        Reset(mode,"!ai !rand");room[4]=3;
        Put<uint32_t>(Info(0),0x450,21025);
        Put<uint32_t>(Info(1),0x450,21026);
        Put<uint32_t>(Info(0),0x44C,9); // adjacent native selection metadata is not ours
        const auto original=Sources();
        CHECK(Start()==0);
        CHECK(Get<uint32_t>(Info(0),0x450)==built.at(1001).front());
        CHECK(Get<uint32_t>(Info(1),0x450)==built.at(1002).front());
        CHECK(Get<uint32_t>(Info(0),0x44C)==9);
        Release(room.data(),nullptr);
        CHECK(Sources()==original);

        Reset(mode,"!ai !rand");room[4]=3;
        Put<uint32_t>(Info(0),0x450,21025);
        const auto failedOriginal=Sources();failAfter=1;
        CHECK(Start()==kStartError);CHECK(Sources()==failedOriginal);CHECK(g_saved.empty());
    }

    SCENARIO("ordinary room types leave the unused alternate announcement untouched");
    Reset(5,"!ai !rand");Put<uint32_t>(Info(0),0x450,21025);
    CHECK(Start()==0);CHECK(Get<uint32_t>(Info(0),0x450)==21025);
    Release(room.data(),nullptr);

    SCENARIO("repeated battles in the same room change both humans without rejoining");
    Reset(3);std::map<uint32_t,std::vector<uint32_t>> last;
    for(int round=0;round<20;++round) {
        CHECK(Start()==0);
        for(auto current:built) {
            std::sort(current.second.begin(),current.second.end());
            if(round) CHECK(current.second!=last[current.first]);
            last[current.first]=current.second;
        }
        Release(room.data(),nullptr);
    }
    CHECK(reads==20);

    SCENARIO("combined AI titles randomize only humans in either order on all start paths");
    for(uint8_t mode=1;mode<=14;++mode) for(const char* title:{"!rand !ai3 lv4","!ai3 lv4 !rand"}) {
        Reset(mode,title);
        Put<uint32_t>(Info(2),kOwner,kAiBit|99);
        Put<uint32_t>(Info(2),kActive,777);
        CHECK(Start()==0);CHECK(builds==2);CHECK(requestedOwners==std::vector<uint32_t>({1001,1002}));
        CHECK(Get<uint32_t>(Info(2),kActive)==777);
        Release(room.data(),nullptr);
        CHECK(Get<uint32_t>(Info(2),kOwner)==(kAiBit|99));CHECK(Get<uint32_t>(Info(2),kActive)==777);
    }

    SCENARIO("no command means no inventory access and no bypass of native validation");
    Reset(1,"normal room");const auto plain=Sources();
    CHECK(Start()==0xFB4);CHECK(reads==0);CHECK(builds==0);CHECK(Sources()==plain);

    SCENARIO("one player's empty pool cancels the entire room before real writes");
    Reset();auto before=Sources();
    inventory.erase(std::remove_if(inventory.begin(),inventory.end(),[](const auto& u){return u.owner==1002;}),inventory.end());
    CHECK(Start()==kStartError);CHECK(builds==0);CHECK(Sources()==before);CHECK(g_saved.empty());CHECK(g_history.empty());

    SCENARIO("late native failure rolls all humans back and does not commit the round");
    Reset(7);before=Sources();failAfter=1;
    CHECK(Start()==kStartError);CHECK(Sources()==before);CHECK(g_saved.empty());CHECK(g_history.empty());

    SCENARIO("query failure and changed room membership fail closed");
    Reset();before=Sources();readFailure=true;
    CHECK(Start()==kStartError);CHECK(Sources()==before);CHECK(g_history.empty());
    Reset();changeOwner=true;
    CHECK(Start()==kStartError);CHECK(Get<uint32_t>(Info(1),kOwner)==9090);CHECK(Get<uint32_t>(Info(1),kActive)==881);CHECK(g_saved.empty());
    Reset();changeReady=true;
    CHECK(Start()==kStartError);CHECK(Info(1)[0x42F]==0);CHECK(Get<uint32_t>(Info(1),kActive)==881);CHECK(g_saved.empty());

    SCENARIO("native validation errors remain intact");
    Reset();before=Sources();validationError=777;
    CHECK(Start()==777);CHECK(builds==0);CHECK(Sources()==before);CHECK(g_history.empty());

    SCENARIO("only C deck cannot pretend to total ten points");
    Reset(7);room[0x20]=1;before=Sources();
    CHECK(Start()==kStartError);CHECK(Sources()==before);CHECK(builds==0);

    SCENARIO("mission title fallback and existing AI occupants coexist");
    InitializeCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(lobbyProc.data()+0x38));
    for(uint8_t mode:{uint8_t{5},uint8_t{8},uint8_t{10},uint8_t{11},uint8_t{12},uint8_t{13},uint8_t{14}}) {
        Reset(mode,"");Title(lobby,"mission !rand !ai3",0x53,0x54);
        g_lobby=lobbyProc.data();g_lobbyFind=reinterpret_cast<LobbyFindFn>(&fixture::LobbyFind);
        g_lobbyRoom=reinterpret_cast<LobbyRoomFn>(&fixture::LobbyRoom);g_lobbyUnlock=reinterpret_cast<LobbyUnlockFn>(&fixture::LobbyUnlock);
        Put<uint32_t>(room.data(),kSlotBase+2*kSlotStride+kOwner,kAiBit|99);
        CHECK(Start()==0);CHECK(builds==2);CHECK(requestedOwners.size()==2);
        Release(room.data(),nullptr);
    }
    DeleteCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(lobbyProc.data()+0x38));

    SCENARIO("confirmed full room title controls random rules beyond byte twenty");
    for(uint8_t mode:{uint8_t{1},uint8_t{5},uint8_t{8},uint8_t{10}}) {
        Reset(mode,"practice");g_titleApi=&fullTitleApi;
        fullTitle=std::string(32,'x')+" !rand !ai3 lv4";
        CHECK(Start()==0);CHECK(reads==1);CHECK(builds==2);
        Release(room.data(),nullptr);
        Reset(mode,"!rand");g_titleApi=&fullTitleApi;fullTitle="practice";
        // This fixture's unchanged native selection has zero energy. Without
        // !rand, the native validator must retain its original empty-unit error.
        const auto untouched=Sources();
        CHECK(Start()==0xFB4);CHECK(reads==0);CHECK(builds==0);CHECK(Sources()==untouched);
    }

    SCENARIO("teardown never restores over a replacement room occupant");
    Reset();CHECK(Start()==0);Put<uint32_t>(Info(0),kOwner,2020);Put<uint32_t>(Info(0),kActive,999);
    Release(room.data(),nullptr);CHECK(Get<uint32_t>(Info(0),kActive)==999);CHECK(g_saved.empty());

    SCENARIO("native record preserves 64-bit ownership identity and custom fields");
    sdgo::owned_unit_record::Row r;
    r.unit=12345;r.serial=0x123456789ABCULL;r.slot=1200;r.level=13;r.normalMax=4;r.locked=1;
    r.colours={1,2,3,4,5,6};r.customSkills={0x7730,0x7720,0x7710,0x7740};r.composeExp=51;
    r.acquired[0]=0xE8;r.acquired[1]=7;r.energy=1234.5f;r.validity=365;r.kills=123;
    auto packed=sdgo::owned_unit_record::Pack(r);
    CHECK(Get<uint64_t>(packed.data(),8)==r.serial);CHECK(Get<uint32_t>(packed.data(),0)==12345);
    CHECK(Get<uint16_t>(packed.data(),56)==6);CHECK(Get<float>(packed.data(),74)==1234.5f);
    CHECK(packed[136]==4);CHECK(packed[137]==1);CHECK(Get<uint16_t>(packed.data(),138)==0x7730);
    CHECK(packed[150]==51);CHECK(packed[86]==0xE8);CHECK(packed.size()==0x99);
    CHECK(packed[7]==1);CHECK(Get<uint16_t>(packed.data(),72)==365);CHECK(Get<uint32_t>(packed.data(),17)==123);
    CHECK(sdgo::owned_unit_record::WordBits(-2048)==0xF800);
    CHECK(sdgo::owned_unit_record::WordBits(-1)==0xFFFF);

    SCENARIO("native AI addition after the human plan does not invalidate the room");
    Reset(11);addAiAfterValidation=true;CHECK(Start()==0);CHECK(builds==2);CHECK(reads==1);
    Release(room.data(),nullptr);

    SCENARIO("special/native random paths cannot overwrite a later player's owned selection");
    Reset(14);nativeOverrideSecond=true;CHECK(Start()==0);CHECK(builds==2);CHECK(reads==1);
    Release(room.data(),nullptr);

    SCENARIO("module entry owns five unique sites, separate from both AI mods");
    Reset();hooks.clear();
    SdgoApi api=ai_test::api;api.ModDirectory=&fixture::Directory;api.SettingString=&fixture::Setting;
    api.Symbol=&fixture::Symbol;api.HookCode=&fixture::Hook;
    CHECK(SdgoModLoad(&api,nullptr)==1);
    CHECK(hooks==std::vector<std::string>({"battle.start_dispatch","battle.start_special","battle.validate_unit","battle.build_human_unit","battle.release_pilots"}));
    for(const auto& hook:hooks) {
        CHECK(hook!="battle.setup_humans");CHECK(hook!="battle.start_notify");
        CHECK(hook!="battle.end");CHECK(hook!="battle.room_tick");
    }
    return ai_test::Finish();
}
