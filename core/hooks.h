#pragma once

#include <windows.h>

#include <string>

namespace sdgo::hooks
{
    bool Initialize(std::string& error);

    // Every hook belongs to exactly one owner (a mod id). A site can only be hooked once.
    bool HookCode(const std::string& owner, void* target, void* detour, void** original, std::string& error);
    bool HookSlot(const std::string& owner, void** slot, void* expected, void* detour, void** original, std::string& error);
    bool HookImport(const std::string& owner, HMODULE module, const char* dll, const char* function,
                    void* detour, void** original, std::string& error);

    // Removes every hook of the owner, newest first. Used when a mod fails to load.
    void Rollback(const std::string& owner);
}
