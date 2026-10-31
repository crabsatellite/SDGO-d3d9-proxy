// Bound the native Agent's per-user UDP allocation to tunnelled local ports.
// Only the verified AddClientUDP bind call is translated; the native receive,
// authentication, address advertisement, and relay handlers remain in charge.
#include <winsock2.h>
#include <windows.h>
#include <intrin.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"

#pragma intrinsic(_ReturnAddress)

namespace {
    const SdgoApi* g_api;
    SdgoMod* g_mod;
    using BindFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    BindFn g_bind;
    uintptr_t g_returnAddress;
    unsigned g_first = 0, g_count = 0;
    LONG g_cursor = 0;
    LONG g_logged = 0;

    bool SettingNumber(const char* key, const char* def, unsigned maximum, unsigned* output)
    {
        char text[32] = {};
        uint32_t length = sdgo::ModSettings(g_api, g_mod).String(key, def, text, sizeof(text));
        if (!length || length >= sizeof(text) - 1) return false;
        unsigned value = 0;
        for (uint32_t i = 0; i < length; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            unsigned digit = static_cast<unsigned>(text[i] - '0');
            if (value > maximum / 10 || (value == maximum / 10 && digit > maximum % 10)) return false;
            value = value * 10 + digit;
        }
        *output = value;
        return true;
    }

    bool Address(const sockaddr* address, int length, sockaddr_in* copy)
    {
        if (!address || length != sizeof(*copy)) return false;
        __try {
            if (address->sa_family != AF_INET) return false;
            std::memcpy(copy, address, sizeof(*copy));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    int WSAAPI Bind(SOCKET socket, const sockaddr* address, int length)
    {
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_returnAddress)
            return g_bind(socket, address, length);
        const int incomingError = WSAGetLastError();
        sockaddr_in selected = {};
        int type = 0, typeLength = sizeof(type);
        if (!Address(address, length, &selected) ||
            getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &typeLength) != 0 || type != SOCK_DGRAM) {
            WSASetLastError(incomingError);
            return g_bind(socket, address, length);
        }
        const unsigned start = (static_cast<uint32_t>(InterlockedIncrement(&g_cursor)) - 1) % g_count;
        int error = WSAEADDRINUSE;
        for (unsigned offset = 0; offset < g_count; ++offset) {
            unsigned port = g_first + (start + offset) % g_count;
            selected.sin_port = htons(static_cast<u_short>(port));
            WSASetLastError(incomingError);
            int result = g_bind(socket, reinterpret_cast<const sockaddr*>(&selected), sizeof(selected));
            error = WSAGetLastError();
            if (result == 0) {
                if (InterlockedIncrement(&g_logged) <= 64)
                    g_api->Log(g_mod, SDGO_LOG_INFO, "native per-user UDP bound to pool port %u", port);
                WSASetLastError(error);
                return result;
            }
            if (error != WSAEADDRINUSE && error != WSAEACCES) break;
        }
        if (InterlockedIncrement(&g_logged) <= 64)
            g_api->Log(g_mod, SDGO_LOG_WARN, "no usable UDP pool port in %u..%u (Winsock %d)",
                       g_first, g_first + g_count - 1, error);
        WSASetLastError(error);
        return SOCKET_ERROR; // Never silently allocate an untunnelled fallback port.
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, ModDirectory) + sizeof(api->ModDirectory)) return 0;
    g_api = api;
    g_mod = mod;
    const char* relayFormat = static_cast<const char*>(api->Symbol(mod, "agent.relay_log_format"));
    if (!relayFormat || std::strcmp(relayFormat, "UDP_RELAY_DATA : SEND TO DATA.. to p:%d %u:%d\n")) {
        api->Log(mod, SDGO_LOG_ERROR, "need agent_relay_log_fix active before enabling the UDP pool");
        return 0;
    }
    if (!SettingNumber("FirstPort", "10001", 65535, &g_first) || g_first < 1024 ||
        !SettingNumber("PortCount", "16", 31, &g_count) || !g_count || g_first + g_count - 1 > 65535) {
        api->Log(mod, SDGO_LOG_ERROR, "need FirstPort=1024..65535, PortCount=1..31, ending at most 65535");
        return 0;
    }
    auto* site = static_cast<const uint8_t*>(api->Symbol(mod, "agent.udp_bind_call"));
    void* import = api->Symbol(mod, "agent.bind_import");
    if (!site || !import) return 0;
    uint32_t operand = 0;
    std::memcpy(&operand, site + 2, sizeof(operand));
    if (site[0] != 0xFF || site[1] != 0x15 || operand != reinterpret_cast<uintptr_t>(import)) {
        api->Log(mod, SDGO_LOG_ERROR, "UDP bind site does not call the expected import slot");
        return 0;
    }
    g_returnAddress = reinterpret_cast<uintptr_t>(site) + 6;
    if (!api->HookImport(mod, "ws2_32.dll", "bind", reinterpret_cast<void*>(&Bind), reinterpret_cast<void**>(&g_bind))) return 0;
    api->Log(mod, SDGO_LOG_INFO, "native per-user UDP pool %u..%u active; actual port is advertised by getsockname",
             g_first, g_first + g_count - 1);
    return 1;
}
