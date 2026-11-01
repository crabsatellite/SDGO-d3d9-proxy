#include "capsule_currency_logic.h"

#include <cstring>
#include <limits>

namespace capsule_currency_fix
{
    namespace
    {
        std::uint32_t ReadU32(const std::uint8_t* address)
        {
            std::uint32_t value = 0;
            std::memcpy(&value, address, sizeof(value));
            return value;
        }
    }

    FixResult InspectActivityMachineVector(std::uint8_t* begin, std::uint8_t* end,
                                          std::uint8_t* capacity, std::uint32_t pageCount,
                                          std::uint8_t** output)
    {
        if (!output)
            return FixResult::InvalidArguments;
        *output = nullptr;
        if (!begin || !end || !capacity)
            return FixResult::InvalidArguments;

        const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
        const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
        const auto capacityAddress = reinterpret_cast<std::uintptr_t>(capacity);
        if (beginAddress > endAddress || endAddress > capacityAddress)
            return FixResult::InvalidShape;

        const std::uintptr_t usedBytes = endAddress - beginAddress;
        const std::uintptr_t capacityBytes = capacityAddress - beginAddress;
        if (usedBytes % kRecordSize != 0 || capacityBytes % kRecordSize != 0)
            return FixResult::InvalidShape;

        const std::uintptr_t slotCount = usedBytes / kRecordSize;
        const std::uintptr_t capacityCount = capacityBytes / kRecordSize;
        if (slotCount == 0 || slotCount > kMaxSlots || capacityCount > kMaxSlots)
            return FixResult::InvalidShape;
        if (pageCount == 0 || pageCount > kMaxSlots / kSlotsPerPage)
            return FixResult::InvalidShape;
        if (static_cast<std::uintptr_t>(pageCount) * kSlotsPerPage != slotCount)
            return FixResult::InvalidShape;

        std::uint8_t* target = nullptr;
        for (std::uintptr_t index = 0; index < slotCount; ++index) {
            std::uint8_t* record = begin + index * kRecordSize;
            if (ReadU32(record + kMachineIdOffset) != kTargetMachineId)
                continue;
            if (target)
                return FixResult::DuplicateTarget;
            target = record;
        }

        if (!target)
            return FixResult::TargetMissing;
        if (ReadU32(target + kPriceOffset) != kTargetPrice)
            return FixResult::UnexpectedPrice;

        // The native table stores a little-endian uint32, although the UI
        // branches read its low byte. Reject malformed nonzero upper bytes.
        const std::uint32_t currencyType = ReadU32(target + kCurrencyTypeOffset);
        if (currencyType == kTokenCurrencyType) {
            *output = target;
            return FixResult::AlreadyCorrect;
        }
        if (currencyType != kGpCurrencyType)
            return FixResult::UnexpectedCurrency;

        *output = target;
        return FixResult::NeedsCorrection;
    }

    FixResult FixActivityMachineVector(std::uint8_t* begin, std::uint8_t* end,
                                       std::uint8_t* capacity, std::uint32_t pageCount)
    {
        std::uint8_t* target = nullptr;
        const auto result = InspectActivityMachineVector(begin, end, capacity, pageCount, &target);
        if (result != FixResult::NeedsCorrection)
            return result;
        target[kCurrencyTypeOffset] = kTokenCurrencyType;
        return FixResult::Patched;
    }

    const char* FixResultName(FixResult result)
    {
        switch (result) {
        case FixResult::Patched:
            return "patched";
        case FixResult::NeedsCorrection:
            return "needs-correction";
        case FixResult::AlreadyCorrect:
            return "already-correct";
        case FixResult::TargetMissing:
            return "target-missing";
        case FixResult::InvalidArguments:
            return "invalid-arguments";
        case FixResult::InvalidShape:
            return "invalid-shape";
        case FixResult::DuplicateTarget:
            return "duplicate-target";
        case FixResult::UnexpectedPrice:
            return "unexpected-price";
        case FixResult::UnexpectedCurrency:
            return "unexpected-currency";
        case FixResult::RegistryMismatch:
            return "registry-mismatch";
        }
        return "unknown";
    }
}
