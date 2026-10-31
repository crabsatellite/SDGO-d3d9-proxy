// startup_crash_fix - random crash at game start (gate loading screen) of this GOnline.exe build.
//
// While the game starts (CGameApp::SetState, SERVICE_LOGO) the gate loading screen
// (0x0090C320) picks a loading set at random (0x00904180: grid data type 5, one of the ids
// from 27102 on, as many as there are sets with a unit; srand(timeGetTime())) and keeps the
// picked id at +0x20E0. It then walks the loading sets from 27101 and shows each set without a
// unit and the picked set (gate.show_set_unit, 0x008FE380). For a set with a unit, that
// function looks the unit up in the unit data (grid data type 2, 0x0059D2B0), creates the
// unit's model at +0x207C, sets it up from the rest of the set, and reads the unit record's
// name at +0x264 without checking that the lookup found it. A set whose unit is not in this
// client's data therefore crashes the game now and then (access violation at 0x008FE610, read
// of address 0x264), whenever the random pick lands on it.
//
// The screen cannot do without the model: later (0x008FEE30, from 0x0090CE90, reached when
// logging in) it plays an animation of the set whose id is at +0x20E0 on the model at +0x207C,
// without checking either. So a set whose unit is missing is replaced by another set, as if
// the random pick had landed on that one: the nearest set before it whose unit is known, else
// the nearest one after it. The replacement's id goes to +0x20E0. A replacement before it has
// already been passed by the walk and is shown at once; one after it is shown by the walk when
// it gets there. Either way the screen shows exactly one unit, set up from that set. The first
// time, the loading sets whose unit is missing are logged.

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
    constexpr uint32_t kMaxLoadingSets = 4096;
    constexpr size_t kSetId = 0x00;
    constexpr size_t kSetUnit = 0x05;            // u32, 0: no unit
    constexpr size_t kScreenPickedSet = 0x20E0;  // u32, id of the loading set whose unit the screen shows

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    ShowSetUnitFn g_original = nullptr;
    GridDataFn g_gridData = nullptr;
    GridFindFn g_gridFind = nullptr;
    bool g_listed = false;

    uint32_t SetId(const uint8_t* set)
    {
        return *reinterpret_cast<const uint32_t*>(set + kSetId);
    }

    uint32_t SetUnit(const uint8_t* set)
    {
        return *reinterpret_cast<const uint32_t*>(set + kSetUnit);
    }

    bool UnitKnown(uint32_t unit)
    {
        const uint8_t* record = nullptr;
        return g_gridFind(g_gridData(), &record, unit, kTypeUnit, 0) != 0 && record != nullptr;
    }

    // nullptr past the last loading set: 0x00904180 stops at the first id without data.
    const uint8_t* FindSet(uint32_t id)
    {
        const uint8_t* set = nullptr;
        return g_gridFind(g_gridData(), &set, id, kTypeLoadingSet, 0) != 0 ? set : nullptr;
    }

    // The set has a unit, but not one in this client's unit data.
    bool UnitMissing(const uint8_t* set)
    {
        return SetUnit(set) != 0 && !UnitKnown(SetUnit(set));
    }

    // The loading sets in the order 0x00904180 walks them, with the ones whose unit is missing.
    void ListBrokenSets()
    {
        int sets = 0;
        int broken = 0;
        for (uint32_t id = kFirstLoadingSet; id < kFirstLoadingSet + kMaxLoadingSets; ++id) {
            const uint8_t* set = FindSet(id);
            if (!set)
                break;
            ++sets;
            if (UnitMissing(set)) {
                ++broken;
                g_api->Log(g_mod, SDGO_LOG_WARN, "loading set %lu: unit %lu is not in this client's unit data",
                           static_cast<unsigned long>(id), static_cast<unsigned long>(SetUnit(set)));
            }
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "%d loading sets, %d with a missing unit", sets, broken);
    }

    // The nearest set before the given one whose unit is known, else the nearest one after it.
    const uint8_t* Replacement(uint32_t id)
    {
        auto shows = [](const uint8_t* set) { return SetUnit(set) != 0 && !UnitMissing(set); };
        for (uint32_t other = id; other-- > kFirstLoadingSet;) {
            const uint8_t* set = FindSet(other);
            if (set && shows(set))
                return set;
        }
        for (uint32_t other = id + 1; other < kFirstLoadingSet + kMaxLoadingSets; ++other) {
            const uint8_t* set = FindSet(other);
            if (!set)
                break;
            if (shows(set))
                return set;
        }
        return nullptr;
    }

    // The set the game's function is called with: the set itself, or for a set whose unit is
    // missing its replacement; nullptr when the walk shows the replacement later.
    const uint8_t* Choose(uint8_t* screen, const uint8_t* set)
    {
        __try {
            if (!g_listed) {
                g_listed = true;
                ListBrokenSets();
            }
            if (!UnitMissing(set))
                return set;
            const uint8_t* other = Replacement(SetId(set));
            if (!other) {
                g_api->Log(g_mod, SDGO_LOG_WARN,
                           "loading set %lu picked: its unit %lu is missing and no set has a known unit; shown as the game would",
                           static_cast<unsigned long>(SetId(set)), static_cast<unsigned long>(SetUnit(set)));
                return set;
            }
            g_api->Log(g_mod, SDGO_LOG_INFO, "loading set %lu picked: its unit %lu is missing, set %lu (unit %lu) shown instead",
                       static_cast<unsigned long>(SetId(set)), static_cast<unsigned long>(SetUnit(set)),
                       static_cast<unsigned long>(SetId(other)), static_cast<unsigned long>(SetUnit(other)));
            const bool passed = SetId(other) < SetId(set);
            *reinterpret_cast<uint32_t*>(screen + kScreenPickedSet) = SetId(other);
            return passed ? other : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "exception while checking the loading set; shown as the game would");
            return set;
        }
    }

    void __fastcall ShowSetUnit(void* screen, void* /*edx*/, int arg, const uint8_t* set)
    {
        if (const uint8_t* shown = Choose(static_cast<uint8_t*>(screen), set))
            g_original(screen, arg, shown);
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
