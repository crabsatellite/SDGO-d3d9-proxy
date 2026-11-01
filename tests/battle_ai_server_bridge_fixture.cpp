#include <cstdint>

namespace {
int g_calls=0;
std::uint32_t g_lastNpc=0;
void* g_lastRoom=nullptr;
void* g_lastPilot=nullptr;
void* g_lastUnit=nullptr;
}

extern "C" __declspec(dllexport) int __cdecl
SdgoBattleAiServerApplyLoadoutV1(std::uint8_t* room,std::uint8_t* pilot,
                                 std::uint8_t* unit,std::uint32_t npc) {
    ++g_calls;
    g_lastRoom=room;
    g_lastPilot=pilot;
    g_lastUnit=unit;
    g_lastNpc=npc;
    if(unit) unit[0x30]=0xA5;
    return 1;
}

extern "C" __declspec(dllexport) int __cdecl TestCalls() {return g_calls;}
extern "C" __declspec(dllexport) std::uint32_t __cdecl TestLastNpc() {return g_lastNpc;}
extern "C" __declspec(dllexport) void* __cdecl TestLastRoom() {return g_lastRoom;}
extern "C" __declspec(dllexport) void* __cdecl TestLastPilot() {return g_lastPilot;}
extern "C" __declspec(dllexport) void* __cdecl TestLastUnit() {return g_lastUnit;}
