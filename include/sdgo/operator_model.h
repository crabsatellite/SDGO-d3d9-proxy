#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace sdgo::operators {
constexpr size_t kOperatorSize = 1671;
constexpr size_t kCategorySize = 614;
constexpr size_t kMissionPartsOffset = 0x686;
constexpr size_t kAvatarOffsets[] = {205, 313, 421, 533, 633, 733, 833};
constexpr size_t kAvatarCapacities[] = {54, 54, 54, 50, 50, 50, 50};

inline uint32_t ReadId(const uint8_t* bytes) { uint32_t id = 0; std::memcpy(&id, bytes, 4); return id; }
inline void WriteId(uint8_t* bytes, uint32_t id) { std::memcpy(bytes, &id, 4); }
inline bool Text(uint8_t* bytes, size_t capacity, const std::wstring& value)
{
    if (value.size() >= capacity) return false;
    std::memset(bytes, 0, capacity * sizeof(wchar_t));
    std::memcpy(bytes, value.data(), value.size() * sizeof(wchar_t));
    return true;
}
inline bool PatchOperator(uint8_t* row, size_t size, uint32_t id, const std::wstring& name,
                          const uint8_t* avatarProfile, const std::wstring& customSprite)
{
    if (!row || size != kOperatorSize || !avatarProfile || id < 60001 || id > 60999 ||
        name.empty() || name.size() > 48 || (!customSprite.empty() && customSprite.size() >= 50)) return false;
    WriteId(row, id);
    if (!Text(row + 5, 100, name)) return false;
    for (size_t i = 0; i < 7; ++i) {
        if (customSprite.empty())
            std::memcpy(row + kAvatarOffsets[i], avatarProfile + kAvatarOffsets[i], kAvatarCapacities[i] * 2);
        else {
            if (customSprite.size() < 4 || customSprite.substr(customSprite.size()-4) != L".spr") return false;
            const std::wstring path = customSprite.substr(0, customSprite.size()-4) + L"_" + std::to_wstring(i) + L".spr";
            if (!Text(row + kAvatarOffsets[i], kAvatarCapacities[i], path)) return false;
        }
    }
    return true;
}
inline bool PatchCategory(uint8_t* row, size_t size, uint32_t category, uint32_t op, const std::wstring& name)
{
    if (!row || size != kCategorySize || category != 1219000 + op - 60000 || name.size() > 48) return false;
    WriteId(row, category);
    if (!Text(row + 5, 50, name)) return false;
    WriteId(row + 105, op);
    row[109] = 0; // permanent item; no changes to existing native categories
    std::memset(row + 110, 0, 4);
    return true;
}
}
