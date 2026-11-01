#include "capsule_runtime.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

namespace
{
    constexpr uint32_t kExpectedSchema = 1;
    constexpr uint32_t kExpectedChunkSize = 512;
    constexpr size_t kMaxIndexBytes = 16u * 1024u * 1024u;
    constexpr uint32_t kMaxEntries = 8192;

    uint32_t RotateRight(uint32_t value, uint32_t bits)
    {
        return (value >> bits) | (value << (32u - bits));
    }

    class Sha256
    {
    public:
        void Update(const uint8_t* data, size_t length)
        {
            m_total += static_cast<uint64_t>(length);
            while (length != 0) {
                size_t take = (std::min)(length, m_buffer.size() - m_used);
                std::memcpy(m_buffer.data() + m_used, data, take);
                m_used += take;
                data += take;
                length -= take;
                if (m_used == m_buffer.size()) {
                    Transform(m_buffer.data());
                    m_used = 0;
                }
            }
        }

        std::array<uint8_t, 32> Finish()
        {
            uint64_t bitLength = m_total * 8u;
            m_buffer[m_used++] = 0x80;
            if (m_used > 56) {
                std::fill(m_buffer.begin() + static_cast<ptrdiff_t>(m_used), m_buffer.end(), uint8_t{0});
                Transform(m_buffer.data());
                m_used = 0;
            }
            std::fill(m_buffer.begin() + static_cast<ptrdiff_t>(m_used), m_buffer.begin() + 56, uint8_t{0});
            for (size_t i = 0; i < 8; ++i)
                m_buffer[63 - i] = static_cast<uint8_t>(bitLength >> (i * 8u));
            Transform(m_buffer.data());

            std::array<uint8_t, 32> result = {};
            for (size_t i = 0; i < m_state.size(); ++i) {
                result[i * 4] = static_cast<uint8_t>(m_state[i] >> 24);
                result[i * 4 + 1] = static_cast<uint8_t>(m_state[i] >> 16);
                result[i * 4 + 2] = static_cast<uint8_t>(m_state[i] >> 8);
                result[i * 4 + 3] = static_cast<uint8_t>(m_state[i]);
            }
            return result;
        }

    private:
        void Transform(const uint8_t* block)
        {
            static constexpr uint32_t constants[64] = {
                0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
            };
            uint32_t schedule[64] = {};
            for (size_t i = 0; i < 16; ++i) {
                schedule[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                              (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                              (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                              static_cast<uint32_t>(block[i * 4 + 3]);
            }
            for (size_t i = 16; i < 64; ++i) {
                uint32_t s0 = RotateRight(schedule[i - 15], 7) ^ RotateRight(schedule[i - 15], 18) ^
                              (schedule[i - 15] >> 3);
                uint32_t s1 = RotateRight(schedule[i - 2], 17) ^ RotateRight(schedule[i - 2], 19) ^
                              (schedule[i - 2] >> 10);
                schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
            }
            uint32_t a = m_state[0], b = m_state[1], c = m_state[2], d = m_state[3];
            uint32_t e = m_state[4], f = m_state[5], g = m_state[6], h = m_state[7];
            for (size_t i = 0; i < 64; ++i) {
                uint32_t sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
                uint32_t choice = (e & f) ^ (~e & g);
                uint32_t temp1 = h + sum1 + choice + constants[i] + schedule[i];
                uint32_t sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
                uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
                uint32_t temp2 = sum0 + majority;
                h = g; g = f; f = e; e = d + temp1;
                d = c; c = b; b = a; a = temp1 + temp2;
            }
            m_state[0] += a; m_state[1] += b; m_state[2] += c; m_state[3] += d;
            m_state[4] += e; m_state[5] += f; m_state[6] += g; m_state[7] += h;
        }

        std::array<uint32_t, 8> m_state = {
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
        };
        std::array<uint8_t, 64> m_buffer = {};
        size_t m_used = 0;
        uint64_t m_total = 0;
    };

    std::string Narrow(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (wchar_t value : text)
            result.push_back(value < 0x80 ? static_cast<char>(value) : '?');
        return result;
    }

    std::wstring IniValue(const std::wstring& ini, const wchar_t* section, const wchar_t* key)
    {
        wchar_t value[1024] = {};
        GetPrivateProfileStringW(section, key, L"", value, static_cast<DWORD>(std::size(value)), ini.c_str());
        return value;
    }

    bool ParseUnsigned(const std::wstring& text, uint32_t& result)
    {
        if (text.empty())
            return false;
        wchar_t* end = nullptr;
        unsigned long value = wcstoul(text.c_str(), &end, 10);
        if (!end || *end != L'\0' || value > (std::numeric_limits<uint32_t>::max)())
            return false;
        result = static_cast<uint32_t>(value);
        return true;
    }

    bool SafeRelativePath(const std::wstring& path)
    {
        if (path.empty() || path.size() > 240 || path[0] == L'\\' || path[0] == L'/' ||
            path.find(L':') != std::wstring::npos)
            return false;
        size_t begin = 0;
        while (begin <= path.size()) {
            size_t end = path.find_first_of(L"\\/", begin);
            std::wstring part = path.substr(begin, end == std::wstring::npos ? std::wstring::npos : end - begin);
            if (part.empty() || part == L"." || part == L"..")
                return false;
            if (end == std::wstring::npos)
                break;
            begin = end + 1;
        }
        return true;
    }

    std::wstring Join(const std::wstring& root, const std::wstring& relative)
    {
        return root + (root.empty() || root.back() == L'\\' ? L"" : L"\\") + relative;
    }

    bool ReadFileBytes(const std::wstring& path, size_t limit, std::vector<uint8_t>& bytes, std::string& error)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "cannot open " + Narrow(path);
            return false;
        }
        LARGE_INTEGER size = {};
        bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0 &&
                  static_cast<uint64_t>(size.QuadPart) <= static_cast<uint64_t>(limit);
        if (ok) {
            bytes.resize(static_cast<size_t>(size.QuadPart));
            size_t cursor = 0;
            while (cursor < bytes.size()) {
                DWORD want = static_cast<DWORD>((std::min)(bytes.size() - cursor, static_cast<size_t>(1u << 20)));
                DWORD got = 0;
                if (!ReadFile(file, bytes.data() + cursor, want, &got, nullptr) || got != want) {
                    ok = false;
                    break;
                }
                cursor += got;
            }
        }
        CloseHandle(file);
        if (!ok) {
            error = "cannot read or file is too large: " + Narrow(path);
            return false;
        }
        return true;
    }

    std::string Hex(const std::array<uint8_t, 32>& hash)
    {
        static constexpr char digits[] = "0123456789abcdef";
        std::string result(64, '0');
        for (size_t i = 0; i < hash.size(); ++i) {
            result[i * 2] = digits[hash[i] >> 4];
            result[i * 2 + 1] = digits[hash[i] & 0x0f];
        }
        return result;
    }

    bool FileHash(const std::wstring& path, std::string& hash, uint64_t* size, std::string& error)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = "cannot open " + Narrow(path);
            return false;
        }
        LARGE_INTEGER length = {};
        if (!GetFileSizeEx(file, &length) || length.QuadPart < 0) {
            CloseHandle(file);
            error = "cannot size " + Narrow(path);
            return false;
        }
        Sha256 sha;
        std::array<uint8_t, 64 * 1024> buffer = {};
        bool ok = true;
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                ok = false;
                break;
            }
            if (read == 0)
                break;
            sha.Update(buffer.data(), read);
        }
        CloseHandle(file);
        if (!ok) {
            error = "cannot hash " + Narrow(path);
            return false;
        }
        hash = Hex(sha.Finish());
        if (size)
            *size = static_cast<uint64_t>(length.QuadPart);
        return true;
    }

    bool ValidHashText(const std::wstring& text)
    {
        return text.size() == 64 && std::all_of(text.begin(), text.end(), [](wchar_t value) {
            return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
                   (value >= L'A' && value <= L'F');
        });
    }

    std::string LowerAscii(const std::wstring& text)
    {
        std::string result;
        result.reserve(text.size());
        for (wchar_t value : text) {
            if (value >= L'A' && value <= L'F')
                value = static_cast<wchar_t>(value + (L'a' - L'A'));
            result.push_back(static_cast<char>(value));
        }
        return result;
    }

    bool VerifyDeclaredFile(const std::wstring& root, const std::wstring& relative,
                            const std::wstring& expected, uint64_t* size, std::string& error)
    {
        if (!SafeRelativePath(relative) || !ValidHashText(expected)) {
            error = "unsafe path or invalid hash in runtime.ini";
            return false;
        }
        std::string actual;
        if (!FileHash(Join(root, relative), actual, size, error))
            return false;
        if (actual != LowerAscii(expected)) {
            error = "SHA-256 mismatch for " + Narrow(relative);
            return false;
        }
        return true;
    }

    uint32_t ReadU32(const std::vector<uint8_t>& bytes, size_t offset)
    {
        uint32_t value = 0;
        std::memcpy(&value, bytes.data() + offset, sizeof(value));
        return value;
    }

    bool AllowedEntry(const std::wstring& name)
    {
        if (name == L"gdrs\\0033.gdt" || name == L"gdrs\\0101.gdt" ||
            name == L"gdrs\\0154.gdt" || name == L"gdrs\\0155.gdt")
            return true;
        if (name == L"uirs\\1161.uir")
            return true;
        const wchar_t* prefix = nullptr;
        const wchar_t* suffix = nullptr;
        if (name.rfind(L"sprs\\", 0) == 0) {
            prefix = L"sprs\\";
            suffix = L".spr";
        } else if (name.rfind(L"txrs\\", 0) == 0) {
            prefix = L"txrs\\";
            suffix = L".txr";
        } else {
            return false;
        }
        size_t prefixLength = wcslen(prefix);
        size_t suffixLength = wcslen(suffix);
        if (name.size() <= prefixLength + suffixLength ||
            name.compare(name.size() - suffixLength, suffixLength, suffix) != 0)
            return false;
        std::wstring number = name.substr(prefixLength, name.size() - prefixLength - suffixLength);
        if (number.empty() || number.find_first_not_of(L"0123456789") != std::wstring::npos)
            return false;
        wchar_t* end = nullptr;
        unsigned long id = wcstoul(number.c_str(), &end, 10);
        return end && *end == L'\0' && id >= 60000 && id <= 65535;
    }

    bool ParseIndex(const std::vector<uint8_t>& bytes, uint64_t zpkSize,
                    capsule_catalog::Bundle& bundle, std::string& error)
    {
        if (bytes.size() < 24) {
            error = "overlay.zdx header is truncated";
            return false;
        }
        uint32_t version = ReadU32(bytes, 0);
        uint32_t chunkSize = ReadU32(bytes, 4);
        uint32_t chunkCount = ReadU32(bytes, 8);
        uint32_t entryCount = ReadU32(bytes, 16);
        if (version != 1 || chunkSize != kExpectedChunkSize || entryCount > kMaxEntries) {
            error = "overlay.zdx header is unsupported";
            return false;
        }
        uint64_t physicalSize = static_cast<uint64_t>(chunkCount) * (static_cast<uint64_t>(chunkSize) + 8u);
        if (physicalSize != zpkSize) {
            error = "overlay.zpk size does not match its chunk table";
            return false;
        }
        size_t cursor = 24;
        for (uint32_t index = 0; index < entryCount; ++index) {
            std::wstring name;
            bool terminated = false;
            for (size_t count = 0; count < 260; ++count) {
                if (cursor + 2 > bytes.size()) {
                    error = "overlay.zdx path is truncated";
                    return false;
                }
                uint16_t encoded = static_cast<uint16_t>(bytes[cursor] | (bytes[cursor + 1] << 8));
                cursor += 2;
                wchar_t value = static_cast<wchar_t>(encoded ^ 0x00aa);
                if (value == L'\0') {
                    terminated = true;
                    break;
                }
                if (value > 0x7f) {
                    error = "overlay entry path is not ASCII";
                    return false;
                }
                if (value == L'/')
                    value = L'\\';
                if (value >= L'A' && value <= L'Z')
                    value = static_cast<wchar_t>(value + (L'a' - L'A'));
                name.push_back(value);
            }
            if (!terminated || !AllowedEntry(name) || cursor + 36 > bytes.size()) {
                error = "overlay entry path or metadata is invalid";
                return false;
            }
            capsule_catalog::OverlayEntry entry;
            for (size_t word = 0; word < entry.info.words.size(); ++word)
                entry.info.words[word] = ReadU32(bytes, cursor + word * 4);
            cursor += 36;
            uint32_t storedBytes = entry.info.words[0];
            uint32_t firstChunk = entry.info.words[1];
            uint32_t chunks = entry.info.words[2];
            if (storedBytes == 0 || chunks == 0 ||
                static_cast<uint64_t>(storedBytes) > static_cast<uint64_t>(chunks) * chunkSize ||
                static_cast<uint64_t>(firstChunk) + chunks > chunkCount) {
                error = "overlay entry chunk range is invalid";
                return false;
            }
            if (!bundle.entries.emplace(name, entry).second) {
                error = "overlay contains a duplicate entry";
                return false;
            }
        }
        if (cursor != bytes.size()) {
            error = "overlay.zdx has trailing bytes";
            return false;
        }
        for (const wchar_t* required : {L"gdrs\\0033.gdt", L"gdrs\\0101.gdt",
                                        L"gdrs\\0154.gdt", L"gdrs\\0155.gdt"}) {
            if (bundle.entries.find(required) == bundle.entries.end()) {
                error = "overlay is missing a required GDT";
                return false;
            }
        }
        bundle.chunkSize = chunkSize;
        return true;
    }
}

bool capsule_catalog::IsOverlayGdtPath(const std::wstring& path)
{
    return path == L"gdrs\\0033.gdt" || path == L"gdrs\\0101.gdt" ||
           path == L"gdrs\\0154.gdt" || path == L"gdrs\\0155.gdt";
}

bool capsule_catalog::ConsumeOverlayGdtChecksum(
    std::unordered_map<void*, std::wstring>& trackedStreams,
    void* stream,
    const std::wstring* nativePath)
{
    auto found = trackedStreams.find(stream);
    if (found == trackedStreams.end())
        return false;

    const bool accepted = nativePath != nullptr &&
                          *nativePath == found->second &&
                          IsOverlayGdtPath(*nativePath);
    trackedStreams.erase(found);
    return accepted;
}

const capsule_catalog::OverlayEntry* capsule_catalog::Bundle::Find(const std::wstring& nativePath) const
{
    std::wstring normalized;
    normalized.reserve(nativePath.size());
    for (wchar_t value : nativePath) {
        if (value == L'/')
            value = L'\\';
        if (value >= L'A' && value <= L'Z')
            value = static_cast<wchar_t>(value + (L'a' - L'A'));
        normalized.push_back(value);
    }
    auto found = entries.find(normalized);
    return found == entries.end() ? nullptr : &found->second;
}

bool capsule_catalog::LoadBundle(const std::wstring& modDirectory, Bundle& out, std::string& error)
{
    const std::wstring ini = Join(modDirectory, L"runtime.ini");
    uint32_t schema = 0;
    if (!ParseUnsigned(IniValue(ini, L"runtime", L"schema"), schema) || schema != kExpectedSchema) {
        error = "runtime.ini schema is missing or unsupported";
        return false;
    }
    std::wstring revision = IniValue(ini, L"runtime", L"catalog_revision");
    if (!ValidHashText(revision)) {
        error = "catalog_revision is not a 64-character digest";
        return false;
    }

    Bundle candidate;
    candidate.catalogRevision = LowerAscii(revision);
    // Counts are used only for the post-load native registration receipt.
    // The compiler always writes them; zero remains a valid count.
    for (const auto& item : {std::pair<const wchar_t*, uint32_t*>(L"fixed", &candidate.fixedCount),
                             {L"coin", &candidate.coinCount},
                             {L"custom", &candidate.customCount},
                             {L"candidates", &candidate.candidateCount}}) {
        if (!ParseUnsigned(IniValue(ini, L"counts", item.first), *item.second)) {
            error = "runtime.ini is missing a valid [counts] value";
            return false;
        }
    }
    for (const wchar_t* table : {L"0033", L"0101", L"0154", L"0155"}) {
        std::wstring relative = IniValue(ini, L"gdt", table);
        std::wstring key = std::wstring(table) + L"_sha256";
        if (!VerifyDeclaredFile(modDirectory, relative, IniValue(ini, L"gdt", key.c_str()), nullptr, error))
            return false;
    }
    const std::wstring uiRelative = IniValue(ini, L"ui", L"1161");
    const bool declaredUi = !uiRelative.empty();
    if (declaredUi) {
        if (uiRelative != L"ui\\1161.uir") {
            error = "unsupported capsule UI declaration";
            return false;
        }
        if (!VerifyDeclaredFile(modDirectory, uiRelative,
                                IniValue(ini, L"ui", L"1161_sha256"), nullptr, error))
            return false;
    }

    std::wstring overlay = IniValue(ini, L"runtime", L"overlay");
    if (!SafeRelativePath(overlay)) {
        error = "overlay path is unsafe";
        return false;
    }
    std::wstring zdxRelative = overlay + L".zdx";
    std::wstring zpkRelative = overlay + L".zpk";
    uint64_t zdxSize = 0;
    uint64_t zpkSize = 0;
    if (!VerifyDeclaredFile(modDirectory, zdxRelative,
                            IniValue(ini, L"runtime", L"overlay_zdx_sha256"), &zdxSize, error) ||
        !VerifyDeclaredFile(modDirectory, zpkRelative,
                            IniValue(ini, L"runtime", L"overlay_zpk_sha256"), &zpkSize, error))
        return false;
    if (zdxSize > kMaxIndexBytes) {
        error = "overlay.zdx is too large";
        return false;
    }
    std::vector<uint8_t> index;
    if (!ReadFileBytes(Join(modDirectory, zdxRelative), kMaxIndexBytes, index, error))
        return false;
    candidate.zpkPath = Join(modDirectory, zpkRelative);
    if (!ParseIndex(index, zpkSize, candidate, error))
        return false;
    if ((candidate.entries.find(L"uirs\\1161.uir") != candidate.entries.end()) != declaredUi) {
        error = "capsule UI overlay declaration differs from its archive";
        return false;
    }

    out = std::move(candidate);
    return true;
}
