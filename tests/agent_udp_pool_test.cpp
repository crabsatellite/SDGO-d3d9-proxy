// Actual-core integration test for agent_udp_pool.dll (no AgentServer needed).
// A small executable-image fixture provides the exact x86 `FF 15 [bind IAT]`
// call used by the mod's return-address gate. The test writes a matching symbol
// file for its own PE image before loading the real core and mod.
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"

#pragma section(".sdgtst", read, execute)
__declspec(allocate(".sdgtst")) __declspec(align(16)) unsigned char g_poolBindCode[64] = {};
char g_safeRelayFormat[] = "UDP_RELAY_DATA : SEND TO DATA.. to p:%d %u:%d\n";

namespace
{
    using PoolBindFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
        if (!ok) ++g_failures;
    }

    sockaddr_in Loopback(unsigned short port)
    {
        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        return address;
    }

    void CloseAll(std::vector<SOCKET>* sockets)
    {
        for (SOCKET socket : *sockets)
            closesocket(socket);
        sockets->clear();
    }

    SOCKET BindGuard(int type, unsigned short port)
    {
        SOCKET socket = ::socket(AF_INET, type, type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
        if (socket == INVALID_SOCKET) return INVALID_SOCKET;
        BOOL exclusive = TRUE;
        setsockopt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in address = Loopback(port);
        if (bind(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
            closesocket(socket);
            return INVALID_SOCKET;
        }
        return socket;
    }

    bool ReservePool(unsigned short* first, unsigned count,
                     std::vector<SOCKET>* udpReservations, std::vector<SOCKET>* tcpGuards)
    {
        for (unsigned candidate = 12000; candidate + count - 1 <= 60000; ++candidate) {
            bool ok = true;
            for (unsigned offset = 0; offset < count && ok; ++offset) {
                SOCKET udp = BindGuard(SOCK_DGRAM, static_cast<unsigned short>(candidate + offset));
                SOCKET tcp = BindGuard(SOCK_STREAM, static_cast<unsigned short>(candidate + offset));
                if (udp == INVALID_SOCKET || tcp == INVALID_SOCKET) {
                    if (udp != INVALID_SOCKET) closesocket(udp);
                    if (tcp != INVALID_SOCKET) closesocket(tcp);
                    ok = false;
                } else {
                    udpReservations->push_back(udp);
                    tcpGuards->push_back(tcp);
                }
            }
            if (ok) {
                *first = static_cast<unsigned short>(candidate);
                return true;
            }
            CloseAll(udpReservations);
            CloseAll(tcpGuards);
        }
        return false;
    }

    std::wstring ExeDirectory()
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) *slash = L'\0';
        return path;
    }

    std::wstring ExeName()
    {
        wchar_t path[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
        wchar_t* slash = wcsrchr(path, L'\\');
        return slash ? slash + 1 : path;
    }

    void** FindBindImportSlot(HMODULE host)
    {
        HMODULE winsock = GetModuleHandleW(L"ws2_32.dll");
        void* target = winsock ? reinterpret_cast<void*>(GetProcAddress(winsock, "bind")) : nullptr;
        if (!target) return nullptr;
        auto* base = reinterpret_cast<unsigned char*>(host);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!imports.VirtualAddress) return nullptr;
        for (auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
             descriptor->Name; ++descriptor) {
            const char* name = reinterpret_cast<const char*>(base + descriptor->Name);
            if (_stricmp(name, "ws2_32.dll") != 0) continue;
            auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
            for (size_t index = 0; iat[index].u1.Function; ++index) {
                auto** slot = reinterpret_cast<void**>(&iat[index].u1.Function);
                if (*slot == target) return slot;
            }
        }
        return nullptr;
    }

    unsigned char* BuildPoolBindFixture(void** bindSlot)
    {
        // stdcall wrapper:
        //   push [esp+0c] three times (length, address, socket)
        //   call dword ptr [absolute bind IAT slot]
        //   ret 12
        const unsigned char code[] = {
            0xFF, 0x74, 0x24, 0x0C,
            0xFF, 0x74, 0x24, 0x0C,
            0xFF, 0x74, 0x24, 0x0C,
            0xFF, 0x15, 0, 0, 0, 0,
            0xC2, 0x0C, 0x00
        };
        DWORD oldProtect = 0;
        if (!VirtualProtect(g_poolBindCode, sizeof(g_poolBindCode), PAGE_EXECUTE_READWRITE, &oldProtect))
            return nullptr;
        std::memset(g_poolBindCode, 0xCC, sizeof(g_poolBindCode));
        std::memcpy(g_poolBindCode, code, sizeof(code));
        const uint32_t operand = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(bindSlot));
        std::memcpy(g_poolBindCode + 14, &operand, sizeof(operand));
        FlushInstructionCache(GetCurrentProcess(), g_poolBindCode, sizeof(g_poolBindCode));
        DWORD ignored = 0;
        VirtualProtect(g_poolBindCode, sizeof(g_poolBindCode), oldProtect, &ignored);

        for (size_t offset = 0; offset + 6 <= sizeof(g_poolBindCode); ++offset) {
            uint32_t foundOperand = 0;
            std::memcpy(&foundOperand, g_poolBindCode + offset + 2, sizeof(foundOperand));
            if (g_poolBindCode[offset] == 0xFF && g_poolBindCode[offset + 1] == 0x15 &&
                foundOperand == operand)
                return g_poolBindCode + offset;
        }
        return nullptr;
    }

    bool WriteTextFile(const std::wstring& path, const std::string& text)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                        written == text.size();
        CloseHandle(file);
        return ok;
    }

    bool WriteSymbols(const std::wstring& root, HMODULE host, unsigned char* site, void** bindSlot)
    {
        auto* base = reinterpret_cast<unsigned char*>(host);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const uintptr_t siteRva = reinterpret_cast<unsigned char*>(site) - base;
        const uintptr_t slotRva = reinterpret_cast<unsigned char*>(bindSlot) - base;
        const uintptr_t formatRva = reinterpret_cast<const unsigned char*>(g_safeRelayFormat) - base;
        if (siteRva >= nt->OptionalHeader.SizeOfImage || slotRva >= nt->OptionalHeader.SizeOfImage)
            return false;

        std::wstring stem = ExeName();
        const size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem.resize(dot);
        wchar_t hostKey[256] = {};
        swprintf_s(hostKey, L"%s-%08X-%08X", stem.c_str(), nt->FileHeader.TimeDateStamp,
                   nt->OptionalHeader.SizeOfImage);
        const std::wstring symbols = root + L"\\sdgo\\symbols";
        CreateDirectoryW((root + L"\\sdgo").c_str(), nullptr);
        CreateDirectoryW(symbols.c_str(), nullptr);

        char expect[64] = {};
        sprintf_s(expect, "%02X %02X %02X %02X %02X %02X",
                  site[0], site[1], site[2], site[3], site[4], site[5]);
        char ini[1024] = {};
        sprintf_s(ini,
                  "[host]\r\n"
                  "image=agent_udp_pool_test.exe\r\n"
                  "timestamp=0x%08X\r\n"
                  "size_of_image=0x%08X\r\n\r\n"
                  "[agent.udp_bind_call]\r\n"
                  "kind=site\r\n"
                  "rva=0x%08X\r\n"
                  "expect=%s\r\n\r\n"
                  "[agent.bind_import]\r\n"
                  "kind=data\r\n"
                  "rva=0x%08X\r\n\r\n"
                  "[agent.relay_log_format]\r\n"
                  "kind=data\r\n"
                  "rva=0x%08X\r\n",
                  nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
                  static_cast<unsigned>(siteRva), expect, static_cast<unsigned>(slotRva), static_cast<unsigned>(formatRva));
        return WriteTextFile(symbols + L"\\" + hostKey + L".ini", ini);
    }

    bool WriteSettings(const std::wstring& root, unsigned short first, bool invalid)
    {
        const std::wstring manifest = root + L"\\sdgo\\mods\\agent_udp_pool\\mod.ini";
        wchar_t firstText[32] = {};
        wchar_t countText[32] = {};
        if (invalid) {
            wcscpy_s(firstText, L"65535");
            wcscpy_s(countText, L"2");
        } else {
            swprintf_s(firstText, L"%u", first);
            wcscpy_s(countText, L"3");
        }
        return WritePrivateProfileStringW(L"settings", L"FirstPort", firstText, manifest.c_str()) &&
               WritePrivateProfileStringW(L"settings", L"PortCount", countText, manifest.c_str());
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
        SdgoBoot boot = {sizeof(SdgoBoot), "agent_udp_pool_test", GetModuleHandleW(nullptr)};
        Check(init(&boot) != 0, "real core initializes with generated host symbols");
        return g_failures == 0;
    }

    unsigned short SocketPort(SOCKET socket)
    {
        sockaddr_in address = {};
        int length = sizeof(address);
        return getsockname(socket, reinterpret_cast<sockaddr*>(&address), &length) == 0
            ? ntohs(address.sin_port) : 0;
    }

    bool InPool(unsigned short port, unsigned short first, unsigned count)
    {
        return port >= first && port < first + count;
    }

    void TestValid(PoolBindFn poolBind, unsigned short first)
    {
        constexpr unsigned count = 3;
        std::vector<SOCKET> allocated;
        std::vector<unsigned short> ports;
        for (unsigned index = 0; index < count; ++index) {
            SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            sockaddr_in requested = Loopback(0);
            const sockaddr_in original = requested;
            Check(poolBind(socket, reinterpret_cast<const sockaddr*>(&requested), sizeof(requested)) == 0,
                  "verified Agent call site binds a UDP socket from the pool");
            Check(std::memcmp(&requested, &original, sizeof(requested)) == 0,
                  "pool selection leaves the native sockaddr input unchanged");
            const unsigned short actual = SocketPort(socket);
            Check(InPool(actual, first, count), "getsockname returns an actual pool port");
            allocated.push_back(socket);
            ports.push_back(actual);
        }
        std::sort(ports.begin(), ports.end());
        Check(ports.size() == count && ports[0] == first && ports[1] == first + 1 && ports[2] == first + 2,
              "three sockets exhaust the complete contiguous pool");

        SOCKET full = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in requested = Loopback(0);
        WSASetLastError(0);
        const int fullResult = poolBind(full, reinterpret_cast<const sockaddr*>(&requested), sizeof(requested));
        const int fullError = WSAGetLastError();
        Check(fullResult == SOCKET_ERROR && (fullError == WSAEADDRINUSE || fullError == WSAEACCES),
              "a fourth socket fails when every bounded pool port is occupied");
        Check(SocketPort(full) == 0, "pool exhaustion never falls back to an untunnelled port");
        closesocket(full);

        const unsigned short released = SocketPort(allocated[0]);
        closesocket(allocated[0]);
        allocated[0] = INVALID_SOCKET;
        SOCKET reused = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        requested = Loopback(0);
        Check(poolBind(reused, reinterpret_cast<const sockaddr*>(&requested), sizeof(requested)) == 0,
              "a later verified bind scans across occupied entries");
        Check(SocketPort(reused) == released,
              "the bounded scan wraps and reuses the released pool port");

        SOCKET normal = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        requested = Loopback(0);
        Check(bind(normal, reinterpret_cast<const sockaddr*>(&requested), sizeof(requested)) == 0,
              "a normal bind outside the verified call site remains native");
        Check(SocketPort(normal) != 0 && !InPool(SocketPort(normal), first, count),
              "outside-site UDP bind keeps native port-zero allocation");

        SOCKET tcp = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        requested = Loopback(0);
        const sockaddr_in originalTcp = requested;
        Check(poolBind(tcp, reinterpret_cast<const sockaddr*>(&requested), sizeof(requested)) == 0,
              "a TCP bind through the verified call site remains native");
        Check(std::memcmp(&requested, &originalTcp, sizeof(requested)) == 0 &&
                  SocketPort(tcp) != 0 && !InPool(SocketPort(tcp), first, count),
              "TCP type gating preserves port-zero allocation and caller storage");

        closesocket(tcp);
        closesocket(normal);
        closesocket(reused);
        for (SOCKET socket : allocated)
            if (socket != INVALID_SOCKET) closesocket(socket);
    }

    void TestInvalid(PoolBindFn poolBind)
    {
        SOCKET socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in requested = Loopback(0);
        const sockaddr_in original = requested;
        Check(poolBind(socket, reinterpret_cast<const sockaddr*>(&requested), sizeof(requested)) == 0,
              "invalid pool configuration leaves the verified call site unhooked");
        Check(std::memcmp(&requested, &original, sizeof(requested)) == 0 && SocketPort(socket) != 0,
              "declined mod preserves native bind behavior");
        closesocket(socket);
    }
}

int wmain(int argc, wchar_t** argv)
{
    const bool invalid = argc > 1 && wcscmp(argv[1], L"invalid") == 0;
    const bool unsafe = argc > 1 && wcscmp(argv[1], L"unsafe") == 0;
    if (unsafe) std::strstr(g_safeRelayFormat, "%u")[1] = 's';
    WSADATA winsock = {};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::puts("FAIL WSAStartup");
        return 2;
    }

    std::vector<SOCKET> udpReservations;
    std::vector<SOCKET> tcpGuards;
    unsigned short first = 0;
    Check(ReservePool(&first, 3, &udpReservations, &tcpGuards),
          "a free contiguous loopback UDP pool is reserved");

    HMODULE host = GetModuleHandleW(nullptr);
    void** bindSlot = FindBindImportSlot(host);
    Check(bindSlot != nullptr, "host bind IAT slot is discoverable");
    unsigned char* callSite = bindSlot ? BuildPoolBindFixture(bindSlot) : nullptr;
    Check(callSite != nullptr, "fixture contains an exact FF 15 bind-IAT call site");
    if (callSite && bindSlot) {
        uint32_t operand = 0;
        std::memcpy(&operand, callSite + 2, sizeof(operand));
        Check(callSite[0] == 0xFF && callSite[1] == 0x15 &&
                  operand == reinterpret_cast<uintptr_t>(bindSlot),
              "fixture call operand is the actual host bind import slot");
    }

    const std::wstring root = ExeDirectory();
    Check(callSite && WriteSymbols(root, host, callSite, bindSlot),
          "generated host symbols verify the exact fixture bytes and IAT slot");
    Check(WriteSettings(root, first, invalid), "test writes isolated pool settings");
    if (argc > 1 && wcscmp(argv[1], L"config") == 0) {
        const auto manifest = root + L"\\sdgo\\mods\\agent_udp_pool\\mod.ini";
        const auto config = root + L"\\sdgo\\mods\\agent_udp_pool\\config.ini";
        wchar_t text[32]; swprintf_s(text, L"%u", first);
        Check(WritePrivateProfileStringW(L"settings",L"FirstPort",text,config.c_str()) &&
              WritePrivateProfileStringW(L"settings",L"PortCount",L"3",config.c_str()), "write separate owner config");
        WritePrivateProfileStringW(L"settings",L"FirstPort",L"65535",manifest.c_str());
        WritePrivateProfileStringW(L"settings",L"PortCount",L"2",manifest.c_str());
    }
    if (g_failures == 0 && StartCore(root)) {
        CloseAll(&udpReservations);
        auto poolBind = reinterpret_cast<PoolBindFn>(static_cast<void*>(g_poolBindCode));
        if (invalid || unsafe) TestInvalid(poolBind);
        else TestValid(poolBind, first);
    }

    CloseAll(&udpReservations);
    CloseAll(&tcpGuards);
    WSACleanup();
    std::puts(g_failures ? "AGENT UDP POOL CHECKS FAILED" : "ALL AGENT UDP POOL CHECKS PASSED");
    return g_failures ? 1 : 0;
}
