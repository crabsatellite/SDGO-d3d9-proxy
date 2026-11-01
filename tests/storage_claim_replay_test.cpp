#include "native_button_replay.h"
#include "storage_ui_fixture.h"
#include <iostream>
namespace {
using native_button_replay::Snippet;
using Mouse=uint8_t(__thiscall*)(void*,uint32_t,int,int,uint32_t,uint32_t);
using Update=uint32_t(__thiscall*)(void*);
using Dispatch=uint32_t(__thiscall*)(void*,uintptr_t,uintptr_t,uintptr_t,uint32_t,const uint8_t*);
sdgo::storage::ui::Page* page=nullptr;int clicks=0;uint32_t dispatched=0;const uint8_t* expectedPacket=nullptr;
void __fastcall Base(void*,void*,uint32_t,int,int,uint32_t,uint32_t){}
void __fastcall Event(void* owner,void*,uint32_t op,uint32_t force,void* control){assert(owner==storage_fixture::dialog.data() && force==1);if(op==0x101){assert(page->Click(control));++clicks;}}
void __fastcall Focus(void*,void*,void* control){storage_fixture::Put<uint8_t>(control,0x165,1);}
uint32_t __fastcall NoDraw(void*,void*){return 1;}
int __fastcall Hit(void* control,void*,int x,int y){return x>=0 && y>=0 && x<storage_fixture::Get<int>(control,0x154) && y<storage_fixture::Get<int>(control,0x158);}
void Record(void* self,const uint8_t* packet,uint32_t code){assert(self==storage_fixture::service.data() && packet==expectedPacket);dispatched=code;}
void __fastcall Unexpected4(void* self,void*,const uint8_t* packet){Record(self,packet,UINT32_MAX);}
void __fastcall Error8(void* self,void*,uint32_t code,const uint8_t* packet){Record(self,packet,code);}
void __fastcall Items(void* self,void*,const uint8_t* packet){Record(self,packet,0x7BD);}
void __fastcall Gifts(void* self,void*,const uint8_t* packet){Record(self,packet,0x7BE);}
void __fastcall ItemCheck(void* self,void*,const uint8_t* packet){Record(self,packet,0x4E7);}
void __fastcall GiftCheck(void* self,void*,const uint8_t* packet){Record(self,packet,0x722);}
void __fastcall ItemClaim(void* self,void*,const uint8_t* packet){Record(self,packet,0x7E8);}
void __fastcall GiftClaim(void* self,void*,const uint8_t* packet){Record(self,packet,0xA23);}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=2)throw std::runtime_error("pass the supported local GOnline.exe; it is only read");
        const auto bytes=room_title_native::detail::ReadFile(argv[1]);
        if(room_title_native::detail::Sha256(bytes.data(),bytes.size())!="e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92")throw std::runtime_error("unsupported client");
        Snippet mouse(bytes,0xC69080,512,"e69debfef6104196f8d5008caa9b157036d027efdba76fef166f0ba243b92c5d");
        mouse.Call(0x1D,&Base);mouse.Call(0x101,&Focus);for(size_t p:{0x7Au,0x9Cu,0x174u,0x1ABu})mouse.Call(p,&Event);mouse.MouseTable();mouse.Seal();
        Snippet update(bytes,0xC6B400,548,"7f59b79ffa250df33f572d7fb392c20ad17325d550466300cd9792e8b8873341");
        for(size_t p:{0x3u,0x214u,0x21Bu})update.Call(p,&NoDraw);update.Seal();
        storage_fixture::Setup();sdgo::storage::ui::Page ui;page=&ui;assert(ui.Attach(storage_fixture::service.data(),storage_fixture::Bind()));
        auto* control=storage_fixture::Pointer(L"ModStorageClaimAll");std::array<void*,34> table{};table[24]=reinterpret_cast<void*>(&Hit);storage_fixture::Put(control,0,table.data());
        auto nativeMouse=mouse.Function<Mouse>();auto nativeUpdate=update.Function<Update>();
        nativeMouse(control,0x200,4,4,0,0);nativeUpdate(control);nativeMouse(control,0x201,4,4,0,0);nativeUpdate(control);nativeMouse(control,0x202,4,4,0,0);assert(clicks==1);
        ui.Display(L"等待回应",L"最后一笔仍在等待回执。",false);nativeMouse(control,0x201,4,4,0,0);nativeUpdate(control);nativeMouse(control,0x202,4,4,0,0);assert(clicks==1);
        // Execute the native storage dispatcher with synthetic handler sinks.
        // This checks the real five-stack-argument ABI and success/error routes.
        Snippet dispatch(bytes,0x57F570,768,"ee3582d8812c8782d93706b5b1e117e920bbf07918e0e726364d0aa7ce0f0d1f");
        dispatch.RelocatePointer(0x34,0x57F570,768);dispatch.RelocatePointer(0x3B,0x57F570,768);
        dispatch.RelocatePointer(0x13A,0x57F570,768);dispatch.RelocatePointer(0x141,0x57F570,768);
        for(size_t p=0x1FC;p<0x214;p+=4)dispatch.RelocatePointer(p,0x57F570,768);
        for(size_t p=0x270;p<0x288;p+=4)dispatch.RelocatePointer(p,0x57F570,768);
        for(size_t p:{0x44u,0x54u,0xB7u,0xC7u,0xD7u,0x106u,0x159u,0x169u,0x197u,0x1BCu,0x1CCu})dispatch.Call(p,&Error8);
        for(size_t p:{0x63u,0x81u,0x90u,0xF6u,0x1DBu})dispatch.Call(p,&Unexpected4);
        dispatch.Call(0x72,&ItemCheck);dispatch.Call(0x115,&GiftCheck);dispatch.Call(0x149,&Gifts);dispatch.Call(0x178,&Items);dispatch.Call(0x187,&ItemClaim);dispatch.Call(0x1F1,&GiftClaim);dispatch.Seal();
        std::array<uint8_t,64> packet{};expectedPacket=packet.data();auto nativeDispatch=dispatch.Function<Dispatch>();
        for(uint32_t code:{0x7BDu,0x7BEu,0x4E7u,0x722u,0x7E8u,0xA23u,0x5D9u,0x814u,0x8DAu,0x53Eu,0x8AFu,0x8B0u}) {
            dispatched=0;assert(nativeDispatch(storage_fixture::service.data(),11,22,33,code,packet.data())==0);assert(dispatched==code);
        }
        std::cout<<"STORAGE NATIVE REPLAY PASSED: production claim button through real mouse/update snippets and native storage packet ABI/routes; synthetic focus/hit-test/render/network handlers, no game entry point or account\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
