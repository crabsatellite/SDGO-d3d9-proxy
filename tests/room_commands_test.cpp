#include "ai_test_support.h"
#include "sdgo/room_commands.h"
#include "sdgo/random_loadout.h"

namespace commands=sdgo::room_commands;
static_assert(offsetof(SdgoApi,HookSite)==offsetof(SdgoApi,HookSystemExport)+sizeof(SdgoApi::HookSystemExport),
              "Retain deployed settlement ABI slot");
static_assert(offsetof(SdgoApi,FindRoomCommand)==offsetof(SdgoApi,HookSite)+sizeof(SdgoApi::HookSite),
              "Room commands must follow the existing settlement extension");
namespace {
int calls=0;
int Service(const char* text,uint32_t length,const char* name,uint32_t flags,SdgoRoomCommand* out) {
    ++calls; return commands::Parse(text,length,name,flags,out);
}
void Matches(const char* text,uint32_t length,const char* name,uint32_t flags,bool expected) {
    SdgoRoomCommand direct{},viaApi{},viaOld{};
    SdgoApi api{};api.size=sizeof(api);api.FindRoomCommand=&Service;
    CHECK(!!commands::Parse(text,length,name,flags,&direct)==expected);
    CHECK(!!commands::Find(&api,text,length,name,flags,&viaApi)==expected);
    CHECK(std::memcmp(&direct,&viaApi,sizeof(direct))==0);
    api.size=static_cast<uint32_t>(offsetof(SdgoApi,FindRoomCommand));
    const int before=calls;
    CHECK(!!commands::Find(&api,text,length,name,flags,&viaOld)==expected);
    CHECK(calls==before);CHECK(std::memcmp(&direct,&viaOld,sizeof(direct))==0);
}
}
int main() {
    SCENARIO("core service and older-core fallback share the grammar");
    for(const char* text:{"!rand !ai3","!ai3 !rand","!rand !ai e2 a1 lv4","!ai e2 a1 lv4 !rand",
                          "!RaNd\t!AI3 LV4","room !random !ai","!ai0 !rand"}) {
        Matches(text,20,"ai",SDGO_ROOM_COMMAND_ALLOW_COUNT,true);
        CHECK(sdgo::random_loadout::Command(text,20));
    }
    SCENARIO("token boundaries, malformed suffixes, aliases, duplicates and unknown commands");
    for(const char* text:{"x!ai3","!aim","!air","!ai3x","!ai-1","!ai!rand","!rand!ai3","!randomize"}) {
        Matches(text,20,"ai",SDGO_ROOM_COMMAND_ALLOW_COUNT,false);
        CHECK(!sdgo::random_loadout::Command(text,20));
    }
    Matches("!rand2",6,"rand",0,false);
    Matches("!ai3",4,"ai",0,false);
    Matches(nullptr,20,"ai",0,false);
    Matches("!ai",3,nullptr,0,false);
    Matches("!ai",3,"!ai",0,false);
    Matches("!ai",3,"ai",2,false);
    Matches("!ai",2,"ai",0,false);
    SdgoRoomCommand command{};
    CHECK(commands::Parse("!ai3 !ai5",9,"ai",1,&command));CHECK(command.count==3);
    CHECK(commands::Parse("!ai e2 !future lv5",18,"ai",1,&command));
    std::string arguments("!ai e2 !future lv5"+command.arguments_offset,command.arguments_length);
    CHECK(arguments==" e2 ");
    CHECK(commands::Parse("!ai !rand lv4",13,"ai",1,&command));
    CHECK(command.arguments_length==1);
    CHECK(commands::Parse("!ai0003",7,"ai",1,&command));CHECK(command.count==3);
    CHECK(commands::Parse("!ai999999999999999999",20,"ai",1,&command));CHECK(command.count==UINT32_MAX);
    SCENARIO("all ASCII separators and bounded byte titles");
    for(char separator:{' ','\t','\r','\n','\v','\f'}) {
        char text[]{'!','r','a','n','d',separator,'!','a','i','3'};
        Matches(text,sizeof(text),"ai",1,true);
        CHECK(sdgo::random_loadout::Command(text,sizeof(text)));
    }
    const char earlyNul[]{'!','a','i',0,'!','r','a','n','d'};
    CHECK(!sdgo::random_loadout::Command(earlyNul,sizeof(earlyNul)));
    SYSTEM_INFO system{};GetSystemInfo(&system);
    auto memory=static_cast<char*>(VirtualAlloc(nullptr,2*system.dwPageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    CHECK(memory!=nullptr);
    if(memory) {
        DWORD old=0;CHECK(VirtualProtect(memory+system.dwPageSize,system.dwPageSize,PAGE_NOACCESS,&old)!=0);
        char* title=memory+system.dwPageSize-20;
        std::memset(title,'x',20);std::memcpy(title+16,"!ai3",4);title[15]=' ';
        Matches(title,20,"ai",1,true);
        CHECK(!commands::Parse(title,19,"rand",0,&command));
        VirtualFree(memory,0,MEM_RELEASE);
    }
    return ai_test::Finish();
}
