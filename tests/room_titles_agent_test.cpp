// Actual-core integration test for room_titles_agent.dll (no AgentServer needed).
// The executable supplies hookable native-style Agent functions and writes a
// symbol file for their exact addresses before loading the real core and Mod.
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sdgo/sdgo_api.h"

#if defined(ROOM_TITLES_AGENT_CONFLICT_MOD)

namespace
{
using SendFn = int(__thiscall*)(void*, DWORD, const void*, int, DWORD);
SendFn g_send = nullptr;

int __fastcall Send(void* self, void*, DWORD destination, const void* packet,
                    int size, DWORD flags)
{
    return g_send(self, destination, packet, size, flags);
}
}  // namespace

SDGO_MOD_ENTRY
{
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, HookCode) + sizeof(api->HookCode) ||
        !api->HookCode || !api->Log)
        return 0;
    if (!api->HookCode(mod, "room_titles.agent_send_server",
                       reinterpret_cast<void*>(&Send),
                       reinterpret_cast<void**>(&g_send)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "test conflict owns room_titles.agent_send_server");
    return 1;
}

#else

#include "../mods/room_metadata/common/room_metadata_common.h"

namespace
{
namespace metadata = sdgo::room_metadata;

using DispatchFn = int(__cdecl*)(DWORD, DWORD, DWORD, void*, int);
using SendFn = int(__thiscall*)(void*, DWORD, const void*, int, DWORD);

constexpr std::size_t kNativeBytes = 0x53;
constexpr std::size_t kHelloNativeBytes = 22;
constexpr std::size_t kRoomListNativeBytes = 0x5B5;
constexpr std::uint16_t kTeamListResponse = 0x0824;
constexpr std::size_t kTeamListHeaderBytes = 0x13;
constexpr std::size_t kTeamListRowBytes = 0x123;
constexpr std::uint16_t kCreateRoom = 0x0640;
constexpr std::uint16_t kRoomList = 0x0617;
constexpr std::uint16_t kHello = 0x07FB;
constexpr std::uint16_t kMissionHello = 0x0A08;
constexpr DWORD kOuterDispatch = 0x11111111;
constexpr DWORD kInnerDispatch = 0x22222222;
constexpr DWORD kDestinationBase = 0x61000000;
constexpr DWORD kFlagsBase = 0x72000000;
constexpr DWORD kExtraDestination = 0x13572468;
constexpr DWORD kExtraFlags = 0x24681357;
constexpr DWORD kOtherDestination = 0x89ABCDEF;
constexpr DWORD kOtherFlags = 0x10203040;

enum class Scenario
{
    Normal,
    ScopeGates,
    Threaded,
    Nested,
    Retry,
    Reentrant
};

enum class DispatchPath
{
    ClientToServer,
    ServerToClient
};

enum class SendPath
{
    ToServer,
    ToClient
};

struct DispatchCapture
{
    DispatchPath path = DispatchPath::ClientToServer;
    DWORD argument0 = 0;
    DWORD argument1 = 0;
    DWORD handle = 0;
    const void* pointer = nullptr;
    int size = 0;
    DWORD thread = 0;
};

struct SendCapture
{
    SendPath path = SendPath::ToServer;
    void* self = nullptr;
    DWORD destination = 0;
    const void* pointer = nullptr;
    int size = 0;
    DWORD flags = 0;
    DWORD thread = 0;
    std::vector<std::uint8_t> bytes;
};

int g_failures = 0;
CRITICAL_SECTION g_capture_lock;
std::vector<DispatchCapture> g_dispatches;
std::vector<SendCapture> g_sends;
Scenario g_scenario = Scenario::Normal;
HANDLE g_threads_ready_event = nullptr;
LONG g_threads_ready = 0;
std::vector<std::uint8_t> g_nested_packet;
const void* g_scope_same_opcode_pointer = nullptr;
const void* g_scope_other_opcode_pointer = nullptr;
const void* g_reentrant_native_pointer = nullptr;
int g_reentrant_native_size = 0;
DWORD g_reentrant_destination = 0;
DWORD g_reentrant_flags = 0;
LONG g_send_server_original_calls = 0;
LONG g_reentrant_entered = 0;
LONG g_reentrant_result = -1;
std::uint8_t g_agent_object[32] = {};

void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

std::uint16_t Read16(const void* pointer)
{
    std::uint16_t value = 0;
    std::memcpy(&value, pointer, sizeof(value));
    return value;
}

std::uint32_t Read32(const void* pointer)
{
    std::uint32_t value = 0;
    std::memcpy(&value, pointer, sizeof(value));
    return value;
}

void Write16(void* pointer, std::uint16_t value)
{
    std::memcpy(pointer, &value, sizeof(value));
}

void Write32(void* pointer, std::uint32_t value)
{
    std::memcpy(pointer, &value, sizeof(value));
}

DWORD NativeUserId(DWORD handle)
{
    return 0x51000000u | (handle & 0x0000FFFFu);
}

DWORD NativeDestination(DWORD handle)
{
    return kDestinationBase | (handle & 0x0000FFFFu);
}

DWORD NativeFlags(DWORD handle)
{
    return kFlagsBase | (handle & 0x0000FFFFu);
}

std::uint8_t NativeTailByte(DWORD handle, std::size_t index)
{
    return static_cast<std::uint8_t>(0xC0u + index +
                                     static_cast<std::uint8_t>(handle));
}

int SendResult(int size)
{
    return 0x31000000 | (size & 0x0000FFFF);
}

void RecordDispatch(DispatchPath path, DWORD argument0, DWORD argument1,
                    DWORD handle, const void* packet, int size)
{
    EnterCriticalSection(&g_capture_lock);
    g_dispatches.push_back(
        {path, argument0, argument1, handle, packet, size,
         GetCurrentThreadId()});
    LeaveCriticalSection(&g_capture_lock);
}

void RecordSend(SendPath path, void* self, DWORD destination,
                const void* packet, int size, DWORD flags)
{
    SendCapture capture;
    capture.path = path;
    capture.self = self;
    capture.destination = destination;
    capture.pointer = packet;
    capture.size = size;
    capture.flags = flags;
    capture.thread = GetCurrentThreadId();
    if (packet && size > 0 && size <= 65535) {
        const auto* bytes = static_cast<const std::uint8_t*>(packet);
        capture.bytes.assign(bytes, bytes + size);
    }
    EnterCriticalSection(&g_capture_lock);
    g_sends.push_back(std::move(capture));
    LeaveCriticalSection(&g_capture_lock);
}

extern "C" __declspec(noinline) int __fastcall AgentSendToServer(
    void* self, void*, DWORD destination, const void* packet, int size,
    DWORD flags)
{
    RecordSend(SendPath::ToServer, self, destination, packet, size, flags);
    const LONG call =
        InterlockedIncrement(&g_send_server_original_calls);
    if (g_scenario == Scenario::Reentrant &&
        InterlockedCompareExchange(&g_reentrant_entered, 1, 0) == 0) {
        auto send = reinterpret_cast<SendFn>(&AgentSendToServer);
        InterlockedExchange(
            &g_reentrant_result,
            send(self, g_reentrant_destination,
                 g_reentrant_native_pointer, g_reentrant_native_size,
                 g_reentrant_flags));
    }
    if (g_scenario == Scenario::Retry && call == 1)
        return 0;
    return SendResult(size);
}

extern "C" __declspec(noinline) int __fastcall AgentSendToClient(
    void* self, void*, DWORD destination, const void* packet, int size,
    DWORD flags)
{
    RecordSend(SendPath::ToClient, self, destination, packet, size, flags);
    return SendResult(size);
}

std::vector<std::uint8_t> NativePacket(
    std::uint8_t seed, std::size_t size = kNativeBytes,
    std::uint16_t opcode = kCreateRoom)
{
    std::vector<std::uint8_t> packet(size);
    for (std::size_t index = 0; index < packet.size(); ++index)
        packet[index] = static_cast<std::uint8_t>(seed + index * 3u);
    if (size >= 2)
        Write16(packet.data(), opcode);
    if (size >= 6)
        Write32(packet.data() + 2, 0xDEADBEEFu);
    if (opcode == kCreateRoom && size >= kNativeBytes) {
        for (std::size_t index = 0; index < 18; ++index)
            packet[0x41 + index] =
                static_cast<std::uint8_t>(0x20u + index);
    }
    return packet;
}

metadata::Message CreationMessage(const std::string& title,
                                  std::uint32_t request_id)
{
    metadata::Message message;
    message.kind = metadata::MessageKind::SetTitle;
    message.request_id = request_id;
    message.title = title;
    return message;
}

std::vector<std::uint8_t> ExtendedPacket(
    const std::vector<std::uint8_t>& native, const std::string& title,
    std::uint32_t request_id)
{
    std::vector<std::uint8_t> packet;
    const bool encoded =
        metadata::EncodePacket(native.data(), native.size(),
                               CreationMessage(title, request_id), &packet);
    Check(encoded, "common protocol encodes the test creation packet");
    return packet;
}

std::vector<std::uint8_t> MessagePacket(
    const std::vector<std::uint8_t>& native,
    const metadata::Message& message)
{
    std::vector<std::uint8_t> packet;
    Check(metadata::EncodePacket(native.data(), native.size(), message,
                                 &packet),
          "common protocol encodes the test metadata packet");
    return packet;
}

void MutateClientPacket(void* packet, DWORD handle, std::uint16_t opcode)
{
    auto* bytes = static_cast<std::uint8_t*>(packet);
    Write32(bytes + 2, NativeUserId(handle));
    if (opcode == kCreateRoom) {
        for (std::size_t index = 0; index < 18; ++index)
            bytes[0x41 + index] = NativeTailByte(handle, index);
    }
}

DWORD ServerResponseToken(DWORD handle)
{
    return 0xA1000000u | (handle & 0x0000FFFFu);
}

std::uint8_t ServerTailByte(DWORD handle, std::size_t index)
{
    return static_cast<std::uint8_t>(0x70u + index +
                                     static_cast<std::uint8_t>(handle));
}

void MutateServerPacket(void* packet, std::size_t native_size,
                        DWORD handle)
{
    auto* bytes = static_cast<std::uint8_t*>(packet);
    Write32(bytes + 2, ServerResponseToken(handle));
    for (std::size_t index = 0;
         native_size >= 24 && index < 18; ++index)
        bytes[native_size - 18 + index] =
            ServerTailByte(handle, index);
}

std::size_t ServerNativeSize(const void* packet, int size)
{
    if (!packet || size < 2)
        return 0;
    const std::uint16_t opcode = Read16(packet);
    if (opcode == kRoomList)
        return kRoomListNativeBytes;
    if (opcode == 0x0A33)
        return 0x21FC;
    if (opcode == 0x054E || opcode == 0x09F4 || opcode == 0x07AC ||
        opcode == 0x08FB || opcode == 0x0709 || opcode == 0x0916)
        return 8;
    if (opcode != kTeamListResponse ||
        size < static_cast<int>(kTeamListHeaderBytes))
        return 0;
    const auto* bytes = static_cast<const std::uint8_t*>(packet);
    if (bytes[0x12] > 10)
        return 0;
    return kTeamListHeaderBytes +
           static_cast<std::size_t>(bytes[0x12]) *
               kTeamListRowBytes;
}

void EmitScopeGateCalls(void* packet)
{
    std::uint8_t same_opcode[kNativeBytes] = {};
    for (std::size_t index = 0; index < kNativeBytes; ++index)
        same_opcode[index] = static_cast<std::uint8_t>(0x30u + index);
    Write16(same_opcode, kCreateRoom);
    g_scope_same_opcode_pointer = same_opcode;
    AgentSendToServer(g_agent_object, nullptr, kExtraDestination,
                      same_opcode,
                      static_cast<int>(sizeof(same_opcode)), kExtraFlags);

    auto* native = static_cast<std::uint8_t*>(packet);
    const std::uint16_t saved_opcode = Read16(native);
    Write16(native, 0x0641);
    g_scope_other_opcode_pointer = native;
    AgentSendToServer(g_agent_object, nullptr, kOtherDestination, native,
                      static_cast<int>(kNativeBytes), kOtherFlags);
    Write16(native, saved_opcode);
}

extern "C" __declspec(noinline) int __cdecl AgentDispatch(
    DWORD argument0, DWORD argument1, DWORD handle, void* packet, int size)
{
    RecordDispatch(DispatchPath::ClientToServer, argument0, argument1,
                   handle, packet, size);
    if (!packet || size < 2)
        return -100;

    const std::uint16_t opcode = Read16(packet);
    const std::size_t native_size =
        opcode == kCreateRoom
            ? kNativeBytes
            : ((opcode == kHello || opcode == kMissionHello)
                   ? kHelloNativeBytes
                   : opcode == 0x089E ? 0x1D : opcode == 0x050F ? 0x37 :
                     opcode == 0x09ED ? 0x3E : 0);
    if (!native_size || size < static_cast<int>(native_size))
        return -101;

    if (opcode == kCreateRoom &&
        g_scenario == Scenario::Nested && argument0 == kOuterDispatch &&
        !g_nested_packet.empty()) {
        auto dispatch = reinterpret_cast<DispatchFn>(&AgentDispatch);
        dispatch(kInnerDispatch, 0xB2B2B2B2, 0x0022,
                 g_nested_packet.data(),
                 static_cast<int>(g_nested_packet.size()));
    }

    if (g_scenario == Scenario::Threaded) {
        if (InterlockedIncrement(&g_threads_ready) == 2)
            SetEvent(g_threads_ready_event);
        WaitForSingleObject(g_threads_ready_event, 5000);
    }

    MutateClientPacket(packet, handle, opcode);
    if (opcode == kCreateRoom && g_scenario == Scenario::ScopeGates)
        EmitScopeGateCalls(packet);

    auto send = reinterpret_cast<SendFn>(&AgentSendToServer);
    const DWORD destination = NativeDestination(handle);
    const DWORD flags = NativeFlags(handle);
    if (g_scenario == Scenario::Reentrant) {
        g_reentrant_native_pointer = packet;
        g_reentrant_native_size = static_cast<int>(native_size);
        g_reentrant_destination = destination;
        g_reentrant_flags = flags;
    }
    const int first =
        send(g_agent_object, destination, packet,
             static_cast<int>(native_size), flags);
    if (g_scenario == Scenario::Retry && first == 0)
        return send(g_agent_object, destination, packet,
                    static_cast<int>(native_size), flags);
    return first;
}

extern "C" __declspec(noinline) int __cdecl AgentServerDispatch(
    DWORD argument0, DWORD argument1, DWORD handle, void* packet, int size)
{
    RecordDispatch(DispatchPath::ServerToClient, argument0, argument1,
                   handle, packet, size);
    const std::size_t native_size =
        ServerNativeSize(packet, size);
    if (!native_size || size < static_cast<int>(native_size))
        return -201;

    MutateServerPacket(packet, native_size, handle);
    auto send = reinterpret_cast<SendFn>(&AgentSendToClient);
    return send(g_agent_object, NativeDestination(handle), packet,
                static_cast<int>(native_size),
                NativeFlags(handle));
}

void ResetCaptures(Scenario scenario)
{
    EnterCriticalSection(&g_capture_lock);
    g_dispatches.clear();
    g_sends.clear();
    LeaveCriticalSection(&g_capture_lock);
    g_scenario = scenario;
    g_scope_same_opcode_pointer = nullptr;
    g_scope_other_opcode_pointer = nullptr;
    g_reentrant_native_pointer = nullptr;
    g_reentrant_native_size = 0;
    g_reentrant_destination = 0;
    g_reentrant_flags = 0;
    InterlockedExchange(&g_send_server_original_calls, 0);
    InterlockedExchange(&g_reentrant_entered, 0);
    InterlockedExchange(&g_reentrant_result, -1);
    g_nested_packet.clear();
    InterlockedExchange(&g_threads_ready, 0);
    if (g_threads_ready_event)
        ResetEvent(g_threads_ready_event);
}

std::vector<DispatchCapture> Dispatches()
{
    EnterCriticalSection(&g_capture_lock);
    std::vector<DispatchCapture> copy = g_dispatches;
    LeaveCriticalSection(&g_capture_lock);
    return copy;
}

std::vector<SendCapture> Sends()
{
    EnterCriticalSection(&g_capture_lock);
    std::vector<SendCapture> copy = g_sends;
    LeaveCriticalSection(&g_capture_lock);
    return copy;
}

bool NativeBytesMatch(const std::vector<std::uint8_t>& actual,
                      const std::vector<std::uint8_t>& before, DWORD handle)
{
    if (actual.size() != before.size() || actual.size() < 6)
        return false;
    const std::uint16_t opcode = Read16(before.data());
    for (std::size_t index = 0; index < kNativeBytes; ++index) {
        if (index >= actual.size())
            break;
        std::uint8_t expected = before[index];
        if (index >= 2 && index < 6) {
            std::uint8_t user[4] = {};
            const DWORD id = NativeUserId(handle);
            std::memcpy(user, &id, sizeof(user));
            expected = user[index - 2];
        } else if (opcode == kCreateRoom &&
                   index >= 0x41 && index < 0x53) {
            expected = NativeTailByte(handle, index - 0x41);
        }
        if (actual[index] != expected)
            return false;
    }
    return true;
}

bool ServerBytesMatch(const std::vector<std::uint8_t>& actual,
                      const std::vector<std::uint8_t>& before, DWORD handle)
{
    if (actual.size() != before.size() || actual.size() < 6)
        return false;
    for (std::size_t index = 0; index < actual.size(); ++index) {
        std::uint8_t expected = before[index];
        if (index >= 2 && index < 6) {
            std::uint8_t token[4] = {};
            const DWORD value = ServerResponseToken(handle);
            std::memcpy(token, &value, sizeof(token));
            expected = token[index - 2];
        } else if (actual.size() >= 24 &&
                   index >= actual.size() - 18) {
            expected =
                ServerTailByte(handle,
                               index - (actual.size() - 18));
        }
        if (actual[index] != expected)
            return false;
    }
    return true;
}

bool DecodeExtended(const SendCapture& capture,
                    metadata::TrailerView* trailer,
                    metadata::Message* message)
{
    return !capture.bytes.empty() &&
           metadata::DecodePacket(capture.bytes.data(), capture.bytes.size(),
                                  trailer, message);
}

bool WriteTextFile(const std::wstring& path, const std::string& text)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok =
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                  nullptr) &&
        written == text.size();
    CloseHandle(file);
    return ok;
}

std::wstring ExeDirectory()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash)
        *slash = L'\0';
    return path;
}

std::wstring ExeName()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH * 2);
    wchar_t* slash = wcsrchr(path, L'\\');
    return slash ? slash + 1 : path;
}

std::string ExpectedBytes(const void* address, std::size_t count)
{
    const auto* bytes = static_cast<const std::uint8_t*>(address);
    std::string text;
    char byte[4] = {};
    for (std::size_t index = 0; index < count; ++index) {
        sprintf_s(byte, "%02X", bytes[index]);
        if (!text.empty())
            text.push_back(' ');
        text += byte;
    }
    return text;
}

bool WriteSymbols(const std::wstring& root)
{
    HMODULE host = GetModuleHandleW(nullptr);
    auto* base = reinterpret_cast<std::uint8_t*>(host);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt =
        reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto* dispatch =
        reinterpret_cast<const std::uint8_t*>(&AgentDispatch);
    const auto* send =
        reinterpret_cast<const std::uint8_t*>(&AgentSendToServer);
    const auto* server_dispatch =
        reinterpret_cast<const std::uint8_t*>(&AgentServerDispatch);
    const auto* send_client =
        reinterpret_cast<const std::uint8_t*>(&AgentSendToClient);
    const std::uintptr_t dispatch_rva = dispatch - base;
    const std::uintptr_t send_rva = send - base;
    const std::uintptr_t server_dispatch_rva = server_dispatch - base;
    const std::uintptr_t send_client_rva = send_client - base;
    if (dispatch_rva >= nt->OptionalHeader.SizeOfImage ||
        send_rva >= nt->OptionalHeader.SizeOfImage ||
        server_dispatch_rva >= nt->OptionalHeader.SizeOfImage ||
        send_client_rva >= nt->OptionalHeader.SizeOfImage)
        return false;

    std::wstring stem = ExeName();
    const std::size_t dot = stem.find_last_of(L'.');
    if (dot != std::wstring::npos)
        stem.resize(dot);
    wchar_t host_key[256] = {};
    swprintf_s(host_key, L"%s-%08X-%08X", stem.c_str(),
               nt->FileHeader.TimeDateStamp,
               nt->OptionalHeader.SizeOfImage);

    const std::wstring sdgo = root + L"\\sdgo";
    const std::wstring symbols = sdgo + L"\\symbols";
    CreateDirectoryW(sdgo.c_str(), nullptr);
    CreateDirectoryW(symbols.c_str(), nullptr);

    const std::string dispatch_expect = ExpectedBytes(dispatch, 16);
    const std::string send_expect = ExpectedBytes(send, 16);
    const std::string server_dispatch_expect =
        ExpectedBytes(server_dispatch, 16);
    const std::string send_client_expect =
        ExpectedBytes(send_client, 16);
    char ini[4096] = {};
    sprintf_s(
        ini,
        "[host]\r\n"
        "image=room_titles_agent_test.exe\r\n"
        "timestamp=0x%08X\r\n"
        "size_of_image=0x%08X\r\n\r\n"
        "[room_titles.agent_client_dispatch]\r\n"
        "kind=code\r\n"
        "rva=0x%08X\r\n"
        "expect=%s\r\n\r\n"
        "[room_titles.agent_send_server]\r\n"
        "kind=code\r\n"
        "rva=0x%08X\r\n"
        "expect=%s\r\n\r\n"
        "[room_titles.agent_server_dispatch]\r\n"
        "kind=code\r\n"
        "rva=0x%08X\r\n"
        "expect=%s\r\n\r\n"
        "[room_titles.agent_send_client]\r\n"
        "kind=code\r\n"
        "rva=0x%08X\r\n"
        "expect=%s\r\n",
        nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage,
        static_cast<unsigned>(dispatch_rva), dispatch_expect.c_str(),
        static_cast<unsigned>(send_rva), send_expect.c_str(),
        static_cast<unsigned>(server_dispatch_rva),
        server_dispatch_expect.c_str(),
        static_cast<unsigned>(send_client_rva),
        send_client_expect.c_str());
    return WriteTextFile(symbols + L"\\" + host_key + L".ini", ini);
}

bool StartCore(const std::wstring& root)
{
    HMODULE core = LoadLibraryW((root + L"\\sdgo\\core.dll").c_str());
    auto init =
        core ? reinterpret_cast<SdgoCoreInitFn>(
                   GetProcAddress(core, "SdgoCoreInit"))
             : nullptr;
    if (!init) {
        std::printf("FAIL core export missing (Win32 %lu)\n", GetLastError());
        ++g_failures;
        return false;
    }
    SdgoBoot boot = {sizeof(SdgoBoot), "room_titles_agent_test",
                     GetModuleHandleW(nullptr)};
    Check(init(&boot) != 0,
          "real core initializes with generated host symbols");
    return g_failures == 0;
}

int CallDispatch(DWORD argument0, DWORD argument1, DWORD handle,
                 std::vector<std::uint8_t>* packet)
{
    auto dispatch = reinterpret_cast<DispatchFn>(&AgentDispatch);
    return dispatch(argument0, argument1, handle, packet->data(),
                    static_cast<int>(packet->size()));
}

int CallServerDispatch(DWORD argument0, DWORD argument1, DWORD handle,
                       std::vector<std::uint8_t>* packet)
{
    auto dispatch =
        reinterpret_cast<DispatchFn>(&AgentServerDispatch);
    return dispatch(argument0, argument1, handle, packet->data(),
                    static_cast<int>(packet->size()));
}

void TestEnabledExtension()
{
    ResetCaptures(Scenario::Normal);
    const DWORD handle = 0x0034;
    const auto native = NativePacket(0x11);
    auto packet =
        ExtendedPacket(native, "mission: long title bridge", 101);
    const auto original = packet;
    const int result =
        CallDispatch(0xA0A0A0A0, 0xB0B0B0B0, handle, &packet);
    const auto dispatches = Dispatches();
    const auto sends = Sends();

    Check(dispatches.size() == 1 &&
              dispatches[0].argument0 == 0xA0A0A0A0 &&
              dispatches[0].argument1 == 0xB0B0B0B0 &&
              dispatches[0].handle == handle &&
              dispatches[0].size == static_cast<int>(kNativeBytes) &&
              dispatches[0].pointer != packet.data(),
          "enabled bridge gives the native dispatcher a private 83-byte copy");
    Check(packet == original,
          "extension forwarding leaves the caller's complete buffer unchanged");
    Check(sends.size() == 1 &&
              sends[0].destination == NativeDestination(handle) &&
              sends[0].flags == NativeFlags(handle) &&
              sends[0].self == g_agent_object &&
              result == SendResult(sends[0].size),
          "extension forwarding preserves native destination, flags, self and return value");

    metadata::TrailerView trailer;
    metadata::Message message;
    const bool decoded =
        sends.size() == 1 && DecodeExtended(sends[0], &trailer, &message);
    Check(decoded && trailer.native_size == kNativeBytes &&
              message.kind == metadata::MessageKind::SetTitle &&
              message.request_id == 101 &&
              message.title == "mission: long title bridge",
          "enabled bridge restores the validated title extension after the native rebuild");
    const std::vector<std::uint8_t> forwarded_native =
        decoded ? std::vector<std::uint8_t>(
                      trailer.native_data,
                      trailer.native_data + trailer.native_size)
                : std::vector<std::uint8_t>();
    Check(NativeBytesMatch(forwarded_native, native, handle),
          "restored packet keeps the authoritative user ID and complete native tail");
}

void TestRejectedPackets()
{
    ResetCaptures(Scenario::Normal);
    const auto native = NativePacket(0x22);
    auto truncated = ExtendedPacket(native, "truncated", 102);
    truncated.erase(truncated.end() - 5);
    const auto truncated_original = truncated;
    const int truncated_result =
        CallDispatch(1, 2, 3, &truncated);
    Check(truncated_result == 0 && Dispatches().empty() && Sends().empty() &&
              truncated == truncated_original,
          "truncated trailer is rejected before native dispatch");

    ResetCaptures(Scenario::Normal);
    metadata::Message query;
    query.kind = metadata::MessageKind::Query;
    query.server_epoch = 7;
    query.room_id = 8;
    query.generation = 9;
    query.request_id = 103;
    std::vector<std::uint8_t> wrong_kind;
    Check(metadata::EncodePacket(native.data(), native.size(), query,
                                 &wrong_kind),
          "common protocol encodes a valid non-SetTitle message");
    const auto wrong_original = wrong_kind;
    const int wrong_result =
        CallDispatch(4, 5, 6, &wrong_kind);
    Check(wrong_result == 0 && Dispatches().empty() && Sends().empty() &&
              wrong_kind == wrong_original,
          "wrong metadata message kind is rejected before native dispatch");
}

void TestNoTrailerCompatibility()
{
    const std::vector<std::uint8_t> suffix = {
        0xD1, 0x42, 0x93, 0x24, 0xF5, 0x66, 0xB7, 0x28};

    ResetCaptures(Scenario::Normal);
    const DWORD client_handle = 0x003D;
    const auto client_native = NativePacket(0x2A);
    auto client_packet = client_native;
    client_packet.insert(client_packet.end(), suffix.begin(), suffix.end());
    const auto client_original = client_packet;
    const int client_result =
        CallDispatch(0x0A0B0C0D, 0x1A1B1C1D, client_handle,
                     &client_packet);
    const auto client_dispatches = Dispatches();
    const auto client_sends = Sends();
    Check(metadata::ParseTrailer(client_original.data(),
                                 client_original.size())
                  .status == metadata::TrailerStatus::NoTrailer &&
              client_dispatches.size() == 1 &&
              client_dispatches[0].path ==
                  DispatchPath::ClientToServer &&
              client_dispatches[0].pointer == client_packet.data() &&
              client_dispatches[0].size ==
                  static_cast<int>(client_packet.size()),
          "known oversized request without RTMD reaches native dispatch with its original pointer and full size");
    Check(client_packet.size() == client_original.size() &&
              NativeBytesMatch(
                  std::vector<std::uint8_t>(
                      client_packet.begin(),
                      client_packet.begin() + kNativeBytes),
                  client_native, client_handle) &&
              std::memcmp(client_packet.data() + kNativeBytes,
                          client_original.data() + kNativeBytes,
                          suffix.size()) == 0,
          "native request dispatch may update its stock prefix while preserving the unrelated suffix");
    Check(client_sends.size() == 1 &&
              client_sends[0].path == SendPath::ToServer &&
              client_sends[0].pointer == client_packet.data() &&
              client_sends[0].size ==
                  static_cast<int>(kNativeBytes) &&
              metadata::ParseTrailer(client_sends[0].bytes.data(),
                                     client_sends[0].bytes.size())
                      .status == metadata::TrailerStatus::NoTrailer &&
              NativeBytesMatch(client_sends[0].bytes, client_native,
                               client_handle) &&
              client_result ==
                  SendResult(static_cast<int>(kNativeBytes)),
          "native request rebuild stays fixed-size and does not invent room-title metadata");

    ResetCaptures(Scenario::Normal);
    const DWORD server_handle = 0x005E;
    const auto server_native =
        NativePacket(0x3B, kRoomListNativeBytes, kRoomList);
    auto server_packet = server_native;
    server_packet.insert(server_packet.end(), suffix.begin(), suffix.end());
    const auto server_original = server_packet;
    const int server_result =
        CallServerDispatch(0x2A2B2C2D, 0x3A3B3C3D, server_handle,
                           &server_packet);
    const auto server_dispatches = Dispatches();
    const auto server_sends = Sends();
    Check(metadata::ParseTrailer(server_original.data(),
                                 server_original.size())
                  .status == metadata::TrailerStatus::NoTrailer &&
              server_dispatches.size() == 1 &&
              server_dispatches[0].path ==
                  DispatchPath::ServerToClient &&
              server_dispatches[0].pointer == server_packet.data() &&
              server_dispatches[0].size ==
                  static_cast<int>(server_packet.size()),
          "known oversized response without RTMD reaches native dispatch with its original pointer and full size");
    Check(server_packet.size() == server_original.size() &&
              ServerBytesMatch(
                  std::vector<std::uint8_t>(
                      server_packet.begin(),
                      server_packet.begin() + kRoomListNativeBytes),
                  server_native, server_handle) &&
              std::memcmp(
                  server_packet.data() + kRoomListNativeBytes,
                  server_original.data() + kRoomListNativeBytes,
                  suffix.size()) == 0,
          "native response dispatch may update its stock prefix while preserving the unrelated suffix");
    Check(server_sends.size() == 1 &&
              server_sends[0].path == SendPath::ToClient &&
              server_sends[0].pointer == server_packet.data() &&
              server_sends[0].size ==
                  static_cast<int>(kRoomListNativeBytes) &&
              metadata::ParseTrailer(server_sends[0].bytes.data(),
                                     server_sends[0].bytes.size())
                      .status == metadata::TrailerStatus::NoTrailer &&
              ServerBytesMatch(server_sends[0].bytes, server_native,
                               server_handle) &&
              server_result ==
                  SendResult(static_cast<int>(kRoomListNativeBytes)),
          "native response rebuild stays fixed-size and does not invent room-title metadata");
}

void TestVanillaPacket()
{
    ResetCaptures(Scenario::Normal);
    const DWORD handle = 0x0045;
    const auto native = NativePacket(0x33);
    auto packet = native;
    const int result =
        CallDispatch(0x10, 0x20, handle, &packet);
    const auto dispatches = Dispatches();
    const auto sends = Sends();
    Check(dispatches.size() == 1 && dispatches[0].pointer == packet.data() &&
              dispatches[0].size == static_cast<int>(kNativeBytes),
          "vanilla 83-byte request follows the native dispatcher path");
    Check(sends.size() == 1 && sends[0].pointer == packet.data() &&
              sends[0].size == static_cast<int>(kNativeBytes) &&
              metadata::ParseTrailer(sends[0].bytes.data(),
                                     sends[0].bytes.size())
                      .status == metadata::TrailerStatus::NoTrailer &&
              result == SendResult(static_cast<int>(kNativeBytes)),
          "vanilla request is sent without an extension or pointer substitution");
    Check(NativeBytesMatch(sends[0].bytes, native, handle),
          "vanilla request retains the fixture's complete native rebuild");
}

void TestHelloRequest()
{
    const std::uint16_t carriers[] = {kHello, kMissionHello, kCreateRoom};
    for (std::size_t index = 0; index < std::size(carriers); ++index) {
        ResetCaptures(Scenario::Normal);
        const DWORD handle = static_cast<DWORD>(0x004F + index);
        const size_t nativeBytes = carriers[index] == kCreateRoom ? kNativeBytes : kHelloNativeBytes;
        const auto native =
            NativePacket(static_cast<std::uint8_t>(0x3A + index),
                         nativeBytes, carriers[index]);
        metadata::Message hello;
        hello.kind = metadata::MessageKind::Hello;
        hello.request_id = 0x11223344u + static_cast<std::uint32_t>(index);
        auto packet = MessagePacket(native, hello);
        const auto original = packet;
        const int result =
            CallDispatch(0x71, 0x72, handle, &packet);
        const auto dispatches = Dispatches();
        const auto sends = Sends();

        Check(dispatches.size() == 1 &&
                  dispatches[0].path == DispatchPath::ClientToServer &&
                  dispatches[0].size ==
                      static_cast<int>(nativeBytes) &&
                  dispatches[0].pointer != packet.data(),
              carriers[index] == kHello
                  ? "Battle-list Hello bridge gives the native dispatcher a private 22-byte copy"
                  : "mission Hello bridge preserves its exact native request prefix");
        metadata::TrailerView trailer;
        metadata::Message decoded_message;
        const bool decoded =
            sends.size() == 1 &&
            sends[0].path == SendPath::ToServer &&
            DecodeExtended(sends[0], &trailer, &decoded_message);
        Check(decoded &&
                  trailer.native_size == nativeBytes &&
                  decoded_message.kind == metadata::MessageKind::Hello &&
                  decoded_message.request_id == hello.request_id &&
                  decoded_message.title.empty() &&
                  result == SendResult(sends[0].size),
              carriers[index] == kHello
                  ? "Battle-list Hello metadata is restored after native fixed-size forwarding"
                  : "mission list or automatic-create Hello survives native fixed-size forwarding");
        const std::vector<std::uint8_t> forwarded_native =
            decoded ? std::vector<std::uint8_t>(
                          trailer.native_data,
                          trailer.native_data + trailer.native_size)
                    : std::vector<std::uint8_t>();
        Check(NativeBytesMatch(forwarded_native, native, handle) &&
                  packet == original,
              "Hello forwarding preserves the authoritative user ID and caller buffer");
    }
}

void TestSnapshotBatchResponse()
{
    ResetCaptures(Scenario::Normal);
    const DWORD handle = 0x0067;
    const auto native =
        NativePacket(0x4B, kRoomListNativeBytes, kRoomList);
    metadata::SnapshotBatch batch;
    batch.server_epoch = 0x0102030405060708ull;
    batch.request_id = 0x11223344;
    metadata::SnapshotRecord mission;
    mission.key.server_epoch = batch.server_epoch;
    mission.key.room_id = 17;
    mission.key.generation = 3;
    mission.revision = 5;
    mission.title = "mission: complete room rules";
    batch.rooms.push_back(mission);
    metadata::SnapshotRecord pvp;
    pvp.key.server_epoch = batch.server_epoch;
    pvp.key.room_id = 23;
    pvp.key.generation = 4;
    pvp.revision = 6;
    pvp.title = "pvp: complete room rules";
    batch.rooms.push_back(pvp);

    std::vector<std::uint8_t> packet;
    Check(metadata::EncodeSnapshotBatchPacket(
              native.data(), native.size(), batch, &packet),
          "common protocol encodes the room-list SnapshotBatch response");
    const auto original = packet;
    const int result =
        CallServerDispatch(0x81, 0x82, handle, &packet);
    const auto dispatches = Dispatches();
    const auto sends = Sends();

    Check(dispatches.size() == 1 &&
              dispatches[0].path == DispatchPath::ServerToClient &&
              dispatches[0].size ==
                  static_cast<int>(kRoomListNativeBytes) &&
              dispatches[0].pointer != packet.data(),
          "room-list response bridge gives the native server dispatcher a private fixed-size copy");
    metadata::TrailerView trailer;
    metadata::SnapshotBatch decoded_batch;
    const bool decoded =
        sends.size() == 1 &&
        sends[0].path == SendPath::ToClient &&
        metadata::DecodeSnapshotBatchPacket(
            sends[0].bytes.data(), sends[0].bytes.size(), &trailer,
            &decoded_batch);
    Check(decoded &&
              trailer.native_size == kRoomListNativeBytes &&
              decoded_batch.server_epoch == batch.server_epoch &&
              decoded_batch.request_id == batch.request_id &&
              decoded_batch.rooms.size() == 2 &&
              decoded_batch.rooms[0].title == mission.title &&
              decoded_batch.rooms[1].title == pvp.title,
          "SnapshotBatch metadata for mission and PvP rooms survives native fixed-size forwarding");
    const std::vector<std::uint8_t> forwarded_native =
        decoded ? std::vector<std::uint8_t>(
                      trailer.native_data,
                      trailer.native_data + trailer.native_size)
                : std::vector<std::uint8_t>();
    Check(ServerBytesMatch(forwarded_native, native, handle) &&
              sends.size() == 1 &&
              sends[0].destination == NativeDestination(handle) &&
              sends[0].flags == NativeFlags(handle) &&
              result == SendResult(sends[0].size) &&
              packet == original,
          "response bridge preserves native rebuild, destination, flags and caller buffer");
}

void TestAutoCreateHelloResponse()
{
    ResetCaptures(Scenario::Normal);
    auto native = NativePacket(0x41, 0x21FC, 0x0A33);
    Write32(native.data() + 6, 5);
    metadata::SnapshotBatch batch;
    batch.server_epoch = 778; batch.request_id = 1;
    metadata::SnapshotRecord room;
    room.key.server_epoch = 778; room.key.room_id = 5; room.key.generation = 1; room.revision = 1;
    batch.rooms.push_back(room);
    std::vector<uint8_t> packet;
    Check(metadata::EncodeSnapshotBatchPacket(native.data(), native.size(), batch, &packet),
          "automatic-create capability acknowledgement encodes");
    const auto result = CallServerDispatch(0x81, 0x82, 73, &packet);
    const auto sends = Sends();
    metadata::TrailerView view;
    metadata::SnapshotBatch decoded;
    Check(result > 0 && sends.size() == 1 &&
              metadata::DecodeSnapshotBatchPacket(sends[0].bytes.data(), sends[0].bytes.size(), &view, &decoded) &&
              view.native_size == 0x21FC && decoded.request_id == 1 && decoded.rooms.size() == 1 &&
              decoded.rooms[0].key.room_id == 5 && decoded.rooms[0].title.empty(),
          "automatic mission-create identity and capability survive reverse Agent forwarding");
}

void TestTitleUpdateRequests()
{
    struct Case { uint16_t opcode; size_t bytes; uint8_t subtype; uint32_t room; };
    const Case cases[] = {{0x089E,0x1D,1,0x80000005u}, {0x089E,0x1D,2,5},
                          {0x050F,0x37,0,0x80000005u}, {0x09ED,0x3E,0,5}};
    for (const auto& item : cases) {
        ResetCaptures(Scenario::Normal);
        auto native = NativePacket(0x41, item.bytes, item.opcode);
        if (item.opcode == 0x089E) native[6] = item.subtype;
        metadata::Message request;
        request.kind = metadata::MessageKind::SetTitle;
        request.server_epoch = 1234; request.room_id = item.room;
        request.generation = 2; request.revision = 3; request.request_id = 73;
        request.title = "complete title with rules beyond byte twenty !ai3";
        auto packet = MessagePacket(native, request);
        const int result = CallDispatch(0x81, 0x82, 73, &packet);
        const auto sends = Sends();
        metadata::TrailerView view;
        metadata::Message decoded;
        Check(result > 0 && sends.size() == 1 && DecodeExtended(sends[0], &view, &decoded) &&
                  view.native_size == item.bytes && decoded.room_id == item.room &&
                  decoded.request_id == request.request_id && decoded.title == request.title &&
                  Read32(view.native_data + 2) == NativeUserId(73),
              "mission and Battle renames/settings keep full metadata and authenticated native identity");
    }
}

void TestMissionListErrorResponse()
{
    const uint16_t carriers[] = {0x054E, 0x09F4, 0x07AC, 0x08FB, 0x0709, 0x0916};
    for (const auto opcode : carriers) {
        ResetCaptures(Scenario::Normal);
        auto native = NativePacket(0x51, 8, opcode);
        metadata::Message error;
        error.kind = metadata::MessageKind::Error;
        error.server_epoch = 1234;
        error.request_id = 73;
        error.error = metadata::RemoteError::InvalidRequest;
        std::vector<uint8_t> packet;
        Check(metadata::EncodePacket(native.data(), native.size(), error, &packet),
              "eight-byte native request error carrier encodes");
        const int result = CallServerDispatch(0x83, 0x84, 73, &packet);
        const auto sends = Sends();
        metadata::TrailerView view;
        metadata::Message decoded;
        Check(result > 0 && sends.size() == 1 &&
                  metadata::DecodePacket(sends[0].bytes.data(), sends[0].bytes.size(), &view, &decoded) &&
                  view.native_size == 8 && decoded.kind == metadata::MessageKind::Error &&
                  decoded.request_id == 73 && decoded.error == error.error,
              "all request rejection carriers survive native Agent error forwarding");
    }
}

void TestVariableMissionListResponse()
{
    const std::uint8_t counts[] = {0, 6, 10};
    for (std::size_t index = 0; index < std::size(counts);
         ++index) {
        ResetCaptures(Scenario::Normal);
        const std::uint8_t count = counts[index];
        const DWORD handle = static_cast<DWORD>(0x0070 + index);
        const std::size_t native_size =
            kTeamListHeaderBytes +
            static_cast<std::size_t>(count) *
                kTeamListRowBytes;
        auto native =
            NativePacket(static_cast<std::uint8_t>(0x50 + index),
                         native_size, kTeamListResponse);
        native[0x12] = count;

        metadata::SnapshotBatch batch;
        batch.server_epoch = 0x1112131415161718ull;
        batch.request_id =
            0x55667700u + static_cast<std::uint32_t>(index);
        metadata::SnapshotRecord mission;
        mission.key.server_epoch = batch.server_epoch;
        mission.key.room_id = 19;
        mission.key.generation = 4;
        mission.revision = 2;
        mission.title = "mission Team-list complete title";
        batch.rooms.push_back(mission);

        std::vector<std::uint8_t> packet;
        Check(metadata::EncodeSnapshotBatchPacket(
                  native.data(), native.size(), batch, &packet),
              "common protocol encodes a variable mission Team-list response");
        const auto original = packet;
        const int result =
            CallServerDispatch(0x83, 0x84, handle, &packet);
        const auto dispatches = Dispatches();
        const auto sends = Sends();
        metadata::TrailerView trailer;
        metadata::SnapshotBatch decoded_batch;
        const bool decoded =
            sends.size() == 1 &&
            metadata::DecodeSnapshotBatchPacket(
                sends[0].bytes.data(), sends[0].bytes.size(),
                &trailer, &decoded_batch);
        const std::vector<std::uint8_t> forwarded_native =
            decoded ? std::vector<std::uint8_t>(
                          trailer.native_data,
                          trailer.native_data +
                              trailer.native_size)
                    : std::vector<std::uint8_t>();
        Check(dispatches.size() == 1 &&
                  dispatches[0].size ==
                      static_cast<int>(native_size) &&
                  dispatches[0].pointer != packet.data() &&
                  decoded &&
                  trailer.native_size == native_size &&
                  decoded_batch.request_id == batch.request_id &&
                  decoded_batch.rooms.size() == 1 &&
                  decoded_batch.rooms[0].title ==
                      mission.title &&
                  ServerBytesMatch(forwarded_native, native,
                                   handle) &&
                  result == SendResult(sends[0].size) &&
                  packet == original,
              count == 0
                  ? "Agent preserves metadata across a zero-row 0x824 native rebuild"
                  : count == 6
                        ? "Agent preserves metadata across a six-row 0x824 native rebuild"
                        : "Agent preserves metadata across the ten-row 0x824 page maximum");
    }
}

void TestSendScopeGates()
{
    ResetCaptures(Scenario::ScopeGates);
    const DWORD handle = 0x0056;
    const auto native = NativePacket(0x44);
    auto packet = ExtendedPacket(native, "pvp: scoped bridge", 104);
    CallDispatch(0x30, 0x40, handle, &packet);
    const auto sends = Sends();
    Check(sends.size() == 3,
          "native dispatch emits both pass-through probes and the authoritative send");
    Check(sends.size() >= 1 &&
              sends[0].pointer == g_scope_same_opcode_pointer &&
              sends[0].destination == kExtraDestination &&
              sends[0].flags == kExtraFlags &&
              sends[0].size == static_cast<int>(kNativeBytes) &&
              Read16(sends[0].bytes.data()) == kCreateRoom &&
              metadata::ParseTrailer(sends[0].bytes.data(),
                                     sends[0].bytes.size())
                      .status == metadata::TrailerStatus::NoTrailer,
          "same opcode on a different pointer inside scope passes unchanged");
    Check(sends.size() >= 2 &&
              sends[1].pointer == g_scope_other_opcode_pointer &&
              sends[1].destination == kOtherDestination &&
              sends[1].flags == kOtherFlags &&
              sends[1].size == static_cast<int>(kNativeBytes) &&
              Read16(sends[1].bytes.data()) == 0x0641,
          "other opcode on the scoped native pointer passes unchanged");
    metadata::TrailerView trailer;
    metadata::Message message;
    Check(sends.size() >= 3 &&
              DecodeExtended(sends[2], &trailer, &message) &&
              message.title == "pvp: scoped bridge",
          "the later authoritative send still receives the stored extension");

    ResetCaptures(Scenario::Normal);
    auto outside = NativePacket(0x55);
    const auto outside_original = outside;
    auto send = reinterpret_cast<SendFn>(&AgentSendToServer);
    const int result =
        send(g_agent_object, 0xABCDEF01, outside.data(),
             static_cast<int>(outside.size()), 0x12345678);
    const auto outside_sends = Sends();
    Check(outside_sends.size() == 1 &&
              outside_sends[0].pointer == outside.data() &&
              outside_sends[0].bytes == outside_original &&
              outside_sends[0].destination == 0xABCDEF01 &&
              outside_sends[0].flags == 0x12345678 &&
              result == SendResult(static_cast<int>(outside.size())),
          "send calls outside dispatch scope pass through unchanged");
}

struct ThreadCall
{
    std::vector<std::uint8_t> packet;
    std::vector<std::uint8_t> original;
    DWORD handle = 0;
    DWORD argument0 = 0;
    int result = 0;
};

DWORD WINAPI DispatchThread(void* parameter)
{
    auto* call = static_cast<ThreadCall*>(parameter);
    call->result =
        CallDispatch(call->argument0, 0x77777777, call->handle,
                     &call->packet);
    return 0;
}

void TestThreadLocalIsolation()
{
    ResetCaptures(Scenario::Threaded);
    const auto native_a = NativePacket(0x61);
    const auto native_b = NativePacket(0x72);
    ThreadCall calls[2];
    calls[0].packet =
        ExtendedPacket(native_a, "thread title alpha", 201);
    calls[0].original = calls[0].packet;
    calls[0].handle = 0x0061;
    calls[0].argument0 = 0xAAAA0001;
    calls[1].packet =
        ExtendedPacket(native_b, "thread title beta", 202);
    calls[1].original = calls[1].packet;
    calls[1].handle = 0x0072;
    calls[1].argument0 = 0xBBBB0002;

    HANDLE threads[2] = {
        CreateThread(nullptr, 0, DispatchThread, &calls[0], 0, nullptr),
        CreateThread(nullptr, 0, DispatchThread, &calls[1], 0, nullptr)};
    Check(threads[0] && threads[1],
          "two overlapping native dispatch threads start");
    if (threads[0] && threads[1])
        WaitForMultipleObjects(2, threads, TRUE, 10000);
    for (HANDLE thread : threads)
        if (thread)
            CloseHandle(thread);

    const auto sends = Sends();
    bool alpha = false;
    bool beta = false;
    for (const SendCapture& capture : sends) {
        metadata::TrailerView trailer;
        metadata::Message message;
        if (!DecodeExtended(capture, &trailer, &message))
            continue;
        const std::vector<std::uint8_t> native(
            trailer.native_data, trailer.native_data + trailer.native_size);
        if (message.title == "thread title alpha")
            alpha = NativeBytesMatch(native, native_a, calls[0].handle) &&
                    capture.destination ==
                        NativeDestination(calls[0].handle) &&
                    capture.flags == NativeFlags(calls[0].handle);
        if (message.title == "thread title beta")
            beta = NativeBytesMatch(native, native_b, calls[1].handle) &&
                   capture.destination ==
                       NativeDestination(calls[1].handle) &&
                   capture.flags == NativeFlags(calls[1].handle);
    }
    Check(sends.size() == 2 && alpha && beta &&
              calls[0].packet == calls[0].original &&
              calls[1].packet == calls[1].original,
          "thread-local contexts keep simultaneous titles and native packets isolated");
}

void TestNestedDispatch()
{
    ResetCaptures(Scenario::Nested);
    const auto outer_native = NativePacket(0x81);
    const auto inner_native = NativePacket(0x92);
    auto outer =
        ExtendedPacket(outer_native, "outer room title", 301);
    const auto outer_original = outer;
    g_nested_packet =
        ExtendedPacket(inner_native, "inner room title", 302);
    const auto inner_original = g_nested_packet;
    CallDispatch(kOuterDispatch, 0xA1A1A1A1, 0x0081, &outer);

    const auto sends = Sends();
    bool outer_ok = false;
    bool inner_ok = false;
    for (const SendCapture& capture : sends) {
        metadata::TrailerView trailer;
        metadata::Message message;
        if (!DecodeExtended(capture, &trailer, &message))
            continue;
        const std::vector<std::uint8_t> native(
            trailer.native_data, trailer.native_data + trailer.native_size);
        if (message.title == "outer room title")
            outer_ok =
                NativeBytesMatch(native, outer_native, 0x0081) &&
                capture.destination == NativeDestination(0x0081);
        if (message.title == "inner room title")
            inner_ok =
                NativeBytesMatch(native, inner_native, 0x0022) &&
                capture.destination == NativeDestination(0x0022);
    }
    Check(sends.size() == 2 && inner_ok && outer_ok,
          "nested dispatch restores the enclosing title context after the inner send");
    Check(outer == outer_original && g_nested_packet == inner_original,
          "nested extension forwarding leaves both caller buffers unchanged");
}

void TestRetryAndReentry()
{
    ResetCaptures(Scenario::Retry);
    const auto retry_native = NativePacket(0xA1);
    auto retry_packet =
        ExtendedPacket(retry_native, "retry keeps extension", 351);
    const auto retry_original = retry_packet;
    const int retry_result =
        CallDispatch(0x91, 0x92, 0x00A1, &retry_packet);
    const auto retry_sends = Sends();
    bool both_extended = retry_sends.size() == 2;
    for (const SendCapture& capture : retry_sends) {
        metadata::TrailerView trailer;
        metadata::Message message;
        both_extended =
            both_extended && DecodeExtended(capture, &trailer, &message) &&
            trailer.native_size == kNativeBytes &&
            message.title == "retry keeps extension";
    }
    Check(both_extended &&
              retry_result ==
                  SendResult(retry_sends.empty()
                                 ? 0
                                 : retry_sends.back().size) &&
              retry_packet == retry_original,
          "native retry after a failed send restores the extension on every attempt");

    ResetCaptures(Scenario::Reentrant);
    const auto reentrant_native = NativePacket(0xB2);
    auto reentrant_packet =
        ExtendedPacket(reentrant_native, "reentry must fail closed", 352);
    const auto reentrant_original = reentrant_packet;
    const int reentrant_result =
        CallDispatch(0x93, 0x94, 0x00B2, &reentrant_packet);
    const auto reentrant_sends = Sends();
    metadata::TrailerView trailer;
    metadata::Message message;
    Check(reentrant_sends.size() == 1 &&
              InterlockedCompareExchange(&g_reentrant_result, -1, -1) == 0 &&
              DecodeExtended(reentrant_sends[0], &trailer, &message) &&
              message.title == "reentry must fail closed" &&
              reentrant_result == SendResult(reentrant_sends[0].size) &&
              reentrant_packet == reentrant_original,
          "same-scope synchronous send reentry returns zero without leaking a native packet");
}

void TestNativeBehavior(const char* reason)
{
    ResetCaptures(Scenario::Normal);
    const DWORD handle = 0x0093;
    const auto native = NativePacket(0xA3);
    auto packet =
        ExtendedPacket(native, "native behavior probe", 401);
    const auto original = packet;
    const int result =
        CallDispatch(0x55, 0x66, handle, &packet);
    const auto dispatches = Dispatches();
    const auto sends = Sends();

    Check(dispatches.size() == 1 &&
              dispatches[0].pointer == packet.data() &&
              dispatches[0].size == static_cast<int>(original.size()),
          reason);
    Check(sends.size() == 1 &&
              sends[0].pointer == packet.data() &&
              sends[0].size == static_cast<int>(kNativeBytes) &&
              metadata::ParseTrailer(sends[0].bytes.data(),
                                     sends[0].bytes.size())
                      .status == metadata::TrailerStatus::NoTrailer &&
              result == SendResult(static_cast<int>(kNativeBytes)),
          "native behavior forwards only its rebuilt 83-byte request");
    Check(packet != original &&
              NativeBytesMatch(
                  std::vector<std::uint8_t>(packet.begin(),
                                            packet.begin() + kNativeBytes),
                  native, handle),
          "native behavior writes authenticated fields into the caller buffer");
}

void RunEnabledCases()
{
    TestEnabledExtension();
    TestRejectedPackets();
    TestNoTrailerCompatibility();
    TestVanillaPacket();
    TestHelloRequest();
    TestSnapshotBatchResponse();
    TestVariableMissionListResponse();
    TestMissionListErrorResponse();
    TestTitleUpdateRequests();
    TestAutoCreateHelloResponse();
    TestSendScopeGates();
    TestThreadLocalIsolation();
    TestNestedDispatch();
    TestRetryAndReentry();
}
}  // namespace

int wmain(int argc, wchar_t** argv)
{
    InitializeCriticalSection(&g_capture_lock);
    g_threads_ready_event =
        CreateEventW(nullptr, TRUE, FALSE, nullptr);

    const std::wstring root = ExeDirectory();
    Check(g_threads_ready_event != nullptr,
          "thread-isolation synchronization fixture initializes");
    Check(WriteSymbols(root),
          "generated symbols verify both synthetic native functions");
    if (g_failures == 0 && StartCore(root)) {
        if (argc > 1 && wcscmp(argv[1], L"disabled") == 0)
            TestNativeBehavior(
                "disabled bridge keeps the native oversized dispatch path");
        else if (argc > 1 && wcscmp(argv[1], L"rollback") == 0)
            TestNativeBehavior(
                "hook conflict rolls the bridge's first dispatch hook back");
        else
            RunEnabledCases();
    }

    if (g_threads_ready_event)
        CloseHandle(g_threads_ready_event);
    DeleteCriticalSection(&g_capture_lock);
    std::puts(g_failures ? "ROOM TITLES AGENT CHECKS FAILED"
                         : "ALL ROOM TITLES AGENT CHECKS PASSED");
    return g_failures ? 1 : 0;
}

#endif
