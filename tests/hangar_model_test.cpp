#include "sdgo/hangar_model.h"
#include "sdgo/hangar_sort_request.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <random>
using namespace sdgo::hangar;
Unit Make(uint64_t uid,uint32_t type,uint16_t slot) {
    Unit u;u.uid=uid;u.type=type;u.slot=slot;u.level=1;u.fullReady=true;
    memcpy(u.full.data(),&type,4);memcpy(u.full.data()+4,&slot,2);memcpy(u.full.data()+8,&uid,8);u.full[16]=1;return u;
}
Rules Definitions() {
    Rules r;for(uint32_t i=1;i<=4;++i){Rule u;u.unit=i*10;u.rank=static_cast<int>(i);u.subrank=2;u.gb=static_cast<int>(400*i);u.item=2220008;u.amount=static_cast<int>(i*3);r[u.unit]=u;}return r;
}
Snapshot Base() {Snapshot s;s.session=91;s.capacity=8;s.activeSlot=8;return s;}
Snapshot Execute(Plan p) {
    auto s=p.before;for(const auto& a:p.actions){assert(Allowed(s,a));s=Applied(s,a);Validate(s);}return s;
}
int main() {
    const auto request=ServerSortRequest("NativeFixture","12345678-1234-1234-1234-123456789abc");
    assert(request[0]==0x91 && request[1]==0x08 && !request[2]);
    for(const auto& invalid:std::vector<std::string>{"",std::string(36,'Z'),std::string(36,'0')}) {
        bool rejected=false;try{ServerSortRequest("NativeFixture",invalid);}catch(const std::invalid_argument&){rejected=true;}assert(rejected);
    }
    const auto rules=Definitions();
    {
        auto s=Base();for(int i=1;i<=50;++i){auto u=Make(i,10,static_cast<uint16_t>(i));u.fullReady=false;s.units.push_back(u);}s.capacity=60;s.activeSlot=60;
        auto plan=GBPreview(s,rules,2);assert(plan.serverBatch && plan.removed==48 && plan.expectedGB==48*400);
        const auto requests=ServerGBRequests("NativeFixture","12345678-1234-1234-1234-123456789abc",plan,2,false);
        assert(requests.size()==8 && std::string(reinterpret_cast<const char*>(requests.back().data()+0x2C)).back()=='C');
        for(const auto& bytes:requests)assert(strlen(reinterpret_cast<const char*>(bytes.data()+0x2C))<=300);
        bool rejected=false;try{Runner run;run.Start(plan,s);}catch(const std::runtime_error&){rejected=true;}assert(rejected);
        auto updated=s;updated.units[0].lock=1;assert(!SameGBPreview(s,updated));
    }
    {
        auto s=Base();s.units={Make(1,10,1),Make(2,40,4),Make(3,30,7),Make(4,20,8)};
        auto after=Execute(Sort(s,rules));assert(ById(after,2)->slot==1);assert(ById(after,3)->slot==2);assert(ById(after,1)->slot==3);assert(ById(after,4)->slot==8);
        s.units[0].lock=1;after=Execute(Sort(s,rules));assert(ById(after,1)->slot==1);
        s.units[1].ship=1;after=Execute(Sort(s,rules));assert(ById(after,2)->slot==4);
    }
    {
        std::mt19937 rng(17);
        for(int n=0;n<250;++n){
            auto s=Base();std::vector<uint16_t> slots={1,2,3,4,5,6,7};std::shuffle(slots.begin(),slots.end(),rng);
            for(uint16_t i=0;i<7;++i)s.units.push_back(Make(100+i,10*(1+i%4),slots[i]));
            auto after=Execute(Sort(s,rules));assert(after.units.size()==s.units.size());
            for(const auto& u:s.units)assert(ById(after,u.uid)->type==u.type);
            assert(Sort(after,rules).actions.empty());
        }
    }
    {
        auto s=Base();s.units={Make(1,10,1),Make(2,10,2),Make(3,10,3),Make(4,10,4),Make(5,10,5),Make(6,20,8)};
        s.units[3].full[40]=7; // state variants remain duplicates of one UnitNumber
        s.units[4].level=2; // trained copy never exchanged
        auto plan=Duplicates(s,rules,2,Kind::GB,false);assert(plan.removed==3 && plan.expectedGB==1200);
        assert(plan.actions[0].first==1 && plan.actions[1].first==2 && plan.actions[2].first==3);
        auto after=Execute(plan);assert(ById(after,4) && ById(after,5));
        plan=Duplicates(s,rules,2,Kind::Item,true);assert(plan.expectedItems[2220008]==9);assert(plan.actions[0].kind==Kind::Item);Execute(plan);
        s.units[0].fullReady=false;assert(Duplicates(s,rules,2,Kind::GB,false).removed==3);
    }
    {
        auto s=Base();s.units={Make(1,10,1),Make(2,10,2),Make(3,10,3)};
        s.units[0].lock=1;assert(Duplicates(s,rules,2,Kind::GB,false).removed==1);
        s.units[0].lock=0;s.units[0].rental=1;assert(Duplicates(s,rules,2,Kind::GB,false).removed==1);
        s.units[0].rental=0;s.units[0].ship=1;assert(Duplicates(s,rules,2,Kind::GB,false).removed==1);
        s.units[0].ship=0;s.activeSlot=1;assert(Duplicates(s,rules,2,Kind::GB,false).removed==1);
        auto bad=s;bad.units[1].uid=1;bool rejected=false;try{Validate(bad);}catch(...){rejected=true;}assert(rejected);
        rejected=false;try{Duplicates(s,rules,0,Kind::GB,false);}catch(...){rejected=true;}assert(rejected);
    }
    {
        Action a;a.first=0x0807060504030201ULL;a.second=0x1817161514131211ULL;a.from=0x2021;a.to=0x3031;
        auto bytes=Packet(a);const std::vector<uint8_t> expected={0,8,0,0,0,0,1,2,3,4,5,6,7,8,0x31,0x30,0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x21,0x20};assert(bytes==expected);
        a.kind=Kind::GB;a.type=0x44332211;bytes=Packet(a);assert(bytes.size()==19 && bytes[0]==0xD2 && bytes[1]==6 && bytes[14]==0x11 && bytes[18]==1);
        a.kind=Kind::Item;assert(Packet(a)[18]==2);
    }
    {
        auto s=Base();s.units={Make(1,10,1),Make(2,40,3),Make(3,30,5)};
        auto plan=Sort(s,rules);assert(plan.actions.size()>1);Runner run;run.Start(plan,s);
        const auto a=*run.Next(s);run.Sent(10,true);run.Observe(s,15,100);assert(run.completed()==0 && !run.Next(s));
        run.Cancel();s=Applied(s,a);run.Observe(s,20,100);assert(run.completed()==1 && run.state()==State::Stopped);
        run.Start(Sort(s,rules),s);run.Next(s);run.Sent(30,true);run.Observe(s,150,100);assert(run.state()==State::Unknown && !run.Next(s));
        run.Start(Sort(s,rules),s);run.Next(s);run.Sent(30,false);assert(run.state()==State::Unknown);
        run.Start(Sort(s,rules),s);auto changed=s;changed.session=99;assert(!run.Next(changed));
    }
    {
        auto s=Base();s.units={Make(1,10,1),Make(2,10,2),Make(3,10,3)};
        auto p=Duplicates(s,rules,2,Kind::GB,false);Runner r;r.Start(p,s);s.units[0].full[45]=1;assert(!r.Next(s));
        s.units[0].full[45]=0;r.Start(p,s);s.units[0].lock=1;assert(!r.Next(s));
    }
    {
        auto before=Base();before.units={Make(1,10,1),Make(2,10,2)};
        assert(SamePreview(before,before));auto changed=before;
        changed.units[0].lock=1;assert(SameLayout(before,changed) && !SamePreview(before,changed));
        changed=before;changed.activeSlot=2;assert(!SamePreview(before,changed));
        changed=before;changed.units[0].full[45]=1;assert(!SamePreview(before,changed));
        changed=before;changed.units[0].fullReady=false;assert(!SamePreview(before,changed));
    }
    std::cout<<"HANGAR MODEL CONTRACTS PASSED (250 permutations, native packet layouts, protection, confirmation, cancellation, stale state)\n";
}
