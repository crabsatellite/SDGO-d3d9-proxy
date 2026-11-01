#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sdgo::catalog {
constexpr uint32_t kFirstNpc = 3604001, kLastNpc = 3607000;
constexpr size_t kClientNpcBytes = 598, kClientUnitBytes = 4976;
constexpr size_t kNativeNpcBytes = 0x26, kNativeUnitPrefixBytes = 0x59;
constexpr size_t kNativeUnitBytes = 0x72;
constexpr uint32_t kMaxRecords = kLastNpc - kFirstNpc + 1;

inline uint32_t U32(const void* bytes) {
    uint32_t value;
    memcpy(&value, bytes, sizeof(value));
    return value;
}
inline void Put32(void* bytes, uint32_t value) { memcpy(bytes, &value, sizeof(value)); }
inline uint32_t Hash32(const void* bytes, size_t size) {
    auto p = static_cast<const uint8_t*>(bytes);
    uint32_t value = 2166136261u;
    for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 16777619u;
    return value;
}
inline uint64_t Hash64(const void* bytes, size_t size) {
    auto p = static_cast<const uint8_t*>(bytes);
    uint64_t value = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) value = (value ^ p[i]) * 1099511628211ull;
    return value;
}

#pragma pack(push, 1)
struct Header {
    char magic[8];                 // SDGOAI1 followed by NUL
    uint32_t version;
    uint32_t recordBytes;
    uint32_t count;
    uint32_t payloadHash;           // corruption check, not an authenticity signature
};
struct Record {
    uint32_t npcId;
    uint32_t unitId;
    uint32_t level;
    uint32_t templateNpcId;
    uint32_t preferredWeapon;       // native 1-based, one of the three non-SP weapons
    uint64_t clientUnitHash;
    std::array<uint8_t, kNativeUnitPrefixBytes> nativeUnitPrefix;
    std::array<uint8_t, kNativeNpcBytes> nativeNpc;
    std::array<uint8_t, kClientNpcBytes> clientNpc;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 24);
static_assert(sizeof(Record) == 753);

class Catalog {
public:
    std::vector<Record> records;
    bool Parse(const std::vector<uint8_t>& bytes, std::string& error) {
        records.clear();
        if (bytes.size() < sizeof(Header)) return Fail(error, "short header");
        Header header;
        memcpy(&header, bytes.data(), sizeof(header));
        if (memcmp(header.magic, "SDGOAI1", 8) || header.version != 1 ||
            header.recordBytes != sizeof(Record) || !header.count || header.count > kMaxRecords)
            return Fail(error, "unsupported header or count");
        const size_t payloadBytes = static_cast<size_t>(header.count) * sizeof(Record);
        if (bytes.size() != sizeof(Header) + payloadBytes ||
            Hash32(bytes.data() + sizeof(Header), payloadBytes) != header.payloadHash)
            return Fail(error, "length or payload checksum mismatch");
        std::vector<Record> parsed(header.count);
        memcpy(parsed.data(), bytes.data() + sizeof(Header), payloadBytes);
        std::vector<uint32_t> unitIds;
        uint32_t previous = 0;
        for (const auto& row : parsed) {
            if (row.npcId < kFirstNpc || row.npcId > kLastNpc || row.npcId <= previous ||
                !row.unitId || row.unitId > 9999999 || row.level < 1 || row.level > 5 ||
                row.templateNpcId < kFirstNpc || row.templateNpcId > kLastNpc ||
                row.preferredWeapon < 1 || row.preferredWeapon > 3 || !row.clientUnitHash)
                return Fail(error, "invalid or unsorted record identity");
            previous = row.npcId;
            unitIds.push_back(row.unitId);
            const uint8_t* npc = row.nativeNpc.data();
            const uint8_t* client = row.clientNpc.data();
            if (U32(npc) != row.npcId || U32(npc + 4) != row.unitId ||
                U32(client) != row.npcId || client[4] || U32(client + 0x69) != row.unitId ||
                !U32(npc + 8) || U32(npc + 8) > 0x7fffffffu || U32(npc + 8) != U32(client + 0xe5) ||
                U32(row.nativeUnitPrefix.data()) != row.unitId ||
                !U32(row.nativeUnitPrefix.data() + 0x49 + (row.preferredWeapon - 1) * 4))
                return Fail(error, "client/server identity, HP or weapon mismatch");
            for (size_t start : {size_t(5), size_t(0x6d), size_t(0x114)}) {
                bool terminated = false;
                for (size_t i = start; i < start + 100; i += 2)
                    terminated = terminated || (client[i] == 0 && client[i + 1] == 0);
                if (!terminated) return Fail(error, "unterminated NPC string");
            }
        }
        std::sort(unitIds.begin(), unitIds.end());
        if (std::adjacent_find(unitIds.begin(), unitIds.end()) != unitIds.end())
            return Fail(error, "duplicate source unit");
        for (const auto& row : parsed) {
            auto donor = std::lower_bound(parsed.begin(), parsed.end(), row.templateNpcId,
                [](const Record& a, uint32_t id) { return a.npcId < id; });
            if (donor != parsed.end() && donor->npcId == row.templateNpcId)
                return Fail(error, "template collides with generated NPC");
        }
        records.swap(parsed);
        return true;
    }
    bool Load(const std::wstring& path, std::string& error) {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") || !file) return Fail(error, "catalog.bin is missing or unreadable");
        if (fseek(file, 0, SEEK_END)) { fclose(file); return Fail(error, "cannot size catalog"); }
        const long length = ftell(file);
        if (length < static_cast<long>(sizeof(Header)) ||
            length > static_cast<long>(sizeof(Header) + kMaxRecords * sizeof(Record))) {
            fclose(file); return Fail(error, "catalog size is outside the supported bound");
        }
        rewind(file);
        std::vector<uint8_t> bytes(static_cast<size_t>(length));
        const size_t count = fread(bytes.data(), 1, bytes.size(), file);
        fclose(file);
        return count == bytes.size() ? Parse(bytes, error) : Fail(error, "short catalog read");
    }
    size_t Find(uint32_t id) const {
        auto found = std::lower_bound(records.begin(), records.end(), id,
            [](const Record& row, uint32_t value) { return row.npcId < value; });
        return found != records.end() && found->npcId == id ? static_cast<size_t>(found - records.begin()) : records.size();
    }
private:
    static bool Fail(std::string& error, const char* message) { error = message; return false; }
};
} // namespace sdgo::catalog
