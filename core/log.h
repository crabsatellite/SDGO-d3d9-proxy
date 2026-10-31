#pragma once

#include <cstdarg>
#include <string>

namespace sdgo::log
{
    // Opens <dir>\sdgo.log for this session; the previous session is kept as sdgo.prev.log.
    void Open(const std::wstring& dir, int minLevel);
    void Write(const char* source, int level, const char* fmt, va_list args);
    void Writef(const char* source, int level, const char* fmt, ...);
}
