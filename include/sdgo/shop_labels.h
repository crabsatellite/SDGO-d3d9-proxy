#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace sdgo::shop {
// The native shop option is a packed 37-byte wire/model record. The last field
// is CP874 text; business values are numeric and must never be changed to fix it.
#pragma pack(push, 1)
struct Option {
    uint32_t productId;
    uint32_t availableQuantity;
    uint32_t id;
    int32_t price;
    int32_t validity;
    uint32_t category;
    uint8_t payment;
    uint8_t flags;
    char text[11];
};
#pragma pack(pop)
static_assert(sizeof(Option) == 37);
static_assert(offsetof(Option, id) == 0x08);
static_assert(offsetof(Option, price) == 0x0C);
static_assert(offsetof(Option, validity) == 0x10);
static_assert(offsetof(Option, category) == 0x14);
static_assert(offsetof(Option, payment) == 0x18);
static_assert(offsetof(Option, text) == 0x1A);

inline std::wstring DurationLabel(const Option& option, int timeLimit)
{
    if (option.validity <= 0 || option.validity > 36500)
        return {};
    const uint32_t kind = option.category / 100000;
    if (timeLimit == 1) {
        const wchar_t* prefix = kind == 11 ? L"租赁 " : L"有效期 ";
        return std::wstring(prefix) + std::to_wstring(option.validity) + L" 天";
    }
    if (timeLimit == 0 && kind == 11)
        return L"永久";
    return {};
}
} // namespace sdgo::shop
