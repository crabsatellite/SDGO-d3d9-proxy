#pragma once

// Narrow manual mapper for one verified BattleServer.exe fixture. It copies PE32
// sections into private memory and applies HIGHLOW relocations only. It never
// resolves imports, invokes the executable entry point, or runs TLS callbacks.
#if !defined(_WIN32) || !defined(_M_IX86)
#error This fixture must be built as a 32-bit Windows executable.
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace room_title_native
{
    namespace detail
    {
        [[noreturn]] inline void Fail(const std::string& message)
        {
            throw std::runtime_error(message);
        }

        inline void Require(bool condition, const std::string& message)
        {
            if (!condition) Fail(message);
        }

        inline std::string Sha256(const std::uint8_t* data, std::size_t size)
        {
            Require(size <= std::numeric_limits<ULONG>::max(), "SHA-256 input is too large");

            BCRYPT_ALG_HANDLE algorithm = nullptr;
            BCRYPT_HASH_HANDLE hash = nullptr;
            std::vector<UCHAR> object;
            std::array<UCHAR, 32> digest = {};

            NTSTATUS status = BCryptOpenAlgorithmProvider(
                &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
            if (!BCRYPT_SUCCESS(status)) Fail("BCryptOpenAlgorithmProvider failed");

            DWORD objectSize = 0;
            DWORD resultSize = 0;
            status = BCryptGetProperty(
                algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                sizeof(objectSize), &resultSize, 0);
            if (!BCRYPT_SUCCESS(status) || resultSize != sizeof(objectSize)) {
                BCryptCloseAlgorithmProvider(algorithm, 0);
                Fail("BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed");
            }
            object.resize(objectSize);

            status = BCryptCreateHash(
                algorithm, &hash, object.data(), static_cast<ULONG>(object.size()),
                nullptr, 0, 0);
            if (!BCRYPT_SUCCESS(status)) {
                BCryptCloseAlgorithmProvider(algorithm, 0);
                Fail("BCryptCreateHash failed");
            }

            status = BCryptHashData(
                hash, const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(data)),
                static_cast<ULONG>(size), 0);
            if (BCRYPT_SUCCESS(status)) {
                status = BCryptFinishHash(
                    hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
            }

            BCryptDestroyHash(hash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            if (!BCRYPT_SUCCESS(status)) Fail("SHA-256 calculation failed");

            std::ostringstream text;
            text << std::hex << std::setfill('0');
            for (UCHAR byte : digest) text << std::setw(2) << static_cast<unsigned>(byte);
            return text.str();
        }

        inline std::vector<std::uint8_t> ReadFile(const std::wstring& path)
        {
            HANDLE file = CreateFileW(
                path.c_str(), GENERIC_READ,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE) {
                Fail("cannot open the BattleServer fixture (Win32 " +
                     std::to_string(GetLastError()) + ")");
            }

            LARGE_INTEGER length = {};
            if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 ||
                static_cast<unsigned long long>(length.QuadPart) >
                    std::numeric_limits<std::size_t>::max()) {
                DWORD error = GetLastError();
                CloseHandle(file);
                Fail("cannot size the BattleServer fixture (Win32 " +
                     std::to_string(error) + ")");
            }

            std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length.QuadPart));
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
                    bytes.size() - offset, std::numeric_limits<DWORD>::max()));
                DWORD read = 0;
                if (!::ReadFile(file, bytes.data() + offset, chunk, &read, nullptr) ||
                    read == 0) {
                    DWORD error = GetLastError();
                    CloseHandle(file);
                    Fail("cannot read the BattleServer fixture (Win32 " +
                         std::to_string(error) + ")");
                }
                offset += read;
            }
            CloseHandle(file);
            return bytes;
        }

        template <typename T>
        inline T ReadStruct(const std::vector<std::uint8_t>& bytes, std::size_t offset)
        {
            Require(offset <= bytes.size() && sizeof(T) <= bytes.size() - offset,
                    "PE structure extends beyond the file");
            T value = {};
            std::memcpy(&value, bytes.data() + offset, sizeof(value));
            return value;
        }
    }

    class KnownBattleServerImage
    {
    public:
        static constexpr std::uint32_t kPreferredBase = 0x00400000;
        static constexpr std::uint32_t kSizeOfImage = 0x0046C000;
        static constexpr std::uint32_t kTimestamp = 0x548A891D;
        static constexpr std::uint32_t kEntryPointRva = 0x00334546;
        static constexpr std::uint32_t kConstructorRva = 0x000F60F0;
        static constexpr std::uint32_t kStrncpyRva = 0x00333210;
        static constexpr std::uint32_t kMemsetRva = 0x00332C00;
        static constexpr std::uint32_t kMemsetSseRva = 0x0033A775;
        static constexpr std::uint32_t kMemsetFlagRva = 0x004394E4;
        static constexpr std::uint32_t kMemsetFlagOperandRva = 0x00332C20;

        explicit KnownBattleServerImage(const std::wstring& path)
            : file_(detail::ReadFile(path))
        {
            VerifyIdentity();
            ParseHeaders();
            MapSections();
            VerifyNativeCode();
            ApplyRelocations();
            VerifyRelocatedPreconditions();
            SealImage();
        }

        ~KnownBattleServerImage()
        {
            if (image_) VirtualFree(image_, 0, MEM_RELEASE);
        }

        KnownBattleServerImage(const KnownBattleServerImage&) = delete;
        KnownBattleServerImage& operator=(const KnownBattleServerImage&) = delete;

        template <typename T>
        T Function(std::uint32_t rva) const
        {
            RequireImageRange(rva, 1);
            return reinterpret_cast<T>(image_ + rva);
        }

        std::uintptr_t mapped_base() const
        {
            return reinterpret_cast<std::uintptr_t>(image_);
        }

        std::size_t relocation_count() const
        {
            return relocationCount_;
        }

        const std::string& file_sha256() const
        {
            return fileSha256_;
        }

    private:
        static constexpr std::size_t kExpectedFileSize = 4558848;
        static constexpr char kExpectedFileSha256[] =
            "f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4";
        static constexpr char kConstructorSha256[] =
            "50537f02d3ee3424e69291cc2531acbe66d6e9777caa528865cf79ba6e432abe";
        static constexpr char kStrncpySha256[] =
            "22006e25651e484a1c6be63bde03c539bb04291083659ef03adadd480f32d18e";
        static constexpr char kMemsetSha256[] =
            "cd83767af97c39f15bbaebaef47c42ceb74942f3635c33fc5275ea5f0f61865e";
        static constexpr char kMemsetSseSha256[] =
            "cadbee47357a0cc79ef660415bd83d57b210cfd3f93c2aa9c09cfddd1a641745";

        std::vector<std::uint8_t> file_;
        std::string fileSha256_;
        IMAGE_DOS_HEADER dos_ = {};
        IMAGE_NT_HEADERS32 nt_ = {};
        std::vector<IMAGE_SECTION_HEADER> sections_;
        std::uint8_t* image_ = nullptr;
        std::size_t relocationCount_ = 0;
        bool sawMemsetFlagRelocation_ = false;

        static bool RangeFits(std::size_t offset, std::size_t length, std::size_t size)
        {
            return offset <= size && length <= size - offset;
        }

        void VerifyIdentity()
        {
            detail::Require(file_.size() == kExpectedFileSize,
                            "unsupported BattleServer.exe file size");
            fileSha256_ = detail::Sha256(file_.data(), file_.size());
            detail::Require(fileSha256_ == kExpectedFileSha256,
                            "unsupported BattleServer.exe SHA-256");
        }

        void ParseHeaders()
        {
            dos_ = detail::ReadStruct<IMAGE_DOS_HEADER>(file_, 0);
            detail::Require(dos_.e_magic == IMAGE_DOS_SIGNATURE,
                            "BattleServer fixture has no DOS header");
            detail::Require(dos_.e_lfanew > 0, "invalid PE header offset");

            const std::size_t ntOffset = static_cast<std::size_t>(dos_.e_lfanew);
            nt_ = detail::ReadStruct<IMAGE_NT_HEADERS32>(file_, ntOffset);
            detail::Require(nt_.Signature == IMAGE_NT_SIGNATURE, "invalid PE signature");
            detail::Require(nt_.FileHeader.Machine == IMAGE_FILE_MACHINE_I386,
                            "fixture is not PE32 x86");
            detail::Require(nt_.FileHeader.TimeDateStamp == kTimestamp,
                            "unsupported BattleServer PE timestamp");
            detail::Require(nt_.FileHeader.NumberOfSections == 7,
                            "unsupported BattleServer section count");
            detail::Require(
                nt_.FileHeader.SizeOfOptionalHeader == sizeof(IMAGE_OPTIONAL_HEADER32),
                "unsupported PE optional-header size");
            detail::Require(nt_.OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR32_MAGIC,
                            "fixture is not PE32");
            detail::Require(nt_.OptionalHeader.ImageBase == kPreferredBase,
                            "unsupported BattleServer preferred base");
            detail::Require(nt_.OptionalHeader.SizeOfImage == kSizeOfImage,
                            "unsupported BattleServer image size");
            detail::Require(nt_.OptionalHeader.AddressOfEntryPoint == kEntryPointRva,
                            "unsupported BattleServer entry point");

            const IMAGE_DATA_DIRECTORY tls =
                nt_.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
            detail::Require(tls.VirtualAddress == 0 && tls.Size == 0,
                            "fixture unexpectedly contains a TLS directory");

            const std::size_t sectionOffset =
                ntOffset + offsetof(IMAGE_NT_HEADERS32, OptionalHeader) +
                nt_.FileHeader.SizeOfOptionalHeader;
            detail::Require(
                RangeFits(sectionOffset,
                          static_cast<std::size_t>(nt_.FileHeader.NumberOfSections) *
                              sizeof(IMAGE_SECTION_HEADER),
                          file_.size()),
                "PE section table extends beyond the file");

            sections_.reserve(nt_.FileHeader.NumberOfSections);
            for (WORD index = 0; index < nt_.FileHeader.NumberOfSections; ++index) {
                sections_.push_back(detail::ReadStruct<IMAGE_SECTION_HEADER>(
                    file_, sectionOffset +
                               static_cast<std::size_t>(index) *
                                   sizeof(IMAGE_SECTION_HEADER)));
            }
        }

        void MapSections()
        {
            image_ = static_cast<std::uint8_t*>(VirtualAlloc(
                nullptr, kSizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            detail::Require(image_ != nullptr,
                            "VirtualAlloc failed for the private PE image");
            detail::Require(reinterpret_cast<std::uintptr_t>(image_) <=
                                std::numeric_limits<std::uint32_t>::max(),
                            "mapped image is outside the PE32 address space");
            detail::Require(nt_.OptionalHeader.SizeOfHeaders <= file_.size() &&
                                nt_.OptionalHeader.SizeOfHeaders <= kSizeOfImage,
                            "invalid PE header span");
            std::memcpy(image_, file_.data(), nt_.OptionalHeader.SizeOfHeaders);

            for (const IMAGE_SECTION_HEADER& section : sections_) {
                const std::size_t virtualSpan =
                    std::max<std::size_t>(section.Misc.VirtualSize,
                                          section.SizeOfRawData);
                detail::Require(
                    RangeFits(section.VirtualAddress, virtualSpan, kSizeOfImage),
                    "PE section extends beyond SizeOfImage");
                if (section.SizeOfRawData == 0) continue;
                detail::Require(
                    RangeFits(section.PointerToRawData, section.SizeOfRawData,
                              file_.size()),
                    "PE section raw bytes extend beyond the file");
                std::memcpy(image_ + section.VirtualAddress,
                            file_.data() + section.PointerToRawData,
                            section.SizeOfRawData);
            }
        }

        void RequireImageRange(std::uint32_t rva, std::size_t length) const
        {
            detail::Require(RangeFits(rva, length, kSizeOfImage),
                            "RVA extends beyond the private image");
        }

        std::uint32_t ReadMappedU32(std::uint32_t rva) const
        {
            RequireImageRange(rva, sizeof(std::uint32_t));
            std::uint32_t value = 0;
            std::memcpy(&value, image_ + rva, sizeof(value));
            return value;
        }

        void RequireSpanHash(std::uint32_t rva, std::size_t length,
                             const char* expected, const char* label) const
        {
            RequireImageRange(rva, length);
            detail::Require(detail::Sha256(image_ + rva, length) == expected,
                            std::string("native signature mismatch: ") + label);
        }

        std::uint32_t Rel32Target(std::uint32_t instructionRva,
                                  std::uint8_t opcode) const
        {
            RequireImageRange(instructionRva, 5);
            detail::Require(image_[instructionRva] == opcode,
                            "unexpected relative-control-flow opcode");
            std::int32_t displacement = 0;
            std::memcpy(&displacement, image_ + instructionRva + 1,
                        sizeof(displacement));
            return instructionRva + 5 + displacement;
        }

        void VerifyNativeCode() const
        {
            RequireSpanHash(kConstructorRva, 0x74, kConstructorSha256,
                            "room-info constructor");
            RequireSpanHash(kStrncpyRva, 0x124, kStrncpySha256, "static strncpy");
            RequireSpanHash(kMemsetRva, 0x7A, kMemsetSha256, "static memset");
            RequireSpanHash(kMemsetSseRva, 0xBA, kMemsetSseSha256,
                            "static zero-fill continuation");

            detail::Require(Rel32Target(0x000F612A, 0xE8) == kStrncpyRva,
                            "constructor strncpy call target changed");
            detail::Require(Rel32Target(0x000F613D, 0xE8) == kMemsetRva,
                            "constructor first memset call target changed");
            detail::Require(Rel32Target(0x000F6153, 0xE8) == kMemsetRva,
                            "constructor second memset call target changed");
            detail::Require(Rel32Target(0x00332C27, 0xE9) == kMemsetSseRva,
                            "memset continuation target changed");
            detail::Require(
                ReadMappedU32(kMemsetFlagOperandRva) ==
                    kPreferredBase + kMemsetFlagRva,
                "memset flag operand changed");
        }

        void ApplyRelocations()
        {
            const std::uintptr_t actualBase =
                reinterpret_cast<std::uintptr_t>(image_);
            if (actualBase == kPreferredBase) return;

            const IMAGE_DATA_DIRECTORY directory =
                nt_.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
            detail::Require(directory.VirtualAddress != 0 && directory.Size != 0,
                            "relocations are required but absent");
            RequireImageRange(directory.VirtualAddress, directory.Size);

            const std::uint32_t delta =
                static_cast<std::uint32_t>(actualBase - kPreferredBase);
            std::size_t offset = 0;
            while (offset < directory.Size) {
                detail::Require(
                    directory.Size - offset >= sizeof(IMAGE_BASE_RELOCATION),
                    "truncated relocation block");
                IMAGE_BASE_RELOCATION block = {};
                std::memcpy(&block, image_ + directory.VirtualAddress + offset,
                            sizeof(block));
                if (block.VirtualAddress == 0 && block.SizeOfBlock == 0) break;
                detail::Require(
                    block.SizeOfBlock >= sizeof(IMAGE_BASE_RELOCATION) &&
                        block.SizeOfBlock <= directory.Size - offset &&
                        (block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) %
                                sizeof(WORD) ==
                            0,
                    "invalid relocation block size");

                const std::size_t count =
                    (block.SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) /
                    sizeof(WORD);
                const std::uint8_t* entries =
                    image_ + directory.VirtualAddress + offset +
                    sizeof(IMAGE_BASE_RELOCATION);
                for (std::size_t index = 0; index < count; ++index) {
                    WORD entry = 0;
                    std::memcpy(&entry, entries + index * sizeof(entry),
                                sizeof(entry));
                    const WORD type = entry >> 12;
                    const std::uint32_t patchRva =
                        block.VirtualAddress + (entry & 0x0FFF);
                    if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
                    detail::Require(type == IMAGE_REL_BASED_HIGHLOW,
                                    "unsupported non-HIGHLOW relocation");
                    RequireImageRange(patchRva, sizeof(std::uint32_t));

                    std::uint32_t value = ReadMappedU32(patchRva);
                    value += delta;
                    std::memcpy(image_ + patchRva, &value, sizeof(value));
                    ++relocationCount_;
                    if (patchRva == kMemsetFlagOperandRva) {
                        sawMemsetFlagRelocation_ = true;
                    }
                }
                offset += block.SizeOfBlock;
            }

            detail::Require(relocationCount_ != 0,
                            "relocation directory contained no HIGHLOW entries");
            detail::Require(sawMemsetFlagRelocation_,
                            "memset global operand was not relocated");
        }

        void VerifyRelocatedPreconditions() const
        {
            const std::uint32_t expectedFlagAddress =
                static_cast<std::uint32_t>(
                    reinterpret_cast<std::uintptr_t>(image_) + kMemsetFlagRva);
            detail::Require(ReadMappedU32(kMemsetFlagOperandRva) ==
                                expectedFlagAddress,
                            "memset global operand has the wrong relocated address");
            detail::Require(ReadMappedU32(kMemsetFlagRva) == 0,
                            "memset runtime-selection flag is not zero");
        }

        void SealImage()
        {
            DWORD oldProtection = 0;
            detail::Require(
                VirtualProtect(image_, kSizeOfImage, PAGE_EXECUTE_READ,
                               &oldProtection) != FALSE,
                "cannot seal the private image read/execute");
            detail::Require(
                FlushInstructionCache(GetCurrentProcess(), image_, kSizeOfImage) !=
                    FALSE,
                "cannot flush the private image instruction cache");
        }
    };
}
