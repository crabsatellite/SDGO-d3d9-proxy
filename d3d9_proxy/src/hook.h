#pragma once
#include <windows.h>
#include <cstdint>

// ============================================================
// Simple x86 function hooking for SDGO mod framework.
//
// Two methods:
//   1. IAT hooking: Replace import table entries. Safe, no code patching.
//   2. Inline hooking: Overwrite function prologue with JMP. Needs care.
//
// IAT hooking is preferred for ZOA engine DLL exports (3715+ functions).
// Inline hooking is for internal GOnline.exe functions.
// ============================================================

namespace Hook
{
    // Hook a function imported by the calling module (or specified module).
    // Scans the IAT of targetModule for imports from importModule::funcName.
    // Returns true on success. *pOriginal receives the original function pointer.
    bool HookIAT(HMODULE targetModule, const char* importModule,
                 const char* funcName, void* detour, void** pOriginal);

    // Unhook an IAT entry by restoring the original pointer.
    bool UnhookIAT(HMODULE targetModule, const char* importModule,
                   const char* funcName, void* original);

    // Inline hook: overwrite the first `stolenBytes` of `target` with a JMP
    // to `detour`. A trampoline is created that executes the stolen bytes
    // then jumps back. *pOriginal receives the trampoline pointer.
    // stolenBytes must be >= 5 and must be instruction-aligned (caller's
    // responsibility — use a disassembler or manual analysis).
    bool HookInline(void* target, void* detour, void** pOriginal,
                    int stolenBytes);

    // Remove an inline hook by restoring the original bytes.
    bool UnhookInline(void** pOriginal);

    // Clean up all inline hook trampolines.
    void Cleanup();
}
