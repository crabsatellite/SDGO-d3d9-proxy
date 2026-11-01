#include <windows.h>
#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

#include "capsule_price_probe_logic.h"
#include "sdgo/sdgo_api.h"

namespace
{
    using RefreshSlotGridFn = void(__thiscall*)(void*, int);
    using SetStaticTextFn = void(__thiscall*)(void*, const void*);

    constexpr std::size_t kVectorBeginOffset = 0x64;
    constexpr std::size_t kVectorEndOffset = 0x68;
    constexpr std::size_t kVectorCapacityOffset = 0x6C;
    constexpr std::size_t kCurrentPageOffset = 0x78;
    constexpr std::size_t kPageCountOffset = 0x7C;
    constexpr std::size_t kPayTypeControlArrayOffset = 0x160;
    constexpr std::size_t kPriceControlArrayOffset = 0x180;
    constexpr std::size_t kControlVisibleOffset = 0x162;
    constexpr std::size_t kGridIdOffset = 0;
    constexpr std::size_t kMachineIdOffset = 5;
    constexpr std::size_t kGpPriceOffset = 15;
    constexpr std::uintptr_t kPriceSetterReturnRva = 0x0039EA54;
    constexpr std::uintptr_t kStaticTextVtableRva = 0x00AF3FFC;

    struct NativeWstring32
    {
        std::uint8_t storage[16];
        std::uint32_t length;
        std::uint32_t capacity;
    };

    struct CapturedPrice
    {
        bool valid = false;
        char text[capsule_price_probe::kMaxPriceTextLength + 1] = {};
    };

    struct RefreshContext
    {
        bool eventReserved = false;
        bool captureEnabled = false;
        std::uint32_t page = 0;
        std::size_t eventNumber = 0;
        void* priceControls[capsule_price_probe::kSlotsPerPage] = {};
        CapturedPrice prices[capsule_price_probe::kSlotsPerPage] = {};
    };

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    RefreshSlotGridFn g_original = nullptr;
    SetStaticTextFn g_originalSetStaticText = nullptr;
    capsule_price_probe::EventGate g_gate;
    DWORD g_contextTls = TLS_OUT_OF_INDEXES;
    std::uintptr_t g_hostBase = 0;

    bool CheckedAdd(std::uintptr_t base, std::size_t offset, std::uintptr_t* result)
    {
        if (!result ||
            base > (std::numeric_limits<std::uintptr_t>::max)() - offset)
            return false;
        *result = base + offset;
        return true;
    }

    bool ReadableRange(const void* address, std::size_t size)
    {
        if (!address || size == 0)
            return false;

        std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(address);
        if (cursor > (std::numeric_limits<std::uintptr_t>::max)() - size)
            return false;
        const std::uintptr_t end = cursor + size;

        while (cursor < end) {
            MEMORY_BASIC_INFORMATION info = {};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) ||
                info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
                return false;

            const DWORD protection = info.Protect & 0xFF;
            if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
                protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
                protection != PAGE_EXECUTE_READWRITE &&
                protection != PAGE_EXECUTE_WRITECOPY)
                return false;

            const std::uintptr_t regionBase =
                reinterpret_cast<std::uintptr_t>(info.BaseAddress);
            if (regionBase >
                (std::numeric_limits<std::uintptr_t>::max)() - info.RegionSize)
                return false;
            const std::uintptr_t regionEnd = regionBase + info.RegionSize;
            if (regionEnd <= cursor)
                return false;
            cursor = regionEnd < end ? regionEnd : end;
        }
        return true;
    }

    template <typename T>
    bool SafeRead(const void* address, T* output)
    {
        static_assert(std::is_trivially_copyable<T>::value,
                      "probe reads only trivially copyable values");
        if (!output || !ReadableRange(address, sizeof(T)))
            return false;
        __try {
            std::memcpy(output, address, sizeof(T));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    template <typename T>
    bool SafeReadField(const void* object, std::size_t offset, T* output)
    {
        std::uintptr_t address = 0;
        return object &&
               CheckedAdd(reinterpret_cast<std::uintptr_t>(object), offset, &address) &&
               SafeRead(reinterpret_cast<const void*>(address), output);
    }

    bool IsExpectedStaticTextControl(void* control)
    {
        void* vtable = nullptr;
        return control && SafeReadField(control, 0, &vtable) &&
               reinterpret_cast<std::uintptr_t>(vtable) ==
                   g_hostBase + kStaticTextVtableRva;
    }

    void PrepareRefreshContext(void* ui, RefreshContext* context)
    {
        if (!context)
            return;
        for (std::size_t slot = 0;
             slot < capsule_price_probe::kSlotsPerPage; ++slot) {
            void* control = nullptr;
            if (SafeReadField(ui, kPriceControlArrayOffset + slot * sizeof(void*),
                              &control) &&
                IsExpectedStaticTextControl(control))
                context->priceControls[slot] = control;
        }
    }

    bool CopyAllowedPriceText(const void* textObject, CapturedPrice* output)
    {
        if (!output)
            return false;
        output->valid = false;
        output->text[0] = '\0';

        NativeWstring32 value = {};
        if (!SafeRead(textObject, &value))
            return false;

        bool inlineStorage = false;
        if (!capsule_price_probe::ResolveWstringStorage(
                value.length, value.capacity, &inlineStorage))
            return false;

        std::uintptr_t textAddress = reinterpret_cast<std::uintptr_t>(textObject);
        if (!inlineStorage) {
            std::uint32_t pointer32 = 0;
            std::memcpy(&pointer32, value.storage, sizeof(pointer32));
            textAddress = pointer32;
        }

        wchar_t text[capsule_price_probe::kMaxPriceTextLength + 1] = {};
        const std::size_t bytes =
            (static_cast<std::size_t>(value.length) + 1) * sizeof(wchar_t);
        if (!ReadableRange(reinterpret_cast<const void*>(textAddress), bytes))
            return false;
        __try {
            std::memcpy(text, reinterpret_cast<const void*>(textAddress), bytes);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (text[value.length] != L'\0' ||
            !capsule_price_probe::IsAllowedPriceText(text, value.length))
            return false;

        for (std::uint32_t i = 0; i < value.length; ++i)
            output->text[i] = static_cast<char>(text[i]);
        output->text[value.length] = '\0';
        output->valid = true;
        return true;
    }

    void CapturePriceText(void* control, const void* textObject,
                          const void* returnAddress)
    {
        if (g_contextTls == TLS_OUT_OF_INDEXES ||
            reinterpret_cast<std::uintptr_t>(returnAddress) !=
                g_hostBase + kPriceSetterReturnRva)
            return;

        auto* context =
            static_cast<RefreshContext*>(TlsGetValue(g_contextTls));
        if (!context)
            return;

        std::size_t matchedSlot = capsule_price_probe::kSlotsPerPage;
        for (std::size_t slot = 0;
             slot < capsule_price_probe::kSlotsPerPage; ++slot) {
            if (context->priceControls[slot] == control) {
                matchedSlot = slot;
                break;
            }
        }
        if (matchedSlot == capsule_price_probe::kSlotsPerPage ||
            !IsExpectedStaticTextControl(control))
            return;

        CopyAllowedPriceText(textObject, &context->prices[matchedSlot]);
    }

    void LogRejected(std::uintptr_t ui, int requestedPage, const char* reason)
    {
        g_api->Log(g_mod, SDGO_LOG_WARN,
                   "capsule-price-probe ui=%p requested_page=%d observation=unknown reason=%s",
                   reinterpret_cast<void*>(ui), requestedPage, reason);
    }

    void LogSlot(std::uintptr_t ui, std::uintptr_t begin, std::size_t recordIndex,
                 std::size_t slot, const CapturedPrice* capturedPrice)
    {
        std::uintptr_t recordOffset = 0;
        if (recordIndex >
            (std::numeric_limits<std::uintptr_t>::max)() /
                capsule_price_probe::kRecordSize) {
            g_api->Log(g_mod, SDGO_LOG_WARN,
                       "capsule-price-probe slot=%u record=unknown "
                       "point_control=unknown point_visible=unknown "
                       "pay_control=unknown pay_visible=unknown text=unknown",
                       static_cast<unsigned>(slot));
            return;
        }
        recordOffset =
            static_cast<std::uintptr_t>(recordIndex) * capsule_price_probe::kRecordSize;

        std::uintptr_t recordAddress = 0;
        std::uint32_t gridId = 0;
        std::uint32_t machineId = 0;
        std::uint32_t gpPrice = 0;
        const bool recordReadable =
            CheckedAdd(begin, static_cast<std::size_t>(recordOffset), &recordAddress) &&
            SafeReadField(reinterpret_cast<const void*>(recordAddress), kGridIdOffset,
                          &gridId) &&
            SafeReadField(reinterpret_cast<const void*>(recordAddress), kMachineIdOffset,
                          &machineId) &&
            SafeReadField(reinterpret_cast<const void*>(recordAddress), kGpPriceOffset,
                          &gpPrice);

        void* priceControl = nullptr;
        const bool priceControlFieldReadable =
            SafeReadField(reinterpret_cast<const void*>(ui),
                          kPriceControlArrayOffset + slot * sizeof(void*), &priceControl);
        std::uint8_t priceVisible = 0;
        const bool priceVisibilityReadable =
            priceControlFieldReadable && priceControl &&
            SafeReadField(priceControl, kControlVisibleOffset, &priceVisible) &&
            priceVisible <= 1;

        void* payControl = nullptr;
        const bool payControlFieldReadable =
            SafeReadField(reinterpret_cast<const void*>(ui),
                          kPayTypeControlArrayOffset + slot * sizeof(void*), &payControl);
        std::uint8_t payVisible = 0;
        const bool payVisibilityReadable =
            payControlFieldReadable && payControl &&
            SafeReadField(payControl, kControlVisibleOffset, &payVisible) &&
            payVisible <= 1;

        const char* actualText =
            capturedPrice && capturedPrice->valid ? capturedPrice->text : "unknown";

        if (!recordReadable) {
            g_api->Log(g_mod, SDGO_LOG_WARN,
                       "capsule-price-probe slot=%u record=unknown "
                       "point_control=%p point_field=%s point_visible=%s "
                       "pay_control=%p pay_field=%s pay_visible=%s text=%s",
                       static_cast<unsigned>(slot), priceControl,
                       priceControlFieldReadable ? "readable" : "unknown",
                       priceVisibilityReadable ? (priceVisible ? "1" : "0") : "unknown",
                       payControl, payControlFieldReadable ? "readable" : "unknown",
                       payVisibilityReadable ? (payVisible ? "1" : "0") : "unknown",
                       actualText);
            return;
        }

        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "capsule-price-probe slot=%u grid=%u machine=%u gp=%u "
                   "point_control=%p point_field=%s point_visible=%s "
                   "pay_control=%p pay_field=%s pay_visible=%s text=%s",
                   static_cast<unsigned>(slot), gridId, machineId, gpPrice, priceControl,
                   priceControlFieldReadable ? "readable" : "unknown",
                   priceVisibilityReadable ? (priceVisible ? "1" : "0") : "unknown",
                   payControl, payControlFieldReadable ? "readable" : "unknown",
                   payVisibilityReadable ? (payVisible ? "1" : "0") : "unknown",
                   actualText);
    }

    void ObserveSlotGrid(void* ui, int requestedPage,
                         const RefreshContext* context)
    {
        if (!context || !context->eventReserved)
            return;

        const std::uintptr_t uiAddress = reinterpret_cast<std::uintptr_t>(ui);
        std::uint32_t currentPage = 0;
        if (!SafeReadField(ui, kCurrentPageOffset, &currentPage)) {
            LogRejected(uiAddress, requestedPage, "current-page-unreadable");
            return;
        }
        if (currentPage != context->page) {
            LogRejected(uiAddress, requestedPage, "page-changed-during-refresh");
            return;
        }

        if (requestedPage < 0 ||
            static_cast<std::uint32_t>(requestedPage) != currentPage) {
            LogRejected(uiAddress, requestedPage, "page-argument-mismatch");
            return;
        }

        std::uint8_t* begin = nullptr;
        std::uint8_t* end = nullptr;
        std::uint8_t* capacity = nullptr;
        std::uint32_t pageCount = 0;
        if (!SafeReadField(ui, kVectorBeginOffset, &begin) ||
            !SafeReadField(ui, kVectorEndOffset, &end) ||
            !SafeReadField(ui, kVectorCapacityOffset, &capacity) ||
            !SafeReadField(ui, kPageCountOffset, &pageCount)) {
            LogRejected(uiAddress, requestedPage, "ui-fields-unreadable");
            return;
        }

        capsule_price_probe::PageWindow window = {};
        const capsule_price_probe::WindowStatus status =
            capsule_price_probe::ResolvePageWindow(
                reinterpret_cast<std::uintptr_t>(begin),
                reinterpret_cast<std::uintptr_t>(end),
                reinterpret_cast<std::uintptr_t>(capacity), currentPage, pageCount, &window);
        if (status != capsule_price_probe::WindowStatus::Valid) {
            LogRejected(uiAddress, requestedPage,
                        capsule_price_probe::WindowStatusName(status));
            return;
        }

        g_api->Log(g_mod, SDGO_LOG_INFO,
                   "capsule-price-probe event=%u ui=%p page_zero=%u page=%u/%u "
                   "slots=8 text_capture=guarded-static-text-setter",
                   static_cast<unsigned>(context->eventNumber), ui, currentPage,
                   currentPage + 1, pageCount);
        const std::uintptr_t beginAddress = reinterpret_cast<std::uintptr_t>(begin);
        for (std::size_t slot = 0;
             slot < capsule_price_probe::kSlotsPerPage; ++slot) {
            const CapturedPrice* capturedPrice =
                context ? &context->prices[slot] : nullptr;
            LogSlot(uiAddress, beginAddress, window.firstRecord + slot, slot,
                    capturedPrice);
        }
    }

    void __fastcall SetStaticTextDetour(void* control, void* /*edx*/,
                                        const void* textObject)
    {
        const void* returnAddress = _ReturnAddress();
        g_originalSetStaticText(control, textObject);
        const DWORD originalLastError = GetLastError();
        CapturePriceText(control, textObject, returnAddress);
        SetLastError(originalLastError);
    }

    void __fastcall RefreshSlotGridDetour(void* ui, void* /*edx*/, int page)
    {
        const DWORD incomingLastError = GetLastError();
        RefreshContext context = {};
        std::uint32_t pageBeforeRefresh = 0;
        if (SafeReadField(ui, kCurrentPageOffset, &pageBeforeRefresh)) {
            context.page = pageBeforeRefresh;
            context.eventReserved = g_gate.ShouldLog(
                reinterpret_cast<std::uintptr_t>(ui), pageBeforeRefresh);
            context.eventNumber = g_gate.events();
            context.captureEnabled =
                context.eventReserved && page >= 0 &&
                static_cast<std::uint32_t>(page) == pageBeforeRefresh;
        }
        if (context.captureEnabled)
            PrepareRefreshContext(ui, &context);

        void* previousContext = nullptr;
        bool contextInstalled = false;
        if (context.captureEnabled && g_contextTls != TLS_OUT_OF_INDEXES) {
            previousContext = TlsGetValue(g_contextTls);
            contextInstalled = TlsSetValue(g_contextTls, &context) != FALSE;
        }
        SetLastError(incomingLastError);
        g_original(ui, page);
        const DWORD originalLastError = GetLastError();
        if (contextInstalled)
            TlsSetValue(g_contextTls, previousContext);
        ObserveSlotGrid(ui, page, &context);
        SetLastError(originalLastError);
    }
}

SDGO_MOD_ENTRY
{
    static_assert(sizeof(void*) == 4,
                  "capsule_price_probe is only valid for the 32-bit GOnline host");
    static_assert(sizeof(wchar_t) == 2,
                  "capsule_price_probe requires Windows UTF-16 wchar_t");
    static_assert(sizeof(NativeWstring32) == 24,
                  "unexpected 32-bit MSVC std::wstring layout");
    g_api = api;
    g_mod = mod;
    HMODULE mainModule = GetModuleHandleW(nullptr);
    if (!mainModule || (api->host_base && api->host_base != mainModule))
        return 0;
    g_hostBase = reinterpret_cast<std::uintptr_t>(mainModule);
    g_contextTls = TlsAlloc();
    if (g_contextTls == TLS_OUT_OF_INDEXES)
        return 0;
    if (!api->HookCode(mod, "ui.static_text.set_text",
                       reinterpret_cast<void*>(&SetStaticTextDetour),
                       reinterpret_cast<void**>(&g_originalSetStaticText)))
        return 0;
    return api->HookCode(mod, "capsule.fixed.refresh_slot_grid",
                         reinterpret_cast<void*>(&RefreshSlotGridDetour),
                         reinterpret_cast<void**>(&g_original));
}
