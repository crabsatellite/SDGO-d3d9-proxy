#include "native_title_image.h"

// Executes only the verified room-info constructor and its verified static CRT
// callees inside the private image. The original executable is opened read-only.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

namespace
{
    constexpr std::size_t kPacketSize = 0x13E;
    constexpr std::size_t kGuardSize = 32;
    constexpr std::uint8_t kFill = 0xA5;
    int g_failures = 0;

    void Check(bool condition, const char* message)
    {
        std::printf("%s %s\n", condition ? "ok  " : "FAIL", message);
        if (!condition) ++g_failures;
    }

    template <typename T>
    void Store(std::uint8_t* destination, T value)
    {
        std::memcpy(destination, &value, sizeof(value));
    }

    void RunCase(const room_title_native::KnownBattleServerImage& image,
                 const char* label, const std::string& text,
                 std::size_t sourceOffset, std::uint8_t suppliedLength)
    {
        alignas(16) std::array<std::uint8_t, 96> source = {};
        source.fill(0xD3);
        Check(sourceOffset + text.size() + 1 <= source.size(),
              "test source fits its private buffer");
        if (sourceOffset + text.size() + 1 > source.size()) return;
        std::memcpy(source.data() + sourceOffset, text.data(), text.size());
        source[sourceOffset + text.size()] = 0;
        const auto sourceBefore = source;

        alignas(16)
            std::array<std::uint8_t, kGuardSize + kPacketSize + kGuardSize> storage;
        storage.fill(kFill);
        std::uint8_t* packet = storage.data() + kGuardSize;

        constexpr std::uint32_t userId = 0x10293847;
        constexpr std::uint32_t roomId = 0xA1B2C3D4;
        using Constructor = void*(__thiscall*)(
            void*, std::uint32_t, std::uint32_t, std::uint8_t, const char*);
        Constructor construct =
            image.Function<Constructor>(
                room_title_native::KnownBattleServerImage::kConstructorRva);
        void* returned = construct(packet, userId, roomId, suppliedLength,
                                   reinterpret_cast<const char*>(
                                       source.data() + sourceOffset));

        std::array<std::uint8_t, kPacketSize> expected = {};
        expected.fill(kFill);
        Store<std::uint16_t>(expected.data(), 0x091F);
        Store<std::uint32_t>(expected.data() + 2, userId);
        expected[6] = suppliedLength;
        const std::size_t copied = std::min<std::size_t>(text.size(), 20);
        std::memcpy(expected.data() + 7, text.data(), copied);
        if (copied < 20) {
            std::memset(expected.data() + 7 + copied, 0, 20 - copied);
        }
        Store<std::uint32_t>(expected.data() + 0x1C, roomId);
        std::memset(expected.data() + 0x20, 0, 0x16);
        std::memset(expected.data() + 0x36, 0, 0x108);

        const std::string prefix = std::string(label) + ": ";
        Check(returned == packet, (prefix + "constructor returns packet").c_str());
        Check(std::memcmp(packet, expected.data(), expected.size()) == 0,
              (prefix + "complete packet bytes match").c_str());
        Check(packet[0x1B] == kFill,
              (prefix + "byte after the 20-byte title stays untouched").c_str());
        Check(std::all_of(storage.begin(), storage.begin() + kGuardSize,
                          [](std::uint8_t byte) { return byte == kFill; }) &&
                  std::all_of(storage.end() - kGuardSize, storage.end(),
                              [](std::uint8_t byte) { return byte == kFill; }),
              (prefix + "outer guard bytes stay untouched").c_str());
        Check(source == sourceBefore,
              (prefix + "source bytes stay unchanged").c_str());
    }
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2) {
        std::fputs(
            "usage: room_title_native_test.exe <known BattleServer.exe>\n",
            stderr);
        return 2;
    }

    if (GetModuleHandleW(L"SDPlus.dll") ||
        GetModuleHandleW(L"SDBattlePlus.dll")) {
        std::fputs("FAIL private SDGO DLL already exists in the test process\n",
                   stderr);
        return 2;
    }

    try {
        room_title_native::KnownBattleServerImage image(argv[1]);
        Check(image.file_sha256() ==
                  "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4",
              "supported BattleServer identity verified");
        Check(image.mapped_base() != 0, "private PE image mapped");
        Check(image.relocation_count() != 0 ||
                  image.mapped_base() ==
                      room_title_native::KnownBattleServerImage::kPreferredBase,
              "HIGHLOW relocations applied when required");
        Check(!GetModuleHandleW(L"SDPlus.dll") &&
                  !GetModuleHandleW(L"SDBattlePlus.dll"),
              "private SDGO DLLs were not loaded");

        RunCase(image, "empty", "", 1, 0);
        RunCase(image, "20 bytes", "ABCDEFGHIJKLMNOPQRST", 0, 20);
        RunCase(image, "21 bytes", "ABCDEFGHIJKLMNOPQRSTU", 3, 21);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL native fixture refused execution: %s\n",
                     error.what());
        return 2;
    }

    std::puts(g_failures ? "NATIVE ROOM TITLE CHECKS FAILED"
                         : "ALL NATIVE ROOM TITLE CHECKS PASSED");
    return g_failures ? 1 : 0;
}
