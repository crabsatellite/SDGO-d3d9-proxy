#pragma once
#include "sdgo/sdgo_api.h"
#include <cstdint>
#include <string>
#include <vector>
namespace sdgo::storage::ui {
struct String {
    union {wchar_t small[8];const wchar_t* pointer;} data{};
    uint32_t length=0,capacity=7;
};
static_assert(sizeof(String)==24,"host string ABI");
struct Bindings {
    using Add=uint32_t(__thiscall*)(void*,const String*,int,int,int,int,uint32_t);
    using Find=void*(__thiscall*)(void*,const String*,int);
    using Text=void(__thiscall*)(void*,int,const String*,int,int,int,int,uint32_t,uint32_t,uint8_t,uint32_t,int);
    using Skin=void(__thiscall*)(void*,const String*,uint8_t,uint32_t,uint32_t);
    Add add=nullptr;Find find=nullptr;Text text=nullptr;Skin skin=nullptr;void* buttonVtable=nullptr;
};
class Page {
public:
    bool Bind(const SdgoApi*,SdgoMod*);
    bool Attach(void* service);
    bool Attach(void* service,const Bindings& b){bindings_=b;return Attach(service);}
    void Detach();
    void Unpublish();
    bool Owns(void* service)const{return service_==service && published_;}
    bool Click(void* pointer)const;
    bool Visible()const;
    void Display(const std::wstring& caption,const std::wstring& status,bool enabled=true);
private:
    Bindings bindings_;void* service_=nullptr;void* dialog_=nullptr;void* identity_=nullptr;
    void* button_=nullptr;void* status_=nullptr;uint32_t font_=1,color_=0xFF9CB8C8,shadow_=0xFF000000;
    int width_=0;bool published_=false;
    std::wstring caption_,message_;
};
}
