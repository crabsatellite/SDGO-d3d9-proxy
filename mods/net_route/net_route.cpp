// Configured endpoint translation at the game's Winsock IAT boundary.
// Immutable routes, no packet payload changes, no DNS or allocation on the hot path.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <intrin.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "sdgo/sdgo_api.h"
#include "sdgo/mod_settings.h"

#pragma intrinsic(_ReturnAddress)

namespace
{
    constexpr size_t kMaxRoutes = 32;
    struct Route {
        int type = 0;
        sockaddr_in logical = {};
        sockaddr_in external = {};
        LONG sent = 0;
        LONG received = 0;
    };
    std::array<Route, kMaxRoutes> g_routes;
    size_t g_count = 0;
    bool g_trace = false;
    LONG g_unmapped = 0;
    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
    using RecvFromFn = int(WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
    ConnectFn g_connect = nullptr;
    SendToFn g_sendTo = nullptr;
    RecvFromFn g_recvFrom = nullptr;
    using ParseArgsFn = LPWSTR*(WINAPI*)(LPCWSTR, int*);
    ParseArgsFn g_parseArgs = nullptr;
    uintptr_t g_loginReturn = 0;
    std::wstring g_loginIP, g_loginPort;

    bool Same(const sockaddr_in& a, const sockaddr_in& b)
    {
        return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
    }

    // Preserve Winsock's handling of invalid pointers/lengths instead of faulting
    // in the translator. No C++ objects with destructors inside SEH helpers.
    bool ReadAddress(const sockaddr* input, int length, sockaddr_in* output)
    {
        if (!input || length < static_cast<int>(sizeof(*output))) return false;
        __try {
            if (input->sa_family != AF_INET) return false;
            std::memcpy(output, input, sizeof(*output));
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    bool ReadSource(sockaddr* source, int* length, sockaddr_in* output)
    {
        __try {
            return length && ReadAddress(source, *length, output);
        } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    void WriteSource(sockaddr* source, const sockaddr_in* logical)
    {
        __try { std::memcpy(source, logical, sizeof(*logical)); }
        __except (EXCEPTION_EXECUTE_HANDLER) { /* original API owns argument validity */ }
    }

    int SocketType(SOCKET socket)
    {
        int type = 0;
        int length = sizeof(type);
        return getsockopt(socket, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&type), &length) == 0 ? type : 0;
    }

    Route* Find(const sockaddr_in& address, int type, bool reverse)
    {
        for (size_t i = 0; i < g_count; ++i) {
            Route& route = g_routes[i];
            if (route.type == type && Same(address, reverse ? route.external : route.logical)) return &route;
        }
        return nullptr;
    }

    void Trace(Route* route, bool reverse)
    {
        if (!g_trace) return;
        LONG count = InterlockedIncrement(reverse ? &route->received : &route->sent);
        if (count <= 8)
            g_api->Log(g_mod, SDGO_LOG_INFO, "route %u %s %s #%ld",
                       static_cast<unsigned>(route - g_routes.data() + 1),
                       route->type == SOCK_STREAM ? "TCP" : "UDP", reverse ? "receive" : "send", count);
    }

    void TraceUnmapped(const sockaddr_in& address, int type)
    {
        if (!g_trace || InterlockedIncrement(&g_unmapped) > 32) return;
        char ip[INET_ADDRSTRLEN] = {};
        InetNtopA(AF_INET, const_cast<IN_ADDR*>(&address.sin_addr), ip, sizeof(ip));
        g_api->Log(g_mod, SDGO_LOG_INFO, "unmapped %s %s:%u (unchanged)",
                   type == SOCK_STREAM ? "TCP" : "UDP", ip, ntohs(address.sin_port));
    }

    int WSAAPI Connect(SOCKET socket, const sockaddr* address, int length)
    {
        const int incomingError = WSAGetLastError();
        sockaddr_in local = {};
        Route* route = nullptr;
        if (ReadAddress(address, length, &local)) {
            int type = SocketType(socket);
            route = Find(local, type, false);
            if (route) { Trace(route, false); local = route->external; }
            else if (type) TraceUnmapped(local, type);
        }
        WSASetLastError(incomingError);
        return g_connect(socket, route ? reinterpret_cast<const sockaddr*>(&local) : address, route ? sizeof(local) : length);
    }

    int WSAAPI SendTo(SOCKET socket, const char* data, int size, int flags, const sockaddr* address, int length)
    {
        const int incomingError = WSAGetLastError();
        sockaddr_in local = {};
        Route* route = nullptr;
        if (ReadAddress(address, length, &local)) {
            int type = SocketType(socket);
            // sendto can also be used on a stream socket. A UDP mapping must
            // never change such a call just because its port happens to match.
            if (type == SOCK_DGRAM) route = Find(local, type, false);
            if (route) { Trace(route, false); local = route->external; }
            else if (type) TraceUnmapped(local, type);
        }
        WSASetLastError(incomingError);
        return g_sendTo(socket, data, size, flags, route ? reinterpret_cast<const sockaddr*>(&local) : address,
                        route ? sizeof(local) : length);
    }

    int WSAAPI RecvFrom(SOCKET socket, char* data, int size, int flags, sockaddr* source, int* sourceLength)
    {
        int result = g_recvFrom(socket, data, size, flags, source, sourceLength);
        const int resultError = WSAGetLastError();
        sockaddr_in address = {};
        // WSAEMSGSIZE still returns the sender and a truncated datagram. It
        // must get the same source translation, while retaining SOCKET_ERROR.
        if ((result >= 0 || resultError == WSAEMSGSIZE) && ReadSource(source, sourceLength, &address)) {
            Route* route = Find(address, SOCK_DGRAM, true);
            if (route && SocketType(socket) == SOCK_DGRAM) {
                WriteSource(source, &route->logical);
                Trace(route, true);
            }
        }
        WSASetLastError(resultError);
        return result;
    }

    std::string Trim(const std::string& value)
    {
        const size_t start = value.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return {};
        return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
    }

    bool Number(const std::string& value, unsigned maximum, unsigned* output)
    {
        if (value.empty()) return false;
        unsigned number = 0;
        for (char c : value) {
            if (c < '0' || c > '9') return false;
            const unsigned digit = static_cast<unsigned>(c - '0');
            if (number > maximum / 10 || (number == maximum / 10 && digit > maximum % 10)) return false;
            number = number * 10 + digit;
        }
        *output = number;
        return true;
    }

    bool Setting(const char* key, const char* def, std::string* out)
    {
        char text[512] = {};
        if (sdgo::ModSettings(g_api, g_mod).String(key, def, text, sizeof(text)) >= sizeof(text) - 1) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "%s is too long", key);
            return false;
        }
        *out = Trim(text);
        return true;
    }

    std::wstring QuoteArgument(const std::wstring& value)
    {
        std::wstring result = L"\"";
        size_t slashes = 0;
        for (wchar_t c : value) {
            if (c == L'\\') { ++slashes; continue; }
            result.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
            slashes = 0;
            if (c == L'\"') result += L'\\';
            result += c;
        }
        result.append(slashes * 2, L'\\');
        return result + L'\"';
    }

    LPWSTR* WINAPI ParseLoginArgs(LPCWSTR command, int* count)
    {
        // Only the verified game login parser sees the configured endpoint.
        // CRT, launcher/event handling and every unrelated parser keep their input.
        if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != g_loginReturn || !command || !count)
            return g_parseArgs(command, count);
        int originalCount = 0;
        LPWSTR* args = g_parseArgs(command, &originalCount);
        if (!args) return nullptr;
        std::wstring configured;
        try {
            if (originalCount < 1) { LocalFree(args); return g_parseArgs(command, count); }
            configured = QuoteArgument(args[0]) + L" " + g_loginIP + L" " + g_loginPort;
            for (int i = 3; i < originalCount; ++i) configured += L" " + QuoteArgument(args[i]);
        } catch (...) {
            LocalFree(args);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return nullptr;
        }
        LocalFree(args);
        return g_parseArgs(configured.c_str(), count);
    }

    bool PrepareLogin()
    {
        std::string ip, portText;
        if (!Setting("LoginIPv4", "", &ip) || !Setting("LoginPort", "", &portText)) return false;
        if (ip.empty() && portText.empty()) return true; // legacy route-only profiles
        unsigned port = 0;
        sockaddr_in target = {};
        target.sin_family = AF_INET;
        if (InetPtonA(AF_INET, ip.c_str(), &target.sin_addr) != 1 ||
            !Number(portText, 65535, &port) || !port) return false;
        target.sin_port = htons(static_cast<u_short>(port));
        if (!Find(target, SOCK_STREAM, false)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "configured login must have an exact TCP route");
            return false;
        }
        auto* site = static_cast<const BYTE*>(g_api->Symbol(g_mod, "net.login_argv_call"));
        if (!site || site[0] != 0xFF || site[1] != 0x15) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "configured login needs verified net.login_argv_call");
            return false;
        }
        uintptr_t slot = 0;
        memcpy(&slot, site + 2, sizeof(slot));
        // The signature verifies the call; validate the actual imported function too.
        if (*reinterpret_cast<void**>(slot) != reinterpret_cast<void*>(&CommandLineToArgvW)) return false;
        g_loginReturn = reinterpret_cast<uintptr_t>(site) + 6;
        g_loginIP.assign(ip.begin(), ip.end());
        g_loginPort.assign(portText.begin(), portText.end());
        return true;
    }

    bool ParseRoute(const std::string& value, Route* route)
    {
        std::array<std::string, 5> cells;
        size_t start = 0;
        for (size_t i = 0; i < cells.size(); ++i) {
            size_t end = value.find(',', start);
            if ((i < cells.size() - 1 && end == std::string::npos) ||
                (i == cells.size() - 1 && end != std::string::npos)) return false;
            cells[i] = Trim(value.substr(start, end == std::string::npos ? end : end - start));
            start = end == std::string::npos ? value.size() : end + 1;
        }
        if (cells[0] == "tcp") route->type = SOCK_STREAM;
        else if (cells[0] == "udp") route->type = SOCK_DGRAM;
        else return false;
        unsigned logicalPort = 0, externalPort = 0;
        route->logical.sin_family = route->external.sin_family = AF_INET;
        if (!Number(cells[2], 65535, &logicalPort) || !logicalPort ||
            !Number(cells[4], 65535, &externalPort) || !externalPort ||
            InetPtonA(AF_INET, cells[1].c_str(), &route->logical.sin_addr) != 1 ||
            route->logical.sin_addr.s_addr == INADDR_ANY || route->logical.sin_addr.s_addr == INADDR_BROADCAST)
            return false;
        route->logical.sin_port = htons(static_cast<u_short>(logicalPort));
        route->external.sin_port = htons(static_cast<u_short>(externalPort));
        // Hostnames only (no schemes, paths, credentials or embedded port).
        if (cells[3].empty() || cells[3].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-")
                                  != std::string::npos) return false;
        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = route->type;
        addrinfo* answers = nullptr;
        int status = getaddrinfo(cells[3].c_str(), nullptr, &hints, &answers);
        if (status != 0 || !answers) {
            if (answers) freeaddrinfo(answers);
            g_api->Log(g_mod, SDGO_LOG_ERROR, "external host could not resolve to IPv4 (Winsock %d)", status);
            return false;
        }
        route->external.sin_addr = reinterpret_cast<sockaddr_in*>(answers->ai_addr)->sin_addr;
        freeaddrinfo(answers);
        return route->external.sin_addr.s_addr != INADDR_ANY && route->external.sin_addr.s_addr != INADDR_BROADCAST;
    }

    bool LoadRoutes()
    {
        std::string setting;
        unsigned count = 0, trace = 0;
        if (!Setting("RouteCount", "0", &setting) || !Number(setting, kMaxRoutes, &count) || !count ||
            !Setting("Trace", "0", &setting) || !Number(setting, 1, &trace)) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "need RouteCount=1..32 and Trace=0 or 1; no network hooks installed");
            return false;
        }
        g_trace = trace != 0;
        WSADATA ws = {};
        if (WSAStartup(MAKEWORD(2, 2), &ws) != 0) return false;
        bool ok = true;
        for (unsigned i = 0; i < count && ok; ++i) {
            char key[32];
            sprintf_s(key, "Route%u", i + 1);
            Route& route = g_routes[i];
            ok = Setting(key, "", &setting) && ParseRoute(setting, &route);
            if (!ok) {
                g_api->Log(g_mod, SDGO_LOG_ERROR, "%s invalid: use tcp|udp,logicalIPv4,port,externalHost,port", key);
                break;
            }
            for (unsigned j = 0; j < i; ++j) {
                if (route.type == g_routes[j].type &&
                    (Same(route.logical, g_routes[j].logical) ||
                     (route.type == SOCK_DGRAM && Same(route.external, g_routes[j].external)))) {
                    g_api->Log(g_mod, SDGO_LOG_ERROR, "%s conflicts with Route%u (logical or reverse endpoint)", key, j + 1);
                    ok = false;
                    break;
                }
            }
        }
        WSACleanup();
        if (!ok) return false;
        g_count = count;
        for (size_t i = 0; i < g_count; ++i) {
            char logical[INET_ADDRSTRLEN] = {}, external[INET_ADDRSTRLEN] = {};
            InetNtopA(AF_INET, &g_routes[i].logical.sin_addr, logical, sizeof(logical));
            InetNtopA(AF_INET, &g_routes[i].external.sin_addr, external, sizeof(external));
            g_api->Log(g_mod, SDGO_LOG_INFO, "route %u %s %s:%u -> %s:%u", static_cast<unsigned>(i + 1),
                       g_routes[i].type == SOCK_STREAM ? "TCP" : "UDP", logical, ntohs(g_routes[i].logical.sin_port),
                       external, ntohs(g_routes[i].external.sin_port));
        }
        return true;
    }
}

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION || api->size < offsetof(SdgoApi, ModDirectory) + sizeof(api->ModDirectory))
        return 0;
    g_api = api;
    g_mod = mod;
    if (!LoadRoutes() || !PrepareLogin()) return 0;
    // The core rolls back ALL imports if any installation fails (e.g. net_probe
    // already owns sendto). Never leave only one half of translation enabled.
    if (!api->HookImport(mod, "ws2_32.dll", "connect", reinterpret_cast<void*>(&Connect), reinterpret_cast<void**>(&g_connect)) ||
        !api->HookImport(mod, "ws2_32.dll", "sendto", reinterpret_cast<void*>(&SendTo), reinterpret_cast<void**>(&g_sendTo)) ||
        !api->HookImport(mod, "ws2_32.dll", "recvfrom", reinterpret_cast<void*>(&RecvFrom), reinterpret_cast<void**>(&g_recvFrom)))
        return 0;
    if (g_loginReturn && !api->HookImport(mod, "shell32.dll", "CommandLineToArgvW",
            reinterpret_cast<void*>(&ParseLoginArgs), reinterpret_cast<void**>(&g_parseArgs))) return 0;
    if (g_loginReturn) api->Log(mod, SDGO_LOG_INFO, "configured login active in native argument parser; existing launcher supported");
    api->Log(mod, SDGO_LOG_INFO, "endpoint translation active (%u routes); peer relay is separate", static_cast<unsigned>(g_count));
    return 1;
}
