#include "../mods/hangar_manager/hangar_manager.cpp"
#include <array>
#include <cassert>
#include <iostream>

namespace fixture {
std::array<uint8_t,64> info{},mode{};
std::array<uint8_t,0x300> profile{};
std::array<uint8_t,0x500> app{};
std::array<uint8_t,0x900> grid{};
std::array<uint8_t,0x200> panel{};
std::array<uint8_t,0x80> session{};
uint8_t* infoPointer=info.data();uint8_t* profilePointer=profile.data();
std::vector<Unit> owned;
std::vector<uint8_t> pending;
int sends=0,paints=0,gb=0,items=0;
bool rejected=false;
template<class T>void Put(void* p,size_t offset,T value){memcpy(static_cast<uint8_t*>(p)+offset,&value,sizeof(value));}
Unit Make(uint64_t uid,uint32_t type,uint16_t slot){
    Unit u;u.uid=uid;u.type=type;u.slot=slot;u.level=1;u.fullReady=true;
    Put(u.full.data(),0,type);Put(u.full.data(),4,slot);Put(u.full.data(),8,uid);u.full[16]=1;return u;
}
uint8_t* __cdecl App(){return app.data();}
uint8_t* __cdecl Session(){return session.data();}
void __fastcall Page(void*,void*){++paints;}
void __fastcall Clear(void*,void*){owned.clear();}
void __fastcall Sync(void*,void*,const uint8_t*){}
void __fastcall Brief(void*,void*,void* out,uint16_t slot){
    memset(out,0,25);
    for(const auto& u:owned)if(u.slot==slot){
        Put(out,0,u.type);Put(out,4,u.uid);Put(out,12,u.slot);
        auto* p=static_cast<uint8_t*>(out);p[14]=u.level;p[15]=u.rental;p[16]=u.ship;p[17]=u.lock;p[18]=u.special;
    }
}
void __fastcall Full(void*,void*,void* out,uint16_t slot){memset(out,0,153);for(const auto& u:owned)if(u.slot==slot && u.fullReady)memcpy(out,u.full.data(),153);}
int __fastcall Send(void*,void*,int connection,const uint8_t* data,int size,int flags){
    assert(connection==7 && flags==0 && pending.empty());pending.assign(data,data+size);++sends;return 1;
}
void __cdecl Log(SdgoMod*,int,const char*,...){}
SdgoApi api{};
void Setup(){
    owned={Make(101,10,1),Make(102,40,3),Make(103,10,4),Make(104,10,5),Make(105,20,8)};
    pending.clear();sends=paints=gb=items=0;rejected=false;
    Put(info.data(),4,1);Put(info.data(),0x0C,8);info[0x20]=1;Put(profile.data(),0x1AD,uint16_t{8});
    Put(app.data(),0x4A4,mode.data());Put(mode.data(),0x0C,static_cast<void*>(nullptr));
    Put(grid.data(),0,reinterpret_cast<void*>(0x12345678));Put(grid.data(),0x44,panel.data());panel[0x162]=1;
    Put(session.data(),0x14,7);Put(session.data(),0x58,uint32_t{0});Put(session.data(),0x5C,uint32_t{0});
    g_info=&infoPointer;g_profile=&profilePointer;g_grid=grid.data();g_gridVtable=Read<void*>(grid.data());
    g_app=&App;g_session=&Session;g_brief=reinterpret_cast<NativeGet>(&Brief);g_full=reinterpret_cast<NativeGet>(&Full);
    g_send=reinterpret_cast<NativeSend>(&Send);g_page=reinterpret_cast<NativeVoid>(&Page);g_view=g_page;g_redraw=g_page;
    g_clear=reinterpret_cast<NativeVoid>(&Clear);g_sync=reinterpret_cast<NativeSync>(&Sync);g_thread=GetCurrentThreadId();g_epoch=1;
    api.Log=&Log;g_api=&api;g_scanning=g_mutating=g_hasPreview=g_needsRelog=false;g_detailPending=0;g_runner=Runner{};
    g_rules.clear();for(uint32_t i=1;i<=4;++i){Rule r;r.unit=i*10;r.rank=static_cast<int>(i);r.subrank=2;r.gb=static_cast<int>(i*400);r.item=2220008;r.amount=static_cast<int>(i*3);r.name=L"Synthetic unit";g_rules[r.unit]=r;}
}
Snapshot Capture(){Snapshot s;assert(GetSnapshot(s));return s;}
void Deliver(){
    assert(!pending.empty());const auto opcode=Read<uint16_t>(pending.data());
    if(opcode==0x78B){
        const auto uid=Read<uint64_t>(pending.data(),6);const auto snapshot=Capture();const auto* unit=ById(snapshot,uid);assert(unit);auto copy=unit->full;
        ::Sync(info.data(),nullptr,copy.data());
    }else if(!rejected){
        Action a;
        if(opcode==0x800){
            assert(pending.size()==26);const auto wire=Read<SwapPacket>(pending.data());
            a.first=wire.first;a.second=wire.second;a.to=wire.destination;a.from=wire.secondDestination;
            const auto s=Capture();const auto* u=ById(s,a.first);assert(u);a.type=u->type;assert(Allowed(s,a));owned=Applied(s,a).units;
            for(auto& item:owned)Put(item.full.data(),4,item.slot);
        }else{
            assert(opcode==0x6D2 && pending.size()==19);const auto wire=Read<DeletePacket>(pending.data());
            a.first=wire.uid;a.type=wire.type;a.kind=wire.reward==1?Kind::GB:Kind::Item;
            const auto s=Capture();const auto* u=ById(s,a.first);assert(u);a.from=u->slot;a.signature=Signature(*u);assert(Allowed(s,a));
            owned=Applied(s,a).units;if(a.kind==Kind::GB)gb+=g_rules.at(a.type).gb;else items+=g_rules.at(a.type).amount;
        }
    }
    pending.clear();
}
void Run(Plan p){
    Runner r;r.Start(std::move(p),Capture());uint64_t now=1;
    while(r.state()!=State::Complete){
        const auto before=Capture();const auto* action=r.Next(before);assert(action);const auto bytes=Packet(*action);
        r.Sent(now,::Send(bytes.data(),static_cast<int>(bytes.size())));r.Observe(Capture(),now+1,100);assert(r.state()==State::Waiting);
        Deliver();r.Observe(Capture(),now+2,100);now+=3;
    }
}
}
int main(){
    static_assert(sizeof(void*)==4,"native ABI fixture is Win32");
    fixture::Setup();assert(Visible());auto s=fixture::Capture();assert(s.units.size()==5 && s.activeSlot==8);
    fixture::Run(sdgo::hangar::Sort(s,g_rules));s=fixture::Capture();assert(ById(s,102)->slot==1 && ById(s,105)->slot==8);
    fixture::Setup();auto duplicates=Duplicates(fixture::Capture(),g_rules,2,Kind::GB,true);assert(duplicates.removed==1);
    fixture::Run(duplicates);assert(fixture::gb==400 && fixture::Capture().units.size()==4);
    fixture::Setup();fixture::Run(Duplicates(fixture::Capture(),g_rules,2,Kind::Item,false));assert(fixture::items==3 && fixture::gb==0);
    fixture::Setup();BeginScan(Kind::GB);assert(g_scanning && g_scanIds.size()==3 && fixture::sends==0);
    g_detailPending=101;DetailPacket detail;detail.uid=101;assert(::Send(reinterpret_cast<uint8_t*>(&detail),sizeof(detail)));fixture::Deliver();assert(g_detailObserved);
    fixture::Setup();s=fixture::Capture();auto plan=sdgo::hangar::Sort(s,g_rules);g_runner.Start(plan,s);auto action=*g_runner.Next(s);auto bytes=Packet(action);
    fixture::rejected=true;g_runner.Sent(1,::Send(bytes.data(),static_cast<int>(bytes.size())));fixture::Deliver();g_runner.Observe(fixture::Capture(),200,100);assert(g_runner.state()==State::Unknown);
    const auto epoch=g_epoch;::Clear(fixture::info.data(),nullptr);assert(g_epoch!=epoch);
    fixture::Setup();fixture::panel[0x162]=0;Snapshot invalid;assert(!GetSnapshot(invalid));
    fixture::Setup();fixture::info[0x20]=0;assert(!GetSnapshot(invalid) && fixture::sends==0);
    fixture::Setup();fixture::Put(fixture::mode.data(),0x0C,reinterpret_cast<void*>(1));assert(!Visible() && !GetSnapshot(invalid));
    fixture::Setup();fixture::owned[1].lock=1;s=fixture::Capture();assert(ById(s,102)->lock==1);fixture::Run(sdgo::hangar::Sort(s,g_rules));assert(ById(fixture::Capture(),102)->slot==3);
    std::cout<<"HANGAR NATIVE ADAPTER FIXTURE PASSED (production snapshot, sends, native acknowledgements, GB/item, detail freshness, logout, hidden/battle gates)\n";
}
