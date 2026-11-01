#include "../mods/storage_claim/storage_claim.cpp"
#include "storage_ui_fixture.h"
#include <array>
#include <cassert>
#include <iostream>
namespace fixture {
using namespace storage_fixture;
using storage_fixture::Put;
std::array<uint8_t,0x80> connection{};
std::array<uint8_t,0x500> app{};
std::vector<Entry> rewards;std::vector<uint8_t> pending;
std::vector<uint8_t> rows;uint32_t total=0;int sends=0,grants=0,nativeChecks=0,manual=0;
int appCalls=0;
bool rejected=false,selection=false;
void __cdecl Log(SdgoMod*,int,const char*,...){}
uint8_t* __cdecl Session(){return connection.data();}
uint8_t* __cdecl App(){++appCalls;return app.data();}
void __fastcall FakeInit(void*,void*){}
uint8_t __fastcall Busy(void*,void*){return 0;}
int __fastcall Send(void*,void*,int socket,const uint8_t* data,int size,int flags){assert(socket==7 && !flags && pending.empty());pending.assign(data,data+size);++sends;return 1;}
uint32_t __fastcall Event(void*,void*,void*,uint32_t,uintptr_t,void*){++manual;return 0;}
uint32_t __fastcall Original(void* self,void*,uintptr_t,uintptr_t,uintptr_t,uint32_t opcode,const uint8_t* data) {
    if(opcode==0x4E7 || opcode==0x722){++nativeChecks;return 0;}
    if(opcode==0x7E8 || opcode==0xA23){++grants;return 0;}
    if(opcode==0x7BD || opcode==0x7BE) {
        const auto tab=opcode==0x7BD?Tab::Items:Tab::Gifts;const auto stride=tab==Tab::Items?28u:402u;
        rows.assign(static_cast<size_t>(data[0])*stride,0);
        for(size_t i=0;i<data[0];++i){const auto key=Read<Key>(data,1+i*sizeof(Entry));auto* p=rows.data()+i*stride;if(tab==Tab::Items)Put(p,0,key.id);else{Put(p,0,static_cast<uint32_t>(key.id));Put(p,0x166,key.stamp);Put<uint16_t>(p,0x156,Read<Entry>(data,1+i*sizeof(Entry)).claimed?1:0);}}
        Put(self,tab==Tab::Items?0x180:0x170,rows.data());Put(self,tab==Tab::Items?0x184:0x174,rows.data()+rows.size());Put(self,0x168,total);
    }
    return 0;
}
SdgoApi api{};
void Setup(Tab tab=Tab::Items) {
    if(g_timer){KillTimer(nullptr,g_timer);g_timer=0;}g_ui.Detach();storage_fixture::Setup();
    rewards.clear();for(uint64_t i=1;i<=12;++i)rewards.push_back({{i+(tab==Tab::Items?0x100000000ULL:0),tab==Tab::Gifts?i*1234567890123ULL:0},false});
    total=12;pending.clear();sends=grants=nativeChecks=manual=appCalls=0;rejected=selection=false;g_retiredChecks.clear();
    app={};connection={};Put(app.data(),0x4A0,uint32_t{6});Put(app.data(),0x4A4,reinterpret_cast<void*>(1));
    Put(connection.data(),0x14,7);Put(service.data(),0x15C,static_cast<int>(tab));Put(service.data(),0x168,total);
    g_session=&Session;g_app=&App;g_busy=reinterpret_cast<NativeBusy>(&Busy);g_send=reinterpret_cast<NativeSend>(&Send);
    g_event=reinterpret_cast<NativeUI>(&Event);g_packet=reinterpret_cast<NativePacket>(&Original);
    api.Log=&Log;g_api=&api;g_mod=nullptr;g_batch=Batch{};g_owner=nullptr;g_service=service.data();g_thread=GetCurrentThreadId();g_pumping=g_armed=false;g_observedSession=g_nextSend=0;g_interval=200;
    assert(g_ui.Attach(service.data(),Bind()));
}
void Click(){UIEvent(nullptr,nullptr,service.data(),0x101,0,Pointer(L"ModStorageClaimAll"));}
void Start(){Click();assert(g_armed && sends==0);Click();assert(!g_armed && g_batch.Busy() && sends==0);}
void Step(){g_nextSend=0;PumpUnsafe();}
void Deliver() {
    assert(!pending.empty());auto request=pending;pending.clear();const uint16_t op=Read<uint16_t>(request.data());
    const auto tab=(op==0x9A1 || op==0x6CB || op==0x9CC)?Tab::Items:Tab::Gifts;
    std::vector<uint8_t> reply(64,0);uint32_t command=0;
    if(op==0x9A1 || op==0x9A2) {
        const auto page=Read<uint32_t>(request.data(),6)-1;const size_t first=page*5,amount=first<rewards.size()?std::min<size_t>(5,rewards.size()-first):0;
        reply.assign(1+amount*sizeof(Entry),0);reply[0]=static_cast<uint8_t>(amount);for(size_t i=0;i<amount;++i)Put(reply.data(),1+i*sizeof(Entry),rewards[first+i]);
        total=static_cast<uint32_t>(rewards.size());command=op==0x9A1?0x7BD:0x7BE;
    }else {
        const Key key=tab==Tab::Items?Key{Read<uint64_t>(request.data(),6),0}:Key{Read<uint32_t>(request.data(),6),Read<uint64_t>(request.data(),10)};
        if(tab==Tab::Items)Put(reply.data(),6,key.id);else{Put(reply.data(),6,static_cast<uint32_t>(key.id));Put(reply.data(),10,key.stamp);}
        if(op==0x6CB || op==0x906) {
            command=tab==Tab::Items?0x4E7:0x722;
            if(selection)reply[0x32]=1;
            if(rejected){command=tab==Tab::Items?0x5D9:0x814;Put<uint16_t>(reply.data(),6,900);}
        }else{
            assert(op==(tab==Tab::Items?0x9CC:0x630));command=tab==Tab::Items?0x7E8:0xA23;
            const auto i=std::find_if(rewards.begin(),rewards.end(),[&](const Entry& e){return e.key==key;});assert(i!=rewards.end());
            if(tab==Tab::Items)rewards.erase(i);else{i->claimed=true;Put(reply.data(),6,static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(key.id))));}
        }
    }
    Receive(service.data(),nullptr,0,0,0,command,reply.data());
}
void Run(){for(int n=0;n<300 && g_batch.Busy();++n){Step();if(!pending.empty())Deliver();}assert(!g_batch.Busy());}
}
int main() {
    using namespace fixture;
    fixture::Setup();auto* button=Pointer(L"ModStorageClaimAll");assert(storage_fixture::Get<uint8_t>(button,0x168)==0);
    assert(children.at(L"ModStorageClaimAll")->skins.size()==3 && storage_fixture::Get<float>(button,0x148)==291.0f && storage_fixture::Get<float>(button,0x14C)==64.0f);
    assert(storage_fixture::Get<float>(Pointer(L"ModStorageClaimStatus"),0x14C)==520.0f);
    Start();Run();assert(g_batch.phase()==Phase::Complete && grants==12 && rewards.empty() && nativeChecks==0);
    fixture::Setup(Tab::Gifts);rewards[0].claimed=true;Start();Run();assert(g_batch.phase()==Phase::Complete && grants==11 && nativeChecks==0);
    fixture::Setup();Start();Step();Deliver();Step();assert(g_batch.Waiting(Operation::Check));Click();Deliver();Step();assert(grants==0 && !g_batch.Outstanding() && pending.empty());
    fixture::Setup();Start();Step();Deliver();Step();Deliver();Step();assert(g_batch.Waiting(Operation::Claim));Click();Deliver();Step();assert(grants==1 && pending.empty() && g_batch.phase()==Phase::Stopped);
    fixture::Setup();rejected=true;Start();Run();assert(grants==0 && g_batch.reason()==Reason::Rejected && rewards.size()==12);
    fixture::Setup(Tab::Gifts);selection=true;Start();Run();assert(grants==0 && g_batch.reason()==Reason::Selection);
    fixture::Setup();Start();Step();Deliver();Step();storage_fixture::Put<uint8_t>(dialog.data(),0x162,0);Step();Deliver();Step();assert(grants==0 && pending.empty());
    fixture::Setup();Start();Step();Deliver();Step();Deliver();Step();g_batch.Tick(GetTickCount64()+20000,10000);assert(g_batch.phase()==Phase::Unknown);Click();assert(sends==3);Deliver();Step();assert(grants==1 && !g_batch.Busy() && pending.empty());
    fixture::Setup();Start();Step();Deliver();Step();storage_fixture::Put(connection.data(),0x14,8);Step();assert(!g_batch.Busy() && g_retiredChecks.size()==1);
    Deliver();Step();assert(nativeChecks==0 && grants==0 && g_retiredChecks.empty());
    fixture::Setup();Start();Step();Deliver();Step();storage_fixture::Put(connection.data(),0x14,8);
    Deliver();Step();assert(nativeChecks==0 && grants==0 && !g_batch.Busy()); // reply before the next timer tick
    fixture::Setup();Start();Step();Deliver();
    storage_fixture::Put(service.data(),0x180,reinterpret_cast<uint8_t*>(1));storage_fixture::Put(service.data(),0x184,reinterpret_cast<uint8_t*>(29));
    g_nextSend=0;Timer(nullptr,WM_TIMER,0,0);assert(!g_ui.Owns(service.data()) && storage_fixture::Get<uint8_t>(Pointer(L"ModStorageClaimAll"),0x162)==0 && sends==1);
    fixture::Setup();g_ui.Detach();UIEvent(nullptr,nullptr,service.data(),0x101,0,Pointer(L"CloseBtn1"));assert(manual==1 && !sends);
    fixture::Setup();Click();assert(g_armed);
    for(uint32_t event:{0x102u,0x103u})UIEvent(nullptr,nullptr,service.data(),event,0,Pointer(L"ModStorageClaimAll"));
    assert(g_armed && !g_batch.Busy() && !sends);Click();assert(g_batch.Busy());
    fixture::Setup();for(uint32_t state:{0u,4u,21u,22u,23u,24u,25u,30u,UINT32_MAX}) {
        storage_fixture::Put(app.data(),0x4A0,state);assert(!Available());Click();assert(!g_armed && !sends);
    }
    fixture::Setup();g_ui.Detach();storage_fixture::Setup();failSkinAt=2;bool failed=false;try{g_ui.Attach(service.data(),Bind());}catch(const std::exception&){failed=true;g_ui.Detach();}
    assert(failed && storage_fixture::Get<uint8_t>(Pointer(L"ModStorageClaimAll"),0x162)==0);
    // Real Win32 thread timers are delivered by the normal message pump, with
    // no shared PeekMessage hook and without starting any game process.
    fixture::Setup();storage_fixture::Setup();g_init=reinterpret_cast<NativeInit>(&fixture::FakeInit);Init(service.data(),nullptr);assert(g_timer && g_ui.Owns(service.data()));
    const auto until=GetTickCount64()+250;const int calls=appCalls;
    while(GetTickCount64()<until){MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}Sleep(1);}
    assert(appCalls>calls && sends==0);KillTimer(nullptr,g_timer);g_timer=0;
    std::cout<<"STORAGE NATIVE ADAPTER PASSED: native page placement/skins, one confirmation, 12-item and gift pagination, preflight cancellation, in-flight stop, choice/capacity refusal, hidden page, timeout/late grant and original UI fallback\n";
}
