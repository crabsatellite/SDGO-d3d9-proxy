/* Minimal Lua 5.0 host for testing mission_ai.lua: base + string libraries only,
   matching what the game's scripts rely on. Usage: lua50test file1.lua [file2.lua ...] */
#include <stdio.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
int main(int argc, char** argv) {
    int i;
    lua_State* L = lua_open();
    luaopen_base(L); lua_settop(L, 0);
    luaopen_string(L); lua_settop(L, 0);
    for (i = 1; i < argc; ++i) {
        if (luaL_loadfile(L, argv[i]) || lua_pcall(L, 0, 0, 0)) {
            fprintf(stderr, "ERROR %s: %s\n", argv[i], lua_tostring(L, -1));
            return 1;
        }
    }
    lua_close(L);
    return 0;
}
