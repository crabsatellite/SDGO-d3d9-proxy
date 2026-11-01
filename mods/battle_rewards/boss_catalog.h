// Portable owner-derived (MissionID,NpcDataID) identities. Model IDs and the
// broad native BossFlag are never used as a substitute for a rank Boss entry.
#pragma once
#include <windows.h>
#include <bcrypt.h>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sdgo::battle_rewards {
class BossCatalog {
    std::set<std::pair<uint32_t, uint32_t>> identities_;
    std::string revision_;

    static bool Integer(const std::string& text, uint32_t* value) {
        if (text.empty()) return false;
        uint32_t n = 0;
        for (char c : text) {
            if (c < '0' || c > '9' || n > (2147483647u - static_cast<uint32_t>(c - '0')) / 10) return false;
            n = n * 10 + static_cast<uint32_t>(c - '0');
        }
        if (!n) return false;
        *value = n; return true;
    }

    static bool Digest(const std::string& body, std::string* digest) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD size = 0, copied = 0;
        bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
        if (ok) ok = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&size), sizeof(size), &copied, 0) >= 0;
        std::vector<UCHAR> object(ok ? size : 0);
        UCHAR bytes[32] = {};
        if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), size, nullptr, 0, 0) >= 0;
        if (ok) ok = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(body.data())), static_cast<ULONG>(body.size()), 0) >= 0;
        if (ok) ok = BCryptFinishHash(hash, bytes, sizeof(bytes), 0) >= 0;
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!ok) return false;
        static const char hex[] = "0123456789abcdef";
        digest->clear(); digest->reserve(64);
        for (UCHAR byte : bytes) { *digest += hex[byte >> 4]; *digest += hex[byte & 15]; }
        return true;
    }
public:
    static bool Sha256(const std::string& bytes, std::string* digest) { return Digest(bytes, digest); }
    bool IsBoss(uint32_t mission, uint32_t npc) const {
        return identities_.find({mission, npc}) != identities_.end();
    }
    size_t Count() const { return identities_.size(); }
    const std::string& Revision() const { return revision_; }

    bool Parse(const std::string& bytes, std::string* error = nullptr) {
        auto fail = [error](const char* message) { if (error) *error = message; return false; };
        if (bytes.size() > 5 * 1024 * 1024) return fail("Boss registry file is too large");
        const std::string magic = "SDGO_BOSS_REGISTRY_V1 ";
        const size_t first = bytes.find('\n');
        if (first != magic.size() + 64 || bytes.compare(0, magic.size(), magic)) return fail("Boss registry header is invalid");
        const std::string expected = bytes.substr(magic.size(), 64);
        const std::string body = bytes.substr(first + 1);
        std::string actual;
        if (!Digest(body, &actual) || actual != expected) return fail("Boss registry checksum mismatch");
        BossCatalog candidate;
        size_t offset = first + 1;
        while (offset < bytes.size()) {
            size_t end = bytes.find('\n', offset);
            if (end == std::string::npos) return fail("Boss registry row is not newline terminated");
            std::string row = bytes.substr(offset, end - offset);
            size_t comma = row.find(',');
            uint32_t mission = 0, npc = 0;
            if (comma == std::string::npos || !Integer(row.substr(0, comma), &mission) || !Integer(row.substr(comma + 1), &npc))
                return fail("Boss registry identity is invalid");
            if (!candidate.identities_.insert({mission, npc}).second || candidate.identities_.size() > 100000)
                return fail("Boss registry duplicate or excessive identity");
            offset = end + 1;
        }
        candidate.revision_ = expected;
        *this = std::move(candidate);
        if (error) error->clear();
        return true;
    }

    bool LoadFile(const wchar_t* path, std::string* error = nullptr) {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { if (error) *error = "Boss registry is unavailable"; return false; }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 5 * 1024 * 1024) {
            CloseHandle(file); if (error) *error = "Boss registry file is too large"; return false;
        }
        std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0;
        bool complete = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
        CloseHandle(file);
        if (!complete) { if (error) *error = "Boss registry read was incomplete"; return false; }
        return Parse(bytes, error);
    }
};
}
