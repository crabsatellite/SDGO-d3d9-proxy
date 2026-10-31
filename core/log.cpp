#include "log.h"

#include <windows.h>

#include <cstdio>

#include "sdgo/sdgo_api.h"

namespace
{
    CRITICAL_SECTION g_lock;
    bool g_lockReady = false;
    FILE* g_file = nullptr;
    int g_minLevel = SDGO_LOG_INFO;

    const char* LevelName(int level)
    {
        switch (level) {
        case SDGO_LOG_DEBUG: return "debug";
        case SDGO_LOG_INFO: return "info";
        case SDGO_LOG_WARN: return "warn";
        default: return "error";
        }
    }
}

void sdgo::log::Open(const std::wstring& dir, int minLevel)
{
    if (!g_lockReady) {
        InitializeCriticalSection(&g_lock);
        g_lockReady = true;
    }
    g_minLevel = minLevel;
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring current = dir + L"\\sdgo.log";
    const std::wstring previous = dir + L"\\sdgo.prev.log";
    MoveFileExW(current.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING);
    g_file = _wfsopen(current.c_str(), L"w", _SH_DENYWR);
}

void sdgo::log::Write(const char* source, int level, const char* fmt, va_list args)
{
    if (!g_lockReady || level < g_minLevel)
        return;

    char message[2048];
    vsnprintf_s(message, sizeof(message), _TRUNCATE, fmt, args);

    SYSTEMTIME now = {};
    GetLocalTime(&now);

    EnterCriticalSection(&g_lock);
    if (g_file) {
        fprintf(g_file, "%02u:%02u:%02u.%03u [%s] %s: %s\n",
                now.wHour, now.wMinute, now.wSecond, now.wMilliseconds,
                LevelName(level), source ? source : "core", message);
        fflush(g_file);
    }
    LeaveCriticalSection(&g_lock);
}

void sdgo::log::Writef(const char* source, int level, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    Write(source, level, fmt, args);
    va_end(args);
}
