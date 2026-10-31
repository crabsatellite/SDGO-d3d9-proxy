#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include "sdgo/sdgo_api.h"
#include "sdgo/hangar_model.h"

namespace {
using namespace sdgo::hangar;
using NativeVoid=void(__thiscall*)(void*);
using NativeGet=void(__thiscall*)(void*,void*,uint16_t);
using NativeSync=void(__thiscall*)(void*,const uint8_t*);
using SessionFn=uint8_t*(__cdecl*)();
using NativeSend=int(__thiscall*)(void*,int,const uint8_t*,int,int);
using PeekFn=BOOL(WINAPI*)(LPMSG,HWND,UINT,UINT,UINT);
const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
NativeVoid g_page=nullptr,g_view=nullptr,g_redraw=nullptr,g_clear=nullptr;
NativeGet g_brief=nullptr,g_full=nullptr;
NativeSync g_sync=nullptr;
SessionFn g_session=nullptr,g_app=nullptr;
NativeSend g_send=nullptr;
PeekFn g_peek=nullptr;
uint8_t** g_info=nullptr;
uint8_t** g_profile=nullptr;
uint8_t* g_grid=nullptr;
void* g_gridVtable=nullptr;
DWORD g_thread=0;
uint32_t g_epoch=1;
Rules g_rules;
Runner g_runner;
Plan g_preview;
bool g_hasPreview=false,g_scanning=false,g_mutating=false,g_pumping=false,g_sortAfter=true;
bool g_needsRelog=false;
Kind g_scanReward=Kind::GB;
int g_keep=2,g_maxSlots=1200;
DWORD g_interval=150,g_timeout=10000;
ULONGLONG g_nextTick=0,g_nextSend=0,g_scanSent=0;
uint64_t g_detailPending=0;
bool g_detailObserved=false;
size_t g_scanIndex=0;
std::vector<uint64_t> g_scanIds;
Snapshot g_scanBefore;
HWND g_game=nullptr,g_toolbar=nullptr,g_popup=nullptr,g_status=nullptr,g_list=nullptr,g_confirm=nullptr;
HFONT g_font=nullptr;
HMODULE g_module=nullptr;
enum {Open=100,QuickSort=101,Sort=102,GB=103,Item=104,Confirm=105,Cancel=106,Redraw=107};

template<class T>T Read(const void* p,size_t offset=0) {T result;memcpy(&result,static_cast<const uint8_t*>(p)+offset,sizeof(T));return result;}
std::wstring Wide(const std::string& s) {
    if(s.empty())return {};
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),nullptr,0);
    if(n<=0)throw std::runtime_error("invalid UTF-8 rules");
    std::wstring w(static_cast<size_t>(n),0);
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),static_cast<int>(s.size()),w.data(),n);return w;
}
void Status(const std::wstring& s) {if(g_status)SetWindowTextW(g_status,s.c_str());}
bool Busy(){return g_scanning || g_mutating;}
void Stop(const wchar_t* text) {
    g_scanning=false;g_detailPending=0;g_hasPreview=false;
    if(g_mutating)g_runner.Disconnect();
    g_mutating=false;g_needsRelog=true;
    if(g_confirm)EnableWindow(g_confirm,FALSE);
    Status(text);
    g_api->Log(g_mod,SDGO_LOG_WARN,"operation paused; no automatic retry");
}
bool VisibleUnsafe() {
    if(!g_grid || Read<void*>(g_grid)!=g_gridVtable || !*g_info || !*g_profile)return false;
    auto* panel=Read<uint8_t*>(g_grid,0x44);
    if(!panel || !Read<uint8_t>(panel,0x162))return false;
    auto* app=g_app();
    auto* game=app?Read<uint8_t*>(app,0x4A4):nullptr;
    if(game && Read<void*>(game,0x0C))return false; // never operate in a battle
    return true;
}
bool Visible() {
    __try {return VisibleUnsafe();} __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void SnapshotUnsafe(Snapshot* s) {
    if(!VisibleUnsafe())throw std::runtime_error("hangar not open");
    auto* info=*g_info;
    if(Read<uint8_t>(info,0x20)!=1)throw std::runtime_error("hangar list is still loading");
    int capacity=Read<int>(info,0x0C);
    if(capacity<=0 || capacity>g_maxSlots || capacity>65535)throw std::runtime_error("unsupported slot capacity");
    auto* session=g_session();
    if(!session || Read<int>(session,0x14)<0)throw std::runtime_error("session disconnected");
    s->session=(static_cast<uint64_t>(g_epoch)<<32)^reinterpret_cast<uintptr_t>(session)^static_cast<uint32_t>(Read<int>(session,0x14));
    s->capacity=static_cast<uint16_t>(capacity);
    s->activeSlot=Read<uint16_t>(*g_profile,0x1AD);
    if(!s->activeSlot || s->activeSlot>s->capacity)throw std::runtime_error("unknown active unit");
    s->units.clear();
    for(int slot=1;slot<=capacity;++slot) {
        uint8_t brief[25]={};g_brief(info,brief,static_cast<uint16_t>(slot));
        if(!Read<uint32_t>(brief))continue;
        Unit u;u.type=Read<uint32_t>(brief);u.uid=Read<uint64_t>(brief,4);u.slot=Read<uint16_t>(brief,12);
        u.level=brief[14];u.rental=brief[15];u.ship=brief[16];u.lock=brief[17];u.special=brief[18];
        if(u.slot!=slot)throw std::runtime_error("slot snapshot mismatch");
        g_full(info,u.full.data(),u.slot);
        u.fullReady=Read<uint32_t>(u.full.data())==u.type && Read<uint16_t>(u.full.data(),4)==u.slot && Read<uint64_t>(u.full.data(),8)==u.uid;
        s->units.push_back(u);
    }
    Validate(*s);
}
bool GuardSnapshot(Snapshot* out) {
    __try {SnapshotUnsafe(out);return true;}
    __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION || GetExceptionCode()==EXCEPTION_IN_PAGE_ERROR?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH){return false;}
}
bool GetSnapshot(Snapshot& out) {
    try {return GuardSnapshot(&out);}catch(const std::exception&){return false;}
}
bool SendUnsafe(const uint8_t* bytes,int size) {
    if(!VisibleUnsafe())return false;
    auto* session=g_session();
    if(!session || Read<int>(session,0x14)<0)return false;
    // These are the native session's packet statistics, maintained by every
    // original send site. Never create sockets or accept a client account ID.
    ++*reinterpret_cast<uint32_t*>(session+0x58);
    *reinterpret_cast<uint32_t*>(session+0x5C)+=static_cast<uint32_t>(size);
    return g_send(session,Read<int>(session,0x14),bytes,size,0)!=0;
}
bool Send(const uint8_t* bytes,int size) {
    __try{return SendUnsafe(bytes,size);}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void RedrawUnsafe() {if(VisibleUnsafe()){g_view(g_grid);g_redraw(g_grid);}}
void RedrawNow() {__try{RedrawUnsafe();}__except(EXCEPTION_EXECUTE_HANDLER){}}
std::wstring Label(const Unit& u) {
    auto r=g_rules.find(u.type);
    if(r==g_rules.end())return L"未知机体 #"+std::to_wstring(u.type);
    const auto& v=r->second;
    std::wstring g=v.rank==4?L"S":v.rank==3?L"A":v.rank==2?L"B":L"C";
    if(v.subrank==3)g+=L"S";else if(v.subrank==4)g+=L"R";else if(v.subrank==5)g+=L"U";
    return L"["+g+L"] "+v.name+L"  槽位 "+std::to_wstring(u.slot);
}
void Preview(Plan p) {
    g_preview=std::move(p);g_hasPreview=true;
    SendMessageW(g_list,LB_RESETCONTENT,0,0);
    for(const auto& a:g_preview.actions)if(a.kind!=Kind::Swap){
        const auto* u=ById(g_preview.before,a.first);
        const auto text=Label(*u);SendMessageW(g_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(text.c_str()));
    }
    std::wstring s=L"将兑换 "+std::to_wstring(g_preview.removed)+L" 台；每种相同状态保留 "+std::to_wstring(g_keep)+L" 台。\r\n";
    if(g_scanReward==Kind::GB)s+=L"预计 "+std::to_wstring(g_preview.expectedGB)+L" GB";
    else for(const auto& item:g_preview.expectedItems){
        std::wstring name=L"原生道具";
        for(const auto& rule:g_rules)if(rule.second.item==item.first && !rule.second.rewardName.empty()){name=rule.second.rewardName;break;}
        s+=name+L" × "+std::to_wstring(item.second)+L"  ";
    }
    s+=L"（实际奖励按服务器原生配置）。\r\n锁定、出击中、舰队、租赁、已培养及不同状态机体不兑换。";
    Status(s);EnableWindow(g_confirm,g_preview.removed?TRUE:FALSE);
}
void BeginScan(Kind reward) {
    if(Busy())return;
    if(g_needsRelog){Status(L"上一笔状态尚未确认，请重新登录后再操作。");return;}
    Snapshot s;if(!GetSnapshot(s)){Status(L"请打开机库后重试；当前机库数据尚未就绪。");return;}
    g_scanBefore=s;g_scanReward=reward;g_scanIds.clear();g_scanIndex=0;g_detailPending=0;g_hasPreview=false;
    std::map<uint32_t,int> count;for(const auto& u:s.units)++count[u.type];
    for(const auto& u:s.units) {
        const auto r=g_rules.find(u.type);
        if(count[u.type]>g_keep && r!=g_rules.end() && r->second.subrank!=1 && !Protected(u,s.activeSlot) && u.level<=1)
            g_scanIds.push_back(u.uid);
    }
    EnableWindow(g_confirm,FALSE);SendMessageW(g_list,LB_RESETCONTENT,0,0);g_scanning=true;
    Status(L"正在逐台向服务器核对重复机体；此步骤不会兑换或删除机体。");
}
void StartPlan(Plan p,const Snapshot& s) {
    g_hasPreview=false;EnableWindow(g_confirm,FALSE);
    try {g_runner.Start(std::move(p),s);g_mutating=true;g_nextSend=0;}
    catch(const std::exception&){Status(L"预览之后机库已变化，请重新预览。");}
}
void BeginSort() {
    if(Busy())return;
    if(g_needsRelog){Status(L"上一笔状态尚未确认，请重新登录后再操作。");return;}
    Snapshot s;if(!GetSnapshot(s)){Status(L"请打开机库后重试；容量超出配置上限时请服主核对 MaxSlots。");return;}
    try {
        auto p=sdgo::hangar::Sort(s,g_rules);
        Status(L"正在整理；锁定、出击中、舰队、租赁与未知机体保留原位。");StartPlan(std::move(p),s);
    }catch(const std::exception&){Status(L"无法生成安全的排序方案，尚未发送请求。");}
}

LRESULT CALLBACK PanelProc(HWND,UINT,WPARAM,LPARAM);
HWND Control(HWND parent,const wchar_t* cls,const wchar_t* title,DWORD style,int id,int x,int y,int w,int h) {
    auto hwnd=CreateWindowExW(0,cls,title,WS_CHILD|WS_VISIBLE|style,x,y,w,h,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),g_module,nullptr);
    if(hwnd)SendMessageW(hwnd,WM_SETFONT,reinterpret_cast<WPARAM>(g_font),TRUE);
    return hwnd;
}
void EnsureManagerWindow() {
    if(!g_popup) {
        g_popup=CreateWindowExW(WS_EX_TOOLWINDOW,L"SDGO.HangarManager",L"机库管理",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
                               CW_USEDEFAULT,CW_USEDEFAULT,760,520,g_game,nullptr,g_module,nullptr);
        g_status=Control(g_popup,L"STATIC",L"排序通过原生换槽请求执行；重复兑换需先预览确认。",SS_LEFT,0,16,16,710,85);
        g_list=Control(g_popup,L"LISTBOX",L"",WS_BORDER|WS_VSCROLL|LBS_NOINTEGRALHEIGHT,0,16,110,710,265);
        Control(g_popup,L"BUTTON",L"一键整理",BS_PUSHBUTTON,Sort,16,395,120,32);
        Control(g_popup,L"BUTTON",L"重复换 GB",BS_PUSHBUTTON,GB,146,395,140,32);
        Control(g_popup,L"BUTTON",L"重复换经验胶囊",BS_PUSHBUTTON,Item,296,395,170,32);
        g_confirm=Control(g_popup,L"BUTTON",L"确认兑换",BS_PUSHBUTTON,Confirm,476,395,120,32);
        Control(g_popup,L"BUTTON",L"取消 / 停止",BS_PUSHBUTTON,Cancel,606,395,120,32);
        Control(g_popup,L"BUTTON",L"刷新显示",BS_PUSHBUTTON,Redraw,16,439,120,28);
        Control(g_popup,L"STATIC",L"只保留已确认结果；超时不会自动重试。状态不明时请重新登录。",SS_LEFT,0,150,443,580,28);
        EnableWindow(g_confirm,FALSE);
    }
}
void ShowManager() {
    EnsureManagerWindow();ShowWindow(g_popup,SW_SHOW);SetForegroundWindow(g_popup);
}
LRESULT CALLBACK PanelProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if(message==WM_COMMAND) {
        const int id=LOWORD(wp);
        if(id==Open){ShowManager();return 0;}
        if(id==QuickSort){ShowManager();BeginSort();return 0;}
        if(id==Sort){BeginSort();return 0;}
        if(id==GB || id==Item){BeginScan(id==GB?Kind::GB:Kind::Item);return 0;}
        if(id==Redraw){if(!Busy())RedrawNow();return 0;}
        if(id==Cancel){
            if(g_scanning){g_scanning=false;g_detailPending=0;Status(L"预览已取消，没有发送兑换请求。");}
            else if(g_mutating){g_runner.Cancel();Status(L"停止追加请求；等待最后一笔已发送请求的结果。");}
            else{g_hasPreview=false;EnableWindow(g_confirm,FALSE);ShowWindow(g_popup,SW_HIDE);}
            return 0;
        }
        if(id==Confirm && g_hasPreview && !Busy()) {
            Snapshot s;
            if(!GetSnapshot(s) || !SameLayout(g_preview.before,s)){Status(L"机库已变化，请重新预览。");g_hasPreview=false;EnableWindow(g_confirm,FALSE);return 0;}
            std::wstring text=L"确认兑换列表中的 "+std::to_wstring(g_preview.removed)+L" 台重复机体？\r\n";
            text+=L"每种相同状态至少保留 "+std::to_wstring(g_keep)+L" 台；奖励由服务器发放。\r\n兑换无法撤销，请核对列表。";
            if(MessageBoxW(g_popup,text.c_str(),L"确认重复机体兑换",MB_ICONWARNING|MB_YESNO|MB_DEFBUTTON2)==IDYES)StartPlan(g_preview,s);
            return 0;
        }
    }
    if(message==WM_CLOSE){
        if(Busy()){g_scanning=false;g_detailPending=0;g_runner.Cancel();Status(L"正在停止，等待已发送请求完成。");}
        else ShowWindow(hwnd,SW_HIDE);
        return 0;
    }
    return DefWindowProcW(hwnd,message,wp,lp);
}
BOOL CALLBACK FindGame(HWND hwnd,LPARAM) {
    DWORD pid=0;GetWindowThreadProcessId(hwnd,&pid);if(pid!=GetCurrentProcessId())return TRUE;
    wchar_t cls[80]={};GetClassNameW(hwnd,cls,80);
    if(wcscmp(cls,L"_GONLINE_")!=0 || !IsWindowVisible(hwnd))return TRUE;
    g_game=hwnd;return FALSE;
}
void EnsureToolbar() {
    if(!IsWindow(g_game)){g_game=nullptr;EnumWindows(FindGame,0);}
    if(!g_game)return;
    if(!g_toolbar) {
        g_toolbar=CreateWindowExW(0,L"SDGO.HangarManager",L"",WS_CHILD,0,0,260,34,g_game,nullptr,g_module,nullptr);
        Control(g_toolbar,L"BUTTON",L"整理机库",BS_PUSHBUTTON,QuickSort,0,0,120,32);
        Control(g_toolbar,L"BUTTON",L"重复兑换…",BS_PUSHBUTTON,Open,126,0,130,32);
    }
    RECT r{};GetClientRect(g_game,&r);
    SetWindowPos(g_toolbar,HWND_TOP,std::max(0,static_cast<int>(r.right)-276),44,260,34,SWP_NOACTIVATE);
    ShowWindow(g_toolbar,Visible()?SW_SHOWNA:SW_HIDE);
}
void Pump() {
    const auto now=GetTickCount64();
    if(g_pumping || now<g_nextTick || (g_thread && GetCurrentThreadId()!=g_thread))return;
    g_pumping=true;g_nextTick=now+75;
    try {
        EnsureToolbar();
        if(Visible() && GetForegroundWindow()==g_game && (GetAsyncKeyState(VK_F8)&1))ShowManager();
        if(Busy()) {
            Snapshot s;
            if(!GetSnapshot(s)){Stop(L"已停止：已离开机库或连接发生变化。请重新登录核对最后一笔状态。");g_pumping=false;return;}
            if(g_scanning) {
                if(!SameLayout(s,g_scanBefore)){Stop(L"机库在预览期间变化，已取消，请重新预览。");g_pumping=false;return;}
                if(g_detailPending) {
                    const auto* u=ById(s,g_detailPending);
                    if(g_detailObserved && u && u->fullReady){++g_scanIndex;g_detailPending=0;}
                    else if(now-g_scanSent>=g_timeout){Stop(L"机体详情未完整同步，未执行兑换；请重新登录后重试。");g_pumping=false;return;}
                }
                if(!g_detailPending && g_scanIndex<g_scanIds.size() && now>=g_nextSend) {
                    g_detailPending=g_scanIds[g_scanIndex];g_detailObserved=false;g_scanSent=now;
                    DetailPacket p;p.uid=g_detailPending;
                    if(!Send(reinterpret_cast<const uint8_t*>(&p),sizeof(p)))Stop(L"无法请求机体详情；没有执行兑换。");
                    g_nextSend=now+g_interval;
                    Status(L"正在核对详情 "+std::to_wstring(g_scanIndex+1)+L" / "+std::to_wstring(g_scanIds.size())+L"；尚未兑换。");
                }
                if(g_scanning && !g_detailPending && g_scanIndex==g_scanIds.size()) {
                    g_scanning=false;Preview(Duplicates(s,g_rules,g_keep,g_scanReward,g_sortAfter));
                }
            } else {
                g_runner.Observe(s,now,g_timeout);
                if(g_runner.state()==State::Ready && now>=g_nextSend) {
                    if(const auto* a=g_runner.Next(s)) {
                        const auto bytes=Packet(*a);
                        g_runner.Sent(now,Send(bytes.data(),static_cast<int>(bytes.size())));
                        g_nextSend=now+g_interval;
                    }
                }
                const auto state=g_runner.state();
                if(state==State::Complete) {
                    g_mutating=false;RedrawNow();
                    Status(L"完成 "+std::to_wstring(g_runner.completed())+L" 笔原生操作，机库已按回包同步，无需重新登录。\r\n奖励按服务器配置发放；道具使用原生奖励领取流程。");
                    g_api->Log(g_mod,SDGO_LOG_INFO,"completed %u native operations",static_cast<unsigned>(g_runner.completed()));
                } else if(state==State::Unknown) {
                    g_mutating=false;g_needsRelog=true;Status(L"操作已暂停。已确认 "+std::to_wstring(g_runner.completed())+L" 笔，最后一笔状态未知。\r\n请重新登录核对；不会自动重试，请勿立即再次兑换。");
                } else if(state==State::Stopped) {
                    g_mutating=false;RedrawNow();Status(L"已停止，保留已确认的 "+std::to_wstring(g_runner.completed())+L" 笔结果。需要继续时请重新预览。");
                } else Status(L"正在等待服务器确认 "+std::to_wstring(g_runner.completed())+L" / "+std::to_wstring(g_runner.total())+L"。可停止，不会回滚已确认结果。");
            }
        }
    }catch(const std::exception&){Stop(L"已暂停：机库数据未通过校验，请重新登录后重试。");}
    g_pumping=false;
}
BOOL WINAPI Peek(LPMSG msg,HWND hwnd,UINT lo,UINT hi,UINT remove) {
    const auto result=g_peek(msg,hwnd,lo,hi,remove);Pump();return result;
}
void __fastcall Page(void* self,void*) {
    g_page(self);g_grid=static_cast<uint8_t*>(self);g_gridVtable=Read<void*>(self);g_thread=GetCurrentThreadId();
}
void __fastcall Clear(void* self,void*) {
    ++g_epoch;g_hasPreview=false;g_scanning=false;g_detailPending=0;
    if(g_mutating){g_runner.Disconnect();g_mutating=false;Status(L"连接或机库缓存已重建，请重新预览。");}
    g_clear(self);
    g_needsRelog=false;
    if(g_confirm)EnableWindow(g_confirm,FALSE);
}
void __fastcall Sync(void* self,void*,const uint8_t* data) {
    g_sync(self,data);
    if(GetCurrentThreadId()==g_thread && data && g_detailPending && Read<uint64_t>(data,8)==g_detailPending)
        g_detailObserved=true;
}
bool LoadRules(const std::wstring& path) {
    std::ifstream f(path,std::ios::binary);if(!f)return false;
    std::string line;
    while(std::getline(f,line)) {
        if(line.empty() || line[0]=='#')continue;
        if(!line.empty() && line.back()=='\r')line.pop_back();
        std::istringstream row(line);Rule r;
        if(!(row>>r.unit>>r.rank>>r.subrank>>r.gb>>r.item>>r.amount) || !r.unit || r.rank<1 || r.rank>4 || r.subrank<1 || r.subrank>5 || r.gb<0 || r.item<0 || r.amount<0 || g_rules.count(r.unit))return false;
        if(row.peek()=='\t')row.get();
        std::string name,rewardName;std::getline(row,name,'\t');std::getline(row,rewardName);
        r.name=Wide(name);r.rewardName=Wide(rewardName);g_rules[r.unit]=std::move(r);
    }
    return !g_rules.empty();
}
template<class T>bool Bind(T& fn,const char* name){fn=reinterpret_cast<T>(g_api->Symbol(g_mod,name));return fn!=nullptr;}
}

SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;
    bool bound=Bind(g_view,"hangar.view_list") && Bind(g_redraw,"hangar.redraw") && Bind(g_info,"hangar.info_global") && Bind(g_profile,"hangar.profile_global") && Bind(g_brief,"hangar.brief") && Bind(g_full,"hangar.full") && Bind(g_session,"hangar.session") && Bind(g_send,"net.send_packet") && Bind(g_app,"game.app");
    if(!bound)return 0;
    g_keep=api->SettingInt(mod,"KeepCopies",2);g_maxSlots=api->SettingInt(mod,"MaxSlots",1200);
    int interval=api->SettingInt(mod,"RequestIntervalMs",150),timeout=api->SettingInt(mod,"ReplyTimeoutMs",10000);
    if(g_keep<1 || g_keep>20 || g_maxSlots<1 || g_maxSlots>65535 || interval<50 || interval>10000 || timeout<1000 || timeout>60000)return 0;
    g_interval=static_cast<DWORD>(interval);g_timeout=static_cast<DWORD>(timeout);g_sortAfter=api->SettingInt(mod,"SortAfterExchange",1)!=0;
    char rules[256]={};api->SettingString(mod,"RulesFile","units.tsv",rules,sizeof(rules));
    std::string relative=rules;
    if(relative.empty() || relative.find(':')!=std::string::npos || relative.find("..")!=std::string::npos || relative[0]=='/' || relative[0]=='\\')return 0;
    try {if(!LoadRules(std::wstring(api->ModDirectory(mod))+L"\\"+Wide(relative)))return 0;}catch(const std::exception&){return 0;}
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&g_api),&g_module);
    g_font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    WNDCLASSW wc{};wc.hInstance=g_module;wc.lpfnWndProc=PanelProc;wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);wc.lpszClassName=L"SDGO.HangarManager";
    if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return 0;
    if(!api->HookCode(mod,"hangar.page_frame",reinterpret_cast<void*>(&Page),reinterpret_cast<void**>(&g_page)) ||
       !api->HookCode(mod,"hangar.clear",reinterpret_cast<void*>(&Clear),reinterpret_cast<void**>(&g_clear)) ||
       !api->HookCode(mod,"hangar.sync_full",reinterpret_cast<void*>(&Sync),reinterpret_cast<void**>(&g_sync)) ||
       !api->HookImport(mod,"user32.dll","PeekMessageW",reinterpret_cast<void*>(&Peek),reinterpret_cast<void**>(&g_peek)))return 0;
    api->Log(mod,SDGO_LOG_INFO,"ready: native authenticated hangar actions, %u unit rules; no SQL credentials",static_cast<unsigned>(g_rules.size()));
    return 1;
}
