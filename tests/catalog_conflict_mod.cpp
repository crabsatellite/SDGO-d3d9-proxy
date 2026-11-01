#include "sdgo/sdgo_api.h"
namespace {
using Create = unsigned char*(__thiscall*)(void*, unsigned, float, float, float, int, float, float, float);
using Build = int(__thiscall*)(void*, unsigned char*, unsigned char*, unsigned);
Create g_create = nullptr;
Build g_build = nullptr;
unsigned char* __fastcall PassCreate(void* self, void*, unsigned id, float x, float y, float z, int team, float dx, float dy, float dz) {
    return g_create(self, id, x, y, z, team, dx, dy, dz);
}
int __fastcall PassBuild(void* self, void*, unsigned char* pilot, unsigned char* unit, unsigned id) {
    return g_build(self, pilot, unit, id);
}
}
SDGO_MOD_ENTRY {
    if (api->SettingInt(mod, "Server", 0))
        return api->HookCode(mod, "battle.build_ai_pilot", reinterpret_cast<void*>(&PassBuild), reinterpret_cast<void**>(&g_build));
    return api->HookCode(mod, "mode.create_remote_npc", reinterpret_cast<void*>(&PassCreate), reinterpret_cast<void**>(&g_create));
}
