#include <windows.h>
#include "sdgo/sdgo_api.h"
#include "cache_sync.h"

namespace {
const SdgoApi* g_api=nullptr;
SdgoMod* g_mod=nullptr;
uint8_t** g_profile=nullptr;
void* g_original=nullptr;
const DWORD g_defaultMxcsr=0x1F80;
room_random_client::CacheSync g_sync;
CRITICAL_SECTION g_lock;
using FactoryFn=void*(__cdecl*)(int,uint32_t);
FactoryFn g_factory=nullptr;

room_random_client::Result GuardedPacket(BYTE* frame,uint16_t* opcode) {
    __try {
        *opcode=static_cast<uint16_t>(room_random_client::Read<uint32_t>(frame,0x14));
        if(*opcode!=0x0861 && *opcode!=0x098B && *opcode!=0x06C3) return {};
        auto* profile=g_profile ? *g_profile : nullptr;
        if(!profile) {room_random_client::Result r;r.reason=1;return r;}
        room_random_client::Context context{profile,room_random_client::Read<uint8_t*>(profile,0xEC),
                                             room_random_client::Read<uint32_t>(profile,0x138)};
        return g_sync.Process(context,*opcode,room_random_client::Read<const uint8_t*>(frame,0x18));
    } __except(EXCEPTION_EXECUTE_HANDLER) {room_random_client::Result r;r.reason=5;return r;}
}
void __stdcall OnPacket(BYTE* frame) {
    const DWORD error=GetLastError();uint16_t opcode=0;
    EnterCriticalSection(&g_lock);
    const auto result=GuardedPacket(frame,&opcode);
    LeaveCriticalSection(&g_lock);
    if(result.applied) g_api->Log(g_mod,SDGO_LOG_INFO,"native %04X applied immediately: %u human loadout record(s), unit %lu",
        opcode,result.applied,static_cast<unsigned long>(result.model));
    else if(result.reason && (opcode==0x0861 || opcode==0x098B || opcode==0x06C3))
        g_api->Log(g_mod,SDGO_LOG_WARN,"native %04X cache update skipped: context reason %u",opcode,result.reason);
    if(opcode==0x06C3 && (result.applied || result.mismatched))
        g_api->Log(g_mod,result.mismatched ? SDGO_LOG_ERROR : SDGO_LOG_INFO,
            "battle start cache: %u refreshed, %u mismatch; server unit %lu, cached unit %lu",
            result.applied,result.mismatched,static_cast<unsigned long>(result.model),static_cast<unsigned long>(result.cached));
    SetLastError(error);
}
// Interior site +5 coexists with match_ai's function-entry hook: its trampoline
// resumes at this site. Preserve the full native context before forwarding.
__declspec(naked) void DispatchSite() {
    __asm { pushfd }
    __asm { pushad }
    __asm { mov eax,esp }
    __asm { sub esp,210h }
    __asm { and esp,0FFFFFFF0h }
    __asm { mov [esp+200h],eax }
    __asm { fxsave [esp] }
    __asm { fninit }
    __asm { ldmxcsr dword ptr [g_defaultMxcsr] }
    __asm { cld }
    __asm { push ebp }
    __asm { call OnPacket }
    __asm { fxrstor [esp] }
    __asm { mov esp,[esp+200h] }
    __asm { popad }
    __asm { popfd }
    __asm { jmp dword ptr [g_original] }
}
void* __cdecl Factory(int type,uint32_t model) {
    void* result=g_factory(type,model);
    const DWORD error=GetLastError();bool expected=false,tracked=false;
    if(type==8 || type==15) {
        EnterCriticalSection(&g_lock);tracked=g_sync.ObserveFactory(model,&expected);LeaveCriticalSection(&g_lock);
        if(tracked) g_api->Log(g_mod,expected ? SDGO_LOG_INFO : SDGO_LOG_ERROR,
            "human factory type %d model %lu: %s; native result %s",type,static_cast<unsigned long>(model),
            expected ? "matches server" : "still the previous model",result ? "created" : "null");
    }
    SetLastError(error);return result;
}
}
SDGO_MOD_ENTRY {
    g_api=api;g_mod=mod;
    if(api->size<offsetof(SdgoApi,HookSite)+sizeof(api->HookSite) || !api->HookSite) return 0;
    g_profile=reinterpret_cast<uint8_t**>(api->Symbol(mod,"room_random.client_profile"));
    if(!g_profile) return 0;
    InitializeCriticalSection(&g_lock);
    if(!api->HookSite(mod,"room_random.client_dispatch",reinterpret_cast<void*>(&DispatchSite),&g_original)) return 0;
    if(!api->HookCode(mod,"room_random.client_factory",reinterpret_cast<void*>(&Factory),reinterpret_cast<void**>(&g_factory))) return 0;
    api->Log(mod,SDGO_LOG_INFO,"native loadout notices update the client cache before queued room-start commands; no client configuration");
    return 1;
}
