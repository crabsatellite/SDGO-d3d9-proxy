// Read the loaded daily_rewards module's startup schedule once. The new reward
// calculation consumes the event factor; it never multiplies the old G amount.
#pragma once
#include <windows.h>
#include <initializer_list>
#include <string>

namespace sdgo::battle_rewards {
struct DailySchedule {
    bool module_loaded = false;
    unsigned start = 18 * 60;
    unsigned end = 22 * 60;

    static bool Parse(const wchar_t* text, unsigned* minute) {
        if (!text || wcslen(text) != 5 || text[2] != L':') return false;
        for (unsigned i : {0u, 1u, 3u, 4u})
            if (text[i] < L'0' || text[i] > L'9') return false;
        unsigned hour = (text[0] - L'0') * 10 + text[1] - L'0';
        unsigned within = (text[3] - L'0') * 10 + text[4] - L'0';
        if (hour > 23 || within > 59) return false;
        *minute = hour * 60 + within;
        return true;
    }

    unsigned Factor(unsigned minute) const {
        if (!module_loaded || minute >= 1440 || start >= 1440 || end >= 1440 || start == end) return 1;
        bool active = start < end ? minute >= start && minute < end : minute >= start || minute < end;
        return active ? 2u : 1u;
    }

    bool Load(HMODULE daily) {
        module_loaded = false;
        if (!daily) return true;
        wchar_t path[32768] = {};
        DWORD n = GetModuleFileNameW(daily, path, 32768);
        if (!n || n == 32768) return false;
        std::wstring directory(path, n);
        directory.resize(directory.find_last_of(L"\\/"));
        std::wstring config = directory + L"\\config.ini";
        if (GetFileAttributesW(config.c_str()) == INVALID_FILE_ATTRIBUTES)
            config = directory + L"\\mod.ini";
        wchar_t from[16] = {}, to[16] = {};
        DWORD fromLength = GetPrivateProfileStringW(L"settings", L"StartTime", L"18:00", from, 16, config.c_str());
        DWORD toLength = GetPrivateProfileStringW(L"settings", L"EndTime", L"22:00", to, 16, config.c_str());
        if (fromLength >= 15 || toLength >= 15 || !Parse(from, &start) || !Parse(to, &end) || start == end)
            return false;
        module_loaded = true;
        return true;
    }
};
}
