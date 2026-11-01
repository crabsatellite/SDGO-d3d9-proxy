// Test-only native BattleServer startup probe.
//
// This DLL belongs only in an isolated startup fixture. It denies every
// outbound connect/WSASendTo call before BattleServer reaches its entry point,
// then verifies that the real battle_rewards.dll remains loaded and its ten
// settlement sites plus effective-damage helper are still detoured after
// SDBattlePlus initialization.
#include <winsock2.h>
#include <windows.h>

#include <array>
#include <cstdint>

#include "sdgo/sdgo_api.h"

namespace {

using ConnectFn = int(WSAAPI*)(SOCKET, const sockaddr*, int);
using SendToFn = int(WSAAPI*)(
    SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int,
    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
ConnectFn g_connect = nullptr;
SendToFn g_send_to = nullptr;
volatile LONG g_blocked_connect = 0;
volatile LONG g_blocked_send_to = 0;

constexpr std::array<const char*, 11> kRewardHooks = {{
    "battle.rewards.augment.personal",
    "battle.rewards.augment.team",
    "battle.rewards.augment.leader",
    "battle.rewards.augment.ai",
    "battle.rewards.augment.ai_ship",
    "battle.rewards.augment.mode13",
    "battle.rewards.augment.mode14",
    "battle.rewards.augment.tag",
    "battle.rewards.augment.death_grid",
    "battle.rewards.augment.mission",
    "battle.rewards.record_damage",
}};

int WSAAPI DenyConnect(SOCKET, const sockaddr*, int) {
    InterlockedIncrement(&g_blocked_connect);
    WSASetLastError(WSAEACCES);
    return SOCKET_ERROR;
}

int WSAAPI DenySendTo(
    SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int,
    LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE) {
    InterlockedIncrement(&g_blocked_send_to);
    WSASetLastError(WSAEACCES);
    return SOCKET_ERROR;
}

bool RelativeJumpTarget(const void* address, const void** target) {
    if (!address || !target) return false;
    __try {
        const auto* bytes = static_cast<const volatile uint8_t*>(address);
        if (bytes[0] != 0xE9) return false;
        const int32_t relative =
            *reinterpret_cast<const volatile int32_t*>(bytes + 1);
        *target = static_cast<const uint8_t*>(address) + 5 + relative;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

DWORD WINAPI AuditThread(void*) {
    Sleep(5000);

    HMODULE rewards_module = GetModuleHandleW(L"battle_rewards.dll");
    unsigned owned = 0;
    for (const char* name : kRewardHooks) {
        void* address = g_api->Symbol(g_mod, name);
        const void* target = nullptr;
        MEMORY_BASIC_INFORMATION memory = {};
        if (RelativeJumpTarget(address, &target) &&
            VirtualQuery(target, &memory, sizeof(memory)) == sizeof(memory) &&
            memory.AllocationBase == rewards_module) {
            ++owned;
        } else {
            g_api->Log(
                g_mod, SDGO_LOG_ERROR,
                "startup audit: reward hook %s is not owned by battle_rewards "
                "(address=%p target=%p owner=%p expected=%p)",
                name, address, target, memory.AllocationBase, rewards_module);
        }
    }

    const bool rewards_loaded = rewards_module != nullptr;
    const bool plus_loaded = GetModuleHandleW(L"SDBattlePlus.dll") != nullptr;
    g_api->Log(
        g_mod,
        rewards_loaded && plus_loaded && owned == kRewardHooks.size()
            ? SDGO_LOG_INFO
            : SDGO_LOG_ERROR,
        "startup audit: battle_rewards=%s SDBattlePlus=%s "
        "battle_rewards_owned_hooks=%u/%u "
        "blocked_connect=%ld blocked_wsasendto=%ld",
        rewards_loaded ? "loaded" : "missing",
        plus_loaded ? "loaded" : "missing", owned,
        static_cast<unsigned>(kRewardHooks.size()),
        InterlockedCompareExchange(&g_blocked_connect, 0, 0),
        InterlockedCompareExchange(&g_blocked_send_to, 0, 0));
    return 0;
}

}  // namespace

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION || !api->HookImport ||
        !api->Symbol || !api->Log) {
        return 0;
    }
    g_api = api;
    g_mod = mod;

    const bool connect_hook =
        api->HookImport(
            mod, "ws2_32.dll", "connect",
            reinterpret_cast<void*>(&DenyConnect),
            reinterpret_cast<void**>(&g_connect)) != 0;
    const bool send_to_hook =
        api->HookImport(
            mod, "ws2_32.dll", "WSASendTo",
            reinterpret_cast<void*>(&DenySendTo),
            reinterpret_cast<void**>(&g_send_to)) != 0;
    if (!connect_hook || !send_to_hook) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "test-only outbound guard failed: connect=%s WSASendTo=%s",
            connect_hook ? "on" : "off", send_to_hook ? "on" : "off");
        return 0;
    }

    HANDLE thread = CreateThread(nullptr, 0, &AuditThread, nullptr, 0, nullptr);
    if (!thread) {
        api->Log(
            mod, SDGO_LOG_ERROR,
            "test-only startup audit thread creation failed (%lu)",
            static_cast<unsigned long>(GetLastError()));
        return 0;
    }
    CloseHandle(thread);
    api->Log(
        mod, SDGO_LOG_INFO,
        "test-only outbound guard active: connect and WSASendTo are denied");
    return 1;
}
