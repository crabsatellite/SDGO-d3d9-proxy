// Integration test for the real core.dll and net_route.dll (no game needed).
// The executable's Winsock IAT stands in for GOnline.exe. The test reserves
// loopback endpoints, lets the real core load the real mod, and then exercises
// the imported connect/sendto/recvfrom calls that the mod owns.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "sdgo/sdgo_api.h"
#include "named_winsock_fixture.h"

namespace
{
    enum class TestCase { Routes, Rollback, Invalid, Duplicate, UdpAlias, Disabled };
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++g_failures;
    }

    sockaddr_in IPv4(const char* dottedAddress, unsigned short port)
    {
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        InetPtonA(AF_INET, dottedAddress, &address.sin_addr);
        address.sin_port = htons(port);
        return address;
    }

    sockaddr_in Loopback(unsigned short port)
    {
        return IPv4("127.0.0.1", port);
    }

    bool BoundPort(SOCKET socket, unsigned short* port)
    {
        sockaddr_in address = {};
        int length = sizeof(address);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR)
            return false;
        *port = ntohs(address.sin_port);
        return true;
    }

    SOCKET BindIPv4At(int type, const char* dottedAddress, unsigned short requestedPort,
                      unsigned short* actualPort)
    {
        SOCKET socket = ::socket(AF_INET, type, type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
        if (socket == INVALID_SOCKET) return INVALID_SOCKET;
        BOOL exclusive = TRUE;
        setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address = IPv4(dottedAddress, requestedPort);
        if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            !BoundPort(socket, actualPort)) {
            std::printf("fixture reservation busy: type %d %s:%u (Winsock %d)\n",
                        type, dottedAddress, requestedPort, WSAGetLastError());
            closesocket(socket);
            return INVALID_SOCKET;
        }
        return socket;
    }

    SOCKET BindLoopbackAt(int type, unsigned short requestedPort, unsigned short* actualPort)
    {
        return BindIPv4At(type, "127.0.0.1", requestedPort, actualPort);
    }

    SOCKET BindLoopback(int type, unsigned short* port)
    {
        return BindLoopbackAt(type, 0, port);
    }

    // A UDP ephemeral port can already be occupied in TCP by an unrelated
    // application. Reserve both protocols together instead of assuming it is free.
    void ReserveProtocolPair(SOCKET* udp, SOCKET* tcp, unsigned short* port, SOCKET* udpAlias = nullptr)
    {
        *udp = *tcp = INVALID_SOCKET;
        if (udpAlias) *udpAlias = INVALID_SOCKET;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            unsigned short selected = 0, udpPort = 0;
            SOCKET tcpGuard = BindLoopback(SOCK_STREAM, &selected);
            if (tcpGuard == INVALID_SOCKET) continue;
            SOCKET udpGuard = BindLoopbackAt(SOCK_DGRAM, selected, &udpPort);
            SOCKET alias = udpAlias && udpGuard != INVALID_SOCKET
                ? BindIPv4At(SOCK_DGRAM, "127.0.0.2", selected, &udpPort) : INVALID_SOCKET;
            if (udpGuard != INVALID_SOCKET && (!udpAlias || alias != INVALID_SOCKET)) {
                *udp = udpGuard; *tcp = tcpGuard; *port = selected;
                if (udpAlias) *udpAlias = alias;
                return;
            }
            if (udpGuard != INVALID_SOCKET) closesocket(udpGuard);
            closesocket(tcpGuard);
        }
    }

    void ReserveTcpAliases(SOCKET* primary, SOCKET* alias, SOCKET* service, unsigned short* port)
    {
        *primary = *alias = *service = INVALID_SOCKET;
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            unsigned short selected = 0, actual = 0;
            SOCKET first = BindLoopback(SOCK_STREAM, &selected);
            if (first == INVALID_SOCKET) continue;
            SOCKET second = BindIPv4At(SOCK_STREAM, "127.0.0.2", selected, &actual);
            SOCKET third = second == INVALID_SOCKET ? INVALID_SOCKET :
                BindIPv4At(SOCK_STREAM, "127.0.0.3", selected, &actual);
            if (second != INVALID_SOCKET && third != INVALID_SOCKET) {
                *primary = first; *alias = second; *service = third; *port = selected;
                return;
            }
            if (second != INVALID_SOCKET) closesocket(second);
            closesocket(first);
        }
    }

    bool WaitReadable(SOCKET socket, long milliseconds)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket, &set);
        timeval timeout = {milliseconds / 1000, (milliseconds % 1000) * 1000};
        return select(0, &set, nullptr, nullptr, &timeout) == 1;
    }

    void SetReceiveTimeout(SOCKET socket)
    {
        DWORD timeout = 2000;
        setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    }

    std::wstring ExeDirectory()
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) *slash = L'\0';
        return path;
    }

    bool WriteSetting(const std::wstring& manifest, const wchar_t* key, const std::wstring& value)
    {
        return WritePrivateProfileStringW(L"settings", key, value.c_str(), manifest.c_str()) != FALSE;
    }

    bool ConfigureRoutes(const std::wstring& root, unsigned short logicalTcp, unsigned short externalTcp,
                         unsigned short externalServiceTcp, unsigned short logicalUdp,
                         unsigned short externalUdp, TestCase testCase)
    {
        const std::wstring manifest = root + L"\\sdgo\\mods\\net_route\\mod.ini";
        wchar_t route[160] = {};
        bool ok = WriteSetting(manifest, L"Trace", L"1");
        if (testCase == TestCase::Invalid)
            swprintf_s(route, L"tcp,127.0.0.1,0,127.0.0.1,%u", externalTcp);
        else
            swprintf_s(route, L"tcp,127.0.0.1,%u,127.0.0.1,%u", logicalTcp, externalTcp);
        ok = WriteSetting(manifest, L"Route1", route) && ok;
        if (testCase == TestCase::Routes) {
            swprintf_s(route, L"udp,127.0.0.1,%u,127.0.0.1,%u", logicalUdp, externalUdp);
            ok = WriteSetting(manifest, L"Route2", route) && ok;
            swprintf_s(route, L"tcp,127.0.0.2,%u,127.0.0.1,%u", logicalTcp, externalTcp);
            ok = WriteSetting(manifest, L"Route3", route) && ok;
            swprintf_s(route, L"tcp,127.0.0.3,%u,127.0.0.1,%u", logicalTcp, externalServiceTcp);
            ok = WriteSetting(manifest, L"Route4", route) && ok;
            ok = WriteSetting(manifest, L"RouteCount", L"4") && ok;
        } else if (testCase == TestCase::Disabled) {
            swprintf_s(route, L"udp,127.0.0.1,%u,127.0.0.1,%u", logicalUdp, externalUdp);
            ok = WriteSetting(manifest, L"Route2", route) && ok;
            ok = WriteSetting(manifest, L"RouteCount", L"2") && ok;
        } else if (testCase == TestCase::Duplicate) {
            swprintf_s(route, L"tcp,127.0.0.1,%u,127.0.0.1,%u", logicalTcp, externalTcp);
            ok = WriteSetting(manifest, L"Route2", route) && ok;
            ok = WriteSetting(manifest, L"RouteCount", L"2") && ok;
        } else if (testCase == TestCase::UdpAlias) {
            swprintf_s(route, L"udp,127.0.0.1,%u,127.0.0.1,%u", logicalUdp, externalUdp);
            ok = WriteSetting(manifest, L"Route2", route) && ok;
            swprintf_s(route, L"udp,127.0.0.2,%u,127.0.0.1,%u", logicalUdp, externalUdp);
            ok = WriteSetting(manifest, L"Route3", route) && ok;
            ok = WriteSetting(manifest, L"RouteCount", L"3") && ok;
        } else {
            ok = WriteSetting(manifest, L"RouteCount", L"1") && ok;
        }
        return ok;
    }

    bool StartCore(const std::wstring& root)
    {
        HMODULE core = LoadLibraryW((root + L"\\sdgo\\core.dll").c_str());
        auto init = core ? reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core, "SdgoCoreInit")) : nullptr;
        if (!init) {
            std::printf("FAIL core export missing (Win32 %lu)\n", GetLastError());
            ++g_failures;
            return false;
        }
        SdgoBoot boot = {sizeof(SdgoBoot), "net_route_test", GetModuleHandleW(nullptr)};
        Check(init(&boot) != 0, "real core initializes with the test manifests");
        return g_failures == 0;
    }

    void TestTcp(SOCKET listener, const char* logicalIp, unsigned short logicalPort)
    {
        const std::string label = std::string("TCP ") + logicalIp;
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in logical = IPv4(logicalIp, logicalPort);
        int result = connect(client, reinterpret_cast<const sockaddr*>(&logical), sizeof(logical));
        Check(result == 0, (label + " logical endpoint reaches the external listener").c_str());
        if (result == 0 && WaitReadable(listener, 2000)) {
            SOCKET peer = accept(listener, nullptr, nullptr);
            Check(peer != INVALID_SOCKET,
                  (label + " translated connection is accepted by the external listener").c_str());
            if (peer != INVALID_SOCKET) {
                const char message[] = "tcp-route";
                char received[sizeof(message)] = {};
                Check(send(client, message, sizeof(message), 0) == sizeof(message),
                      (label + " payload sends").c_str());
                Check(recv(peer, received, sizeof(received), 0) == sizeof(received) &&
                          std::memcmp(received, message, sizeof(message)) == 0,
                      (label + " payload arrives unchanged").c_str());
                closesocket(peer);
            }
        } else if (result == 0) {
            Check(false, (label + " makes the external listener readable").c_str());
        }
        if (client != INVALID_SOCKET) closesocket(client);
    }

    bool IsLogicalSource(const sockaddr_in& source, unsigned short logicalPort)
    {
        return source.sin_family == AF_INET && source.sin_addr.s_addr == htonl(INADDR_LOOPBACK) &&
               ntohs(source.sin_port) == logicalPort;
    }

    void FillSourceSentinel(sockaddr_in* source)
    {
        std::memset(source, 0xA5, sizeof(*source));
    }

    bool SendReply(SOCKET external, const sockaddr_in& destination, int destinationLength,
                   const char* data, int size)
    {
        return sendto(external, data, size, 0, reinterpret_cast<const sockaddr*>(&destination),
                      destinationLength) == size;
    }

    void TestUdp(SOCKET external, unsigned short logicalPort,
                 SOCKET unmappedListener, unsigned short unmappedPort)
    {
        SOCKET client = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        SetReceiveTimeout(client);
        SetReceiveTimeout(external);
        sockaddr_in local = Loopback(0);
        Check(bind(client, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) == 0,
              "UDP client binds on loopback");

        const char request[] = "udp-route";
        sockaddr_in logical = Loopback(logicalPort);
        const sockaddr_in originalLogical = logical;
        Check(sendto(client, request, sizeof(request), 0, reinterpret_cast<const sockaddr*>(&logical),
                     sizeof(logical)) == sizeof(request),
              "UDP sendto accepts the logical endpoint");
        Check(std::memcmp(&logical, &originalLogical, sizeof(logical)) == 0,
              "UDP forward mapping does not modify the caller's destination");

        char received[64] = {};
        sockaddr_in clientAddress = {};
        int clientLength = sizeof(clientAddress);
        int count = recvfrom(external, received, sizeof(received), 0,
                             reinterpret_cast<sockaddr*>(&clientAddress), &clientLength);
        Check(count == sizeof(request) && std::memcmp(received, request, sizeof(request)) == 0,
              "UDP datagram reaches the external endpoint unchanged");

        const char passThrough[] = "udp-unmapped";
        sockaddr_in unmappedDestination = Loopback(unmappedPort);
        const sockaddr_in originalUnmappedDestination = unmappedDestination;
        Check(sendto(client, passThrough, sizeof(passThrough), 0,
                     reinterpret_cast<const sockaddr*>(&unmappedDestination),
                     sizeof(unmappedDestination)) == sizeof(passThrough),
              "unmapped UDP sendto succeeds");
        sockaddr_in passThroughSource = {};
        int passThroughLength = sizeof(passThroughSource);
        count = recvfrom(unmappedListener, received, sizeof(received), 0,
                         reinterpret_cast<sockaddr*>(&passThroughSource), &passThroughLength);
        Check(count == sizeof(passThrough) &&
                  std::memcmp(received, passThrough, sizeof(passThrough)) == 0,
              "unmapped UDP datagram keeps its original destination");
        Check(std::memcmp(&unmappedDestination, &originalUnmappedDestination,
                          sizeof(unmappedDestination)) == 0,
              "unmapped UDP pass-through leaves the caller's destination unchanged");

        const char reply[] = "udp-peek-reply";
        Check(count >= 0 && SendReply(external, clientAddress, clientLength, reply, sizeof(reply)),
              "external UDP endpoint sends a reply for MSG_PEEK");
        sockaddr_in reportedSource = {};
        int sourceLength = sizeof(reportedSource);
        count = recvfrom(client, received, sizeof(received), MSG_PEEK,
                         reinterpret_cast<sockaddr*>(&reportedSource), &sourceLength);
        Check(count == sizeof(reply) && std::memcmp(received, reply, sizeof(reply)) == 0 &&
                  IsLogicalSource(reportedSource, logicalPort),
              "MSG_PEEK reports the logical source without changing the payload");
        FillSourceSentinel(&reportedSource);
        sourceLength = sizeof(reportedSource);
        count = recvfrom(client, received, sizeof(received), 0,
                         reinterpret_cast<sockaddr*>(&reportedSource), &sourceLength);
        Check(count == sizeof(reply) && std::memcmp(received, reply, sizeof(reply)) == 0 &&
                  IsLogicalSource(reportedSource, logicalPort),
              "the peeked datagram remains queued and reports the logical source");

        Check(SendReply(external, clientAddress, clientLength, "", 0),
              "external UDP endpoint sends a zero-length datagram");
        FillSourceSentinel(&reportedSource);
        sourceLength = sizeof(reportedSource);
        count = recvfrom(client, received, sizeof(received), 0,
                         reinterpret_cast<sockaddr*>(&reportedSource), &sourceLength);
        Check(count == 0 && IsLogicalSource(reportedSource, logicalPort),
              "zero-length UDP datagram reports the logical source");

        const char oversized[] = "udp-reply-too-large-for-buffer";
        Check(SendReply(external, clientAddress, clientLength, oversized, sizeof(oversized)),
              "external UDP endpoint sends a datagram for truncation");
        char shortBuffer[4] = {};
        FillSourceSentinel(&reportedSource);
        sourceLength = sizeof(reportedSource);
        WSASetLastError(0);
        count = recvfrom(client, shortBuffer, sizeof(shortBuffer), 0,
                         reinterpret_cast<sockaddr*>(&reportedSource), &sourceLength);
        const int truncationError = WSAGetLastError();
        Check(count == SOCKET_ERROR && truncationError == WSAEMSGSIZE,
              "truncated UDP recvfrom retains WSAEMSGSIZE");
        Check(IsLogicalSource(reportedSource, logicalPort),
              "truncated UDP recvfrom still reports the logical source");

        u_long nonblocking = 1;
        Check(ioctlsocket(client, FIONBIO, &nonblocking) == 0, "UDP client enters nonblocking mode");
        FillSourceSentinel(&reportedSource);
        const sockaddr_in originalSource = reportedSource;
        sourceLength = sizeof(reportedSource);
        const int originalLength = sourceLength;
        WSASetLastError(0);
        count = recvfrom(client, received, sizeof(received), 0,
                         reinterpret_cast<sockaddr*>(&reportedSource), &sourceLength);
        const int nonblockingError = WSAGetLastError();
        Check(count == SOCKET_ERROR && nonblockingError == WSAEWOULDBLOCK,
              "empty nonblocking UDP recvfrom retains WSAEWOULDBLOCK");
        Check(std::memcmp(&reportedSource, &originalSource, sizeof(reportedSource)) == 0 &&
                  sourceLength == originalLength,
              "failed nonblocking recvfrom leaves caller source storage unchanged");
        closesocket(client);
    }

    void TestUdpRouteDoesNotCatchTcp(SOCKET udpPortTcpListener, unsigned short udpLogicalPort)
    {
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in udpLogical = Loopback(udpLogicalPort);
        int result = connect(client, reinterpret_cast<const sockaddr*>(&udpLogical), sizeof(udpLogical));
        const int error = WSAGetLastError();
        Check(result == SOCKET_ERROR && error == WSAECONNREFUSED,
              "TCP connect ignores a UDP-only route for the same endpoint");
        Check(!WaitReadable(udpPortTcpListener, 200),
              "TCP connect does not reach the UDP route's external port");
        closesocket(client);
    }

    void TestUnmodifiedConnect(SOCKET externalListener, unsigned short logicalPort, const char* reason)
    {
        SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in logical = Loopback(logicalPort);
        const sockaddr_in originalLogical = logical;
        int result = connect(client, reinterpret_cast<const sockaddr*>(&logical), sizeof(logical));
        int error = WSAGetLastError();
        Check(result == SOCKET_ERROR, reason);
        Check(result == SOCKET_ERROR && error == WSAECONNREFUSED,
              "the reserved non-listening logical endpoint refuses the unmodified connection");
        Check(!WaitReadable(externalListener, 200),
              "unmodified connect does not reach the configured external listener");
        Check(std::memcmp(&logical, &originalLogical, sizeof(logical)) == 0,
              "connect leaves the caller's destination unchanged");
        closesocket(client);
    }
}

int wmain(int argc, wchar_t** argv)
{
    Check(NameClientWinsockImports(), "synthetic host models GOnline named Winsock imports");
    TestCase testCase = TestCase::Routes;
    if (argc > 1 && wcscmp(argv[1], L"rollback") == 0) testCase = TestCase::Rollback;
    else if (argc > 1 && wcscmp(argv[1], L"invalid") == 0) testCase = TestCase::Invalid;
    else if (argc > 1 && wcscmp(argv[1], L"duplicate") == 0) testCase = TestCase::Duplicate;
    else if (argc > 1 && wcscmp(argv[1], L"udp_alias") == 0) testCase = TestCase::UdpAlias;
    else if (argc > 1 && wcscmp(argv[1], L"disabled") == 0) testCase = TestCase::Disabled;
    WSADATA winsock = {};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::puts("FAIL WSAStartup");
        return 2;
    }

    unsigned short externalTcpPort = 0;
    unsigned short externalServiceTcpPort = 0;
    unsigned short logicalTcpPort = 0;
    unsigned short externalUdpPort = 0;
    unsigned short logicalUdpPort = 0;
    SOCKET externalTcp = BindLoopback(SOCK_STREAM, &externalTcpPort);
    SOCKET logicalTcpGuard = INVALID_SOCKET, logicalTcpAliasGuard = INVALID_SOCKET, logicalTcpServiceGuard = INVALID_SOCKET;
    ReserveTcpAliases(&logicalTcpGuard, &logicalTcpAliasGuard, &logicalTcpServiceGuard, &logicalTcpPort);
    unsigned short logicalTcpAliasPort = 0;
    BoundPort(logicalTcpAliasGuard, &logicalTcpAliasPort);
    unsigned short logicalTcpServicePort = 0;
    BoundPort(logicalTcpServiceGuard, &logicalTcpServicePort);
    SOCKET externalUdp = INVALID_SOCKET, udpExternalTcp = INVALID_SOCKET;
    SOCKET logicalUdpGuard = INVALID_SOCKET, udpLogicalTcpGuard = INVALID_SOCKET;
    ReserveProtocolPair(&externalUdp, &udpExternalTcp, &externalUdpPort);
    SOCKET logicalUdpAliasGuard = INVALID_SOCKET;
    ReserveProtocolPair(&logicalUdpGuard, &udpLogicalTcpGuard, &logicalUdpPort, &logicalUdpAliasGuard);
    unsigned short logicalUdpAliasPort = 0;
    BoundPort(logicalUdpAliasGuard, &logicalUdpAliasPort);
    unsigned short unmappedUdpPort = 0;
    SOCKET unmappedUdp = BindLoopback(SOCK_DGRAM, &unmappedUdpPort);
    const unsigned short udpExternalTcpPort = externalUdpPort;
    const unsigned short udpLogicalTcpPort = logicalUdpPort;
    // Reserve the independent listener after the cross-protocol TCP guards so
    // Winsock cannot choose an ephemeral TCP port that a later guard must own.
    SOCKET externalServiceTcp = BindLoopback(SOCK_STREAM, &externalServiceTcpPort);
    if (externalTcp == INVALID_SOCKET || logicalTcpGuard == INVALID_SOCKET ||
        logicalTcpAliasGuard == INVALID_SOCKET || externalServiceTcp == INVALID_SOCKET ||
        logicalTcpServiceGuard == INVALID_SOCKET || externalUdp == INVALID_SOCKET ||
        logicalUdpGuard == INVALID_SOCKET || logicalUdpAliasGuard == INVALID_SOCKET ||
        unmappedUdp == INVALID_SOCKET || udpExternalTcp == INVALID_SOCKET ||
        udpLogicalTcpGuard == INVALID_SOCKET) {
        std::printf("FAIL cannot reserve loopback endpoints (Winsock %d)\n", WSAGetLastError());
        return 2;
    }
    if (udpExternalTcpPort != externalUdpPort || udpLogicalTcpPort != logicalUdpPort ||
        logicalTcpAliasPort != logicalTcpPort || logicalTcpServicePort != logicalTcpPort ||
        logicalUdpAliasPort != logicalUdpPort) {
        std::puts("FAIL reserved loopback endpoint ports do not match");
        return 2;
    }
    if (listen(externalTcp, 2) == SOCKET_ERROR ||
        listen(externalServiceTcp, 1) == SOCKET_ERROR ||
        listen(udpExternalTcp, 2) == SOCKET_ERROR) {
        std::printf("FAIL cannot listen on reserved loopback endpoints (Winsock %d)\n", WSAGetLastError());
        return 2;
    }
    SetReceiveTimeout(unmappedUdp);

    const std::wstring root = ExeDirectory();
    Check(ConfigureRoutes(root, logicalTcpPort, externalTcpPort, externalServiceTcpPort,
                          logicalUdpPort, externalUdpPort, testCase),
          "test writes temporary route settings");
    if (g_failures == 0 && StartCore(root)) {
        if (testCase == TestCase::Routes) {
            TestTcp(externalTcp, "127.0.0.1", logicalTcpPort);
            TestTcp(externalTcp, "127.0.0.2", logicalTcpPort);
            TestTcp(externalServiceTcp, "127.0.0.3", logicalTcpPort);
            Check(!WaitReadable(externalTcp, 200),
                  "TCP 127.0.0.3 independent service does not reach the login listener");
            TestUdp(externalUdp, logicalUdpPort, unmappedUdp, unmappedUdpPort);
            TestUdpRouteDoesNotCatchTcp(udpExternalTcp, logicalUdpPort);
        } else {
            const char* reason = testCase == TestCase::Rollback
                ? "failed net_route load restores the original connect import"
                : testCase == TestCase::Invalid
                    ? "invalid route configuration installs no connect hook"
                    : testCase == TestCase::Duplicate
                        ? "duplicate route configuration installs no connect hook"
                        : testCase == TestCase::UdpAlias
                            ? "ambiguous UDP reverse endpoint installs no connect hook"
                            : "disabled net_route installs no connect hook";
            TestUnmodifiedConnect(externalTcp, logicalTcpPort, reason);
        }
    }

    closesocket(udpLogicalTcpGuard);
    closesocket(udpExternalTcp);
    closesocket(unmappedUdp);
    closesocket(logicalUdpAliasGuard);
    closesocket(logicalUdpGuard);
    closesocket(externalUdp);
    closesocket(logicalTcpServiceGuard);
    closesocket(externalServiceTcp);
    closesocket(logicalTcpAliasGuard);
    closesocket(logicalTcpGuard);
    closesocket(externalTcp);
    WSACleanup();
    std::puts(g_failures ? "NET ROUTE CHECKS FAILED" : "ALL NET ROUTE CHECKS PASSED");
    return g_failures ? 1 : 0;
}
