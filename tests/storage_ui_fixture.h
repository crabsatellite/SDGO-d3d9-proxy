#pragma once
#include "../mods/storage_claim/native_ui.h"
#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
namespace storage_fixture {
using namespace sdgo::storage::ui;
template<class T>void Put(void* p,size_t n,T v){memcpy(static_cast<uint8_t*>(p)+n,&v,sizeof(v));}
template<class T>T Get(const void* p,size_t n){T v;memcpy(&v,static_cast<const uint8_t*>(p)+n,sizeof(v));return v;}
inline std::wstring Copy(const String* s){return {s->capacity<8?s->data.small:s->data.pointer,s->length};}
struct Child {
    std::array<uint8_t,0x300> memory{};std::array<uint8_t,0x7C> item{};void* itemPointer=item.data();
    struct Skin {std::array<uint8_t,0x48> bytes{};std::wstring name;};
    std::vector<std::unique_ptr<Skin>> skins;std::vector<void*> pointers;
    std::wstring caption;uint32_t color=0;int align=0;
};
inline std::array<uint8_t,0x400> service{},dialog{};
inline std::map<std::wstring,std::unique_ptr<Child>> children;
inline void* buttonVtable=reinterpret_cast<void*>(0x123400);
inline int skinCalls=0,failSkinAt=0;
inline Child& Create(const std::wstring& name,int type,int x,int y,int w,int h) {
    auto c=std::make_unique<Child>();auto* p=c->memory.data();
    Put(p,0,type==4?buttonVtable:reinterpret_cast<void*>(0x123800));Put(p,0x134,type);Put(p,0x15C,dialog.data());
    Put(p,0x148,static_cast<float>(x));Put(p,0x14C,static_cast<float>(y));Put(p,0x154,w);Put(p,0x158,h);
    Put<uint8_t>(p,0x162,1);Put<uint8_t>(p,0x163,1);Put<uint8_t>(p,0x167,1);Put<uint8_t>(p,0x168,1);
    Put(p,0x190,&c->itemPointer);Put(p,0x194,&c->itemPointer+1);
    Put<uint32_t>(c->item.data(),0x58,0xFF9CB8C8);Put<uint32_t>(c->item.data(),0x5C,0xFF000000);Put<uint32_t>(c->item.data(),0x6C,1);
    auto& ref=*c;children.emplace(name,std::move(c));return ref;
}
inline void AddSkin(Child& c,const std::wstring& name,uint8_t mode,uint32_t x,uint32_t y) {
    auto s=std::make_unique<Child::Skin>();s->name=name;String path;path.data.pointer=s->name.c_str();path.length=static_cast<uint32_t>(name.size());path.capacity=path.length;
    Put(s->bytes.data(),0,static_cast<void*>(s.get()));Put(s->bytes.data(),4,path);Put(s->bytes.data(),0x44,mode);Put(s->bytes.data(),0x3C,x);Put(s->bytes.data(),0x40,y);
    c.skins.push_back(std::move(s));c.pointers.clear();for(auto& skin:c.skins)c.pointers.push_back(skin->bytes.data());
    Put(c.memory.data(),0x1A4,c.pointers.data());Put(c.memory.data(),0x1A8,c.pointers.data()+c.pointers.size());
}
inline uint32_t __fastcall Add(void* owner,void*,const String* key,int x,int y,int w,int h,uint32_t hotkey){assert(owner==dialog.data() && !hotkey);Create(Copy(key),4,x,y,w,h);return 0;}
inline void* __fastcall Find(void* owner,void*,const String* key,int type){assert(owner==dialog.data() && type==4);const auto i=children.find(Copy(key));return i==children.end()?nullptr:i->second->memory.data();}
inline void __fastcall Skin(void* owner,void*,const String* name,uint8_t mode,uint32_t x,uint32_t y) {
    if(++skinCalls==failSkinAt)throw std::runtime_error("synthetic skin failure");
    for(auto& i:children)if(i.second->memory.data()==owner){AddSkin(*i.second,Copy(name),mode,x,y);return;}assert(false);
}
inline void __fastcall Text(void* owner,void*,int index,const String* text,int,int,int w,int h,uint32_t color,uint32_t,uint8_t,uint32_t font,int align) {
    assert(index==0 && w>0 && h==14 && font==1);
    for(auto& i:children)if(i.second->memory.data()==owner){i.second->caption=Copy(text);i.second->color=color;i.second->align=align;return;}assert(false);
}
inline Bindings Bind(){return {reinterpret_cast<Bindings::Add>(&Add),reinterpret_cast<Bindings::Find>(&Find),reinterpret_cast<Bindings::Text>(&Text),reinterpret_cast<Bindings::Skin>(&Skin),buttonVtable};}
inline void* Pointer(const wchar_t* key){return children.at(key)->memory.data();}
inline void Setup() {
    children.clear();service={};dialog={};skinCalls=failSkinAt=0;
    Put(dialog.data(),0,reinterpret_cast<void*>(0x987600));Put(dialog.data(),0x154,450);Put(dialog.data(),0x158,540);Put<uint8_t>(dialog.data(),0x162,1);
    auto& a=Create(L"TabRadio0",12,35,64,124,23);auto& b=Create(L"TabRadio1",12,158,64,124,23);
    for(size_t i=0;i<4;++i)AddSkin(a,L"synthetic/storage-tab-"+std::to_wstring(i),0,0,0);
    auto& close=Create(L"CloseBtn1",4,120,484,220,31);auto& cross=Create(L"CloseBtn0",4,413,14,22,23);
    Put(service.data(),0x70,dialog.data());Put(service.data(),0xB4,a.memory.data());Put(service.data(),0xB8,b.memory.data());
    Put(service.data(),0xA0,close.memory.data());Put(service.data(),0x9C,cross.memory.data());Put<uint8_t>(service.data(),0x190,1);
}
}
