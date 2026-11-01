#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace sdgo {
// A truncated dependency list can cut a symbol in half or omit required
// symbols. Retry the actual Windows INI read until the complete value fits.
inline std::wstring ReadIniString(const std::wstring& path, const wchar_t* section,
    const wchar_t* key, const wchar_t* fallback)
{
    std::vector<wchar_t> buffer(2048);
    for (;;) {
        const DWORD length = GetPrivateProfileStringW(section, key, fallback,
            buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
        if (length < buffer.size() - 1)
            return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}
}
