#include <windows.h>
#include <cstdio>
#include <string>

#include "catalog_runtime.h"

namespace {
void Log(bool error, const std::wstring& text)
{
    char utf8[4096] = {};
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, utf8, 4096, nullptr, nullptr);
    fprintf(error ? stderr : stdout, "%s\n", utf8);
}
}
int wmain(int argc, wchar_t** argv)
{
    std::wstring host, catalog, output;
    bool disable = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--disable") disable = true;
        else if (arg == L"--host-ini" && i + 1 < argc) host = argv[++i];
        else if (arg == L"--catalog" && i + 1 < argc) catalog = argv[++i];
        else if (arg == L"--output" && i + 1 < argc) output = argv[++i];
        else { fprintf(stderr, "Invalid arguments\n"); return 2; }
    }
    if (host.empty() || catalog.empty() || output.empty()) {
        fprintf(stderr, "Usage: shop_catalog_apply --host-ini WorldServer.ini --catalog catalog.ini --output <mod directory> [--disable]\n");
        return 2;
    }
    if (GetFileAttributesW(output.c_str()) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "Output directory must already exist\n"); return 2;
    }
    return sdgo::shop_catalog::Apply(host, catalog, output, disable, Log) ? 0 : 1;
}
