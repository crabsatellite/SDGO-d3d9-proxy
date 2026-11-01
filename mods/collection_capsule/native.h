#pragma once

#include <cstddef>
#include <cstdint>

namespace sdgo::collection_capsule {

inline constexpr std::int32_t kDefaultMachineId = 990001;
inline constexpr std::int32_t kDatabaseFailure = 1;
inline constexpr std::int32_t kUnitCategoryBegin = 1100000;
inline constexpr std::int32_t kUnitCategoryEnd = 1200000;
inline constexpr std::size_t kNativeCommandOffset = 0x08;
inline constexpr std::size_t kNativeErrorOffset = 0x240;

// WorldServer 548A86E3/0010C000, RVA 0001F490:
// int __thiscall PickActivity(void* connection, ActivityPickRecord*), ret 0x08.
// The shipped wrapper binds {return I4, account I4, machine I4, drop-box serial
// I8 output, item category I4 output, amount I4 output} against
// dbo.sp_CreateMachineItem.
#pragma pack(push, 1)
struct ActivityPickRecord {
    std::int32_t result;
    std::int32_t account;
    std::int32_t machine;
    std::int64_t dropBoxSerial;
    std::int32_t itemCategory;
    std::int32_t amount;
};
#pragma pack(pop)

static_assert(sizeof(ActivityPickRecord) == 0x1c);
static_assert(offsetof(ActivityPickRecord, result) == 0x00);
static_assert(offsetof(ActivityPickRecord, account) == 0x04);
static_assert(offsetof(ActivityPickRecord, machine) == 0x08);
static_assert(offsetof(ActivityPickRecord, dropBoxSerial) == 0x0c);
static_assert(offsetof(ActivityPickRecord, itemCategory) == 0x14);
static_assert(offsetof(ActivityPickRecord, amount) == 0x18);

using ActivityPickFunction =
    int(__thiscall*)(void* self, void* connection, ActivityPickRecord* record);

} // namespace sdgo::collection_capsule
