#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "capsule_currency_logic.h"
#include "sdgo/sdgo_api.h"

namespace
{
    using LoadActivityItemsFn = void(__thiscall*)(void*);
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn = bool(__thiscall*)(void*, void**, int, int, int);

    constexpr std::size_t kVectorBeginOffset = 0x1A4;
    constexpr std::size_t kVectorEndOffset = 0x1A8;
    constexpr std::size_t kVectorCapacityOffset = 0x1AC;
    constexpr std::size_t kPageCountOffset = 0x1DC;

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    LoadActivityItemsFn g_original = nullptr;
    GridDataFn g_gridData = nullptr;
    GridFindFn g_gridFind = nullptr;

    bool WritableRange(const void* address, std::size_t size)
    {
        if (!address || size == 0)
            return false;

        std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(address);
        if (size > static_cast<std::size_t>(capsule_currency_fix::kRecordSize *
                                            capsule_currency_fix::kMaxSlots))
            return false;
        if (cursor > (std::numeric_limits<std::uintptr_t>::max)() - size)
            return false;
        const std::uintptr_t end = cursor + size;

        while (cursor < end) {
            MEMORY_BASIC_INFORMATION info = {};
            if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) ||
                info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
                return false;

            const DWORD protection = info.Protect & 0xFF;
            if (protection != PAGE_READWRITE && protection != PAGE_WRITECOPY &&
                protection != PAGE_EXECUTE_READWRITE && protection != PAGE_EXECUTE_WRITECOPY)
                return false;

            const std::uintptr_t regionEnd =
                reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (regionEnd <= cursor)
                return false;
            cursor = regionEnd < end ? regionEnd : end;
        }
        return true;
    }

    void LogResult(capsule_currency_fix::FixResult result)
    {
        using capsule_currency_fix::FixResult;
        const int level =
            result == FixResult::Patched || result == FixResult::AlreadyCorrect ? SDGO_LOG_INFO : SDGO_LOG_WARN;
        if (result == FixResult::Patched) {
            g_api->Log(g_mod, level,
                       "activity machine 9301 display currency corrected from type 0 (G currency) "
                       "to type 2 (token) in the UI copy and verified native registry row; "
                       "confirmation source corrected, visual acceptance remains manual; disk GDT, price and server state unchanged");
            return;
        }
        if (result == FixResult::AlreadyCorrect) {
            g_api->Log(g_mod, level,
                       "activity machine 9301 display currency is already type 2 (token); no change made");
            return;
        }
        g_api->Log(g_mod, level,
                   "activity machine 9301 display fix refused (%s); UI copy and native registry left unchanged",
                   capsule_currency_fix::FixResultName(result));
    }

    void __fastcall LoadActivityItemsDetour(void* ui, void* /*edx*/)
    {
        g_original(ui);

        capsule_currency_fix::FixResult result = capsule_currency_fix::FixResult::InvalidArguments;
        __try {
            if (!ui) {
                LogResult(result);
                return;
            }

            auto* object = static_cast<std::uint8_t*>(ui);
            auto* begin = *reinterpret_cast<std::uint8_t**>(object + kVectorBeginOffset);
            auto* end = *reinterpret_cast<std::uint8_t**>(object + kVectorEndOffset);
            auto* capacity = *reinterpret_cast<std::uint8_t**>(object + kVectorCapacityOffset);
            const std::uint32_t pageCount =
                *reinterpret_cast<const std::uint32_t*>(object + kPageCountOffset);

            const auto beginAddress = reinterpret_cast<std::uintptr_t>(begin);
            const auto endAddress = reinterpret_cast<std::uintptr_t>(end);
            if (!begin || beginAddress > endAddress ||
                !WritableRange(begin, static_cast<std::size_t>(endAddress - beginAddress))) {
                result = capsule_currency_fix::FixResult::InvalidShape;
            } else {
                using namespace capsule_currency_fix;
                std::uint8_t* target = nullptr;
                result = InspectActivityMachineVector(begin, end, capacity, pageCount, &target);
                if (result == FixResult::NeedsCorrection || result == FixResult::AlreadyCorrect) {
                    std::uint32_t gridId = 0;
                    std::memcpy(&gridId, target, sizeof(gridId));
                    void* registry = nullptr;
                    void* grid = g_gridData();
                    if (gridId != kTargetGridId || !grid ||
                        !g_gridFind(grid, &registry, static_cast<int>(gridId), 0x3C, 0) ||
                        !WritableRange(registry, kRecordSize) ||
                        std::memcmp(target, registry, kRecordSize) != 0) {
                        result = FixResult::RegistryMismatch;
                    } else if (result == FixResult::NeedsCorrection) {
                        // Both consumers are client-side. The confirmation initializer
                        // re-enumerates GridData rather than reading the list copy.
                        static_cast<std::uint8_t*>(registry)[kCurrencyTypeOffset] = kTokenCurrencyType;
                        target[kCurrencyTypeOffset] = kTokenCurrencyType;
                        result = FixResult::Patched;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            result = capsule_currency_fix::FixResult::InvalidShape;
        }
        LogResult(result);
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION || !api->Symbol || !api->HookCode || !api->Log)
        return 0;
    g_api = api;
    g_mod = mod;
    g_gridData = reinterpret_cast<GridDataFn>(api->Symbol(mod, "grid.data"));
    g_gridFind = reinterpret_cast<GridFindFn>(api->Symbol(mod, "grid.find"));
    if (!g_gridData || !g_gridFind)
        return 0;
    return api->HookCode(mod, "capsule.activity.load_items",
                         reinterpret_cast<void*>(&LoadActivityItemsDetour),
                         reinterpret_cast<void**>(&g_original));
}
