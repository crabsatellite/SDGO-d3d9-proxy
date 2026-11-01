#include <windows.h>
#include "sdgo/sdgo_api.h"
namespace {
using Fn=int(__thiscall*)(void*,int,int,int,int,void*);
Fn original=nullptr;
int __fastcall Dispatch(void* self,void*,int a,int b,int c,int opcode,void* packet) {
    return original(self,a,b,c,opcode,packet);
}
}
SDGO_MOD_ENTRY {
    return api->HookCode(mod,"test.dispatch.entry",reinterpret_cast<void*>(&Dispatch),reinterpret_cast<void**>(&original));
}
