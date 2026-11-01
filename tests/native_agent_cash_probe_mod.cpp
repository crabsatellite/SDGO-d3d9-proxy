// Copied-Agent test only. Creates one synthetic ephemeral user, then lets the
// unmodified Agent receive 0x064B through a real EXTEND BATTLE connection and
// forward it through a separate native client TCP connection.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "sdgo/sdgo_api.h"

namespace {

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;

using ConnectFn = DWORD(__thiscall*)(void*, const char*, WORD, DWORD);
using AddUserFn = int(__thiscall*)(void*, DWORD, const void*);
using RemoveUserFn = int(__thiscall*)(void*, DWORD);
using LookupFn = int(__thiscall*)(void*, DWORD, void*);
using CloseClientFn = int(__thiscall*)(void*, DWORD);

void** g_frame_proc_slot = nullptr;
uint8_t* g_users = nullptr;
ConnectFn g_connect = nullptr;
AddUserFn g_add_user = nullptr;
RemoveUserFn g_remove_user = nullptr;
LookupFn g_lookup_id = nullptr;
CloseClientFn g_close_tcp = nullptr;

constexpr DWORD kAccount = 4100643;

void PutDword(void* destination, DWORD value) {
    std::memcpy(destination, &value, sizeof(value));
}

bool WriteNewFile(const wchar_t* name, const char* text) {
    const std::wstring path =
        std::wstring(g_api->ModDirectory(g_mod)) + L"\\" + name;
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>(std::strlen(text));
    const bool ok =
        WriteFile(file, text, size, &written, nullptr) != 0 && written == size;
    CloseHandle(file);
    return ok;
}

bool ReadPeerResult(bool* pass) {
    if (!pass) return false;
    const std::wstring path =
        std::wstring(g_api->ModDirectory(g_mod)) + L"\\peer-result.json";
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    char text[2048] = {};
    DWORD read = 0;
    const bool ok =
        ReadFile(file, text, sizeof(text) - 1, &read, nullptr) != 0;
    CloseHandle(file);
    if (!ok) return false;
    *pass =
        std::strstr(text, "\"pass\": true") != nullptr ||
        std::strstr(text, "\"pass\":true") != nullptr;
    return true;
}

bool Ready(void** frame_proc) {
    if (!frame_proc) return false;
    __try {
        *frame_proc = *g_frame_proc_slot;
        return *frame_proc != nullptr &&
               *reinterpret_cast<void**>(
                   static_cast<uint8_t*>(*frame_proc) + 0xB0) != nullptr &&
               *reinterpret_cast<uint16_t*>(g_users) != 0 &&
               *reinterpret_cast<DWORD*>(g_users + 0x54) == 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Observation {
    bool ready = false;
    bool connected = false;
    bool user_added = false;
    bool peer_completed = false;
    bool peer_pass = false;
    bool cleanup = false;
    DWORD tcp_handle = 0;
    DWORD exception = 0;
};

bool AddUser(DWORD tcp_handle, DWORD* exception) {
    uint8_t source[0x788] = {};
    PutDword(source + 0x42C, kAccount);
    PutDword(source + 0x430, tcp_handle);
    PutDword(source + 0x434, 0);
    __try {
        return g_add_user(g_users, kAccount, source) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (exception) *exception = GetExceptionCode();
        return false;
    }
}

void Run(void* frame_proc, Observation* observation) {
    if (!observation) return;
    char port_text[16] = {};
    const DWORD length = GetEnvironmentVariableA(
        "SDGO_CASH_PROBE_CLIENT_PORT", port_text,
        static_cast<DWORD>(sizeof(port_text)));
    unsigned port = 0;
    if (!length || length >= sizeof(port_text) ||
        sscanf_s(port_text, "%u", &port) != 1 || port == 0 || port > 65535) {
        return;
    }

    observation->ready = true;
    const DWORD timer = *reinterpret_cast<DWORD*>(g_users + 0x54);
    *reinterpret_cast<DWORD*>(g_users + 0x54) = 0;
    bool added = false;
    do {
        observation->tcp_handle = g_connect(
            frame_proc, "127.88.0.1", static_cast<WORD>(port), 0);
        observation->connected = observation->tcp_handle != 0;
        if (!observation->connected) break;

        added = AddUser(observation->tcp_handle, &observation->exception);
        observation->user_added = added;
        if (!added) break;

        char ready_json[320] = {};
        sprintf_s(
            ready_json,
            "{\"scope\":\"copied Agent synthetic user only\","
            "\"ready\":true,\"account\":%lu,\"tcp_handle\":%lu}\n",
            kAccount, observation->tcp_handle);
        if (!WriteNewFile(L"ready.json", ready_json)) break;

        const ULONGLONG deadline = GetTickCount64() + 12000;
        while (GetTickCount64() < deadline) {
            bool peer_pass = false;
            if (ReadPeerResult(&peer_pass)) {
                observation->peer_completed = true;
                observation->peer_pass = peer_pass;
                break;
            }
            Sleep(25);
        }
    } while (false);

    bool cleanup = true;
    if (added) {
        cleanup = g_remove_user(g_users, kAccount) != 0;
    } else if (observation->tcp_handle) {
        cleanup = g_close_tcp(frame_proc, observation->tcp_handle) != 0;
    }
    uint8_t absent[0x788] = {};
    cleanup = !g_lookup_id(g_users, kAccount, absent) && cleanup;
    observation->cleanup = cleanup;
    *reinterpret_cast<DWORD*>(g_users + 0x54) = timer;
}

DWORD WINAPI Worker(void*) {
    void* frame_proc = nullptr;
    bool ready = false;
    for (unsigned i = 0; i < 240 && !(ready = Ready(&frame_proc)); ++i) {
        Sleep(50);
    }

    Observation observation;
    if (ready) {
        __try {
            Run(frame_proc, &observation);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            observation.exception = GetExceptionCode();
        }
    }

    const bool pass =
        observation.ready && observation.connected && observation.user_added &&
        observation.peer_completed && observation.peer_pass &&
        observation.cleanup && observation.exception == 0;
    char result[1000] = {};
    sprintf_s(
        result,
        "{\"scope\":\"copied Agent native BATTLE ingress to native client "
        "egress with a synthetic ephemeral user; no authentication/gameplay\","
        "\"ready\":%s,\"connected\":%s,\"user_added\":%s,"
        "\"peer_completed\":%s,\"peer_pass\":%s,"
        "\"native_user_cleanup\":%s,\"tcp_handle\":%lu,"
        "\"native_exception\":%lu,\"pass\":%s}\n",
        observation.ready ? "true" : "false",
        observation.connected ? "true" : "false",
        observation.user_added ? "true" : "false",
        observation.peer_completed ? "true" : "false",
        observation.peer_pass ? "true" : "false",
        observation.cleanup ? "true" : "false",
        observation.tcp_handle, observation.exception,
        pass ? "true" : "false");
    WriteNewFile(L"fixture-result.json", result);
    g_api->Log(
        g_mod, pass ? SDGO_LOG_INFO : SDGO_LOG_ERROR,
        "native Agent 064B forwarding probe %s", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

}  // namespace

SDGO_MOD_ENTRY {
    g_api = api;
    g_mod = mod;
    g_frame_proc_slot =
        static_cast<void**>(api->Symbol(mod, "agent.frame_proc"));
    g_users = static_cast<uint8_t*>(
        api->Symbol(mod, "agent.user_manager"));
    g_connect = reinterpret_cast<ConnectFn>(
        api->Symbol(mod, "agent.connect_client"));
    g_add_user = reinterpret_cast<AddUserFn>(
        api->Symbol(mod, "agent.add_user"));
    g_remove_user = reinterpret_cast<RemoveUserFn>(
        api->Symbol(mod, "agent.remove_user"));
    g_lookup_id = reinterpret_cast<LookupFn>(
        api->Symbol(mod, "agent.user_by_id"));
    g_close_tcp = reinterpret_cast<CloseClientFn>(
        api->Symbol(mod, "agent.close_client_tcp"));
    if (!g_frame_proc_slot || !g_users || !g_connect || !g_add_user ||
        !g_remove_user || !g_lookup_id || !g_close_tcp) {
        return 0;
    }
    HANDLE worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (!worker) return 0;
    CloseHandle(worker);
    return 1;
}
