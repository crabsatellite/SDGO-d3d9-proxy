#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace capsule_price_probe
{
    constexpr std::size_t kSlotsPerPage = 8;
    constexpr std::size_t kRecordSize = 0x3FB;
    constexpr std::size_t kMaxRecords = 4096;
    constexpr std::size_t kMaxEvents = 64;
    constexpr std::size_t kMaxPriceTextLength = 32;
    constexpr std::uint32_t kWstringInlineCapacity = 7;
    constexpr std::uint32_t kMaxWstringCapacity = 4096;

    enum class WindowStatus
    {
        Valid,
        NullBegin,
        ReversedPointers,
        MisalignedSpan,
        TooManyRecords,
        InvalidPageCount,
        PageOutOfRange,
        CountPageMismatch
    };

    struct PageWindow
    {
        std::size_t firstRecord = 0;
        std::size_t recordCount = 0;
    };

    inline WindowStatus ResolvePageWindow(std::uintptr_t begin, std::uintptr_t end,
                                          std::uintptr_t capacity, std::uint32_t page,
                                          std::uint32_t pageCount, PageWindow* output)
    {
        if (output)
            *output = {};
        if (begin == 0)
            return WindowStatus::NullBegin;
        if (begin > end || end > capacity)
            return WindowStatus::ReversedPointers;

        const std::uintptr_t byteCount = end - begin;
        if (byteCount % kRecordSize != 0)
            return WindowStatus::MisalignedSpan;
        const std::size_t recordCount = static_cast<std::size_t>(byteCount / kRecordSize);
        if (recordCount > kMaxRecords)
            return WindowStatus::TooManyRecords;
        if (pageCount == 0)
            return WindowStatus::InvalidPageCount;
        const std::size_t pageCountValue = static_cast<std::size_t>(pageCount);
        if (pageCountValue >
            (std::numeric_limits<std::size_t>::max)() / kSlotsPerPage)
            return WindowStatus::InvalidPageCount;
        if (page >= pageCount)
            return WindowStatus::PageOutOfRange;

        const std::size_t expectedRecords =
            pageCountValue * kSlotsPerPage;
        if (recordCount != expectedRecords)
            return WindowStatus::CountPageMismatch;

        const std::size_t firstRecord = static_cast<std::size_t>(page) * kSlotsPerPage;
        if (firstRecord > recordCount || recordCount - firstRecord < kSlotsPerPage)
            return WindowStatus::PageOutOfRange;
        if (output) {
            output->firstRecord = firstRecord;
            output->recordCount = recordCount;
        }
        return WindowStatus::Valid;
    }

    inline const char* WindowStatusName(WindowStatus status)
    {
        switch (status) {
        case WindowStatus::Valid:
            return "valid";
        case WindowStatus::NullBegin:
            return "null-begin";
        case WindowStatus::ReversedPointers:
            return "reversed-pointers";
        case WindowStatus::MisalignedSpan:
            return "misaligned-span";
        case WindowStatus::TooManyRecords:
            return "too-many-records";
        case WindowStatus::InvalidPageCount:
            return "invalid-page-count";
        case WindowStatus::PageOutOfRange:
            return "page-out-of-range";
        case WindowStatus::CountPageMismatch:
            return "count-page-mismatch";
        }
        return "unknown";
    }

    inline bool IsAllowedPriceText(const wchar_t* text, std::size_t length)
    {
        if (!text || length == 0 || length > kMaxPriceTextLength)
            return false;

        bool hasDigit = false;
        for (std::size_t i = 0; i < length; ++i) {
            const wchar_t value = text[i];
            if (value >= L'0' && value <= L'9') {
                hasDigit = true;
                continue;
            }
            if (value == L',' || value == L' ' || value == L'\t' || value == L'\r' ||
                value == L'\n' || value == L'G' || value == L'g' || value == L'B' ||
                value == L'b' || value == L'P' || value == L'p')
                continue;
            return false;
        }
        return hasDigit;
    }

    inline bool ResolveWstringStorage(std::uint32_t length, std::uint32_t capacity,
                                      bool* inlineStorage)
    {
        if (inlineStorage)
            *inlineStorage = false;
        if (length == 0 || length > kMaxPriceTextLength || length > capacity)
            return false;
        if (capacity == kWstringInlineCapacity) {
            if (inlineStorage)
                *inlineStorage = true;
            return true;
        }
        return capacity > kWstringInlineCapacity &&
               capacity <= kMaxWstringCapacity;
    }

    class EventGate
    {
    public:
        bool ShouldLog(std::uintptr_t ui, std::uint32_t page)
        {
            if (events_ >= kMaxEvents)
                return false;
            if (hasLast_ && lastUi_ == ui && lastPage_ == page)
                return false;
            hasLast_ = true;
            lastUi_ = ui;
            lastPage_ = page;
            ++events_;
            return true;
        }

        std::size_t events() const
        {
            return events_;
        }

    private:
        bool hasLast_ = false;
        std::uintptr_t lastUi_ = 0;
        std::uint32_t lastPage_ = 0;
        std::size_t events_ = 0;
    };
}
