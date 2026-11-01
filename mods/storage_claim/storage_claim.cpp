#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <stdexcept>
#include <vector>
#include "sdgo/sdgo_api.h"
#include "sdgo/storage_claim_model.h"
#include "native_ui.h"

namespace {
using namespace sdgo::storage;
using NativeInit=void(__thiscall*)(void*);
using NativeUI=uint32_t(__thiscall*)(void*,void*,uint32_t,uintptr_t,void*);
using NativePacket=uint32_t(__thiscall*)(void*,uintptr_t,uintptr_t,uintptr_t,uint32_t,const uint8_t*);
using NativeSession=uint8_t*(__cdecl*)();
using NativeSend=int(__thiscall*)(void*,int,const uint8_t*,int,int);
using NativeBusy=uint8_t(__thiscall*)(void*);
const SdgoApi* g_api=nullptr;SdgoMod* g_mod=nullptr;
NativeInit g_init=nullptr;NativeUI g_event=nullptr;NativePacket g_packet=nullptr;
NativeSession g_session=nullptr,g_app=nullptr;NativeSend g_send=nullptr;NativeBusy g_busy=nullptr;
ui::Page g_ui;Batch g_batch;void* g_service=nullptr;void* g_owner=nullptr;
UINT_PTR g_timer=0;DWORD g_thread=0;bool g_armed=false,g_pumping=false;
Tab g_armTab=Tab::Items;uint64_t g_armSession=0,g_nextSend=0,g_observedSession=0;
uint32_t g_interval=200,g_timeout=10000,g_limit=5000;uint16_t g_error=0;
struct RetiredCheck {Tab tab;Key key;};
std::vector<RetiredCheck> g_retiredChecks;

template<class T>T Read(const void* p,size_t offset=0){T v;memcpy(&v,static_cast<const uint8_t*>(p)+offset,sizeof(v));return v;}
template<class T>void Put(void* p,size_t offset,T value){memcpy(static_cast<uint8_t*>(p)+offset,&value,sizeof(value));}
uint64_t SessionUnsafe() {
    auto* session=g_session();if(!session || Read<int>(session,0x14)<0)return 0;
    return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(session))<<32)|Read<uint32_t>(session,0x14);
}
uint64_t Session(){__try{return SessionUnsafe();}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}}
bool AvailableUnsafe() {
    if(!g_ui.Owns(g_service) || !g_ui.Visible())return false;
    auto* app=g_app();if(!app)return false;auto* game=Read<uint8_t*>(app,0x4A4);
    return !game || !Read<void*>(game,0x0C);
}
bool Available(){__try{return AvailableUnsafe();}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
Tab CurrentTab(){const auto tab=Read<int>(g_service,0x15C);if(tab<0 || tab>1)throw std::runtime_error("unknown storage tab");return static_cast<Tab>(tab);}
std::vector<Entry> Snapshot(void* service,Tab tab,uint32_t& total) {
    total=Read<uint32_t>(service,0x168);const auto stride=tab==Tab::Items?28u:402u;
    auto* first=Read<uint8_t*>(service,tab==Tab::Items?0x180:0x170);
    auto* end=Read<uint8_t*>(service,tab==Tab::Items?0x184:0x174);
    const auto a=reinterpret_cast<uintptr_t>(first),b=reinterpret_cast<uintptr_t>(end);
    if(b<a || b-a>stride*5 || (b-a)%stride || total>1000000)throw std::runtime_error("invalid storage page");
    std::vector<Entry> entries;
    for(auto* p=first;p!=end;p+=stride) {
        Entry entry;
        if(tab==Tab::Items)entry.key.id=Read<uint64_t>(p);
        else{entry.key.id=Read<uint32_t>(p);entry.key.stamp=Read<uint64_t>(p,0x166);entry.claimed=Read<uint16_t>(p,0x156)!=0;}
        if(!entry.key.id)throw std::runtime_error("invalid reward identity");entries.push_back(entry);
    }
    return entries;
}
bool SendUnsafe(const Request& request,const uint8_t* data,int size) {
    if(!AvailableUnsafe() || SessionUnsafe()!=g_batch.session() || CurrentTab()!=request.tab)return false;
    if(request.operation==Operation::List)Put(g_service,0x160,static_cast<int>(request.page));
    auto* session=g_session();++*reinterpret_cast<uint32_t*>(session+0x58);*reinterpret_cast<uint32_t*>(session+0x5C)+=static_cast<uint32_t>(size);
    return g_send(session,Read<int>(session,0x14),data,size,0)!=0;
}
bool Send(const Request& request,const std::vector<uint8_t>& bytes){__try{return SendUnsafe(request,bytes.data(),static_cast<int>(bytes.size()));}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
void DisplayState() {
    const auto count=std::to_wstring(g_batch.completed());std::wstring text;
    if(g_batch.Busy()){g_ui.Display(L"停止领取",L"已领取 "+count+L" 项；正在等待服务器确认。");return;}
    if(g_batch.Outstanding()) {
        if(g_batch.phase()==Phase::Unknown)text=L"最后一笔结果未知，请重新登录核对；不会重试。";
        else if(g_batch.reason()==Reason::Timeout || g_batch.reason()==Reason::Disconnected)text=L"回应状态未能确认，请重新登录后再领取。";
        else text=L"正在停止；等待最后回应，不再追加领取。";
        g_ui.Display(L"等待回应",text,false);return;
    }
    if(g_batch.phase()==Phase::Complete)text=L"已完成：领取 "+count+L" 项。";
    else if(g_batch.phase()==Phase::Stopped) {
        switch(g_batch.reason()) {
        case Reason::Selection:text=L"遇到需手动选择的礼包，请单独领取。";break;
        case Reason::Rejected:text=L"服务器拒绝领取（"+std::to_wstring(g_error)+L"）；已领取 "+count+L" 项。";break;
        case Reason::Limit:text=L"达到本次领取上限，可再次领取；已领取 "+count+L" 项。";break;
        case Reason::PageLimit:text=L"礼物超出原生分页范围，请先清理已领取的信件。";break;
        case Reason::Changed:text=L"列表或页签已变化，已停止；请重新确认。";break;
        case Reason::Timeout:text=L"回应超时，已停止；请重新打开保管库核对。";break;
        case Reason::Disconnected:text=L"连接或页面已变化，已停止；请核对已领物品。";break;
        case Reason::SendFailed:text=L"请求未能确认，已停止；请核对后重试。";break;
        default:text=L"已停止，已确认领取 "+count+L" 项。";break;
        }
    }else text=L"领取当前页签全部分页；限时物品可能开始计时。";
    g_ui.Display(g_armed?L"确认领取":L"一键领取",g_armed?L"再次点击确认：领取当前页签所有奖品。":text);
}
void PumpUnsafe() {
    if(GetCurrentThreadId()!=g_thread || g_pumping)return;
    g_pumping=true;
    try {
        const auto session=Session();
        if(session && g_observedSession && session!=g_observedSession) {
            if(g_batch.Waiting(Operation::Check)) {
                if(g_retiredChecks.size()>=32)throw std::runtime_error("too many unresolved preflight sessions");
                g_retiredChecks.push_back({g_batch.tab(),g_batch.pending()});
            }
            g_batch=Batch{};g_armed=false;g_owner=nullptr;
        }
        if(session)g_observedSession=session;
        if(!Available() || !session || (g_batch.Busy() && CurrentTab()!=g_batch.tab())) {
            if(g_batch.Busy())g_batch.Disconnect();g_armed=false;
        }else {
            const auto now=GetTickCount64();g_batch.Tick(now,g_timeout);
            if(now>=g_nextSend)if(const auto request=g_batch.Next()) {
                if(request->operation!=Operation::List) {
                    uint32_t total=0;const auto entries=Snapshot(g_service,request->tab,total);
                    const auto entry=std::find_if(entries.begin(),entries.end(),[&](const Entry& e){return e.key==request->key && !e.claimed;});
                    if(entry==entries.end()){g_batch.Reject(Reason::Changed);DisplayState();g_pumping=false;return;}
                    // Native failure dialogs also consult this row index.
                    Put(g_service,0x16C,static_cast<int>(entry-entries.begin()));
                }
                const auto bytes=Packet(*request);const bool sent=Send(*request,bytes);
                g_batch.Sent(*request,now,sent);g_nextSend=now+g_interval;
                if(sent)g_api->Log(g_mod,SDGO_LOG_DEBUG,"native storage request %d, page %u",static_cast<int>(request->operation),request->page+1);
            }
            DisplayState();
        }
    }catch(const std::exception&){g_batch.Disconnect();g_armed=false;g_api->Log(g_mod,SDGO_LOG_WARN,"storage state changed; no automatic claim retry");}
    g_pumping=false;
}
void DisableUI() {
    __try{g_ui.Unpublish();}__except(EXCEPTION_EXECUTE_HANDLER){}
    g_ui.Detach();
}
void CALLBACK Timer(HWND,UINT,UINT_PTR,DWORD) {
    __try{PumpUnsafe();}__except(EXCEPTION_EXECUTE_HANDLER){g_pumping=false;g_batch.Disconnect();DisableUI();}
}
bool AttachUnsafe(void* self){return g_ui.Attach(self);}
bool Attach(void* self){__try{return AttachUnsafe(self);}__except(EXCEPTION_EXECUTE_HANDLER){g_ui.Detach();return false;}}
void __fastcall Init(void* self,void*) {
    if(g_timer){KillTimer(nullptr,g_timer);g_timer=0;}
    if(g_batch.Busy())g_batch.Stop(Reason::Changed);g_armed=false;g_ui.Detach();
    g_init(self);g_service=self;g_thread=GetCurrentThreadId();
    try {
        if(Attach(self))g_timer=SetTimer(nullptr,0,75,&Timer);
        if(!g_timer){g_ui.Unpublish();g_ui.Detach();g_api->Log(g_mod,SDGO_LOG_ERROR,"storage UI unavailable; native controls preserved");}
    }catch(const std::exception&){g_ui.Detach();g_api->Log(g_mod,SDGO_LOG_ERROR,"storage UI initialization failed; native controls preserved");}
}
uint32_t HandleUI(void* handler,void* self,uint32_t event,uintptr_t second,void* control) {
    if(self==g_service && Available()) {
        if(event==0x101 && g_ui.Click(control)) {
            if(g_batch.Busy()){g_batch.Stop(Reason::Cancelled);DisplayState();return 0;}
            if(g_batch.Outstanding()){DisplayState();return 0;}
            const auto session=Session();const auto tab=CurrentTab();
            if(!session || !Read<uint8_t>(self,0x190) || g_busy(self)) {g_ui.Display(L"一键领取",L"原生操作仍在处理，请稍候再领取。");return 0;}
            if(!g_armed || g_armTab!=tab || g_armSession!=session) {
                g_armed=true;g_armTab=tab;g_armSession=session;DisplayState();return 0;
            }
            g_armed=false;g_error=0;g_owner=self;
            if(g_batch.Start(tab,session,Read<uint32_t>(self,0x168),g_limit,GetTickCount64()))g_nextSend=0;
            DisplayState();return 0;
        }
        // The close buttons still work. Mutating rows, tab changes and native
        // pagination cannot race a batch; a stopped batch restores native input.
        const bool close=event==0x101 && (control==Read<void*>(self,0x9C) || control==Read<void*>(self,0xA0));
        if(g_batch.Busy() || g_batch.Outstanding()) {
            if(!close)return 0;
            g_batch.Stop(Reason::Cancelled);
        }
        g_armed=false;
    }
    return g_event(handler,self,event,second,control);
}
uint32_t __fastcall UIEvent(void* handler,void*,void* self,uint32_t event,uintptr_t second,void* control) {
    try{return HandleUI(handler,self,event,second,control);}catch(const std::exception&){g_batch.Disconnect();return 0;}
}
bool OwnsReply(void* self){return self==g_owner && g_batch.Outstanding() && Session()==g_batch.session();}
Key ReplyKey(Tab tab,const uint8_t* data,Key expected,bool giftClaim=false) {
    Key key;
    if(tab==Tab::Items)key.id=Read<uint64_t>(data,6);
    else {
        key.id=Read<uint32_t>(data,6);
        if(giftClaim) {
            const uint32_t high=Read<uint32_t>(data,10);
            const uint32_t sign=(static_cast<int32_t>(static_cast<uint32_t>(expected.id))<0)?UINT32_MAX:0;
            if(high!=sign)key.id=UINT64_MAX;
            key.stamp=expected.stamp;
        }else key.stamp=Read<uint64_t>(data,10);
    }
    return key;
}
uint32_t PacketUnsafe(void* self,uintptr_t a,uintptr_t b,uintptr_t c,uint32_t command,const uint8_t* data) {
    const auto op=static_cast<uint16_t>(command);
    if(op==0x4E7 || op==0x722) {
        const auto tab=op==0x4E7?Tab::Items:Tab::Gifts;
        const auto key=ReplyKey(tab,data,{});
        const auto retired=std::find_if(g_retiredChecks.begin(),g_retiredChecks.end(),[&](const RetiredCheck& p){return p.tab==tab && p.key==key;});
        if(retired!=g_retiredChecks.end()){g_retiredChecks.erase(retired);return 0;}
        if(g_batch.Waiting(Operation::Check) && g_batch.tab()==tab && key==g_batch.pending() &&
           (self!=g_owner || Session()!=g_batch.session())) {
            g_batch.Disconnect();g_batch.CheckReply(key,true);return 0;
        }
    }
    if(op==0x5D9 || op==0x814) {
        const auto tab=op==0x5D9?Tab::Items:Tab::Gifts;
        const auto retired=std::find_if(g_retiredChecks.begin(),g_retiredChecks.end(),[&](const RetiredCheck& p){return p.tab==tab;});
        if(retired!=g_retiredChecks.end()) {
            g_retiredChecks.erase(retired);
            // Error replies carry no stable ID. Stop an overlapping new
            // preflight rather than guess which session the error belonged to.
            if(g_batch.Waiting(Operation::Check) && g_batch.tab()==tab)g_batch.Stop(Reason::Changed);
            return 0;
        }
    }
    if(OwnsReply(self)) {
        const auto tab=g_batch.tab();
        if(g_batch.Waiting(Operation::Check)) {
            const bool success=op==(tab==Tab::Items?0x4E7:0x722);
            const bool error=op==(tab==Tab::Items?0x5D9:0x814);
            if(success) {
                const auto key=ReplyKey(tab,data,g_batch.pending());
                const bool choice=tab==Tab::Gifts && (Read<uint32_t>(data,0x32)!=0);
                g_batch.CheckReply(key,true,choice);
                // Our preflight is deliberately not entered into native request
                // tracking. Consume it here so cancellation/selection can never
                // fall through to the game's automatic claim sender.
                return 0;
            }
            if(error){g_error=Read<uint16_t>(data,6);g_batch.Reject();return 0;}
        }
    }
    const bool owned=OwnsReply(self);const auto tab=g_batch.tab();
    const bool list=owned && g_batch.Waiting(Operation::List) && op==(tab==Tab::Items?0x7BD:0x7BE);
    const bool claim=owned && g_batch.Waiting(Operation::Claim) && op==(tab==Tab::Items?0x7E8:0xA23);
    const auto result=g_packet(self,a,b,c,command,data); // native grants, inventory/UI refresh and ownership checks
    if(list) {
        if(CurrentTab()!=tab || Read<uint32_t>(self,0x160)!=g_batch.page()){g_batch.Reject(Reason::Changed);return result;}
        uint32_t total=0;const auto entries=Snapshot(self,tab,total);g_batch.ListReply(tab,total,entries);
    }else if(claim)g_batch.ClaimReply(ReplyKey(tab,data,g_batch.pending(),tab==Tab::Gifts),true);
    else if(owned) {
        const bool error=(g_batch.Waiting(Operation::List) && op==(tab==Tab::Items?0x8AF:0x8B0)) ||
                         (g_batch.Waiting(Operation::Claim) && op==(tab==Tab::Items?0x8DA:0x53E));
        if(error){g_error=Read<uint16_t>(data,6);g_batch.Reject();}
    }
    return result;
}
uint32_t PacketGuard(void* self,uintptr_t a,uintptr_t b,uintptr_t c,uint32_t command,const uint8_t* data) {
    try{return PacketUnsafe(self,a,b,c,command,data);}catch(const std::exception&){g_batch.Disconnect();return 0;}
}
uint32_t __fastcall Receive(void* self,void*,uintptr_t a,uintptr_t b,uintptr_t c,uint32_t command,const uint8_t* data) {
    __try{return PacketGuard(self,a,b,c,command,data);}__except(EXCEPTION_EXECUTE_HANDLER){g_batch.Disconnect();return 0;}
}
template<class T>bool Bind(T& fn,const char* name){fn=reinterpret_cast<T>(g_api->Symbol(g_mod,name));return fn!=nullptr;}
}
SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;
    const int interval=api->SettingInt(mod,"RequestIntervalMs",200),timeout=api->SettingInt(mod,"ReplyTimeoutMs",10000),limit=api->SettingInt(mod,"MaxClaims",5000);
    if(interval<100 || interval>10000 || timeout<1000 || timeout>60000 || limit<1 || limit>100000)return 0;
    g_interval=static_cast<uint32_t>(interval);g_timeout=static_cast<uint32_t>(timeout);g_limit=static_cast<uint32_t>(limit);
    if(!Bind(g_session,"storage.session") || !Bind(g_app,"game.app") || !Bind(g_send,"net.send_packet") || !Bind(g_busy,"storage.native_busy") || !g_ui.Bind(api,mod))return 0;
    if(!api->HookCode(mod,"storage.init",reinterpret_cast<void*>(&Init),reinterpret_cast<void**>(&g_init)) ||
       !api->HookCode(mod,"storage.ui_event",reinterpret_cast<void*>(&UIEvent),reinterpret_cast<void**>(&g_event)) ||
       !api->HookCode(mod,"storage.packet",reinterpret_cast<void*>(&Receive),reinterpret_cast<void**>(&g_packet)))return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: native storage claim-all UI; original authenticated preflight/grant protocol, no SQL or shared message-loop hook");
    return 1;
}
