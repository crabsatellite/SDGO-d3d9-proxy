#pragma once

#include <cstddef>
#include <cstdint>

namespace capsule_currency_fix
{
    constexpr std::size_t kRecordSize = 0xB3;
    constexpr std::size_t kMachineIdOffset = 5;
    constexpr std::size_t kCurrencyTypeOffset = 9;
    constexpr std::size_t kPriceOffset = 15;
    constexpr std::uint32_t kTargetMachineId = 9301;
    constexpr std::uint32_t kTargetGridId = 145093;
    constexpr std::uint32_t kTargetPrice = 99;
    constexpr std::uint8_t kGpCurrencyType = 0;
    constexpr std::uint8_t kTokenCurrencyType = 2;
    constexpr std::size_t kSlotsPerPage = 8;
    constexpr std::size_t kMaxSlots = 4096;

    enum class FixResult
    {
        Patched,
        NeedsCorrection,
        AlreadyCorrect,
        TargetMissing,
        InvalidArguments,
        InvalidShape,
        DuplicateTarget,
        UnexpectedPrice,
        UnexpectedCurrency,
        RegistryMismatch
    };

    // Inspect the complete native display vector without writing either copy.
    FixResult InspectActivityMachineVector(std::uint8_t* begin, std::uint8_t* end,
                                          std::uint8_t* capacity, std::uint32_t pageCount,
                                          std::uint8_t** target);

    // Display-only helper retained for the original bounds/shape regression tests.
    // Runtime also validates and corrects the corresponding in-memory registry row.
    FixResult FixActivityMachineVector(std::uint8_t* begin, std::uint8_t* end,
                                       std::uint8_t* capacity, std::uint32_t pageCount);

    const char* FixResultName(FixResult result);
}
