#include "hangar_page_fixture.h"
#include <iostream>

int main() {
    using namespace page_fixture;
    PageUi ui;Setup();assert(ui.Attach(service.data(),Bind()));
    assert(Shown(L"ModHangarSort") && Shown(L"ModHangarOpen"));
    assert(!Shown(L"ModHangarConfirm") && Shown(L"UnitDeleteBtn") && !Shown(L"HiddenNativeIcon"));
    const auto count=creates;assert(ui.Attach(service.data()) && creates==count);
    ui.Open();assert(!Shown(L"ModHangarSort") && !Shown(L"UnitDeleteBtn") && Shown(L"ModHangarGB"));
    assert(!Enabled(L"ModHangarConfirm") && ui.Click(Pointer(L"ModHangarConfirm"))==Action::None);
    ui.Rows(std::vector<std::wstring>(20,L"槽位 5 [C] 合成重复机体"));ui.State(false,true);
    assert(Enabled(L"ModHangarNext") && !Enabled(L"ModHangarPrevious"));
    ui.Page(1);assert(Enabled(L"ModHangarPrevious"));ui.Page(1);assert(!Enabled(L"ModHangarNext"));
    assert(children.at(L"ModHangarPage")->text.find(L"3 / 3")!=std::wstring::npos);
    ui.Arm(true);assert(ui.Armed() && children.at(L"ModHangarConfirm")->text.find(L"再次确认")!=std::wstring::npos);
    ui.State(true,false);assert(!ui.Armed() && !Enabled(L"ModHangarGB") && Enabled(L"ModHangarCancel"));
    ui.BeforeNativeRefresh();assert(Shown(L"UnitDeleteBtn") && !Shown(L"HiddenNativeIcon"));
    Put<uint8_t>(Pointer(L"HiddenNativeIcon"),0x162,1); // native refresh legitimately changes a child
    ui.AfterNativeRefresh();assert(!Shown(L"HiddenNativeIcon"));
    ui.Close();assert(Shown(L"UnitDeleteBtn") && Shown(L"HiddenNativeIcon") && !Shown(L"ModHangarConfirm"));
    ui.Open();ui.Arm(true);ui.Close();ui.Open();assert(!ui.Armed());
    // Bounds follow the dialog's own coordinate system and stay below its header.
    for(const auto& entry:children)if(entry.first.find(L"ModHangar")==0) {
        auto* p=entry.second->memory.data();
        assert(Get<float>(p,0x148)>=0 && Get<float>(p,0x148)+Get<int>(p,0x154)<=600);
        assert(Get<float>(p,0x14C)>=0 && Get<float>(p,0x14C)+Get<int>(p,0x158)<=700);
        assert(Get<void*>(p,0x15C)==dialog.data());
    }
    Put(service.data(),0,reinterpret_cast<void*>(0xBAD));ui.Close();assert(!ui.Ready());
    Setup();failAdd=true;assert(!ui.Attach(service.data(),Bind()) && !ui.Ready());
    Setup();Put(dialog.data(),0x154,400);assert(!ui.Attach(service.data(),Bind()) && creates==0);
    std::cout<<"HANGAR IN-GAME PAGE FIXTURE PASSED (host children, placement, pagination, two-step confirmation, original visibility, lifecycle, fail-closed layout)\n";
}
