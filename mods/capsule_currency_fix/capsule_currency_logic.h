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
    constexpr std::uint32_t kTargetPrice = 99;
    constexpr std::uint8_t kGpCurrencyType = 0;
    constexpr std::uint8_t kTokenCurrencyType = 2;
    constexpr std::size_t kSlotsPerPage = 8;
    constexpr std::size_t kMaxSlots = 4096;

    enum class FixResult
    {
        Patched,
        AlreadyCorrect,
        TargetMissing,
        InvalidArguments,
        InvalidShape,
        DuplicateTarget,
        UnexpectedPrice,
        UnexpectedCurrency
    };

    // Fixes only the display-vector copy produced by
    // CapsuleMachineUI::LoadCapsuleItemData. It never touches the shared GDT.
    FixResult FixActivityMachineVector(std::uint8_t* begin, std::uint8_t* end,
                                       std::uint8_t* capacity, std::uint32_t pageCount);

    const char* FixResultName(FixResult result);
}
