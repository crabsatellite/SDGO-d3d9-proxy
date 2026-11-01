// Execute only the verified mouse/state snippets from a locally supplied client.
// No executable entry point, imports, renderer, sockets or account are started.
#include "hangar_page_fixture.h"
#include "native_button_replay.h"
#include <iostream>

namespace {
using namespace page_fixture;
using Mouse=uint8_t(__thiscall*)(void*,uint32_t,int,int,uint32_t,uint32_t);
using Update=uint32_t(__thiscall*)(void*);
int clicks=0;
Action last=Action::None;
PageUi* current=nullptr;

void __fastcall BaseMouse(void*,void*,uint32_t,int,int,uint32_t,uint32_t) {}
void __fastcall Event(void* dialogOwner,void*,uint32_t event,uint32_t force,void* control) {
    assert(dialogOwner==dialog.data() && force==1);
    if(event==0x101){++clicks;last=current->Click(control);}
}
void __fastcall Focus(void* owner,void*,void* control) {
    assert(owner==dialog.data());Put<uint8_t>(control,0x165,1);
}
uint32_t __fastcall NoRender(void*,void*) {return 1;}
int __fastcall Hit(void* control,void*,int x,int y) {
    return x>=0 && y>=0 && x<Get<int>(control,0x154) && y<Get<int>(control,0x158);
}

using native_button_replay::Snippet;
}

int wmain(int argc,wchar_t** argv) {
    try {
        if(argc!=2)throw std::runtime_error("pass the supported local GOnline.exe; never started");
        const auto file=room_title_native::detail::ReadFile(argv[1]);
        if(room_title_native::detail::Sha256(file.data(),file.size())!="e2dba73c10302b50cb0e6f2e58dd0a14effa8d224989ae570a7f8e05af79ff92")throw std::runtime_error("unsupported client identity");
        Snippet mouse(file,0xC69080,512,"e69debfef6104196f8d5008caa9b157036d027efdba76fef166f0ba243b92c5d");
        mouse.Call(0x1D,&BaseMouse);mouse.Call(0x101,&Focus);
        for(size_t offset:{0x7Au,0x9Cu,0x174u,0x1ABu})mouse.Call(offset,&Event);
        mouse.MouseTable();mouse.Seal();
        Snippet update(file,0xC6B400,548,"7f59b79ffa250df33f572d7fb392c20ad17325d550466300cd9792e8b8873341");
        for(size_t offset:{0x3u,0x214u,0x21Bu})update.Call(offset,&NoRender);
        update.Seal(); // three skins, no animations: other native branches cannot run
        auto nativeMouse=mouse.Function<Mouse>();auto nativeUpdate=update.Function<Update>();
        std::array<void*,34> table{};table[0x60/4]=reinterpret_cast<void*>(&Hit);
        PageUi ui;current=&ui;Setup();assert(ui.Attach(service.data(),Bind()));
        auto cycle=[&](void* control) {
            Put(control,0,table.data());
            nativeMouse(control,0x200,-1,-1,0,0);nativeUpdate(control);
            nativeMouse(control,0x200,4,4,0,0);nativeUpdate(control);
            nativeMouse(control,0x201,4,4,0,0);nativeUpdate(control);
            nativeMouse(control,0x202,4,4,0,0);
        };
        // Reproduce 0.2.0: visible and input-enabled, but no state textures.
        auto& broken=Create(L"BrokenEmptyButton",0,0,128,24);
        cycle(broken.memory.data());assert(clicks==0 && Get<uint32_t>(broken.memory.data(),0x18C)==0);
        for(const auto* name:{L"ModHangarSort",L"ModHangarOpen"}) {
            const int before=clicks;cycle(Pointer(name));assert(clicks==before+1);
            assert(last==(std::wstring(name)==L"ModHangarSort"?Action::Sort:Action::Open));
        }
        ui.Open();ui.State(false,true);
        for(const auto* name:{L"ModHangarGB",L"ModHangarItem",L"ModHangarConfirm",L"ModHangarCancel"}) {
            const int before=clicks;cycle(Pointer(name));assert(clicks==before+1 && last!=Action::None);
        }
        ui.State(true,false);const auto before=clicks;
        cycle(Pointer(L"ModHangarConfirm"));cycle(Pointer(L"ModHangarGB"));assert(clicks==before);
        cycle(Pointer(L"ModHangarCancel"));assert(clicks==before+1 && last==Action::Cancel);
        // Press inside, release outside: the native state machine must not click.
        ui.State(false,true);auto* control=Pointer(L"ModHangarConfirm");
        nativeMouse(control,0x200,-1,-1,0,0);nativeUpdate(control);
        nativeMouse(control,0x200,4,4,0,0);nativeUpdate(control);
        nativeMouse(control,0x201,4,4,0,0);nativeUpdate(control);
        const auto prior=clicks;nativeMouse(control,0x202,-1,-1,0,0);assert(clicks==prior);
        std::cout<<"NATIVE MOUSE REPLAY PASSED: empty-button regression, hover/press/release, production skins, disabled gates, cancel and outside release; synthetic hit-test/focus/event sinks, no rendering or network\n";
        return 0;
    }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
