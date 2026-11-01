#include "hangar_page_fixture.h"
#include <iostream>

int main() {
    using namespace page_fixture;
    PageUi ui;Setup();assert(ui.Attach(service.data(),Bind()));
    assert(Shown(L"ModHangarSort") && Shown(L"ModHangarOpen"));
    assert(children.at(L"ModHangarSort")->text==L"一键整理" && children.at(L"ModHangarSort")->align==2);
    for(const auto* name:{L"ModHangarSort",L"ModHangarOpen",L"ModHangarGB",L"ModHangarItem",L"ModHangarConfirm",L"ModHangarCancel"}) {
        const auto& c=*children.at(name);assert(c.skins.size()==3);
        for(size_t i=0;i<3;++i) {
            const auto& source=*children.at(L"UnitDeleteBtn")->skins[i];
            assert(c.skins[i]->memory.data()!=source.memory.data() && c.skins[i]->path==source.path);
        }
        assert(Get<int>(c.memory.data(),0x154)==127 && Get<int>(c.memory.data(),0x158)==18);
    }
    assert(children.at(L"ModHangarSort")->color==0xFF9CB8C8);
    assert(!Shown(L"ModHangarConfirm") && Shown(L"UnitDeleteBtn") && !Shown(L"HiddenNativeIcon"));
    assert(!Shown(L"ModHangarNotice0"));
    ui.Status(L"正在整理\n等待服务器确认");ui.State(true,false);
    assert(!ui.Exchange() && Shown(L"UnitDeleteBtn") && Shown(L"ModHangarNotice0"));
    assert(children.at(L"ModHangarSort")->text==L"停止整理");
    assert(ui.Click(Pointer(L"ModHangarSort"))==Action::Cancel && !Enabled(L"ModHangarOpen"));
    ui.State(false,false);assert(children.at(L"ModHangarSort")->text==L"一键整理");
    const auto count=creates;assert(ui.Attach(service.data()) && creates==count);
    ui.Open();assert(!Shown(L"ModHangarSort") && !Shown(L"UnitDeleteBtn") && Shown(L"ModHangarGB"));
    assert(!Enabled(L"ModHangarConfirm") && ui.Click(Pointer(L"ModHangarConfirm"))==Action::None);
    ui.Rows(std::vector<std::wstring>(20,L"槽位 5 [C] 合成重复机体"));ui.State(false,true);
    assert(Enabled(L"ModHangarNext") && !Enabled(L"ModHangarPrevious"));
    ui.Page(1);assert(Enabled(L"ModHangarPrevious"));ui.Page(1);assert(!Enabled(L"ModHangarNext"));
    assert(children.at(L"ModHangarPage")->text.find(L"3 / 3")!=std::wstring::npos);
    ui.Arm(true);assert(ui.Armed() && children.at(L"ModHangarConfirm")->text.find(L"再次确认")!=std::wstring::npos);
    ui.State(true,false);assert(!ui.Armed() && !Enabled(L"ModHangarGB") && Enabled(L"ModHangarCancel"));
    assert(children.at(L"ModHangarGB")->color==0xFF70808A);
    Put<uint16_t>(Pointer(L"ModHangarGB"),0x21C,0x101);Put<uint32_t>(Pointer(L"ModHangarGB"),0x18C,2);
    ui.AfterNativeRefresh();assert(Get<uint16_t>(Pointer(L"ModHangarGB"),0x21C)==0 && Get<uint32_t>(Pointer(L"ModHangarGB"),0x18C)==0);
    ui.BeforeNativeRefresh();assert(Shown(L"UnitDeleteBtn") && !Shown(L"HiddenNativeIcon"));
    Put<uint8_t>(Pointer(L"HiddenNativeIcon"),0x162,1); // native refresh legitimately changes a child
    ui.AfterNativeRefresh();assert(!Shown(L"HiddenNativeIcon"));
    ui.Close();assert(Shown(L"UnitDeleteBtn") && Shown(L"HiddenNativeIcon") && !Shown(L"ModHangarConfirm"));
    ui.Open();ui.Arm(true);ui.Close();ui.Open();assert(!ui.Armed());
    // Bounds follow the dialog's own coordinate system and stay below its header.
    for(const auto& entry:children)if(entry.first.find(L"ModHangar")==0) {
        auto* p=entry.second->memory.data();
        assert(Get<float>(p,0x148)>=418 && Get<float>(p,0x148)+Get<int>(p,0x154)<=1024);
        assert(Get<float>(p,0x14C)>=33 && Get<float>(p,0x14C)+Get<int>(p,0x158)<=768);
        assert(Get<void*>(p,0x15C)==dialog.data());
        assert(Get<uint8_t>(p,0x168)==0); // no constructor debug rectangles
    }
    Put(service.data(),0,reinterpret_cast<void*>(0xBAD));ui.Close();assert(!ui.Ready());
    Setup();failAdd=true;assert(!ui.Attach(service.data(),Bind()) && !ui.Ready());
    Setup();failSkin=true;assert(!ui.Attach(service.data(),Bind()) && !ui.Ready() && !Shown(L"ModHangarSort"));
    // Match Init's exception path after the first button was fully constructed.
    // A later skin failure must never publish either partial button.
    Setup();throwSkinAt=4;bool threw=false;
    try {ui.Attach(service.data(),Bind());}catch(const std::exception&){threw=true;ui.Detach();}
    assert(threw && !ui.Ready() && !Shown(L"ModHangarSort") && !Shown(L"ModHangarOpen"));
    assert(Shown(L"UnitDeleteBtn"));
    Setup();auto& source=*children.at(L"UnitDeleteBtn");source.skins.pop_back();SetSkins(source);
    assert(!ui.Attach(service.data(),Bind()) && creates==0);
    Setup();Put(dialog.data(),0x154,400);assert(!ui.Attach(service.data(),Bind()) && creates==0);
    Setup();Put(service.data(),0x48,static_cast<void*>(nullptr));assert(!ui.Attach(service.data(),Bind()) && creates==0);
    std::cout<<"HANGAR IN-GAME PAGE FIXTURE PASSED (host children, placement, pagination, two-step confirmation, original visibility, lifecycle, fail-closed layout)\n";
}
