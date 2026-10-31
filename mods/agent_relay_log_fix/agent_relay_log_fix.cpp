// The shipped Agent passes an integer IPv4 value to printf's %s at the end of
// relay forwarding. Correct only that verified format character before traffic.
#include <windows.h>
#include <cstring>
#include "sdgo/sdgo_api.h"

namespace {
    constexpr char kUnsafe[] = "UDP_RELAY_DATA : SEND TO DATA.. to p:%d %s:%d\n";
    constexpr char kSafe[] = "UDP_RELAY_DATA : SEND TO DATA.. to p:%d %u:%d\n";
}

SDGO_MOD_ENTRY
{
    auto* format = static_cast<char*>(api->Symbol(mod, "agent.relay_log_format"));
    if (!format) return 0;
    if (!std::strcmp(format, kSafe)) {
        api->Log(mod, SDGO_LOG_INFO, "native relay log already uses an integer IP specifier");
        return 1;
    }
    if (std::strcmp(format, kUnsafe)) {
        api->Log(mod, SDGO_LOG_ERROR, "native relay log format differs; no patch applied");
        return 0;
    }
    const size_t character = std::strstr(kUnsafe, "%s") - kUnsafe + 1;
    DWORD old = 0;
    if (!VirtualProtect(format + character, 1, PAGE_READWRITE, &old)) return 0;
    *reinterpret_cast<volatile char*>(format + character) = 'u';
    DWORD ignored = 0;
    if (!VirtualProtect(format + character, 1, old, &ignored)) {
        *reinterpret_cast<volatile char*>(format + character) = 's';
        VirtualProtect(format + character, 1, old, &ignored);
        api->Log(mod, SDGO_LOG_ERROR, "could not restore relay format protection; change rolled back");
        return 0;
    }
    api->Log(mod, SDGO_LOG_INFO, "corrected native relay printf IP from string pointer to unsigned integer");
    return 1;
}
