// startup_crash_fix - random crash at game start (gate loading screen) of this GOnline.exe build.
//
// While the game starts (CGameApp::SetState, SERVICE_LOGO) the gate loading screen
// (0x0090C320) picks one of the loading sets at random (0x00904180: grid data type 5, ids
// from 27101, srand(timeGetTime())) and shows the set's unit (gate.show_set_unit,
// 0x008FE380). That function returns at once for a set without a unit (unit id 0); otherwise
// it looks the unit up in the unit data (grid data type 2, 0x0059D2B0) and reads the unit
// record's name at +0x264 without checking that the lookup found it. A set whose unit is not
// in this client's data therefore crashes the game now and then (access violation at
// 0x008FE610, read of address 0x264), whenever the random pick lands on it.
//
// A set whose unit is missing is treated like a set without a unit: the screen shows no
// unit. Before that early return the function only looks up the screen's "unit_pos" node and
// keeps it at +0x2084, which only the same function reads, so returning before it changes
// nothing else. The first time, the loading sets whose unit is missing are logged.

#include <windows.h>

#include <cstdint>

#include "sdgo/sdgo_api.h"

namespace
{
    using ShowSetUnitFn = void(__thiscall*)(void* screen, int arg, const uint8_t* set);
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn = int(__thiscall*)(void* grid, const uint8_t** record, uint32_t id, int type, char log);

    constexpr int kTypeUnit = 2;
    constexpr int kTypeLoadingSet = 5;
    constexpr uint32_t kFirstLoadingSet = 27101; // 0x69DD, where 0x00904180 starts counting
    constexpr size_t kSetId = 0x00;
    constexpr size_t kSetUnit = 0x05;            // u32, 0: no unit

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    ShowSetUnitFn g_original = nullptr;
    GridDataFn g_gridData = nullptr;
    GridFindFn g_gridFind = nullptr;
    bool g_listed = false;

    bool UnitKnown(uint32_t unit)
    {
        const uint8_t* record = nullptr;
        return g_gridFind(g_gridData(), &record, unit, kTypeUnit, 0) != 0 && record != nullptr;
    }

    // The loading sets in the order 0x00904180 walks them, with the ones whose unit is missing.
    void ListBrokenSets()
    {
        int sets = 0;
        int broken = 0;
        for (uint32_t id = kFirstLoadingSet; id < kFirstLoadingSet + 4096; ++id) {
            const uint8_t* set = nullptr;
            if (!g_gridFind(g_gridData(), &set, id, kTypeLoadingSet, 0) || !set)
                break;
            ++sets;
            uint32_t unit = *reinterpret_cast<const uint32_t*>(set + kSetUnit);
            if (unit != 0 && !UnitKnown(unit)) {
                ++broken;
                g_api->Log(g_mod, SDGO_LOG_WARN, "loading set %lu: unit %lu is not in this client's unit data",
                           static_cast<unsigned long>(id), static_cast<unsigned long>(unit));
            }
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "%d loading sets, %d with a missing unit", sets, broken);
    }

    // 0: show it, 1: unit missing, -1: exception while checking.
    int CheckSet(const uint8_t* set)
    {
        __try {
            if (!g_listed) {
                g_listed = true;
                ListBrokenSets();
            }
            if (!set)
                return 0;
            uint32_t unit = *reinterpret_cast<const uint32_t*>(set + kSetUnit);
            return unit == 0 || UnitKnown(unit) ? 0 : 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return -1;
        }
    }

    void __fastcall ShowSetUnit(void* screen, void* /*edx*/, int arg, const uint8_t* set)
    {
        int check = CheckSet(set);
        if (check == 1) {
            g_api->Log(g_mod, SDGO_LOG_INFO, "loading set %lu picked: its unit %lu is missing, no unit shown",
                       static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(set + kSetId)),
                       static_cast<unsigned long>(*reinterpret_cast<const uint32_t*>(set + kSetUnit)));
            return;
        }
        if (check < 0)
            g_api->Log(g_mod, SDGO_LOG_WARN, "exception while checking the loading set; shown as the game would");
        g_original(screen, arg, set);
    }

    template <typename Fn>
    bool Bind(Fn& out, const char* symbol)
    {
        out = reinterpret_cast<Fn>(g_api->Symbol(g_mod, symbol));
        return out != nullptr;
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    if (!Bind(g_gridData, "grid.data") || !Bind(g_gridFind, "grid.find"))
        return 0;
    return api->HookCode(mod, "gate.show_set_unit", reinterpret_cast<void*>(&ShowSetUnit),
                         reinterpret_cast<void**>(&g_original));
}
