// ime_fix - Chinese/Korean IME commit fix for this GOnline.exe build.
//
// One call site reads GCS_RESULTSTR from ImmGetCompositionStringW and treats
// the returned BYTE count as a CHARACTER count, so the terminator it writes
// lands past the committed text (or past the buffer). Only for that call site
// (recognised by its return address) the hook returns the character count and
// terminates the string itself. Every other caller sees the normal Win32 contract.

#include <windows.h>
#include <imm.h>
#include <intrin.h>

#include <cstdint>

#include "sdgo/sdgo_api.h"

#pragma intrinsic(_ReturnAddress)

namespace
{
    using ImmGetCompositionStringWFn = LONG(WINAPI*)(HIMC, DWORD, LPVOID, DWORD);

    // ime.result_call starts at the "push 0x208" before the call; the call returns 0x17 bytes later.
    constexpr uintptr_t kReturnOffset = 0x17;

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    ImmGetCompositionStringWFn g_original = nullptr;
    uintptr_t g_buggyReturn = 0;
    bool g_trace = false;

    LONG WINAPI FixedImmGetCompositionStringW(HIMC context, DWORD index, LPVOID buffer, DWORD bufferBytes)
    {
        const LONG result = g_original(context, index, buffer, bufferBytes);
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_buggyReturn || index != GCS_RESULTSTR)
            return result;

        if (result <= 0) {
            if (buffer && bufferBytes >= sizeof(wchar_t))
                *static_cast<wchar_t*>(buffer) = L'\0';
            if (g_trace)
                g_api->Log(g_mod, SDGO_LOG_INFO, "GCS_RESULTSTR error %ld normalized to 0", result);
            return 0;
        }
        if (!buffer || bufferBytes < sizeof(wchar_t))
            return 0;

        DWORD bytes = static_cast<DWORD>(result);
        if (bytes > bufferBytes - sizeof(wchar_t))
            bytes = bufferBytes - sizeof(wchar_t);
        bytes &= ~static_cast<DWORD>(1);
        static_cast<wchar_t*>(buffer)[bytes / sizeof(wchar_t)] = L'\0';

        const LONG characters = static_cast<LONG>(bytes / sizeof(wchar_t));
        if (g_trace)
            g_api->Log(g_mod, SDGO_LOG_INFO, "GCS_RESULTSTR %ld bytes -> %ld chars (hkl %p)", result, characters,
                       GetKeyboardLayout(0));
        return characters;
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    g_trace = api->SettingInt(mod, "Trace", 0) != 0;

    auto site = reinterpret_cast<uintptr_t>(api->Symbol(mod, "ime.result_call"));
    if (!site)
        return 0;
    g_buggyReturn = site + kReturnOffset;

    return api->HookImport(mod, "imm32.dll", "ImmGetCompositionStringW",
                           reinterpret_cast<void*>(&FixedImmGetCompositionStringW),
                           reinterpret_cast<void**>(&g_original));
}
