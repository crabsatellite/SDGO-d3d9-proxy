#include <windows.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"

namespace {
std::wstring g_directory;
std::string g_ini;
unsigned g_messages = 0;
void __cdecl Log(SdgoMod*, int level, const char*, ...)
{
    if (level == SDGO_LOG_INFO) ++g_messages;
}
const wchar_t* Directory(SdgoMod*) { return g_directory.c_str(); }
uint32_t Setting(SdgoMod*, const char*, const char*, char* out, uint32_t capacity)
{
    if (g_ini.size() + 1 > capacity) return 0;
    memcpy(out, g_ini.c_str(), g_ini.size() + 1);
    return static_cast<uint32_t>(g_ini.size());
}
}
int wmain(int argc, wchar_t** argv)
{
    if (argc != 4) return 2;
    wchar_t executable[1024] = {};
    GetModuleFileNameW(nullptr, executable, 1024);
    std::wstring directory = executable;
    directory.resize(directory.find_last_of(L"\\/") + 1);
    g_ini = "shop-catalog-test-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + ".ini";
    const std::wstring copy = directory + std::wstring(g_ini.begin(), g_ini.end());
    if (!CopyFileW(argv[2], copy.c_str(), TRUE)) return 3;
    g_directory = argv[3];
    HMODULE module = LoadLibraryW(argv[1]);
    auto entry = module ? reinterpret_cast<SdgoModLoadFn>(GetProcAddress(module, "SdgoModLoad")) : nullptr;
    std::array<uint8_t, 1024> header = {};
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(header.data());
    dos->e_magic = IMAGE_DOS_SIGNATURE; dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(header.data() + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE; nt->FileHeader.TimeDateStamp = 0x548A86E3;
    nt->OptionalHeader.SizeOfImage = 0x0010C000;
    SdgoApi api = {}; api.size = sizeof(api); api.version = 1;
    api.host_base = header.data(); api.ModDirectory = Directory; api.SettingString = Setting; api.Log = Log;
    const bool applied = entry && entry(&api, nullptr) == 1 && g_messages > 0;
    nt->FileHeader.TimeDateStamp = 0;
    const bool rejected = entry && entry(&api, nullptr) == 0;
    // Only the fresh credential copy created by this test executable is removed.
    DeleteFileW(copy.c_str());
    printf("SHOP CATALOG ACTUAL DLL %s (matching World ABI fixture, real isolated ODBC; unknown host refused)\n", applied && rejected ? "PASS" : "FAIL");
    return applied && rejected ? 0 : 1;
}
