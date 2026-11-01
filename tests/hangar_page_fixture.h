#pragma once
#include "../mods/hangar_manager/native_ui.h"
#include <array>
#include <cassert>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace page_fixture {
using namespace sdgo::hangar::ui;
template<class T>void Put(void* p,size_t n,T v) {memcpy(static_cast<uint8_t*>(p)+n,&v,sizeof(v));}
template<class T>T Get(const void* p,size_t n) {T v;memcpy(&v,static_cast<const uint8_t*>(p)+n,sizeof(v));return v;}
inline std::wstring Copy(const NativeString* v) {return {v->capacity<8?v->data.small:v->data.pointer,v->length};}
struct Child {
    std::array<uint8_t,0x300> memory{};
    std::array<uint8_t,0x7C> item{};
    void* itemPointer=item.data();
    std::wstring text;
    struct Skin {std::array<uint8_t,0x48> memory{}; std::wstring path;};
    std::vector<std::unique_ptr<Skin>> skins;
    std::vector<void*> skinPointers;
    uint32_t color=0;
    int align=0;
};
inline std::array<uint8_t,0x900> service{};
inline std::array<uint8_t,0x300> dialog{};
inline std::array<void*,3> head{};
inline std::map<std::wstring,std::unique_ptr<Child>> children;
inline std::vector<std::unique_ptr<std::array<void*,3>>> nodes;
inline int creates=0;
inline void* buttonVtable=reinterpret_cast<void*>(0x123400);
inline bool failAdd=false,failSkin=false;
inline int skinCalls=0,throwSkinAt=0;
inline void SetSkins(Child& child) {
    child.skinPointers.clear();
    for(const auto& skin:child.skins)child.skinPointers.push_back(skin->memory.data());
    Put(child.memory.data(),0x1A4,child.skinPointers.data());
    Put(child.memory.data(),0x1A8,child.skinPointers.data()+child.skinPointers.size());
}
inline void AddSkin(Child& child,const std::wstring& path,uint8_t mode,uint32_t x,uint32_t y) {
    auto skin=std::make_unique<Child::Skin>();skin->path=path;
    NativeString name;name.data.pointer=skin->path.c_str();name.length=static_cast<uint32_t>(path.size());name.capacity=name.length;
    Put(skin->memory.data(),0,static_cast<void*>(skin.get())); // synthetic loaded sprite
    Put(skin->memory.data(),4,name);Put(skin->memory.data(),0x44,mode);
    Put(skin->memory.data(),0x3C,x);Put(skin->memory.data(),0x40,y);
    child.skins.push_back(std::move(skin));SetSkins(child);
}
inline Child& Create(const std::wstring& name,int x,int y,int w,int h) {
    auto child=std::make_unique<Child>();auto* p=child->memory.data();
    Put(p,0,buttonVtable);Put(p,0x15C,static_cast<void*>(dialog.data()));
    Put(p,0x148,static_cast<float>(x));Put(p,0x14C,static_cast<float>(y));Put(p,0x154,w);Put(p,0x158,h);
    Put<uint8_t>(p,0x162,1);Put<uint8_t>(p,0x163,1);Put<uint8_t>(p,0x167,1);
    Put<uint8_t>(p,0x168,1); // actual constructor default: random debug quad
    Put<uint32_t>(child->item.data(),0x6C,1);Put<uint32_t>(child->item.data(),0x58,0xFF9CB8C8);
    Put<uint32_t>(child->item.data(),0x5C,0xFF000000);
    Put(p,0x190,&child->itemPointer);Put(p,0x194,&child->itemPointer+1);
    auto node=std::make_unique<std::array<void*,3>>();(*node)[0]=head.data();(*node)[1]=head[1];(*node)[2]=p;
    *static_cast<void**>(head[1])=node->data();head[1]=node->data();nodes.push_back(std::move(node));
    auto& ref=*child;children.emplace(name,std::move(child));return ref;
}
inline uint32_t __fastcall Add(void* owner,void*,const NativeString* key,int x,int y,int w,int h,uint32_t hotkey) {
    assert(owner==dialog.data() && hotkey==0);++creates;
    if(!failAdd)Create(Copy(key),x,y,w,h);
    return 0; // real factory's status is not a pointer or a reliable success flag
}
inline void* __fastcall Find(void* owner,void*,const NativeString* key,int type) {
    assert(owner==dialog.data() && type==4);auto it=children.find(Copy(key));return it==children.end()?nullptr:it->second->memory.data();
}
inline void __fastcall Text(void* owner,void*,int index,const NativeString* value,int x,int y,int w,int h,uint32_t color,uint32_t,uint8_t,uint32_t font,int align) {
    assert(index==0 && x==2 && y==3 && w>0 && h==14 && font==1 && (align==1 || align==2));
    for(auto& entry:children)if(entry.second->memory.data()==owner){entry.second->text=Copy(value);entry.second->color=color;entry.second->align=align;return;}
    assert(false);
}
inline void __fastcall Skin(void* owner,void*,const NativeString* path,uint8_t mode,uint32_t x,uint32_t y) {
    ++skinCalls;
    if(throwSkinAt && skinCalls==throwSkinAt)throw std::runtime_error("synthetic skin initialization failure");
    if(failSkin)return;
    for(auto& entry:children)if(entry.second->memory.data()==owner){AddSkin(*entry.second,Copy(path),mode,x,y);return;}
    assert(false);
}
inline Bindings Bind() {
    return {reinterpret_cast<Bindings::Add>(&Add),reinterpret_cast<Bindings::Find>(&Find),reinterpret_cast<Bindings::Text>(&Text),buttonVtable,reinterpret_cast<Bindings::Skin>(&Skin)};
}
inline void Setup(void* existingService=nullptr) {
    children.clear();nodes.clear();dialog={};service={};creates=skinCalls=throwSkinAt=0;failAdd=failSkin=false;
    head={head.data(),head.data(),nullptr};Put(dialog.data(),0x23C,head.data());
    Put(dialog.data(),0,static_cast<void*>(reinterpret_cast<void*>(0x87654300)));
    Put(dialog.data(),0x154,600);Put(dialog.data(),0x158,700);Put<uint8_t>(dialog.data(),0x162,1);
    auto& original=Create(L"UnitDeleteBtn",132,0,128,24);
    for(int i=0;i<3;++i)AddSkin(original,L"synthetic/native-button-state-"+std::to_wstring(i),0,0,0);
    auto& hidden=Create(L"HiddenNativeIcon",0,100,20,20);hidden.memory[0x162]=0;
    void* s=existingService?existingService:service.data();
    Put(s,0,static_cast<void*>(reinterpret_cast<void*>(0x12345678)));
    Put(s,0x44,dialog.data());Put(s,0x4C,original.memory.data());
}
inline void* Pointer(const wchar_t* name) {return children.at(name)->memory.data();}
inline bool Shown(const wchar_t* name) {return Get<uint8_t>(Pointer(name),0x162)!=0;}
inline bool Enabled(const wchar_t* name) {return Get<uint8_t>(Pointer(name),0x167)!=0;}
}
