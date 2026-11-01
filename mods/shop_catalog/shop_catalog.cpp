#include <windows.h>
#include <string>

#include "sdgo/sdgo_api.h"
#include "catalog_runtime.h"

namespace {
const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
void Log(bool error, const std::wstring& text)
{
    g_api->Log(g_mod, error ? SDGO_LOG_ERROR : SDGO_LOG_INFO, "%ls", text.c_str());
}
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION || !api->host_base || !api->ModDirectory || !api->Log)
        return 0;
    const auto* base = static_cast<const uint8_t*>(api->host_base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    // Data integration needs no detour. Limit the startup entry point to the
    // verified WorldServer build, including safely renamed independent copies.
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.TimeDateStamp != 0x548A86E3 || nt->OptionalHeader.SizeOfImage != 0x0010C000)
        return 0;
    wchar_t file[1024] = {};
    const DWORD length = GetModuleFileNameW(nullptr, file, 1024);
    if (!length || length >= 1024) return 0;
    std::wstring exe = file;
    const auto slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return 0;
    std::wstring hostIni = exe.substr(0, exe.find_last_of(L'.')) + L".ini";
    char configured[1024] = {};
    if (api->SettingString && api->SettingString(mod, "HostIni", "", configured, 1024) && configured[0]) {
        // HostIni is an optional UTF-8 relative filename. Absolute credentials
        // paths are unnecessary for distributing this mod.
        wchar_t relative[1024] = {};
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, configured, -1, relative, 1024)) return 0;
        const std::wstring value = relative;
        if (value.find(L':') != std::wstring::npos || value.find(L"..") != std::wstring::npos || value[0] == L'\\' || value[0] == L'/')
            return 0;
        hostIni = exe.substr(0, slash + 1) + value;
    }
    g_api = api; g_mod = mod;
    const std::wstring directory = api->ModDirectory(mod);
    return sdgo::shop_catalog::Apply(hostIni, directory + L"\\catalog.ini", directory, false, Log) ? 1 : 0;
}
