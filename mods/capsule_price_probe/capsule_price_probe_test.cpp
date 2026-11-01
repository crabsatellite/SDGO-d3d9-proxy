#include "capsule_price_probe_logic.h"

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
    bool Expect(bool condition, const char* test, int line)
    {
        if (condition)
            return true;
        std::fprintf(stderr, "FAIL %s:%d\n", test, line);
        return false;
    }

#define CHECK(condition)                         \
    do {                                         \
        if (!Expect((condition), __func__, __LINE__)) \
            return false;                        \
    } while (false)

    bool ResolvesValidLastPage()
    {
        constexpr std::uintptr_t begin = 0x100000;
        constexpr std::uint32_t pageCount = 13;
        constexpr std::size_t records =
            pageCount * capsule_price_probe::kSlotsPerPage;
        constexpr std::uintptr_t end =
            begin + records * capsule_price_probe::kRecordSize;
        capsule_price_probe::PageWindow window = {};
        CHECK(capsule_price_probe::ResolvePageWindow(
                  begin, end, end, 12, pageCount, &window) ==
              capsule_price_probe::WindowStatus::Valid);
        CHECK(window.firstRecord == 96);
        CHECK(window.recordCount == 104);
        return true;
    }

    bool RejectsUnsafeVectorShapes()
    {
        using capsule_price_probe::ResolvePageWindow;
        using capsule_price_probe::WindowStatus;
        capsule_price_probe::PageWindow window = {};
        constexpr std::uintptr_t begin = 0x200000;
        constexpr std::uintptr_t onePage =
            capsule_price_probe::kSlotsPerPage * capsule_price_probe::kRecordSize;

        CHECK(ResolvePageWindow(0, begin + onePage, begin + onePage, 0, 1, &window) ==
              WindowStatus::NullBegin);
        CHECK(ResolvePageWindow(begin + onePage, begin, begin + onePage, 0, 1, &window) ==
              WindowStatus::ReversedPointers);
        CHECK(ResolvePageWindow(begin, begin + onePage - 1, begin + onePage, 0, 1,
                                &window) == WindowStatus::MisalignedSpan);
        CHECK(ResolvePageWindow(begin, begin + onePage, begin + onePage, 1, 1, &window) ==
              WindowStatus::PageOutOfRange);
        CHECK(ResolvePageWindow(begin, begin + onePage, begin + onePage, 0, 2, &window) ==
              WindowStatus::CountPageMismatch);

        constexpr std::uintptr_t tooManyBytes =
            (capsule_price_probe::kMaxRecords + 1) *
            capsule_price_probe::kRecordSize;
        CHECK(ResolvePageWindow(begin, begin + tooManyBytes, begin + tooManyBytes, 0, 1,
                                &window) == WindowStatus::TooManyRecords);
        return true;
    }

    bool AllowsOnlyBoundedNumericPriceText()
    {
        const std::wstring gp = L"1,200 GP";
        const std::wstring gb = L" 99 GB ";
        const std::wstring digits = L"1200";
        const std::wstring account = L"Alex: 1200 GP";
        const std::wstring markup = L"<b>1200 GP</b>";
        const std::wstring noDigits = L"GP";
        const std::wstring embeddedNull(L"12\0GP", 5);

        CHECK(capsule_price_probe::IsAllowedPriceText(gp.data(), gp.size()));
        CHECK(capsule_price_probe::IsAllowedPriceText(gb.data(), gb.size()));
        CHECK(capsule_price_probe::IsAllowedPriceText(digits.data(), digits.size()));
        CHECK(!capsule_price_probe::IsAllowedPriceText(account.data(), account.size()));
        CHECK(!capsule_price_probe::IsAllowedPriceText(markup.data(), markup.size()));
        CHECK(!capsule_price_probe::IsAllowedPriceText(noDigits.data(), noDigits.size()));
        CHECK(!capsule_price_probe::IsAllowedPriceText(embeddedNull.data(),
                                                       embeddedNull.size()));
        CHECK(!capsule_price_probe::IsAllowedPriceText(nullptr, 1));
        CHECK(!capsule_price_probe::IsAllowedPriceText(gp.data(), 0));

        const std::wstring maxLength(capsule_price_probe::kMaxPriceTextLength, L'7');
        const std::wstring tooLong(capsule_price_probe::kMaxPriceTextLength + 1, L'7');
        CHECK(capsule_price_probe::IsAllowedPriceText(maxLength.data(),
                                                      maxLength.size()));
        CHECK(!capsule_price_probe::IsAllowedPriceText(tooLong.data(), tooLong.size()));
        return true;
    }

    bool AcceptsOnlyKnownWstringLayouts()
    {
        bool inlineStorage = false;
        CHECK(capsule_price_probe::ResolveWstringStorage(
            4, capsule_price_probe::kWstringInlineCapacity, &inlineStorage));
        CHECK(inlineStorage);
        CHECK(!capsule_price_probe::ResolveWstringStorage(
            0, capsule_price_probe::kWstringInlineCapacity, &inlineStorage));
        CHECK(!capsule_price_probe::ResolveWstringStorage(
            8, capsule_price_probe::kWstringInlineCapacity, &inlineStorage));
        CHECK(capsule_price_probe::ResolveWstringStorage(8, 15, &inlineStorage));
        CHECK(!inlineStorage);
        CHECK(!capsule_price_probe::ResolveWstringStorage(
            capsule_price_probe::kMaxPriceTextLength + 1, 63, &inlineStorage));
        CHECK(!capsule_price_probe::ResolveWstringStorage(
            4, capsule_price_probe::kMaxWstringCapacity + 1, &inlineStorage));
        CHECK(!capsule_price_probe::ResolveWstringStorage(4, 6, &inlineStorage));
        return true;
    }

    bool LogsOnlyChangesWithinBudget()
    {
        capsule_price_probe::EventGate gate;
        CHECK(gate.ShouldLog(0x1000, 0));
        CHECK(!gate.ShouldLog(0x1000, 0));
        CHECK(gate.ShouldLog(0x1000, 1));
        CHECK(gate.ShouldLog(0x2000, 1));
        CHECK(gate.events() == 3);

        for (std::size_t i = gate.events(); i < capsule_price_probe::kMaxEvents; ++i)
            CHECK(gate.ShouldLog(0x2000, static_cast<std::uint32_t>(i + 10)));
        CHECK(gate.events() == capsule_price_probe::kMaxEvents);
        CHECK(!gate.ShouldLog(0x3000, 0));
        CHECK(gate.events() == capsule_price_probe::kMaxEvents);
        return true;
    }
}

int main()
{
    const bool ok = ResolvesValidLastPage() && RejectsUnsafeVectorShapes() &&
                    AllowsOnlyBoundedNumericPriceText() &&
                    AcceptsOnlyKnownWstringLayouts() &&
                    LogsOnlyChangesWithinBudget();
    if (!ok)
        return 1;
    std::puts("capsule_price_probe_test: all checks passed");
    return 0;
}
