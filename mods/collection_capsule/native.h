#pragma once

#include <cstddef>
#include <cstdint>

namespace sdgo::collection_capsule {

inline constexpr std::int32_t kDefaultMachineId = 990001;
inline constexpr std::int32_t kDatabaseFailure = 1;
inline constexpr std::size_t kNativeCommandOffset = 0x08;
inline constexpr std::size_t kNativeErrorOffset = 0x240;

// WorldServer 548A86E3/0010C000, RVA 0000E510:
// int __thiscall Pick(void* connection, PickInput*, PickOutput*), ret 0x0c.
// The shipped wrapper binds {return I4, account I4, machine I4} and reads one
// packed {I8,I4,I4,I8,UI1} row from dbo.sp_CreateCapsule.
#pragma pack(push, 1)
struct PickInput {
    std::int32_t result;
    std::int32_t account;
    std::int32_t machine;
};

struct PickOutput {
    std::int64_t userUnitNumber;
    std::int32_t unitId;
    std::int32_t slotIndex;
    std::int64_t serialNumber;
    std::uint8_t customLevelMax;
};
#pragma pack(pop)

static_assert(sizeof(PickInput) == 0x0c);
static_assert(offsetof(PickInput, result) == 0x00);
static_assert(offsetof(PickInput, account) == 0x04);
static_assert(offsetof(PickInput, machine) == 0x08);
static_assert(sizeof(PickOutput) == 0x19);
static_assert(offsetof(PickOutput, userUnitNumber) == 0x00);
static_assert(offsetof(PickOutput, unitId) == 0x08);
static_assert(offsetof(PickOutput, slotIndex) == 0x0c);
static_assert(offsetof(PickOutput, serialNumber) == 0x10);
static_assert(offsetof(PickOutput, customLevelMax) == 0x18);

using PickFunction =
    int(__thiscall*)(void* self, void* connection, PickInput* input, PickOutput* output);

} // namespace sdgo::collection_capsule
