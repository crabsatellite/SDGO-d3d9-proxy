#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "../mods/room_random/loadout_sync.h"
#include "../mods/room_random_client/cache_sync.h"
#include "room_random_native_copy.h"

namespace {
int failures=0,startedModel=0,factoryCalls=0,fillCalls=0;
std::array<uint8_t,0x5DA8> profile{};
std::array<uint8_t,0x4000> room{};
std::array<uint8_t,0x4C0> app{};
std::array<uint8_t,0x10> owner{};
std::array<uint8_t,0x80> manager{},model{},ownerVtable{};
std::array<uint8_t,0xA0> modelVtable{};
std::array<uint8_t,0x4000> missionRoom{};
uint8_t* profilePointer=profile.data();
uint8_t* appPointer=app.data();
bool starting=true;
bool threeD=false;
std::array<std::array<uint8_t,0x48E>,12> frozen{};
std::array<uint8_t,0x48E> lastConstruction{};
std::array<uint8_t,0x344> lastOutput{};
std::unique_ptr<room_random_native_test::ClientCopies> nativeCopies;
template<class T> void Put(uint8_t* p,size_t n,T v){std::memcpy(p+n,&v,sizeof(v));}
void Check(bool ok,const char* name){std::printf("%s %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;}
__declspec(noinline) void* __cdecl NativeFactory(int type,uint32_t modelId) {
    ++factoryCalls;return type==15 ? reinterpret_cast<void*>(static_cast<uintptr_t>(modelId)+0x1000) : nullptr;
}
__declspec(noinline) void __fastcall NativeFill(uint8_t*,void*,uint8_t* output,
    const uint8_t* source,const uint8_t*,uint8_t deck) {
    ++fillCalls;std::memcpy(lastConstruction.data(),source,lastConstruction.size());
    for(size_t i=0;i<(deck ? 4u : 1u);++i) {
        auto* dst=output+0xB4*i;
        if(nativeCopies)nativeCopies->Project(dst,source+0x99*i,deck);
        else {
            const auto* unit=source+(deck ? 0x19E : 0x49)+0x99*i;
            Put<uint32_t>(dst,0x44,room_random_client::Read<uint32_t>(unit,0));
            std::memcpy(dst+0x48,unit+0x10,4);std::memcpy(dst+0x4C,unit+0x14,8);
        }
    }
}
void Construct(const uint8_t* entry,const uint8_t* source,uint8_t deck) {
    std::array<uint8_t,0x344> output{};
    NativeFill(model.data(),nullptr,output.data(),source,entry,deck);lastOutput=output;
    startedModel=static_cast<int>(room_random_client::Read<uint32_t>(output.data(),0x44));
    NativeFactory(15,static_cast<uint32_t>(startedModel));
}
__declspec(noinline) uint8_t* __fastcall NativeRoomInfo(uint8_t* object,void*) {
    return object==model.data() ? room.data() : nullptr;
}
#define SLOT_METHODS(Name,Storage,Base,Count) \
__declspec(noinline) uint32_t __fastcall Name##Owner(uint8_t*,void*,int slot) {return slot>=0 && slot<Count ? room_random_client::Read<uint32_t>(Storage.data(),Base+slot*0x48E+0x429) : 0;} \
__declspec(noinline) uint8_t* __fastcall Name##Unit(uint8_t* self,void*,int slot) {return Name##Owner(self,nullptr,slot) ? Storage.data()+Base+slot*0x48E+0x49 : nullptr;} \
__declspec(noinline) uint8_t* __fastcall Name##Ship(uint8_t* self,void*,int slot) {return Name##Owner(self,nullptr,slot) ? Storage.data()+Base+slot*0x48E+0x146 : nullptr;} \
__declspec(noinline) void __fastcall Name##SetUnit(uint8_t* self,void*,int slot,const uint8_t* data) {auto* p=Name##Unit(self,nullptr,slot);if(p)std::memcpy(p,data,0x99);} \
__declspec(noinline) void __fastcall Name##SetShip(uint8_t* self,void*,int slot,const uint8_t* data) {auto* p=Name##Ship(self,nullptr,slot);if(p)std::memcpy(p,data,0x2BC);}
SLOT_METHODS(Normal,room,0x533,12)
SLOT_METHODS(Mission,missionRoom,0x5C6,4)
#undef SLOT_METHODS
void __stdcall QueuedHandler(uint32_t opcode,const uint8_t* packet) {
    // Native 3D room notifications are commands. Starting prevents their cache
    // operation from completing before the start command constructs the mode.
    if(opcode==0x0861 && !starting)
        std::memcpy(room.data()+0x57C,packet+0x12,0x99);
    if(opcode==0x06C3) {
        if(threeD) {
            // Conversion also happens after the application leaves room state.
            // The visible cache and pre-start source are independent objects.
            Put<uint32_t>(app.data(),0x4A0,22);
            Construct(packet+0x20,frozen[0].data(),room[5]==6 || room[5]==7 ? 1 : 0);
        } else {
            startedModel=static_cast<int>(room_random_client::Read<uint32_t>(room.data(),0x57C));
            NativeFactory(15,static_cast<uint32_t>(startedModel));
        }
    }
}
__declspec(naked) int NativeDispatch() {
    __asm {
        push ebp
        mov ebp,esp
        push esi
        push edi
        mov edi,ecx
        mov eax,[edi+44h]
        mov esi,[eax]
        push dword ptr [ebp+18h]
        push dword ptr [ebp+14h]
        call QueuedHandler
        pop edi
        pop esi
        mov esp,ebp
        pop ebp
        ret 14h
    }
}
using DispatchFn=int(__thiscall*)(void*,int,int,int,int,void*);
void Symbol(FILE* f,const char* name,const void* address,const char* kind) {
    auto base=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    std::fprintf(f,"[%s]\nkind=%s\nrva=0x%08lX\n",name,kind,static_cast<unsigned long>(reinterpret_cast<uintptr_t>(address)-base));
    if(std::strcmp(kind,"data")) {
        auto bytes=static_cast<const uint8_t*>(address);std::fprintf(f,"expect=");
        for(int i=0;i<7;++i)std::fprintf(f,"%02X ",bytes[i]);std::fprintf(f,"\n");
    }
    std::fprintf(f,"\n");
}
void Symbols() {
    auto* base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);wchar_t file[160];
    swprintf_s(file,L"sdgo/symbols/room_random_client_runtime_test-%08lX-%08lX.ini",nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    FILE* f=nullptr;_wfopen_s(&f,file,L"wb");if(!f){Check(false,"fixture symbols created");return;}
    std::fprintf(f,"[host]\ntimestamp=0x%08lX\nsize_of_image=0x%08lX\n\n",nt->FileHeader.TimeDateStamp,nt->OptionalHeader.SizeOfImage);
    Symbol(f,"test.dispatch.entry",reinterpret_cast<void*>(&NativeDispatch),"code");
    Symbol(f,"room_random.client_dispatch",reinterpret_cast<const uint8_t*>(&NativeDispatch)+5,"site");
    Symbol(f,"room_random.client_profile",&profilePointer,"data");
    Symbol(f,"room_random.client_factory",reinterpret_cast<void*>(&NativeFactory),"code");
    Symbol(f,"room_random.client_fill_human",reinterpret_cast<void*>(&NativeFill),"code");
    Symbol(f,"room_random.client_app",&appPointer,"data");
    Symbol(f,"room_random.client_waitroom_vtable",ownerVtable.data(),"data");
    Symbol(f,"room_random.client_room_info",reinterpret_cast<void*>(&NativeRoomInfo),"code");
    Symbol(f,"room_random.client_normal_owner",reinterpret_cast<void*>(&NormalOwner),"code");
    Symbol(f,"room_random.client_normal_unit",reinterpret_cast<void*>(&NormalUnit),"code");
    Symbol(f,"room_random.client_normal_ship",reinterpret_cast<void*>(&NormalShip),"code");
    Symbol(f,"room_random.client_normal_set_unit",reinterpret_cast<void*>(&NormalSetUnit),"code");
    Symbol(f,"room_random.client_normal_set_ship",reinterpret_cast<void*>(&NormalSetShip),"code");
    Symbol(f,"room_random.client_mission_owner",reinterpret_cast<void*>(&MissionOwner),"code");
    Symbol(f,"room_random.client_mission_unit",reinterpret_cast<void*>(&MissionUnit),"code");
    Symbol(f,"room_random.client_mission_ship",reinterpret_cast<void*>(&MissionShip),"code");
    Symbol(f,"room_random.client_mission_set_unit",reinterpret_cast<void*>(&MissionSetUnit),"code");
    Symbol(f,"room_random.client_mission_set_ship",reinterpret_cast<void*>(&MissionSetShip),"code");
    fclose(f);
}
}
int wmain(int argc,wchar_t** argv) {
    const bool enabled=argc>=2 && !std::wcscmp(argv[1],L"enabled");
    if(argc>=3) {
        try {nativeCopies=std::make_unique<room_random_native_test::ClientCopies>(argv[2]);}
        catch(const std::exception& e){std::printf("native fixture: %s\n",e.what());return 2;}
        std::printf("Using authenticated GOnline human-source/model/attribute instruction spans\n");
    }
    Put<uint8_t*>(profile.data(),0xEC,room.data());Put<uint32_t>(profile.data(),0x138,1001);
    Put<uint32_t>(room.data(),0,7);room[4]=2;room[5]=8;
    Put<uint32_t>(room.data(),0x95C,1001);Put<uint32_t>(room.data(),0x57C,15019);
    std::array<uint8_t,0x99> unit{};Put<uint32_t>(unit.data(),0,11032);Put<uint64_t>(unit.data(),8,0x123456789ABCULL);
    auto notice=room_random_sync::Packet(0x0861,1001,0,7,unit);
    std::array<uint8_t,0xCD4> start{};Put<uint16_t>(start.data(),0,0x06C3);
    Put<uint32_t>(start.data(),0x20,1001);Put<uint32_t>(start.data(),0x2E,11032);
    std::array<uint8_t,0x50> dispatcher{};void* head=nullptr;Put<void*>(dispatcher.data(),0x44,&head);
    auto dispatch=reinterpret_cast<DispatchFn>(&NativeDispatch);
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    Check(startedModel==15019,"baseline reproduces stale model while native start is pending");
    Symbols();HMODULE core=LoadLibraryW(L"sdgo/core.dll");Check(core!=nullptr,"real core loads");if(!core)return 2;
    auto init=reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core,"SdgoCoreInit"));
    SdgoBoot boot{sizeof(SdgoBoot),"room_random_client_runtime_test",GetModuleHandleW(nullptr)};
    Check(init && init(&boot),"real core initializes both entry and interior hook owners");
    startedModel=0;factoryCalls=0;
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    Check(startedModel==(enabled?11032:15019),"start uses the authoritative model only when synchronization is enabled");
    Check(factoryCalls==1,"native factory still executes once");
    if(enabled) {
        Check(room_random_client::Read<uint64_t>(profile.data(),0x1B1)==0x123456789ABCULL,"own profile receives the complete 64-bit inventory identity");
        Put<uint32_t>(unit.data(),0,15019);Put<uint64_t>(unit.data(),8,777);
        auto restore=room_random_sync::Packet(0x0861,1001,0,7,unit);
        dispatch(dispatcher.data(),0,0,0,0x0861,restore.data());
        Check(room_random_client::Read<uint32_t>(room.data(),0x57C)==15019,"restore notice updates the room cache without a UI queue tick");
    }
    Put<uint8_t*>(profile.data(),0xEC,nullptr);
    Put<uint32_t>(app.data(),0x4A0,18);Put<uint8_t*>(app.data(),0x4A4,owner.data());
    Put<uint8_t*>(owner.data(),0,ownerVtable.data());Put<uint8_t*>(owner.data(),4,manager.data());
    Put<uint8_t*>(manager.data(),0x38,model.data());Put<uint8_t*>(model.data(),0,modelVtable.data());
    Put<void*>(modelVtable.data(),0x1C,reinterpret_cast<void*>(&NativeRoomInfo));
    Put<void*>(modelVtable.data(),0x20,reinterpret_cast<void*>(&MissionOwner));
    Put<void*>(modelVtable.data(),0x84,reinterpret_cast<void*>(&MissionSetUnit));
    Put<void*>(modelVtable.data(),0x88,reinterpret_cast<void*>(&MissionUnit));
    Put<void*>(modelVtable.data(),0x8C,reinterpret_cast<void*>(&MissionSetShip));
    Put<void*>(modelVtable.data(),0x90,reinterpret_cast<void*>(&MissionShip));
    Put<uint32_t>(missionRoom.data(),0x9EF,1001);Put<uint32_t>(missionRoom.data(),0x60F,15019);threeD=true;
    for(size_t i=0;i<frozen[0].size();++i)frozen[0][i]=static_cast<uint8_t>(i*13);
    Put<uint32_t>(frozen[0].data(),0x429,1001);Put<uint32_t>(frozen[0].data(),0x49,15019);
    const auto original=frozen[0];
    Put<uint32_t>(room.data(),0x57C,15019);startedModel=0;
    fillCalls=0;
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    Check(startedModel==(enabled?11032:15019),"real DLL corrects the frozen 3D mission source after the scene transition");
    Check(fillCalls==1,"native human converter executes exactly once");
    auto expected=original;
    if(enabled)std::memcpy(expected.data()+0x49,notice.data()+0x12,0x99);
    Check(lastConstruction==expected && frozen[0]==original,"complete construction input changes without mutating the frozen source");
    Check(room_random_client::Read<uint8_t*>(profile.data(),0xEC)==nullptr,"3D synchronization never fabricates a legacy room publication");
    Check(room_random_client::Read<uint32_t>(room.data(),0x57C)==15019,"3D mission updates never write the unrelated normal-layout row");
    // Switch to the normal-room native interface; final conversion is shared.
    Put<void*>(modelVtable.data(),0x20,reinterpret_cast<void*>(&NormalOwner));
    Put<void*>(modelVtable.data(),0x84,reinterpret_cast<void*>(&NormalSetUnit));
    Put<void*>(modelVtable.data(),0x88,reinterpret_cast<void*>(&NormalUnit));
    Put<void*>(modelVtable.data(),0x8C,reinterpret_cast<void*>(&NormalSetShip));
    Put<void*>(modelVtable.data(),0x90,reinterpret_cast<void*>(&NormalShip));
    Put<uint32_t>(app.data(),0x4A0,18);room[5]=1;startedModel=0;
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    Check(startedModel==(enabled?11032:15019),"ordinary PvP also uses the corrected complete construction source");
    // Full ship and every native model/attribute projection survive the hook.
    room[5]=7;Put<uint32_t>(app.data(),0x4A0,18);
    std::array<uint8_t,0x2BC> ship{};
    for(size_t i=0;i<4;++i) {
        auto id=static_cast<uint32_t>(11032+i);
        Put<uint32_t>(ship.data(),0x58+i*0x99,id);
        Put<uint32_t>(ship.data(),0x58+i*0x99+0x14,static_cast<uint32_t>(0xB0000000+i));
        Put<uint64_t>(ship.data(),0x34+i*8,0x100000001ULL+i);
        Put<uint64_t>(ship.data(),0x60+i*0x99,0x100000001ULL+i);
        Put<uint32_t>(start.data(),0x2E+i*0x32,id);
    }
    Put<uint64_t>(ship.data(),0x1C,0x100000001ULL);
    auto deck=room_random_sync::Packet(0x098B,1001,0,7,ship);
    dispatch(dispatcher.data(),0,0,0,0x098B,deck.data());dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());
    dispatch(dispatcher.data(),0,0,0,0x06C3,start.data());
    expected=original;
    if(enabled){std::memcpy(expected.data()+0x49,notice.data()+0x12,0x99);std::memcpy(expected.data()+0x146,ship.data(),ship.size());}
    Check(lastConstruction==expected && frozen[0]==original,"real DLL passes the complete ship with original pilot metadata and immutable source");
    if(enabled)for(size_t i=0;i<4;++i) {
        Check(room_random_client::Read<uint32_t>(lastOutput.data(),0x44+i*0xB4)==11032+i,"native converter projects each deck model from the supplied full source");
        Check(room_random_client::Read<uint32_t>(lastOutput.data(),0x4C+i*0xB4)==0xB0000000+i,"native converter preserves the original full-width per-unit field");
    }
    auto ai=start;Put<uint32_t>(ai.data(),0x20,0x80000001u);Construct(ai.data()+0x20,frozen[0].data(),0);
    Check(lastConstruction==original,"AI-tagged source passes through unchanged");
    Put<uint32_t>(app.data(),0x4A0,18);
    dispatch(dispatcher.data(),0,0,0,0x0861,notice.data());Construct(start.data()+0x20,frozen[0].data(),1);
    Check(lastConstruction==original,"a later loadout notice invalidates previous construction authority");
    std::printf("%s CLIENT START-STATE RUNTIME CHECKS\n",failures?"FAILED":"PASSED");return failures?1:0;
}
