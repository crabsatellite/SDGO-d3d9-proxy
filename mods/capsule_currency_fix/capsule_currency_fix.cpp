#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

#include "capsule_currency_logic.h"
#include "sdgo/sdgo_api.h"
#include "sdgo/game_test_page_observer.h"
#include "sdgo/game_test_capsule_observer.h"

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
    SdgoCapsuleGameTestSnapshotV1 g_testSnapshot{};

    uint64_t TestRunHash() {
        char run[64]{},role[16]{};
        const auto n=GetEnvironmentVariableA("SDGO_GAME_TEST_RUN",run,sizeof(run));
        const auto r=GetEnvironmentVariableA("SDGO_GAME_TEST_ROLE",role,sizeof(role));
        return n<sizeof(run) && r<sizeof(role)?sdgo::game_test_page_observer::ClientRunHash(run,n,role,r):0;
    }

    void ObserveLoad(void* ui,capsule_currency_fix::FixResult result) {
        const auto hash=TestRunHash();if(!hash)return;
        const auto generation=g_testSnapshot.run_hash==hash?g_testSnapshot.generation+1:1;
        g_testSnapshot={};g_testSnapshot.size=sizeof(g_testSnapshot);g_testSnapshot.version=1;
        g_testSnapshot.run_hash=hash;g_testSnapshot.process_id=GetCurrentProcessId();
        g_testSnapshot.ui_thread_id=GetCurrentThreadId();g_testSnapshot.generation=generation;
        g_testSnapshot.result=static_cast<uint32_t>(result);
        __try {
            if(!ui)return;
            auto* object=static_cast<uint8_t*>(ui);
            auto* first=*reinterpret_cast<uint8_t**>(object+kVectorBeginOffset);
            auto* end=*reinterpret_cast<uint8_t**>(object+kVectorEndOffset);
            auto* capacity=*reinterpret_cast<uint8_t**>(object+kVectorCapacityOffset);
            uint8_t* target=nullptr;
            if(capsule_currency_fix::InspectActivityMachineVector(first,end,capacity,
                *reinterpret_cast<uint32_t*>(object+kPageCountOffset),&target)!=capsule_currency_fix::FixResult::AlreadyCorrect || !target)return;
            memcpy(&g_testSnapshot.grid_id,target,4);memcpy(&g_testSnapshot.machine_id,target+5,4);
            memcpy(&g_testSnapshot.price,target+15,4);g_testSnapshot.currency=target[9];
            void* registry=nullptr;
            if(g_gridFind(g_gridData(),&registry,static_cast<int>(g_testSnapshot.grid_id),0x3C,0) && registry)
                g_testSnapshot.registry_currency=static_cast<uint8_t*>(registry)[9];
        }__except(EXCEPTION_EXECUTE_HANDLER){}
    }

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
        ObserveLoad(ui,result);
    }
}

extern "C" __declspec(dllexport) int __cdecl SdgoCapsuleGameTestGetSnapshotV1(SdgoCapsuleGameTestSnapshotV1* output) {
    if(!output || output->size<sizeof(*output) || output->version!=1)return 0;
    const auto hash=TestRunHash();
    if(!hash)return 0;
    if(g_testSnapshot.generation && (g_testSnapshot.run_hash!=hash || g_testSnapshot.ui_thread_id!=GetCurrentThreadId()))return 0;
    auto value=g_testSnapshot;value.size=sizeof(value);value.version=1;value.run_hash=hash;value.process_id=GetCurrentProcessId();
    __try {*output=value;return 1;}__except(EXCEPTION_EXECUTE_HANDLER){return 0;}
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
