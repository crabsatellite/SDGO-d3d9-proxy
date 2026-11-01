#include "early_boot.h"
#include "launcher_handshake.h"

#include "sdgo/sdgo_api.h"

namespace
{
    const char* g_loaderName = "loader";
    INIT_ONCE g_bootOnce = INIT_ONCE_STATIC_INIT;
    sdgo::loader::LauncherHandshake g_launcherHandshake;

    using GetSystemTimeAsFileTimeFn = void(WINAPI*)(LPFILETIME);
    GetSystemTimeAsFileTimeFn* g_earlySlot = nullptr;
    GetSystemTimeAsFileTimeFn g_earlyOriginal = nullptr;
    using OpenEventFn = HANDLE(WINAPI*)(DWORD, BOOL, LPCWSTR);
    OpenEventFn* g_launcherSlot = nullptr;
    OpenEventFn g_launcherOriginal = nullptr;
    void** FindImportSlot(HMODULE exe, const char* importName);
    HANDLE WINAPI OpenLauncherEvent(DWORD access, BOOL inherit, LPCWSTR name);
    template<typename Fn> bool PatchSlot(Fn* slot, Fn value);

    BOOL CALLBACK BootCallback(PINIT_ONCE, PVOID, PVOID*)
    {
        auto host = reinterpret_cast<const BYTE*>(GetModuleHandleW(nullptr));
        auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(host);
        if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
            auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(host + dos->e_lfanew);
            if (nt->Signature == IMAGE_NT_SIGNATURE &&
                g_launcherHandshake.Acquire(nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage)) {
                g_launcherSlot = reinterpret_cast<OpenEventFn*>(FindImportSlot(GetModuleHandleW(nullptr), "OpenEventW"));
                if (g_launcherSlot) {
                    g_launcherOriginal = *g_launcherSlot;
                    PatchSlot(g_launcherSlot, &OpenLauncherEvent);
                }
            }
        }
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

    template<typename Fn> bool PatchSlot(Fn* slot, Fn value)
    {
        DWORD old = 0;
        if (!VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &old))
            return false;
        InterlockedExchangePointer(reinterpret_cast<PVOID*>(slot), reinterpret_cast<PVOID>(value));
        VirtualProtect(slot, sizeof(*slot), old, &old);
        return true;
    }

    HANDLE WINAPI OpenLauncherEvent(DWORD access, BOOL inherit, LPCWSTR name)
    {
        PatchSlot(g_launcherSlot, g_launcherOriginal);
        HANDLE event = g_launcherOriginal(access, inherit, name);
        DWORD error = GetLastError();
        // Native WinMain owns its own handle now. Retaining the object after
        // that point could give the next launch a stale signalled event.
        if (event) g_launcherHandshake.Release();
        SetLastError(error);
        return event;
    }

    void WINAPI EarlyGetSystemTimeAsFileTime(LPFILETIME time)
    {
        PatchSlot(g_earlySlot, g_earlyOriginal);
        g_earlyOriginal(time);
        EarlyBoot();
    }

    // Finds one named kernel32 import in the executable's import table.
    void** FindImportSlot(HMODULE exe, const char* importName)
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

        FARPROC resolved = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), importName);
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
                    match = lstrcmpA(reinterpret_cast<char*>(byName->Name), importName) == 0;
                } else {
                    match = resolved && iat[i].u1.Function == reinterpret_cast<ULONG_PTR>(resolved);
                }
                if (match)
                    return reinterpret_cast<void**>(&iat[i].u1.Function);
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
    auto slot = reinterpret_cast<GetSystemTimeAsFileTimeFn*>(FindImportSlot(GetModuleHandleW(nullptr), "GetSystemTimeAsFileTime"));
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
