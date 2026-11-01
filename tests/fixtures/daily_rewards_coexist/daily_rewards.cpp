// Daily server-local double rewards at verified final settlement boundaries.
#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"
#include "reward_window.h"
#include "reward_sites.h"

namespace {
    const SdgoApi* g_api;
    SdgoMod* g_mod;
    sdgo::rewards::Window g_window;
    bool g_trace = false;
    LONG g_logged = 0;
    const DWORD g_defaultMxcsr = 0x1F80;

    // No function-call ABI exists at the intercepted instruction boundary.
    // All register/flag/FP state is saved by the thunks below. Native state is
    // changed once, before both the in-memory unit EXP update and DB/UI records.
    bool ApplyFrame(unsigned index, BYTE* frame, bool active)
    {
        if (!active || !frame || index >= sdgo::rewards::kSites.size()) return false;
        const auto& fields = sdgo::rewards::kSites[index].fields;
        uint16_t before[3] = {}, after[3] = {}, event[3] = {};
        __try {
            for (unsigned i = 0; i < 3; ++i) {
                memcpy(&before[i], frame + fields[i], sizeof(uint16_t));
                memcpy(&event[i], frame + fields[i + 3], sizeof(uint16_t));
                after[i] = static_cast<uint16_t>(sdgo::rewards::DoubleBounded(before[i], 65535));
                const uint32_t bonus = event[i] + uint32_t(after[i] - before[i]);
                event[i] = static_cast<uint16_t>(bonus > 65535 ? 65535 : bonus);
            }
            for (unsigned i = 0; i < 3; ++i) {
                memcpy(frame + fields[i], &after[i], sizeof(uint16_t));
                memcpy(frame + fields[i + 3], &event[i], sizeof(uint16_t));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        if (g_trace && InterlockedIncrement(&g_logged) <= 32)
            g_api->Log(g_mod, SDGO_LOG_INFO, "%s pilot EXP %u->%u, G %u->%u, unit EXP %u->%u",
                sdgo::rewards::kSites[index].symbol, before[0], after[0], before[1], after[1], before[2], after[2]);
        return true;
    }

    void __stdcall OnSettlement(unsigned index, BYTE* frame)
    {
        const DWORD error = GetLastError();
        SYSTEMTIME local = {};
        GetLocalTime(&local); // evaluated at settlement, never frozen at server startup
        ApplyFrame(index, frame, g_window.Contains(local.wHour * 60u + local.wMinute));
        SetLastError(error);
    }

    // FXSAVE needs a 16-byte-aligned 512-byte area. The saved stack pointer lives
    // outside that area. FNINIT/CLD give the C++ callback a normal ABI environment;
    // FXRSTOR/POPFD restore the native floating-point and direction/carry flags.
    // A JMP to the real trampoline preserves the original native stack layout.
#define REWARD_THUNK(N) \
    void* g_original##N = nullptr; \
    __declspec(naked) void Site##N() { \
        __asm { pushfd } \
        __asm { pushad } \
        __asm { mov eax, esp } \
        __asm { sub esp, 0x210 } \
        __asm { and esp, 0xfffffff0 } \
        __asm { mov [esp + 0x200], eax } \
        __asm { fxsave [esp] } \
        __asm { fninit } \
        __asm { ldmxcsr dword ptr [g_defaultMxcsr] } \
        __asm { cld } \
        __asm { push ebp } \
        __asm { push N } \
        __asm { call OnSettlement } \
        __asm { fxrstor [esp] } \
        __asm { mov esp, [esp + 0x200] } \
        __asm { popad } \
        __asm { popfd } \
        __asm { jmp dword ptr [g_original##N] } \
    }
    REWARD_THUNK(0) REWARD_THUNK(1) REWARD_THUNK(2) REWARD_THUNK(3) REWARD_THUNK(4)
    REWARD_THUNK(5) REWARD_THUNK(6) REWARD_THUNK(7) REWARD_THUNK(8) REWARD_THUNK(9)
#undef REWARD_THUNK

    const std::array<void*, 10> kDetours = {{
        reinterpret_cast<void*>(&Site0), reinterpret_cast<void*>(&Site1), reinterpret_cast<void*>(&Site2),
        reinterpret_cast<void*>(&Site3), reinterpret_cast<void*>(&Site4), reinterpret_cast<void*>(&Site5),
        reinterpret_cast<void*>(&Site6), reinterpret_cast<void*>(&Site7), reinterpret_cast<void*>(&Site8), reinterpret_cast<void*>(&Site9)
    }};
    const std::array<void**, 10> kOriginals = {{
        &g_original0, &g_original1, &g_original2, &g_original3, &g_original4,
        &g_original5, &g_original6, &g_original7, &g_original8, &g_original9
    }};

    bool Configuration()
    {
        sdgo::ModSettings settings(g_api, g_mod);
        char start[16] = {}, end[16] = {}, trace[8] = {};
        if (settings.String("StartTime", "18:00", start, sizeof(start)) >= sizeof(start) - 1 ||
            settings.String("EndTime", "22:00", end, sizeof(end)) >= sizeof(end) - 1 ||
            settings.String("Trace", "0", trace, sizeof(trace)) >= sizeof(trace) - 1 ||
            !sdgo::rewards::ParseTime(start, g_window.start) || !sdgo::rewards::ParseTime(end, g_window.end) ||
            !g_window.Valid() || (strcmp(trace, "0") && strcmp(trace, "1"))) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "need distinct StartTime/EndTime in HH:MM (00:00..23:59), Trace=0 or 1; no settlement hooks installed");
            return false;
        }
        g_trace = trace[0] == '1';
        g_api->Log(g_mod, SDGO_LOG_INFO, "daily double rewards %s inclusive to %s exclusive, server local time; pilot EXP, G and unit EXP", start, end);
        return true;
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookSite) + sizeof(api->HookSite) || !api->HookSite) return 0;
    g_api = api;
    g_mod = mod;
    if (!Configuration()) return 0;
    for (unsigned i = 0; i < sdgo::rewards::kSites.size(); ++i)
        if (!api->HookSite(mod, sdgo::rewards::kSites[i].symbol, kDetours[i], kOriginals[i])) return 0;
    return 1;
}
