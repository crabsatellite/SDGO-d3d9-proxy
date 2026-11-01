// Test-only mod that owns the host's sendto import before net_route loads.
// net_route must decline after its second HookImport and the core must roll its
// already-installed connect hook back.
#include <winsock2.h>
#include <windows.h>

#include <cstddef>

#include "sdgo/sdgo_api.h"

namespace
{
    using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
    SendToFn g_original = nullptr;

    int WSAAPI SendTo(SOCKET socket, const char* data, int size, int flags,
                      const sockaddr* destination, int length)
    {
        return g_original(socket, data, size, flags, destination, length);
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookImport) + sizeof(api->HookImport) || !api->HookImport)
        return 0;
    if (!api->HookImport(mod, "ws2_32.dll", "sendto", reinterpret_cast<void*>(&SendTo),
                         reinterpret_cast<void**>(&g_original)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "test conflict owns ws2_32.dll!sendto");
    return 1;
}
