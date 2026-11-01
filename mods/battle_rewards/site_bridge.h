// An instruction boundary has no C++ calling convention. Preserve native
// registers, flags, x87 and SIMD state before calling the settlement collector.
#pragma once
#include <windows.h>
#include <array>

namespace sdgo::battle_rewards::bridge {
using Callback = void(__stdcall*)(unsigned, BYTE*);
inline Callback callback = nullptr;
inline const DWORD defaultMxcsr = 0x1F80;

inline void __stdcall Dispatch(unsigned index, BYTE* frame) noexcept {
    const DWORD error = GetLastError();
    try { if (callback) callback(index, frame); }
    catch (...) { /* no C++ exception may unwind through native frames */ }
    SetLastError(error);
}

#define SDGO_REWARD_SITE(N) \
    inline void* original##N = nullptr; \
    __declspec(naked) inline void Site##N() { \
        __asm { pushfd } \
        __asm { pushad } \
        __asm { mov eax, esp } \
        __asm { sub esp, 0x210 } \
        __asm { and esp, 0xfffffff0 } \
        __asm { mov [esp + 0x200], eax } \
        __asm { fxsave [esp] } \
        __asm { fninit } \
        __asm { ldmxcsr dword ptr [defaultMxcsr] } \
        __asm { cld } \
        __asm { push ebp } \
        __asm { push N } \
        __asm { call Dispatch } \
        __asm { fxrstor [esp] } \
        __asm { mov esp, [esp + 0x200] } \
        __asm { popad } \
        __asm { popfd } \
        __asm { jmp dword ptr [original##N] } \
    }
SDGO_REWARD_SITE(0) SDGO_REWARD_SITE(1) SDGO_REWARD_SITE(2)
SDGO_REWARD_SITE(3) SDGO_REWARD_SITE(4) SDGO_REWARD_SITE(5)
SDGO_REWARD_SITE(6) SDGO_REWARD_SITE(7) SDGO_REWARD_SITE(8)
SDGO_REWARD_SITE(9)
#undef SDGO_REWARD_SITE

inline const std::array<void*, 10> detours = {{
    reinterpret_cast<void*>(&Site0), reinterpret_cast<void*>(&Site1),
    reinterpret_cast<void*>(&Site2), reinterpret_cast<void*>(&Site3),
    reinterpret_cast<void*>(&Site4), reinterpret_cast<void*>(&Site5),
    reinterpret_cast<void*>(&Site6), reinterpret_cast<void*>(&Site7),
    reinterpret_cast<void*>(&Site8), reinterpret_cast<void*>(&Site9)
}};
inline const std::array<void**, 10> originals = {{
    &original0, &original1, &original2, &original3, &original4,
    &original5, &original6, &original7, &original8, &original9
}};
}
