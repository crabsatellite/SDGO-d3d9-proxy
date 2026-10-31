#include "sdgo/sdgo_api.h"
namespace {
using Grid = int(__thiscall*)(void*, const unsigned char**, unsigned, int, char);
using Build = int(__thiscall*)(void*, unsigned char*, unsigned char*, unsigned);
Grid g_grid = nullptr;
Build g_build = nullptr;
int __fastcall PassGrid(void* self, void*, const unsigned char** out, unsigned id, int type, char log) {
    return g_grid(self, out, id, type, log);
}
int __fastcall PassBuild(void* self, void*, unsigned char* pilot, unsigned char* unit, unsigned id) {
    return g_build(self, pilot, unit, id);
}
}
SDGO_MOD_ENTRY {
    if (api->SettingInt(mod, "Server", 0))
        return api->HookCode(mod, "battle.build_ai_pilot", reinterpret_cast<void*>(&PassBuild), reinterpret_cast<void**>(&g_build));
    return api->HookCode(mod, "grid.find", reinterpret_cast<void*>(&PassGrid), reinterpret_cast<void**>(&g_grid));
}
