#pragma once
#include <string>

namespace sdgo::shop_catalog {
using LogFn = void(*)(bool error, const std::wstring& message);
bool Apply(const std::wstring& hostIni, const std::wstring& catalogIni,
           const std::wstring& outputDirectory, bool disable, LogFn log);
}
