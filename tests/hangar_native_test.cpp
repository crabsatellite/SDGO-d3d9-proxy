#include "../mods/hangar_manager/hangar_manager.cpp"
#include "hangar_page_fixture.h"
#include <array>
#include <cassert>
#include <iostream>
#include <thread>

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
std::vector<std::vector<uint8_t>> mailRequests;
int sends=0,paints=0,gb=0,items=0,nativeClicks=0;
std::string refreshOrder;
bool rejected=false;
template<class T>void Put(void* p,size_t offset,T value){memcpy(static_cast<uint8_t*>(p)+offset,&value,sizeof(value));}
Unit Make(uint64_t uid,uint32_t type,uint16_t slot){
    Unit u;u.uid=uid;u.type=type;u.slot=slot;u.level=1;u.fullReady=true;
    Put(u.full.data(),0,type);Put(u.full.data(),4,slot);Put(u.full.data(),8,uid);u.full[16]=1;return u;
}
uint8_t* __cdecl App(){return app.data();}
uint8_t* __cdecl Session(){return session.data();}
void __fastcall Page(void*,void*){++paints;refreshOrder+='P';}
void __fastcall View(void*,void*){refreshOrder+='V';}
void __fastcall Redraw(void*,void*){refreshOrder+='R';}
void __fastcall Clear(void*,void*){owned.clear();}
void __fastcall Sync(void*,void*,const uint8_t*){}
void __fastcall NativeClick(void*,void*,void*){++nativeClicks;}
void __fastcall Brief(void*,void*,void* out,uint16_t slot){
    memset(out,0,25);
    for(const auto& u:owned)if(u.slot==slot){
        Put(out,0,u.type);Put(out,4,u.uid);Put(out,12,u.slot);
        auto* p=static_cast<uint8_t*>(out);p[14]=u.level;p[15]=u.rental;p[16]=u.ship;p[17]=u.lock;p[18]=u.special;
    }
}
void __fastcall Full(void*,void*,void* out,uint16_t slot){memset(out,0,153);for(const auto& u:owned)if(u.slot==slot && u.fullReady)memcpy(out,u.full.data(),153);}
int __fastcall Send(void*,void*,int connection,const uint8_t* data,int size,int flags){
    assert(connection==7 && flags==0);
    if(Read<uint16_t>(data)==0x891)mailRequests.emplace_back(data,data+size);else assert(pending.empty());
    pending.assign(data,data+size);++sends;return 1;
}
void __cdecl Log(SdgoMod*,int,const char*,...){}
SdgoApi api{};
void Setup(){
    g_ui.Detach();
    owned={Make(101,10,1),Make(102,40,3),Make(103,10,4),Make(104,10,5),Make(105,20,8)};
    pending.clear();mailRequests.clear();sends=paints=gb=items=nativeClicks=0;rejected=false;refreshOrder.clear();
    Put(info.data(),4,1);Put(info.data(),0x0C,8);info[0x20]=1;Put(profile.data(),0x1AD,uint16_t{8});
    memcpy(profile.data()+0x161,"NativeFixture",14);
    Put(app.data(),0x4A0,uint32_t{6});Put(app.data(),0x4A4,mode.data());Put(mode.data(),0x0C,reinterpret_cast<void*>(0xBAADF00D));
    Put(grid.data(),0,reinterpret_cast<void*>(0x12345678));Put(grid.data(),0x44,panel.data());panel[0x162]=1;
    Put(session.data(),0x14,7);Put(session.data(),0x58,uint32_t{0});Put(session.data(),0x5C,uint32_t{0});
    g_info=&infoPointer;g_profile=&profilePointer;g_grid=grid.data();g_gridVtable=Read<void*>(grid.data());
    g_app=&App;g_session=&Session;g_brief=reinterpret_cast<NativeGet>(&Brief);g_full=reinterpret_cast<NativeGet>(&Full);
    g_send=reinterpret_cast<NativeSend>(&Send);g_page=reinterpret_cast<NativeVoid>(&Page);g_view=reinterpret_cast<NativeVoid>(&View);g_redraw=reinterpret_cast<NativeVoid>(&Redraw);
    g_clear=reinterpret_cast<NativeVoid>(&Clear);g_sync=reinterpret_cast<NativeSync>(&Sync);g_thread=GetCurrentThreadId();g_epoch=1;
    g_button=reinterpret_cast<ButtonFn>(&NativeClick);
    api.Log=&Log;g_api=&api;g_scanning=g_mutating=g_hasPreview=g_needsRelog=false;g_detailPending=0;g_runner=Runner{};g_sortAfter=false;
    g_submittingGB=false;g_gbRequests.clear();g_gbIndex=0;
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
    fixture::Setup();BeginScan(Kind::GB);assert(!g_scanning && g_hasPreview && g_preview.serverBatch && fixture::sends==0);
    fixture::Setup();BeginScan(Kind::Item);assert(g_scanning && g_scanIds.size()==3 && fixture::sends==0);
    g_detailPending=101;DetailPacket detail;detail.uid=101;assert(::Send(reinterpret_cast<uint8_t*>(&detail),sizeof(detail)));fixture::Deliver();assert(g_detailObserved);
    fixture::Setup();s=fixture::Capture();auto plan=sdgo::hangar::Sort(s,g_rules);g_runner.Start(plan,s);auto action=*g_runner.Next(s);auto bytes=Packet(action);
    fixture::rejected=true;g_runner.Sent(1,::Send(bytes.data(),static_cast<int>(bytes.size())));fixture::Deliver();g_runner.Observe(fixture::Capture(),200,100);assert(g_runner.state()==State::Unknown);
    const auto epoch=g_epoch;::Clear(fixture::info.data(),nullptr);assert(g_epoch!=epoch);
    fixture::Setup();fixture::panel[0x162]=0;Snapshot invalid;assert(!GetSnapshot(invalid));
    fixture::Setup();fixture::info[0x20]=0;assert(!GetSnapshot(invalid) && fixture::sends==0);
    fixture::Setup();fixture::Put(fixture::app.data(),0x4A0,uint32_t{22});assert(!Visible() && !GetSnapshot(invalid));
    // The real lobby's service object has no game-mode member at +0C. Both
    // nonzero neighboring heap bytes and an unreadable service must be ignored.
    fixture::Setup();fixture::Put(fixture::app.data(),0x4A4,reinterpret_cast<void*>(1));assert(Visible() && GetSnapshot(invalid));
    auto* guarded=static_cast<uint8_t*>(VirtualAlloc(nullptr,8192,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));assert(guarded);
    DWORD oldProtection=0;assert(VirtualProtect(guarded+4096,4096,PAGE_NOACCESS,&oldProtection));
    fixture::Put(fixture::app.data(),0x4A4,guarded+4096-8);assert(Visible() && GetSnapshot(invalid));
    assert(VirtualFree(guarded,0,MEM_RELEASE));
    for(uint32_t state:{0u,4u,21u,22u,23u,24u,25u,30u,UINT32_MAX}){
        fixture::Put(fixture::app.data(),0x4A0,state);assert(!Visible() && !GetSnapshot(invalid));
    }
    fixture::Setup();fixture::owned[1].lock=1;s=fixture::Capture();assert(ById(s,102)->lock==1);fixture::Run(sdgo::hangar::Sort(s,g_rules));assert(ById(fixture::Capture(),102)->slot==3);
    // Failed Mod initialization leaves the original unit-management actions usable.
    fixture::Setup();page_fixture::Setup(fixture::grid.data());page_fixture::failSkin=true;
    assert(!g_ui.Attach(fixture::grid.data(),page_fixture::Bind()));
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"UnitDeleteBtn"));
    assert(fixture::nativeClicks==1 && fixture::sends==0);
    // Sorting submits exactly one native authenticated server request. It never
    // moves client-only slots or reports transport acceptance as completion.
    fixture::Setup();page_fixture::Setup(fixture::grid.data());assert(g_ui.Attach(fixture::grid.data(),page_fixture::Bind()));
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarSort"));
    assert(!g_mutating && g_needsRelog && !g_ui.Exchange() && page_fixture::Shown(L"UnitDeleteBtn"));
    assert(fixture::sends==1 && fixture::pending.size()==0x159 && Read<uint16_t>(fixture::pending.data())==0x891);
    assert(!memcmp(fixture::pending.data()+0x1B,kSortRecipient,sizeof(kSortRecipient)-1));
    assert(fixture::refreshOrder.empty() && ById(fixture::Capture(),102)->slot==3);
    assert(page_fixture::children.at(L"ModHangarNotice0")->text.find(L"已提交")!=std::wstring::npos);
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarSort"));assert(fixture::sends==1);
    // Exercise the production native page event handler all the way through
    // detail requests, preview, both confirmations and synthetic native replies.
    fixture::Setup();page_fixture::Setup(fixture::grid.data());assert(g_ui.Attach(fixture::grid.data(),page_fixture::Bind()));
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarOpen"));
    assert(g_ui.Exchange() && fixture::sends==0);
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarGB"));
    assert(!g_scanning && g_hasPreview && g_preview.removed==1 && !g_mutating && fixture::gb==0 && fixture::sends==0);
    const auto beforeConfirm=fixture::sends;
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarConfirm"));
    assert(g_ui.Armed() && !g_mutating && fixture::sends==beforeConfirm);
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarConfirm"));
    assert(g_submittingGB && !g_mutating && fixture::sends==beforeConfirm);
    for(int i=0;i<30 && g_submittingGB;++i){g_nextTick=0;Pump();}
    assert(!g_submittingGB && g_needsRelog && fixture::mailRequests.size()==3 && fixture::gb==0 && fixture::owned.size()==5);
    assert(std::string(reinterpret_cast<const char*>(fixture::mailRequests.back().data()+0x2C)).back()=='C');
    fixture::Setup();page_fixture::Setup(fixture::grid.data());assert(g_ui.Attach(fixture::grid.data(),page_fixture::Bind()));
    g_ui.Open();BeginScan(Kind::GB);g_ui.Arm(true);SubmitGB();
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarCancel"));g_nextTick=0;Pump();
    assert(!g_submittingGB && fixture::sends==0);
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarCancel"));assert(!g_ui.Exchange());
    // A second confirmation must never bypass the current native snapshot.
    g_ui.Open();g_scanReward=Kind::GB;Preview(GBPreview(fixture::Capture(),g_rules,1));
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarConfirm"));assert(g_ui.Armed());
    fixture::owned[0].lock=1;const auto beforeChange=fixture::sends;
    HandleButton(fixture::grid.data(),page_fixture::Pointer(L"ModHangarConfirm"));
    assert(!g_hasPreview && !g_mutating && fixture::sends==beforeChange && !g_ui.Armed());
    g_ui.Open();g_ui.Arm(true);page_fixture::dialog[0x162]=0;g_nextTick=0;Pump();
    assert(!g_ui.Exchange() && !g_ui.Armed());

    // The exported observer is read-only without a valid run. Mutating actions
    // require the exact canonical client run hash, native UI thread and real
    // visible/enabled control.
    SetEnvironmentVariableA("SDGO_GAME_TEST_RUN",nullptr);
    SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE",nullptr);
    fixture::Setup();page_fixture::Setup(fixture::grid.data());assert(g_ui.Attach(fixture::grid.data(),page_fixture::Bind()));
    SdgoHangarGameTestSnapshotV1 observed{sizeof(observed),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&observed));
    assert(!observed.run_hash && observed.process_id==GetCurrentProcessId() &&
           observed.ui_thread_id==GetCurrentThreadId() && observed.loaded && observed.visible &&
           observed.unit_count==5 && observed.capacity==8 && observed.active_slot==8);
    assert(!SdgoHangarGameTestActionV1(1,SDGO_HANGAR_GAME_TEST_ACTION_OPEN));
    SdgoHangarGameTestUnitV1 untouched{};untouched.uid=UINT64_MAX;
    assert(!SdgoHangarGameTestGetUnitsV1(1,0,&untouched,sizeof(untouched),1) && untouched.uid==UINT64_MAX);

    SetEnvironmentVariableA("SDGO_GAME_TEST_RUN","not-a-uuid");
    SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE","client");
    observed={sizeof(observed),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&observed) && !observed.run_hash);
    SetEnvironmentVariableA("SDGO_GAME_TEST_RUN","12345678-1234-1234-1234-123456789abc");
    SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE","server");
    observed={sizeof(observed),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&observed) && !observed.run_hash);
    SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE","client");
    observed={sizeof(observed),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&observed) && observed.run_hash);
    const auto runHash=observed.run_hash;

    SdgoHangarGameTestSnapshotV1 tooSmall{};
    tooSmall.size=sizeof(tooSmall)-1;tooSmall.version=SDGO_GAME_TEST_PAGE_OBSERVER_VERSION;tooSmall.session=UINT64_MAX;
    assert(!SdgoHangarGameTestGetSnapshotV1(&tooSmall) && tooSmall.session==UINT64_MAX);
    assert(!SdgoHangarGameTestGetSnapshotV1(reinterpret_cast<SdgoHangarGameTestSnapshotV1*>(1)));
    std::array<SdgoHangarGameTestUnitV1,5> unitPage{};
    unitPage[0].uid=UINT64_MAX;
    assert(!SdgoHangarGameTestGetUnitsV1(runHash,5,unitPage.data(),sizeof(unitPage[0]),5) && unitPage[0].uid==UINT64_MAX);
    assert(!SdgoHangarGameTestGetUnitsV1(runHash,0,unitPage.data(),sizeof(unitPage[0])-1,5) && unitPage[0].uid==UINT64_MAX);
    assert(SdgoHangarGameTestGetUnitsV1(runHash,0,unitPage.data(),sizeof(unitPage[0]),5)==5);
    assert(unitPage[0].uid==101 && unitPage[0].unit_id==10 && unitPage[0].slot==1);
    assert(unitPage[4].uid==105 && unitPage[4].slot==8 &&
           (unitPage[4].protection_flags&SDGO_HANGAR_GAME_TEST_PROTECTED_ACTIVE));

    int otherThreadAccepted=1;
    std::thread other([&]{otherThreadAccepted=SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_OPEN);});
    other.join();assert(!otherThreadAccepted);
    assert(!SdgoHangarGameTestActionV1(runHash^1,SDGO_HANGAR_GAME_TEST_ACTION_OPEN));
    auto* savedGrid=g_grid;g_grid=fixture::panel.data();
    assert(!SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_OPEN));g_grid=savedGrid;
    const auto dialogIdentity=Read<void*>(page_fixture::dialog.data());
    page_fixture::Put(page_fixture::dialog.data(),0,reinterpret_cast<void*>(0xBAD));
    assert(!SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_OPEN));
    page_fixture::Put(page_fixture::dialog.data(),0,dialogIdentity);
    assert(SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_OPEN));
    observed={sizeof(observed),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&observed) && observed.action_sequence==1 &&
           observed.last_action==SDGO_HANGAR_GAME_TEST_ACTION_OPEN && !observed.reply_sequence);
    assert(!SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_SORT)); // hidden after Open
    assert(!SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM)); // disabled before preview
    assert(SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_GB));
    assert(SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM));
    observed={sizeof(observed),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&observed) && observed.action_sequence==3 &&
           observed.armed && !observed.busy && !observed.reply_sequence);
    assert(SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_CONFIRM));
    observed={sizeof(observed),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&observed) && observed.action_sequence==4 &&
           observed.busy && !observed.reply_sequence && !observed.needs_relog);
    assert(SdgoHangarGameTestActionV1(runHash,SDGO_HANGAR_GAME_TEST_ACTION_CANCEL));

    uint8_t detailReply[153]={};fixture::Put(detailReply,8,uint64_t{101});
    g_detailPending=101;g_detailObserved=false;
    ::Sync(fixture::info.data(),nullptr,detailReply);
    observed={sizeof(observed),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&observed) && observed.reply_sequence==1);
    ::Sync(fixture::info.data(),nullptr,detailReply);
    SdgoHangarGameTestSnapshotV1 duplicateReply{sizeof(duplicateReply),SDGO_GAME_TEST_PAGE_OBSERVER_VERSION};
    assert(SdgoHangarGameTestGetSnapshotV1(&duplicateReply) && duplicateReply.reply_sequence==1);
    SetEnvironmentVariableA("SDGO_GAME_TEST_RUN",nullptr);
    SetEnvironmentVariableA("SDGO_GAME_TEST_ROLE",nullptr);
    std::cout<<"HANGAR NATIVE ADAPTER FIXTURE PASSED (production snapshot, sends, native acknowledgements, GB/item, detail freshness, logout, hidden/battle gates)\n";
}
