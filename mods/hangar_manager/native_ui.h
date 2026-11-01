#pragma once

#include "sdgo/sdgo_api.h"
#include <cstdint>
#include <string>
#include <vector>

namespace sdgo::hangar::ui {
enum class Action { None, Sort, Open, GB, Item, Confirm, Cancel, Previous, Next };
struct NativeString {
    union { wchar_t small[8]; const wchar_t* pointer; } data{};
    uint32_t length=0,capacity=7;
};
static_assert(sizeof(NativeString)==24,"host UTF-16 string ABI");
struct Bindings {
    using Add=uint32_t(__thiscall*)(void*,const NativeString*,int,int,int,int,uint32_t);
    using Find=void*(__thiscall*)(void*,const NativeString*,int);
    using Text=void(__thiscall*)(void*,int,const NativeString*,int,int,int,int,uint32_t,uint32_t,uint8_t,uint32_t,int);
    using Skin=void(__thiscall*)(void*,const NativeString*,uint8_t,uint32_t,uint32_t);
    Add add=nullptr;
    Find find=nullptr;
    Text text=nullptr;
    void* buttonVtable=nullptr;
    Skin skin=nullptr;
};

// All controls are host-allocated children of the original 1330.uir dialog.
// The dialog owns their destruction. No HWND, extra renderer or asset override.
class PageUi {
public:
    bool Bind(const SdgoApi*,SdgoMod*);
    bool Attach(void* service);
    bool Attach(void* service,const Bindings& bindings);
    void Detach(); // called before native dialog destruction; never dereference stale children
    bool Ready() const {return dialog_!=nullptr;}
    bool Owns(void* service) const {return Ready() && service_==service;}
    bool Exchange() const {return exchange_;}
    bool Armed() const {return armed_;}
    void Arm(bool value);
    void Open();
    void Close();
    void Status(const std::wstring&);
    void Rows(std::vector<std::wstring>);
    void State(bool busy,bool confirm);
    void Page(int delta);
    Action Click(void* control) const;
    void BeforeNativeRefresh();
    void AfterNativeRefresh();
private:
    struct Control {void* pointer; Action action; int width; bool body; std::wstring text; uint32_t color=0; bool textSet=false;};
    struct Hidden {void* pointer; uint8_t visible;};
    Bindings bindings_;
    void* service_=nullptr;
    void* dialog_=nullptr;
    void* serviceVtable_=nullptr;
    void* dialogVtable_=nullptr;
    void* styleSource_=nullptr;
    std::vector<Control> controls_;
    std::vector<Hidden> hidden_;
    std::vector<std::wstring> rows_;
    std::wstring status_;
    uint32_t font_=1;
    uint32_t textColor_=0xFF8ED9FF,shadowColor_=0xFF000000;
    int buttonWidth_=0,buttonHeight_=0;
    int width_=0,height_=0;
    size_t page_=0;
    bool exchange_=false,busy_=false,confirm_=false,armed_=false;
    bool Add(const wchar_t*,const wchar_t*,Action,int,int,int,int,bool);
    void Text(Control&,const std::wstring&,uint32_t color);
    void Render();
    void HideOriginal();
    void RestoreOriginal();
    bool Connected();
};
}
