#include "native_ui.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace sdgo::hangar::ui {
namespace {
constexpr size_t kRows=9,kSummaryLines=6;
template<class T>T Read(const void* p,size_t offset=0) {T v;memcpy(&v,static_cast<const uint8_t*>(p)+offset,sizeof(v));return v;}
template<class T>void Write(void* p,size_t offset,T v) {memcpy(static_cast<uint8_t*>(p)+offset,&v,sizeof(v));}
NativeString View(const std::wstring& text) {
    NativeString v;v.length=static_cast<uint32_t>(text.size());
    if(text.size()<8)memcpy(v.data.small,text.c_str(),(text.size()+1)*sizeof(wchar_t));
    else {v.data.pointer=text.c_str();v.capacity=v.length;}
    return v;
}
bool Button(void* p,void* vtable) {return p && Read<void*>(p)==vtable;}
bool IdentityMatches(void* service,void* serviceVtable,void* dialog,void* dialogVtable) {
    __try {return Read<void*>(service)==serviceVtable && Read<void*>(service,0x44)==dialog && Read<void*>(dialog)==dialogVtable;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
std::vector<std::wstring> Lines(const std::wstring& text,size_t width) {
    std::vector<std::wstring> lines;std::wstring line;
    for(wchar_t c:text) {
        if(c==L'\r')continue;
        if(c==L'\n' || line.size()==width) {lines.push_back(line);line.clear();}
        if(c!=L'\n')line+=c;
    }
    if(!line.empty())lines.push_back(line);
    return lines;
}
}
bool PageUi::Bind(const SdgoApi* api,SdgoMod* mod) {
    bindings_.add=reinterpret_cast<Bindings::Add>(api->Symbol(mod,"hangar.ui_add_button"));
    bindings_.find=reinterpret_cast<Bindings::Find>(api->Symbol(mod,"hangar.ui_find_control"));
    bindings_.text=reinterpret_cast<Bindings::Text>(api->Symbol(mod,"hangar.ui_text_item"));
    bindings_.buttonVtable=api->Symbol(mod,"hangar.ui_button_vtable");
    bindings_.skin=reinterpret_cast<Bindings::Skin>(api->Symbol(mod,"hangar.ui_add_skin"));
    return bindings_.add && bindings_.find && bindings_.text && bindings_.buttonVtable && bindings_.skin;
}
void PageUi::Detach() {
    // Native init may already have freed the previous dialog, including all its
    // children. Drop references without attempting to restore old visibility.
    service_=dialog_=serviceVtable_=dialogVtable_=styleSource_=nullptr;controls_.clear();hidden_.clear();rows_.clear();
    page_=0;exchange_=busy_=confirm_=armed_=published_=false;
}
bool PageUi::Attach(void* service,const Bindings& bindings) {bindings_=bindings;return Attach(service);}
bool PageUi::Add(const wchar_t* name,const wchar_t* label,Action action,int x,int y,int w,int h,bool body) {
    if(action!=Action::None){w=buttonWidth_;h=buttonHeight_;}
    const std::wstring key=name;const auto native=View(key);
    bindings_.add(dialog_,&native,x,y,w,h,0);
    auto* child=bindings_.find(dialog_,&native,4);
    if(!Button(child,bindings_.buttonVtable) || Read<void*>(child,0x15C)!=dialog_)return false;
    // Keep every child private until the whole page (including skins and text)
    // is ready. Native allocation defaults to visible, even before skin loading.
    Write<uint8_t>(child,0x162,0);Write<uint8_t>(child,0x167,0);
    controls_.push_back({child,action,w,body,{}});
    // Fresh CUIBase controls draw a random debug quad by default. This flag
    // is independent of the enabled/input flags and must be cleared for labels too.
    Write<uint8_t>(child,0x168,0);
    if(action!=Action::None) {
        auto** skins=Read<uint8_t**>(styleSource_,0x1A4);
        for(size_t i=0;i<3;++i) {
            auto* skin=skins[i];
            bindings_.skin(child,reinterpret_cast<NativeString*>(skin+4),Read<uint8_t>(skin,0x44),
                           Read<uint32_t>(skin,0x3C),Read<uint32_t>(skin,0x40));
        }
        // The native mouse state machine only emits ButtonClicked after skin
        // states 1 (hover) and 2 (pressed). No textures means no click, even if
        // the control is visible and accepts input. Never alias host vectors.
        auto** first=Read<uint8_t**>(child,0x1A4);auto** last=Read<uint8_t**>(child,0x1A8);
        if(!first || reinterpret_cast<uintptr_t>(last)-reinterpret_cast<uintptr_t>(first)!=3*sizeof(void*))return false;
        for(size_t i=0;i<3;++i)if(!first[i] || !Read<void*>(first[i]))return false;
    }
    Text(controls_.back(),label,textColor_);
    return true;
}
bool PageUi::Attach(void* service) {
    if(Owns(service))return true;
    Detach();
    if(!service || !bindings_.add || !bindings_.find || !bindings_.text || !bindings_.buttonVtable || !bindings_.skin)return false;
    auto* dialog=Read<void*>(service,0x44);
    auto* source=Read<void*>(service,0x4C); // native UnitDeleteBtn
    if(!dialog || !Button(source,bindings_.buttonVtable) || Read<void*>(source,0x15C)!=dialog)return false;
    const int w=Read<int>(dialog,0x154),h=Read<int>(dialog,0x158);
    const float sx=Read<float>(source,0x148),sy=Read<float>(source,0x14C);
    if(!std::isfinite(sx) || !std::isfinite(sy) || sx<0 || sy<0 || sx>2048 || sy>128 || w<520 || w>2048 || h<600 || h>2048)return false;
    buttonWidth_=Read<int>(source,0x154);buttonHeight_=Read<int>(source,0x158);
    if(buttonWidth_<100 || buttonWidth_>256 || buttonHeight_<16 || buttonHeight_>40)return false;
    const int x=static_cast<int>(sx)+buttonWidth_+8;
    if(x+2*buttonWidth_+8>w)return false; // do not overlap existing native controls
    // 1330.uir is a full-screen 1024x768 dialog. Its unit grid begins at
    // UnitLockBtn.x=418, not at x=0. The left unit-detail view is a sibling.
    auto* lock=Read<void*>(service,0x48);
    if(!Button(lock,bindings_.buttonVtable) || Read<void*>(lock,0x15C)!=dialog)return false;
    const float left=Read<float>(lock,0x148);
    if(!std::isfinite(left) || left<0 || left>sx)return false;
    contentLeft_=static_cast<int>(left);
    contentTop_=static_cast<int>(sy)+buttonHeight_+12;
    contentWidth_=w-contentLeft_-12;
    const int bottom=h-180; // leave the native page-selector/footer entirely free
    const int rowTop=contentTop_+static_cast<int>(kSummaryLines)*22+20;
    if(contentWidth_<3*buttonWidth_+24 || rowTop+static_cast<int>(kRows)*28>bottom-50)return false;
    auto** skins=Read<uint8_t**>(source,0x1A4);auto** skinsEnd=Read<uint8_t**>(source,0x1A8);
    const auto first=reinterpret_cast<uintptr_t>(skins),last=reinterpret_cast<uintptr_t>(skinsEnd);
    if(!first || last<first || last-first<3*sizeof(void*) || last-first>16*sizeof(void*) || (last-first)%sizeof(void*))return false;
    for(size_t i=0;i<3;++i) {
        if(!skins[i] || !Read<void*>(skins[i]) || !Read<uint32_t>(skins[i],0x14))return false;
    }
    auto** begin=Read<uint8_t**>(source,0x190);auto** end=Read<uint8_t**>(source,0x194);
    if(!begin || reinterpret_cast<uintptr_t>(end)<reinterpret_cast<uintptr_t>(begin) ||
       reinterpret_cast<uintptr_t>(end)-reinterpret_cast<uintptr_t>(begin)>64*sizeof(void*) || begin==end || !*begin)return false;
    font_=Read<uint32_t>(*begin,0x6C);
    textColor_=Read<uint32_t>(*begin,0x58);shadowColor_=Read<uint32_t>(*begin,0x5C);
    styleSource_=source;
    service_=service;dialog_=dialog;width_=w;height_=h;
    serviceVtable_=Read<void*>(service);dialogVtable_=Read<void*>(dialog);
    bool ok=Add(L"ModHangarSort",L"一键整理",Action::Sort,x,static_cast<int>(sy),buttonWidth_,buttonHeight_,false) &&
        Add(L"ModHangarOpen",L"重复兑换",Action::Open,x+buttonWidth_+8,static_cast<int>(sy),buttonWidth_,buttonHeight_,false);
    for(int i=0;ok && i<1;++i) {
        const auto name=L"ModHangarNotice"+std::to_wstring(i);
        ok=Add(name.c_str(),L"",Action::None,contentLeft_,static_cast<int>(sy)+buttonHeight_,contentWidth_,14,false);
        if(ok)controls_.back().notice=true;
    }
    for(size_t i=0;ok && i<kSummaryLines;++i) {
        const auto name=L"ModHangarStatus"+std::to_wstring(i);
        ok=Add(name.c_str(),L"",Action::None,contentLeft_,contentTop_+static_cast<int>(i)*22,contentWidth_,22,true);
    }
    for(size_t i=0;ok && i<kRows;++i) {
        const auto name=L"ModHangarRow"+std::to_wstring(i);
        ok=Add(name.c_str(),L"",Action::None,contentLeft_,rowTop+static_cast<int>(i)*28,contentWidth_,26,true);
    }
    ok=ok && Add(L"ModHangarPrevious",L"上一页",Action::Previous,contentLeft_,bottom-38,96,26,true) &&
        Add(L"ModHangarNext",L"下一页",Action::Next,contentLeft_+12+buttonWidth_,bottom-38,96,26,true) &&
        Add(L"ModHangarPage",L"",Action::None,contentLeft_+24+2*buttonWidth_,bottom-38,contentWidth_-24-2*buttonWidth_,26,true) &&
        Add(L"ModHangarGB",L"预览换 GB",Action::GB,contentLeft_,bottom,150,26,true) &&
        Add(L"ModHangarItem",L"预览换经验胶囊",Action::Item,contentLeft_+12+buttonWidth_,bottom,200,26,true) &&
        Add(L"ModHangarConfirm",L"确认兑换",Action::Confirm,contentLeft_,bottom+36,274,28,true) &&
        Add(L"ModHangarCancel",L"返回机库",Action::Cancel,contentLeft_+12+buttonWidth_,bottom+36,180,28,true);
    if(!ok) {
        for(auto& control:controls_)Write<uint8_t>(control.pointer,0x162,0);
        Detach();return false;
    }
    status_.clear();
    Render(); // prepare every caption and state while all children stay hidden
    published_=true;
    Render();
    return true;
}
void PageUi::Text(Control& control,const std::wstring& value,uint32_t color) {
    if(control.textSet && control.text==value && control.color==color)return;
    const auto native=View(value);
    bindings_.text(control.pointer,0,&native,2,control.notice?0:3,control.width-4,14,color,shadowColor_,0,font_,control.action==Action::None?1:2);
    control.text=value;control.color=color;control.textSet=true;
}
bool PageUi::Connected() {
    if(!Ready())return false;
    if(!IdentityMatches(service_,serviceVtable_,dialog_,dialogVtable_)) {Detach();return false;}
    return true;
}
void PageUi::HideOriginal() {
    if(!Connected() || !exchange_)return;
    auto* head=Read<uint8_t*>(dialog_,0x23C);
    if(!head)throw std::runtime_error("missing native child list");
    auto* node=Read<uint8_t*>(head);size_t count=0;
    while(node!=head) {
        if(!node || ++count>2048)throw std::runtime_error("invalid native child list");
        auto* child=Read<void*>(node,8);
        if(!child)throw std::runtime_error("missing native child");
        const bool ours=std::any_of(controls_.begin(),controls_.end(),[child](const Control& c){return c.pointer==child;});
        if(!ours) {
            const auto old=std::find_if(hidden_.begin(),hidden_.end(),[child](const Hidden& h){return h.pointer==child;});
            if(old==hidden_.end())hidden_.push_back({child,Read<uint8_t>(child,0x162)});
            Write<uint8_t>(child,0x162,0);
        }
        node=Read<uint8_t*>(node);
    }
}
void PageUi::RestoreOriginal() {
    if(!Connected())return;
    for(const auto& h:hidden_)Write<uint8_t>(h.pointer,0x162,h.visible);
    hidden_.clear();
}
void PageUi::BeforeNativeRefresh() {if(Ready() && exchange_)RestoreOriginal();}
void PageUi::AfterNativeRefresh() {if(Ready()){HideOriginal();Render();}}
void PageUi::Open() {if(Ready()){exchange_=true;armed_=false;page_=0;HideOriginal();Render();}}
void PageUi::Close() {if(Ready()){RestoreOriginal();exchange_=false;armed_=false;confirm_=false;rows_.clear();Render();}}
void PageUi::Arm(bool value) {armed_=value;Render();}
void PageUi::Status(const std::wstring& value) {status_=value;Render();}
void PageUi::Rows(std::vector<std::wstring> value) {rows_=std::move(value);page_=0;armed_=false;Render();}
void PageUi::State(bool busy,bool confirm) {if(busy_==busy && confirm_==confirm)return;busy_=busy;confirm_=confirm;if(!confirm)armed_=false;Render();}
void PageUi::Page(int delta) {
    const size_t pages=std::max<size_t>(1,(rows_.size()+kRows-1)/kRows);
    if(delta<0 && page_)--page_;else if(delta>0 && page_+1<pages)++page_;
    Render();
}
Action PageUi::Click(void* pointer) const {
    if(!IdentityMatches(service_,serviceVtable_,dialog_,dialogVtable_))return Action::None;
    for(const auto& c:controls_)if(c.pointer==pointer && Read<uint8_t>(pointer,0x162) && Read<uint8_t>(pointer,0x167))return c.action==Action::Sort && busy_?Action::Cancel:c.action;
    return Action::None;
}
void* PageUi::ControlFor(Action action) const {
    if(action==Action::None)return nullptr;
    for(const auto& control:controls_)if(control.action==action)return control.pointer;
    return nullptr;
}
void PageUi::Render() {
    if(!Connected())return;
    const auto summary=Lines(armed_?L"再次确认将兑换清单中的机体，无法撤销。\n请核对奖励、保留份数与全部清单。\n"+status_:status_,static_cast<size_t>(contentWidth_/14));
    const size_t pages=std::max<size_t>(1,(rows_.size()+kRows-1)/kRows);
    size_t line=0,row=0;
    for(auto& c:controls_) {
        bool visible=c.body?exchange_:!exchange_;
        if(c.notice)visible=visible && !status_.empty();
        bool enabled=c.action!=Action::None && !busy_;
        if(c.action==Action::Sort)enabled=true; // same entry stops a running sort
        if(c.action==Action::Confirm)enabled=!busy_ && confirm_;
        if(c.action==Action::Cancel)enabled=true;
        if(c.action==Action::Previous)enabled=page_>0;
        if(c.action==Action::Next)enabled=page_+1<pages;
        if(c.notice) {
            std::wstring compact=status_;
            for(auto& ch:compact)if(ch==L'\r' || ch==L'\n')ch=L' ';
            const auto limit=static_cast<size_t>((contentWidth_-4)/14);
            if(compact.size()>limit)compact=compact.substr(0,limit-1)+L"…";
            Text(c,compact,0xFFE3EEF6);
        }
        else if(c.body && c.action==Action::None) {
            if(line<kSummaryLines) {Text(c,line<summary.size()?summary[line]:L"",0xFFE3EEF6);++line;}
            else if(row<kRows) {
                const size_t index=page_*kRows+row++;
                std::wstring value=index<rows_.size()?std::to_wstring(index+1)+L". "+rows_[index]:L"";
                const size_t max=static_cast<size_t>((contentWidth_-4)/14);
                if(value.size()>max)value=value.substr(0,max-1)+L"…";
                Text(c,value,0xFFE3EEF6);
            }else Text(c,L"清单 "+std::to_wstring(page_+1)+L" / "+std::to_wstring(pages)+L" 页，共 "+std::to_wstring(rows_.size())+L" 台",0xFFE3EEF6);
        }
        if(c.action==Action::Sort)Text(c,busy_?L"停止整理":L"一键整理",textColor_);
        else if(c.action==Action::Confirm)Text(c,armed_?L"再次确认兑换":L"确认兑换",enabled?textColor_:0xFF70808A);
        else if(c.action==Action::Cancel)Text(c,busy_?L"停止后续请求":L"返回机库",textColor_);
        else if(c.action!=Action::None)Text(c,c.text,enabled?textColor_:0xFF70808A);
        if(!visible || !enabled) {
            Write<uint16_t>(c.pointer,0x21C,0);Write<uint8_t>(c.pointer,0x164,0);
            Write<uint32_t>(c.pointer,0x18C,0);
        }
        Write<uint8_t>(c.pointer,0x162,published_ && visible?1:0);
        Write<uint8_t>(c.pointer,0x167,enabled?1:0);
    }
}
}
