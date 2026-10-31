// net_probe - diagnostic: logs what the client sends to the servers.
//
// Hooks the game's TCP packet send (net.send_packet, plain payload before any
// transport framing) and ws2_32!sendto (UDP battle traffic as it leaves the
// process). The first MaxPerOpcode packets of every 16-bit opcode are logged
// with their size and a hex dump, so a short session shows which packets carry
// attacks and damage and how the attacker is identified.
// Disable in sdgo/sdgo.ini ([mods] net_probe=0) when not investigating.

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>

#include "sdgo/sdgo_api.h"

namespace
{
    using SendPacketFn = int(__thiscall*)(void* session, int conn, const uint8_t* data, int size, int flags);
    using SendToFn = int(WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    SendPacketFn g_sendPacket = nullptr;
    SendToFn g_sendTo = nullptr;

    CRITICAL_SECTION g_lock;
    std::map<uint32_t, int> g_seen; // (channel << 16 | opcode) -> packets logged
    int g_maxPerOpcode = 60;
    int g_dumpBytes = 64;

    void Record(char channel, const uint8_t* data, int size)
    {
        if (!data || size < 2)
            return;
        uint16_t opcode = static_cast<uint16_t>(data[0] | (data[1] << 8));
        uint32_t key = (static_cast<uint32_t>(channel) << 16) | opcode;
        EnterCriticalSection(&g_lock);
        int count = ++g_seen[key];
        LeaveCriticalSection(&g_lock);
        if (count > g_maxPerOpcode)
            return;

        std::string hex;
        int bytes = size < g_dumpBytes ? size : g_dumpBytes;
        for (int i = 0; i < bytes; ++i) {
            char cell[4];
            sprintf_s(cell, "%02X%s", data[i], (i % 4 == 3) ? " " : "");
            hex += cell;
        }
        g_api->Log(g_mod, SDGO_LOG_INFO, "%c op %04X size %d #%d: %s%s", channel, opcode, size, count, hex.c_str(),
                   size > bytes ? "..." : "");
    }

    int __fastcall SendPacket(void* session, void* /*edx*/, int conn, const uint8_t* data, int size, int flags)
    {
        Record('T', data, size);
        return g_sendPacket(session, conn, data, size, flags);
    }

    int WSAAPI SendTo(SOCKET s, const char* data, int size, int flags, const sockaddr* to, int toLength)
    {
        Record('U', reinterpret_cast<const uint8_t*>(data), size);
        return g_sendTo(s, data, size, flags, to, toLength);
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    InitializeCriticalSection(&g_lock);
    g_maxPerOpcode = api->SettingInt(mod, "MaxPerOpcode", 60);
    g_dumpBytes = api->SettingInt(mod, "DumpBytes", 64);

    bool tcp = api->HookCode(mod, "net.send_packet", reinterpret_cast<void*>(&SendPacket),
                             reinterpret_cast<void**>(&g_sendPacket)) != 0;
    bool udp = api->HookImport(mod, "ws2_32.dll", "sendto", reinterpret_cast<void*>(&SendTo),
                               reinterpret_cast<void**>(&g_sendTo)) != 0;
    api->Log(mod, SDGO_LOG_INFO, "probing TCP %s, UDP %s; first %d packets per opcode", tcp ? "on" : "off",
             udp ? "on" : "off", g_maxPerOpcode);
    return tcp || udp;
}
