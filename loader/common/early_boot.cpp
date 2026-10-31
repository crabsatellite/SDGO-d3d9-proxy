#include "early_boot.h"

#include "sdgo/sdgo_api.h"

namespace
{
    const char* g_loaderName = "loader";
    INIT_ONCE g_bootOnce = INIT_ONCE_STATIC_INIT;

    using GetSystemTimeAsFileTimeFn = void(WINAPI*)(LPFILETIME);
    GetSystemTimeAsFileTimeFn* g_earlySlot = nullptr;
    GetSystemTimeAsFileTimeFn g_earlyOriginal = nullptr;

    BOOL CALLBACK BootCallback(PINIT_ONCE, PVOID, PVOID*)
    {
        wchar_t path[MAX_PATH * 2] = {};
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        if (length == 0 || length >= MAX_PATH * 2)
            return TRUE;
        wchar_t* slash = wcsrchr(path, L'\\');
        if (!slash)
            return TRUE;
        slash[1] = L'\0';
        if (wcslen(path) + 16 >= MAX_PATH * 2)
            return TRUE;
        wcscat_s(path, L"sdgo\\core.dll");

        HMODULE core = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!core) {
            EarlyBootTrace(L"sdgo\\core.dll not loaded; running without mods");
            return TRUE;
        }
        auto init = reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core, "SdgoCoreInit"));
        if (!init) {
            EarlyBootTrace(L"core.dll has no SdgoCoreInit export");
            return TRUE;
        }
        SdgoBoot boot = {};
        boot.size = sizeof(boot);
        boot.loader = g_loaderName;
        boot.host_module = GetModuleHandleW(nullptr);
        init(&boot);
        return TRUE;
    }

    bool PatchSlot(GetSystemTimeAsFileTimeFn* slot, GetSystemTimeAsFileTimeFn value)
    {
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
            return false;
        InterlockedExchangePointer(reinterpret_cast<PVOID*>(slot), reinterpret_cast<PVOID>(value));
        VirtualProtect(slot, sizeof(*slot), old, &old);
        return true;
    }

    void WINAPI EarlyGetSystemTimeAsFileTime(LPFILETIME time)
    {
        PatchSlot(g_earlySlot, g_earlyOriginal);
        g_earlyOriginal(time);
        EarlyBoot();
    }

    // Finds kernel32!GetSystemTimeAsFileTime in the executable's import table.
    GetSystemTimeAsFileTimeFn* FindEarlySlot(HMODULE exe)
    {
        auto base = reinterpret_cast<BYTE*>(exe);
        auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return nullptr;
        auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return nullptr;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress)
            return nullptr;

        FARPROC resolved = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetSystemTimeAsFileTime");
        for (auto desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); desc->Name; ++desc) {
            if (lstrcmpiA(reinterpret_cast<char*>(base + desc->Name), "kernel32.dll") != 0)
                continue;
            auto iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);
            auto names = desc->OriginalFirstThunk
                ? reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk)
                : nullptr;
            for (size_t i = 0; iat[i].u1.Function; ++i) {
                bool match = false;
                if (names && !(names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
                    auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
                    match = lstrcmpA(reinterpret_cast<char*>(byName->Name), "GetSystemTimeAsFileTime") == 0;
                } else {
                    match = resolved && iat[i].u1.Function == reinterpret_cast<ULONG_PTR>(resolved);
                }
                if (match)
                    return reinterpret_cast<GetSystemTimeAsFileTimeFn*>(&iat[i].u1.Function);
            }
        }
        return nullptr;
    }
}

HMODULE LoadForwardTarget(const wchar_t* chainKey, const wchar_t* systemName)
{
    wchar_t exeDir[MAX_PATH * 2] = {};
    DWORD length = GetModuleFileNameW(nullptr, exeDir, MAX_PATH * 2);
    wchar_t* slash = (length > 0 && length < MAX_PATH * 2) ? wcsrchr(exeDir, L'\\') : nullptr;
    if (slash) {
        slash[1] = L'\0';
        wchar_t ini[MAX_PATH * 2] = {};
        wchar_t chain[MAX_PATH] = {};
        swprintf_s(ini, L"%ssdgo\\sdgo.ini", exeDir);
        GetPrivateProfileStringW(L"loader", chainKey, L"", chain, MAX_PATH, ini);
        if (chain[0]) {
            wchar_t path[MAX_PATH * 3] = {};
            bool absolute = chain[0] == L'\\' || chain[1] == L':';
            swprintf_s(path, L"%s%s", absolute ? L"" : exeDir, chain);
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(&LoadForwardTarget), &self);
            HMODULE target = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (target && target != self)
                return target;
            if (target)
                FreeLibrary(target);
            EarlyBootTrace(L"[loader] chain DLL not usable; forwarding to the system copy");
        }
    }
    wchar_t system[MAX_PATH] = {};
    UINT systemLength = GetSystemDirectoryW(system, MAX_PATH);
    if (systemLength == 0 || systemLength + wcslen(systemName) + 2 >= MAX_PATH)
        return nullptr;
    wcscat_s(system, L"\\");
    wcscat_s(system, systemName);
    return LoadLibraryW(system);
}

void EarlyBootTrace(const wchar_t* message)
{
    OutputDebugStringW(L"[sdgo loader] ");
    OutputDebugStringW(message);
    OutputDebugStringW(L"\n");
}

void EarlyBoot()
{
    InitOnceExecuteOnce(&g_bootOnce, BootCallback, nullptr, nullptr);
}

void EarlyBootInstall(const char* loaderName)
{
    g_loaderName = loaderName;
    GetSystemTimeAsFileTimeFn* slot = FindEarlySlot(GetModuleHandleW(nullptr));
    if (!slot) {
        EarlyBootTrace(L"early boot import not found; core starts at the first forwarded call");
        return;
    }
    g_earlySlot = slot;
    g_earlyOriginal = *slot;
    if (!PatchSlot(slot, &EarlyGetSystemTimeAsFileTime)) {
        g_earlySlot = nullptr;
        EarlyBootTrace(L"early boot import not writable; core starts at the first forwarded call");
    }
}
