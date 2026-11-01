#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "capsule_runtime.h"

namespace
{
    using NewFn = void*(__cdecl*)(size_t);
    using WfopenFn = void*(__cdecl*)(const wchar_t*, const wchar_t*);
    using CtorFn = void*(__thiscall*)(void*, void*, capsule_catalog::NativeFileInfo&, uint32_t, bool);
    using SizeFn = uint32_t(__thiscall*)(void*);
    using ReadFn = uint32_t(__thiscall*)(void*, void*, uint32_t, uint32_t);
    using CloseFn = int(__cdecl*)(void*);

    bool ReadWhole(const wchar_t* path, std::vector<uint8_t>& bytes)
    {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        LARGE_INTEGER size = {};
        bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 && size.QuadPart <= 16 * 1024 * 1024;
        if (ok) {
            bytes.resize(static_cast<size_t>(size.QuadPart));
            DWORD read = 0;
            ok = bytes.empty() || (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                                   read == bytes.size());
        }
        CloseHandle(file);
        return ok;
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 5) {
        std::puts("usage: capsule_client_native_stream_test <zoa_core.dll> <bundle> <native-path> <expected-file>");
        return 2;
    }
    capsule_catalog::Bundle bundle;
    std::string error;
    if (!capsule_catalog::LoadBundle(argv[2], bundle, error)) {
        std::printf("FAIL bundle: %s\n", error.c_str());
        return 3;
    }
    const auto* entry = bundle.Find(argv[3]);
    if (!entry) {
        std::puts("FAIL entry not found");
        return 4;
    }
    HMODULE module = LoadLibraryW(argv[1]);
    if (!module) {
        std::printf("FAIL LoadLibrary %lu\n", static_cast<unsigned long>(GetLastError()));
        return 5;
    }
    auto base = reinterpret_cast<uintptr_t>(module);
    auto allocate = reinterpret_cast<NewFn>(base + 0x1a3d6);
    auto open = *reinterpret_cast<WfopenFn*>(base + 0x1c4dc);
    auto ctor = reinterpret_cast<CtorFn>(GetProcAddress(
        module, "??0CZoaFile@@QAE@PAU_iobuf@@AAU_zoa_pack_file_info@@K_N@Z"));
    auto size = reinterpret_cast<SizeFn>(GetProcAddress(module, "?GetFileSize@CZoaFile@@QAEKXZ"));
    auto read = reinterpret_cast<ReadFn>(GetProcAddress(module, "?Fread@CZoaFile@@QAEIPAXII@Z"));
    auto close = reinterpret_cast<CloseFn>(GetProcAddress(module, "?zoa_fclose@@YAHPAVCZoaFile@@@Z"));
    if (!allocate || !open || !ctor || !size || !read || !close) {
        std::puts("FAIL native functions");
        FreeLibrary(module);
        return 6;
    }
    void* nativeFile = open(bundle.zpkPath.c_str(), L"rb");
    void* storage = allocate(0x78);
    if (!nativeFile || !storage) {
        std::puts("FAIL native open/allocation");
        FreeLibrary(module);
        return 7;
    }
    void* stream = ctor(storage, nativeFile,
                        const_cast<capsule_catalog::NativeFileInfo&>(entry->info), bundle.chunkSize, false);
    if (!stream) {
        std::puts("FAIL constructor");
        FreeLibrary(module);
        return 8;
    }
    std::vector<uint8_t> actual(size(stream));
    uint32_t got = actual.empty() ? 0 : read(stream, actual.data(), 1, static_cast<uint32_t>(actual.size()));
    close(stream);
    std::vector<uint8_t> expected;
    if (!ReadWhole(argv[4], expected)) {
        std::puts("FAIL expected file");
        FreeLibrary(module);
        return 9;
    }
    FreeLibrary(module);
    if (got != actual.size() || actual != expected) {
        std::printf("FAIL readback got=%u actual=%u expected=%u\n", got,
                    static_cast<unsigned>(actual.size()), static_cast<unsigned>(expected.size()));
        return 10;
    }
    std::printf("OK native CZoaFile readback %u bytes\n", static_cast<unsigned>(actual.size()));
    return 0;
}
