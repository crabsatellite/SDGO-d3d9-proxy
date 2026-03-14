#include "hook.h"
#include <cstring>
#include <vector>

// ============================================================
// IAT Hooking
// ============================================================

// Walk the PE import directory of a module to find and replace an IAT entry.
bool Hook::HookIAT(HMODULE targetModule, const char* importModule,
                   const char* funcName, void* detour, void** pOriginal)
{
    if (!targetModule || !importModule || !funcName || !detour)
        return false;

    auto base = (uint8_t*)targetModule;
    auto dos = (IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;

    auto& importDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (importDir.VirtualAddress == 0)
        return false;

    auto importDesc = (IMAGE_IMPORT_DESCRIPTOR*)(base + importDir.VirtualAddress);

    for (; importDesc->Name != 0; importDesc++) {
        const char* modName = (const char*)(base + importDesc->Name);
        if (_stricmp(modName, importModule) != 0)
            continue;

        // Found the import module. Walk the thunks.
        auto origThunk = (IMAGE_THUNK_DATA*)(base + importDesc->OriginalFirstThunk);
        auto iatThunk  = (IMAGE_THUNK_DATA*)(base + importDesc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; origThunk++, iatThunk++) {
            // Skip ordinal imports
            if (origThunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;

            auto importByName = (IMAGE_IMPORT_BY_NAME*)(base + origThunk->u1.AddressOfData);
            if (strcmp((const char*)importByName->Name, funcName) != 0)
                continue;

            // Found the function. Replace the IAT entry.
            if (pOriginal)
                *pOriginal = (void*)iatThunk->u1.Function;

            DWORD oldProtect;
            if (!VirtualProtect(&iatThunk->u1.Function, sizeof(void*),
                                PAGE_READWRITE, &oldProtect))
                return false;

            iatThunk->u1.Function = (DWORD_PTR)detour;
            VirtualProtect(&iatThunk->u1.Function, sizeof(void*),
                           oldProtect, &oldProtect);
            return true;
        }
    }
    return false;
}

bool Hook::UnhookIAT(HMODULE targetModule, const char* importModule,
                     const char* funcName, void* original)
{
    void* dummy = nullptr;
    // Re-hook with the original to restore it
    return HookIAT(targetModule, importModule, funcName, original, &dummy);
}

// ============================================================
// Inline Hooking
// ============================================================

struct InlineHookEntry {
    void* target;          // original function address
    void* trampoline;      // allocated trampoline
    uint8_t savedBytes[32]; // original bytes
    int stolenBytes;        // number of stolen bytes
};

static std::vector<InlineHookEntry> s_inlineHooks;

bool Hook::HookInline(void* target, void* detour, void** pOriginal,
                      int stolenBytes)
{
    if (!target || !detour || !pOriginal || stolenBytes < 5 || stolenBytes > 30)
        return false;

    // Allocate trampoline: stolen bytes + JMP back (5 bytes)
    int trampolineSize = stolenBytes + 5;
    void* trampoline = VirtualAlloc(nullptr, trampolineSize,
                                    MEM_COMMIT | MEM_RESERVE,
                                    PAGE_EXECUTE_READWRITE);
    if (!trampoline)
        return false;

    // Save original bytes
    InlineHookEntry entry = {};
    entry.target = target;
    entry.trampoline = trampoline;
    entry.stolenBytes = stolenBytes;
    memcpy(entry.savedBytes, target, stolenBytes);

    // Build trampoline: copy stolen bytes, then JMP to target + stolenBytes
    uint8_t* tp = (uint8_t*)trampoline;
    memcpy(tp, target, stolenBytes);

    // If stolen bytes contain a relative call/jump, we need to fix up the offset.
    // For now, handle the common case of E8 (CALL rel32) and E9 (JMP rel32).
    for (int i = 0; i < stolenBytes; ) {
        uint8_t opcode = ((uint8_t*)target)[i];
        if ((opcode == 0xE8 || opcode == 0xE9) && i + 5 <= stolenBytes) {
            // Fix relative offset: original target address + offset + 5
            int32_t origOffset = *(int32_t*)((uint8_t*)target + i + 1);
            void* absTarget = (uint8_t*)target + i + 5 + origOffset;
            int32_t newOffset = (int32_t)((uint8_t*)absTarget - (tp + i + 5));
            *(int32_t*)(tp + i + 1) = newOffset;
            i += 5;
        } else {
            i++; // Simple byte copy (already done above)
        }
    }

    // JMP back to original function after stolen bytes
    tp[stolenBytes] = 0xE9;  // JMP rel32
    int32_t jmpBack = (int32_t)((uint8_t*)target + stolenBytes - (tp + stolenBytes + 5));
    *(int32_t*)(tp + stolenBytes + 1) = jmpBack;

    // Patch the target function: overwrite with JMP to detour
    DWORD oldProtect;
    if (!VirtualProtect(target, stolenBytes, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }

    uint8_t* t = (uint8_t*)target;
    t[0] = 0xE9;  // JMP rel32
    int32_t jmpDetour = (int32_t)((uint8_t*)detour - (t + 5));
    *(int32_t*)(t + 1) = jmpDetour;

    // NOP remaining stolen bytes
    for (int i = 5; i < stolenBytes; i++)
        t[i] = 0x90;  // NOP

    VirtualProtect(target, stolenBytes, oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, stolenBytes);

    *pOriginal = trampoline;
    s_inlineHooks.push_back(entry);
    return true;
}

bool Hook::UnhookInline(void** pOriginal)
{
    if (!pOriginal || !*pOriginal)
        return false;

    for (auto it = s_inlineHooks.begin(); it != s_inlineHooks.end(); ++it) {
        if (it->trampoline == *pOriginal) {
            // Restore original bytes
            DWORD oldProtect;
            if (VirtualProtect(it->target, it->stolenBytes,
                               PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(it->target, it->savedBytes, it->stolenBytes);
                VirtualProtect(it->target, it->stolenBytes,
                               oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(),
                                     it->target, it->stolenBytes);
            }

            VirtualFree(it->trampoline, 0, MEM_RELEASE);
            s_inlineHooks.erase(it);
            *pOriginal = nullptr;
            return true;
        }
    }
    return false;
}

void Hook::Cleanup()
{
    for (auto& entry : s_inlineHooks) {
        DWORD oldProtect;
        if (VirtualProtect(entry.target, entry.stolenBytes,
                           PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(entry.target, entry.savedBytes, entry.stolenBytes);
            VirtualProtect(entry.target, entry.stolenBytes,
                           oldProtect, &oldProtect);
        }
        VirtualFree(entry.trampoline, 0, MEM_RELEASE);
    }
    s_inlineHooks.clear();
}
