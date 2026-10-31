#include "hooks.h"

#include <MinHook.h>

#include <vector>

namespace
{
    enum class Kind { Code, Slot };

    struct Record
    {
        std::string owner;
        Kind kind;
        void* site;     // code address or pointer slot
        void* original; // trampoline (code) or previous pointer (slot)
    };

    std::vector<Record> g_records;
    CRITICAL_SECTION g_lock;
    bool g_ready = false;
    bool g_batch = false;
    size_t g_queued = 0;

    struct Guard
    {
        Guard() { EnterCriticalSection(&g_lock); }
        ~Guard() { LeaveCriticalSection(&g_lock); }
    };

    const Record* FindSite(void* site)
    {
        for (const Record& record : g_records) {
            if (record.site == site)
                return &record;
        }
        return nullptr;
    }

    bool WritePointer(void** slot, void* value)
    {
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
            return false;
        InterlockedExchangePointer(slot, value);
        VirtualProtect(slot, sizeof(void*), old, &old);
        FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
        return true;
    }

    bool SiteTaken(void* site, std::string& error)
    {
        if (const Record* existing = FindSite(site)) {
            error = "site already hooked by " + existing->owner;
            return true;
        }
        return false;
    }
}

bool sdgo::hooks::Initialize(std::string& error)
{
    if (g_ready)
        return true;
    InitializeCriticalSection(&g_lock);
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK) {
        error = MH_StatusToString(status);
        return false;
    }
    g_ready = true;
    return true;
}

bool sdgo::hooks::HookCode(const std::string& owner, void* target, void* detour, void** original, std::string& error)
{
    if (!g_ready || !target || !detour || !original) {
        error = "invalid arguments";
        return false;
    }
    Guard guard;
    if (SiteTaken(target, error))
        return false;
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status == MH_OK)
        status = g_batch ? MH_QueueEnableHook(target) : MH_EnableHook(target);
    if (status != MH_OK) {
        MH_RemoveHook(target);
        error = MH_StatusToString(status);
        return false;
    }
    if (g_batch)
        ++g_queued;
    g_records.push_back({owner, Kind::Code, target, *original});
    return true;
}

void sdgo::hooks::BeginBatch()
{
    if (!g_ready)
        return;
    Guard guard;
    g_batch = true;
    g_queued = 0;
}

bool sdgo::hooks::EndBatch(size_t* enabled, std::string& error)
{
    if (!g_ready)
        return true;
    Guard guard;
    g_batch = false;
    if (enabled)
        *enabled = g_queued;
    MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) {
        error = MH_StatusToString(status);
        return false;
    }
    return true;
}

bool sdgo::hooks::HookSlot(const std::string& owner, void** slot, void* expected, void* detour, void** original,
                           std::string& error)
{
    if (!g_ready || !slot || !detour || !original) {
        error = "invalid arguments";
        return false;
    }
    Guard guard;
    if (SiteTaken(slot, error))
        return false;
    if (*slot != expected) {
        error = "slot changed since verification";
        return false;
    }
    *original = *slot;
    if (!WritePointer(slot, detour)) {
        error = "slot not writable";
        return false;
    }
    g_records.push_back({owner, Kind::Slot, slot, *original});
    return true;
}

bool sdgo::hooks::HookImport(const std::string& owner, HMODULE module, const char* dll, const char* function,
                             void* detour, void** original, std::string& error)
{
    auto base = reinterpret_cast<BYTE*>(module);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) {
        error = "module has no imports";
        return false;
    }
    for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
        if (_stricmp(reinterpret_cast<char*>(base + desc->Name), dll) != 0 || !desc->OriginalFirstThunk)
            continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (strcmp(reinterpret_cast<char*>(byName->Name), function) != 0)
                continue;
            auto slot = reinterpret_cast<void**>(&iat[i].u1.Function);
            return HookSlot(owner, slot, *slot, detour, original, error);
        }
    }
    error = std::string("import ") + dll + "!" + function + " not found";
    return false;
}

void sdgo::hooks::Rollback(const std::string& owner)
{
    if (!g_ready)
        return;
    Guard guard;
    for (size_t i = g_records.size(); i-- > 0;) {
        Record& record = g_records[i];
        if (record.owner != owner)
            continue;
        if (record.kind == Kind::Code) {
            MH_DisableHook(record.site);
            MH_RemoveHook(record.site);
            if (g_batch && g_queued > 0)
                --g_queued;
        } else {
            WritePointer(static_cast<void**>(record.site), record.original);
        }
        g_records.erase(g_records.begin() + static_cast<std::ptrdiff_t>(i));
    }
}
