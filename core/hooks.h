#pragma once

#include <windows.h>

#include <string>

namespace sdgo::hooks
{
    bool Initialize(std::string& error);

    // Every hook belongs to exactly one owner (a mod id). A site can only be hooked once.
    // A deferrable hook made while a batch is open is switched on by EndBatch; any other hook
    // at once.
    bool HookCode(const std::string& owner, void* target, void* detour, void** original, std::string& error,
                  bool deferrable = false);
    bool HookSlot(const std::string& owner, void** slot, void* expected, void* detour, void** original, std::string& error);
    bool HookImport(const std::string& owner, HMODULE module, const char* dll, const char* function,
                    void* detour, void** original, std::string& error);

    // Removes every hook of the owner, newest first. Used when a mod fails to load.
    void Rollback(const std::string& owner);

    // While a batch is open (the mods' load), HookCode queues the deferrable code hooks it
    // creates; EndBatch switches them all on with one thread freeze instead of one per hook
    // (each freeze takes a snapshot of every thread in the system, tens of milliseconds).
    void BeginBatch();
    bool EndBatch(size_t* enabled, std::string& error);
}
