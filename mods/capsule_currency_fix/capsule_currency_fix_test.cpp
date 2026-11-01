#include "capsule_currency_logic.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    using capsule_currency_fix::FixResult;
    constexpr std::size_t kSlots = capsule_currency_fix::kSlotsPerPage;
    using Page = std::array<std::uint8_t, capsule_currency_fix::kRecordSize * kSlots>;

    void WriteU32(std::uint8_t* address, std::uint32_t value)
    {
        std::memcpy(address, &value, sizeof(value));
    }

    std::uint8_t* Record(Page& page, std::size_t index)
    {
        return page.data() + index * capsule_currency_fix::kRecordSize;
    }

    const std::uint8_t* Record(const Page& page, std::size_t index)
    {
        return page.data() + index * capsule_currency_fix::kRecordSize;
    }

    void SetRecord(Page& page, std::size_t index, std::uint32_t machineId,
                   std::uint8_t currencyType, std::uint32_t price)
    {
        std::uint8_t* record = Record(page, index);
        WriteU32(record + capsule_currency_fix::kMachineIdOffset, machineId);
        record[capsule_currency_fix::kCurrencyTypeOffset] = currencyType;
        WriteU32(record + capsule_currency_fix::kPriceOffset, price);
    }

    bool Expect(bool condition, const char* test, int line)
    {
        if (condition)
            return true;
        std::fprintf(stderr, "FAIL %s:%d\n", test, line);
        return false;
    }

#define CHECK(condition)            \
    do {                            \
        if (!Expect((condition), __func__, __LINE__)) \
            return false;           \
    } while (false)

    FixResult Fix(Page& page)
    {
        return capsule_currency_fix::FixActivityMachineVector(
            page.data(), page.data() + page.size(), page.data() + page.size(), 1);
    }

    bool PatchesOnlyExpectedByte()
    {
        Page page = {};
        SetRecord(page, 0, 9201, 0, 10000);
        SetRecord(page, 1, 9301, 0, 99);
        SetRecord(page, 2, 9101, 2, 3);
        const Page before = page;

        CHECK(Fix(page) == FixResult::Patched);
        std::size_t changed = 0;
        std::size_t changedOffset = 0;
        for (std::size_t i = 0; i < page.size(); ++i) {
            if (page[i] != before[i]) {
                ++changed;
                changedOffset = i;
            }
        }
        CHECK(changed == 1);
        CHECK(changedOffset == capsule_currency_fix::kRecordSize +
                                   capsule_currency_fix::kCurrencyTypeOffset);
        CHECK(page[changedOffset] == 2);
        CHECK(std::memcmp(Record(page, 0), Record(before, 0),
                          capsule_currency_fix::kRecordSize) == 0);
        CHECK(std::memcmp(Record(page, 2), Record(before, 2),
                          capsule_currency_fix::kRecordSize) == 0);
        return true;
    }

    bool StockPaddedCountShapeIsAccepted()
    {
        constexpr std::size_t stockRecordCount = 93;
        constexpr std::size_t paddedSlotCount = 96;
        constexpr std::uint32_t pageCount = 12;
        static_assert(paddedSlotCount == pageCount * capsule_currency_fix::kSlotsPerPage);

        std::vector<std::uint8_t> vector(
            paddedSlotCount * capsule_currency_fix::kRecordSize, 0);
        auto set = [&vector](std::size_t index, std::uint32_t machineId,
                             std::uint8_t currencyType, std::uint32_t price) {
            std::uint8_t* record =
                vector.data() + index * capsule_currency_fix::kRecordSize;
            WriteU32(record + capsule_currency_fix::kMachineIdOffset, machineId);
            record[capsule_currency_fix::kCurrencyTypeOffset] = currencyType;
            WriteU32(record + capsule_currency_fix::kPriceOffset, price);
        };

        // LoadCapsuleItemData reverses the 93 source records into the first
        // 93 slots, then leaves three zero padding slots at the end.
        set(0, 9301, 0, 99);
        set(1, 9201, 0, 10000);
        set(stockRecordCount - 1, 101, 0, 1200);
        const auto before = vector;

        CHECK(capsule_currency_fix::FixActivityMachineVector(
                  vector.data(), vector.data() + vector.size(),
                  vector.data() + vector.size(), pageCount) == FixResult::Patched);
        std::size_t changed = 0;
        for (std::size_t i = 0; i < vector.size(); ++i)
            changed += vector[i] != before[i] ? 1U : 0U;
        CHECK(changed == 1);
        CHECK(vector[capsule_currency_fix::kCurrencyTypeOffset] == 2);
        return true;
    }

    bool AlreadyCorrectIsIdempotent()
    {
        Page page = {};
        SetRecord(page, 3, 9301, 2, 99);
        const Page before = page;
        CHECK(Fix(page) == FixResult::AlreadyCorrect);
        CHECK(page == before);
        return true;
    }

    bool WrongPriceFailsClosed()
    {
        Page page = {};
        SetRecord(page, 3, 9301, 0, 100);
        const Page before = page;
        CHECK(Fix(page) == FixResult::UnexpectedPrice);
        CHECK(page == before);
        return true;
    }

    bool UnknownCurrencyFailsClosed()
    {
        Page page = {};
        SetRecord(page, 3, 9301, 1, 99);
        const Page before = page;
        CHECK(Fix(page) == FixResult::UnexpectedCurrency);
        CHECK(page == before);
        SetRecord(page, 3, 9301, 7, 99);
        const Page secondBefore = page;
        CHECK(Fix(page) == FixResult::UnexpectedCurrency);
        CHECK(page == secondBefore);
        SetRecord(page, 3, 9301, 0, 99);
        Record(page, 3)[capsule_currency_fix::kCurrencyTypeOffset + 1] = 1;
        const Page malformed = page;
        CHECK(Fix(page) == FixResult::UnexpectedCurrency);
        CHECK(page == malformed);
        return true;
    }

    bool DuplicateTargetFailsClosed()
    {
        Page page = {};
        SetRecord(page, 2, 9301, 0, 99);
        SetRecord(page, 6, 9301, 0, 99);
        const Page before = page;
        CHECK(Fix(page) == FixResult::DuplicateTarget);
        CHECK(page == before);
        return true;
    }

    bool MissingTargetDoesNothing()
    {
        Page page = {};
        SetRecord(page, 0, 9201, 0, 10000);
        SetRecord(page, 1, 9101, 2, 3);
        const Page before = page;
        CHECK(Fix(page) == FixResult::TargetMissing);
        CHECK(page == before);
        return true;
    }

    bool InvalidCountShapeFailsClosed()
    {
        Page page = {};
        SetRecord(page, 1, 9301, 0, 99);
        const Page before = page;

        CHECK(capsule_currency_fix::FixActivityMachineVector(
                  page.data(), page.data() + page.size() - 1, page.data() + page.size(), 1) ==
              FixResult::InvalidShape);
        CHECK(page == before);
        CHECK(capsule_currency_fix::FixActivityMachineVector(
                  page.data(), page.data() + page.size(), page.data() + page.size(), 2) ==
              FixResult::InvalidShape);
        CHECK(page == before);
        CHECK(capsule_currency_fix::FixActivityMachineVector(
                  page.data(), page.data() + page.size(), page.data() + page.size() - 1, 1) ==
              FixResult::InvalidShape);
        CHECK(page == before);
        return true;
    }
}

int main()
{
    const bool ok = PatchesOnlyExpectedByte() && StockPaddedCountShapeIsAccepted() &&
                    AlreadyCorrectIsIdempotent() && WrongPriceFailsClosed() &&
                    UnknownCurrencyFailsClosed() && DuplicateTargetFailsClosed() &&
                    MissingTargetDoesNothing() && InvalidCountShapeFailsClosed();
    if (!ok)
        return 1;
    std::puts("capsule_currency_fix_test: all checks passed");
    return 0;
}
