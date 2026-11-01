// OFFLINE-ONLY ABI and boundary tests for ChannelActions.
//
// The fixture models the shipped x86 object layouts and calls the production
// adapter through SdgoApi Symbol/HookCode stubs. It never starts GOnline,
// contacts a server, writes a database, or claims gameplay/runtime evidence.

#include <windows.h>
#include <array>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../mods/game_test_client/channel_actions.h"

namespace
{
constexpr size_t kGroupStride = 0x764;
constexpr size_t kChannelStride = 0x1D;
constexpr size_t kRowStride = 0x28;
constexpr uint16_t kTargetChannel = 9305;

using NetworkSendDetour =
    int(__fastcall*)(
        void*,
        void*,
        void*,
        const void*,
        int,
        char);
using ReplyDetour =
    int(__fastcall*)(
        void*,
        void*,
        int,
        int,
        int,
        int,
        void*);

int g_failures = 0;
uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x6000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_groups[kGroupStride * 4] = {};
uint8_t g_service[0x200] = {};
uint8_t g_lobbyService[0x100] = {};
uint8_t g_root[4] = {};
uint8_t g_rows[kRowStride * 16] = {};
uint8_t g_leftControl[0x160] = {};
uint8_t g_rightControl[0x160] = {};
uint8_t g_channelControls[7][0x160] = {};
uint8_t g_network[0x60] = {};
uint8_t g_stream = 0;
uint8_t g_serviceManagerObject = 0;
void* g_serviceManager = &g_serviceManagerObject;
void* g_vtable[16] = {};
void* g_lobbyVtable[16] = {};
NetworkSendDetour g_networkSendDetour = nullptr;
ReplyDetour g_replyDetour = nullptr;
ReplyDetour g_lobbyReplyDetour = nullptr;
ChannelActions* g_channels = nullptr;

unsigned g_openCalls = 0;
unsigned g_clickCalls = 0;
unsigned g_lobbyJoinCalls = 0;
unsigned g_networkOriginalCalls = 0;
uint16_t g_lobbyRequestedChannel = 0;
uint16_t g_lastJoinPacketChannel = 0;
bool g_selectorAvailable = true;
bool g_lobbyAvailable = true;
bool g_lobbyWritesRequestState = true;
bool g_clickAbiValid = true;
bool g_replyUpdateProfile = false;
bool g_replyPacketReleased = false;
uint32_t g_replySequenceInsideOriginal = UINT32_MAX;
void* g_replyAllocation = nullptr;
wchar_t g_lastClickName[32] = {};

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

void DestroyNativeString(void* storage)
{
    if (!storage)
        return;
    auto* bytes = static_cast<uint8_t*>(storage);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(bytes + 0x14);
    if (capacity >= 8) {
        void* allocation = *reinterpret_cast<void**>(bytes);
        if (allocation)
            HeapFree(GetProcessHeap(), 0, allocation);
    }
    std::memset(storage, 0, 0x1C);
}

bool WriteNativeString(void* storage, const wchar_t* value)
{
    if (!storage || !value)
        return false;
    DestroyNativeString(storage);
    auto* bytes = static_cast<uint8_t*>(storage);
    const size_t length = std::wcslen(value);
    if (length < 8) {
        std::memcpy(bytes, value, (length + 1) * sizeof(wchar_t));
        *reinterpret_cast<uint32_t*>(bytes + 0x14) = 7;
    } else {
        const size_t byteCount = (length + 1) * sizeof(wchar_t);
        auto* allocation = static_cast<wchar_t*>(
            HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, byteCount));
        if (!allocation)
            return false;
        std::memcpy(allocation, value, byteCount);
        *reinterpret_cast<wchar_t**>(bytes) = allocation;
        *reinterpret_cast<uint32_t*>(bytes + 0x14) =
            static_cast<uint32_t>(length);
    }
    *reinterpret_cast<uint32_t*>(bytes + 0x10) =
        static_cast<uint32_t>(length);
    return true;
}

const wchar_t* ReadNativeString(const void* storage)
{
    if (!storage)
        return nullptr;
    const auto* bytes = static_cast<const uint8_t*>(storage);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(bytes + 0x14);
    return capacity < 8
        ? reinterpret_cast<const wchar_t*>(bytes)
        : *reinterpret_cast<wchar_t* const*>(bytes);
}

void PutChannel(
    uint8_t* entry,
    uint16_t id,
    uint32_t capacity,
    uint32_t population,
    uint32_t category)
{
    std::memset(entry, 0, kChannelStride);
    *reinterpret_cast<uint16_t*>(entry) = id;
    *reinterpret_cast<uint32_t*>(entry + 0x02) = 1;
    *reinterpret_cast<uint32_t*>(entry + 0x06) = 1;
    *reinterpret_cast<uint32_t*>(entry + 0x0A) = capacity;
    *reinterpret_cast<uint32_t*>(entry + 0x0E) = population;
    *reinterpret_cast<uint32_t*>(entry + 0x16) = category;
}

uint8_t* GroupChannel(unsigned groupIndex, unsigned channelIndex)
{
    return g_groups + groupIndex * kGroupStride + 0x24 +
        channelIndex * kChannelStride;
}

void SetSelectedChannel(uint32_t group, uint16_t channel)
{
    *reinterpret_cast<uint16_t*>(g_profileStorage + 0x5D0) = channel;
    *reinterpret_cast<uint32_t*>(g_profileStorage + 0x5D4) = group;
}

void ResetFixture(
    ChannelActions& channels,
    uint32_t selectedGroup = 1,
    uint16_t selectedChannel = 200,
    unsigned targetRow = 0)
{
    channels.Reset();
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(g_profileStorage, 0, sizeof(g_profileStorage));
    std::memset(g_groups, 0, sizeof(g_groups));
    std::memset(g_service, 0, sizeof(g_service));
    std::memset(g_lobbyService, 0, sizeof(g_lobbyService));
    std::memset(g_rows, 0, sizeof(g_rows));
    std::memset(g_network, 0, sizeof(g_network));
    *reinterpret_cast<uint32_t*>(g_app + 0x4A0) = 6;
    g_profile = g_profileStorage;
    g_serviceManager = &g_serviceManagerObject;
    *reinterpret_cast<void***>(g_service) = g_vtable;
    *reinterpret_cast<void***>(g_lobbyService) = g_lobbyVtable;
    *reinterpret_cast<void**>(g_service + 0x60) = g_root;
    *reinterpret_cast<uint32_t*>(g_service + 0x1A4) = 0;
    *reinterpret_cast<uint32_t*>(g_service + 0x1A8) =
        targetRow / 7;
    g_service[0x1AC] = 0;
    *reinterpret_cast<uint8_t**>(g_service + 0x1B0) = g_rows;
    *reinterpret_cast<uint8_t**>(g_service + 0x1B4) =
        g_rows + (targetRow + 1) * kRowStride;

    *reinterpret_cast<uint8_t**>(g_profileStorage + 0x5B0) =
        g_groups;
    *reinterpret_cast<uint8_t**>(g_profileStorage + 0x5B4) =
        g_groups + sizeof(g_groups);
    *reinterpret_cast<uint16_t*>(g_groups + 0x22) = 1;
    *reinterpret_cast<uint16_t*>(
        g_groups + kGroupStride + 0x22) = 1;
    *reinterpret_cast<uint16_t*>(
        g_groups + 2 * kGroupStride + 0x22) = 1;
    *reinterpret_cast<uint16_t*>(g_groups+3*kGroupStride+0x22)=1;
    PutChannel(GroupChannel(0, 0), 100, 100, 1, 1);
    PutChannel(GroupChannel(1, 0), 200, 100, 1, 3);
    PutChannel(GroupChannel(2, 0), kTargetChannel, 100, 1, 3);
    PutChannel(GroupChannel(3,0),400,100,1,3);
    *reinterpret_cast<uint32_t*>(GroupChannel(3,0)+0x06)=2;
    SetSelectedChannel(selectedGroup, selectedChannel);

    uint8_t* row = g_rows + targetRow * kRowStride;
    row[0x08] = 0;
    std::memcpy(
        row + 0x09,
        GroupChannel(2, 0),
        kChannelStride);

    g_openCalls = 0;
    g_clickCalls = 0;
    g_lobbyJoinCalls = 0;
    g_networkOriginalCalls = 0;
    g_lobbyRequestedChannel = 0;
    g_lastJoinPacketChannel = 0;
    g_selectorAvailable = true;
    g_lobbyAvailable = true;
    g_lobbyWritesRequestState = true;
    g_clickAbiValid = true;
    g_replyUpdateProfile = false;
    g_replyPacketReleased = false;
    g_replySequenceInsideOriginal = UINT32_MAX;
    g_replyAllocation = nullptr;
    g_lastClickName[0] = L'\0';
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

void* __fastcall FakeLoginPacketCtor(
    void* self,
    void*,
    const char*,
    const char*)
{
    return self;
}

char* __cdecl FakePasswordMd5Hex(const void*, char* output, int)
{
    if (output)
        std::memset(output, '0', 32);
    return output;
}

uint8_t* __cdecl FakeNetwork()
{
    return g_network;
}

int __fastcall FakeNetworkSend(
    void*,
    void*,
    void*,
    const void* packet,
    int size,
    char)
{
    ++g_networkOriginalCalls;
    if (packet && size == 0x0E &&
        *reinterpret_cast<const uint16_t*>(packet) == 0x04FA)
        g_lastJoinPacketChannel =
            *reinterpret_cast<const uint16_t*>(
                static_cast<const uint8_t*>(packet) + 0x0A);
    return 1;
}

int __fastcall FakeLoginDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    return 0;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t* value)
{
    return WriteNativeString(self, value) ? self : nullptr;
}

void __fastcall FakeNativeWStringDtor(void* self, void*)
{
    DestroyNativeString(self);
}

void* __fastcall FakeNicknameDialogCtor(void* self, void*)
{
    return self;
}

void* __cdecl FakeEnqueueUiObject(void* object, int)
{
    return object;
}

void __fastcall FakeNicknameDialogDtor(void*, void*)
{
}

void __fastcall FakeNicknameDialogConfirm(void*, void*)
{
}

void* __fastcall FakeServiceFindByName(
    void* manager,
    void*,
    const void* descriptor)
{
    const auto* bytes = static_cast<const uint8_t*>(descriptor);
    const wchar_t* name = bytes
        ? ReadNativeString(bytes + sizeof(uint32_t))
        : nullptr;
    const bool descriptorValid =
        manager == g_serviceManager &&
        bytes &&
        *reinterpret_cast<const uint32_t*>(bytes) == 1 &&
        name;
    Check(
        descriptorValid,
        "service lookup used a managed native-string descriptor");
    if (!descriptorValid)
        return nullptr;
    if (std::wcscmp(name, L"VSChannelSelect") == 0)
        return g_selectorAvailable ? g_service : nullptr;
    if (std::wcscmp(name, L"upmenu") == 0)
        return g_lobbyAvailable ? g_lobbyService : nullptr;
    Check(false, "service lookup requested only a verified channel service");
    return nullptr;
}

void* ControlForName(const wchar_t* name)
{
    if (!name)
        return nullptr;
    if (std::wcscmp(name, L"LeftBtn") == 0)
        return g_leftControl;
    if (std::wcscmp(name, L"RightBtn") == 0)
        return g_rightControl;
    for (unsigned i = 0; i < 7; ++i) {
        wchar_t expected[16] = {};
        swprintf_s(expected, L"ChannelBtn%u", i);
        if (std::wcscmp(name, expected) == 0)
            return g_channelControls[i];
    }
    return nullptr;
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t type)
{
    if (root != g_root || type != 4)
        return nullptr;
    return ControlForName(ReadNativeString(nativeName));
}

void __fastcall FakeVsOpen(void* service, void*, int)
{
    ++g_openCalls;
    if (service == g_service)
        *reinterpret_cast<void**>(g_service + 0x60) = g_root;
}

void __stdcall FakeVsClick(
    void* service,
    uint32_t message,
    const void* nativeName,
    void* control)
{
    ++g_clickCalls;
    const wchar_t* name = ReadNativeString(nativeName);
    g_clickAbiValid =
        g_clickAbiValid &&
        service == g_service &&
        message == 0x101 &&
        control != nullptr &&
        nativeName == static_cast<uint8_t*>(control) + 0x118 &&
        name != nullptr;
    if (!name)
        return;
    wcscpy_s(g_lastClickName, name);
    if (std::wcscmp(name, L"RightBtn") == 0) {
        ++*reinterpret_cast<uint32_t*>(g_service + 0x1A4);
        return;
    }
    if (std::wcscmp(name, L"LeftBtn") == 0) {
        --*reinterpret_cast<uint32_t*>(g_service + 0x1A4);
        return;
    }
    if (std::wcsncmp(name, L"ChannelBtn", 10) != 0)
        return;

    uint8_t packet[0x0E] = {};
    *reinterpret_cast<uint16_t*>(packet) = 0x04FA;
    if (g_networkSendDetour) {
        g_networkSendDetour(
            g_network,
            nullptr,
            &g_stream,
            packet,
            static_cast<int>(sizeof(packet)),
            0);
    }
    g_service[0x1AC] = 1;
}

void __fastcall FakeLobbyJoinChannel(
    void* service,
    void*,
    uint16_t channel)
{
    ++g_lobbyJoinCalls;
    g_lobbyRequestedChannel = channel;
    if (service != g_lobbyService)
        return;
    uint8_t packet[0x0E] = {};
    *reinterpret_cast<uint16_t*>(packet) = 0x04FA;
    *reinterpret_cast<uint16_t*>(packet + 0x0A) = channel;
    if (g_networkSendDetour) {
        g_networkSendDetour(
            g_network,
            nullptr,
            &g_stream,
            packet,
            static_cast<int>(sizeof(packet)),
            0);
    }
    if (g_lobbyWritesRequestState)
        *reinterpret_cast<uint32_t*>(
            g_lobbyService + 0x68) = 4;
}

uint32_t FindGroupForChannel(uint16_t channel)
{
    for (uint32_t group = 0; group < 4; ++group) {
        const uint16_t count = *reinterpret_cast<const uint16_t*>(
            g_groups + group * kGroupStride + 0x22);
        for (uint16_t index = 0; index < count; ++index) {
            if (*reinterpret_cast<const uint16_t*>(
                    GroupChannel(group, index)) == channel)
                return group;
        }
    }
    return UINT32_MAX;
}

int __fastcall FakeVsReply(
    void*,
    void*,
    int,
    int,
    int,
    int opcode,
    void* packet)
{
    g_replySequenceInsideOriginal =
        g_channels
            ? g_channels->State().reply_sequence
            : UINT32_MAX;
    uint16_t channel = 0;
    uint16_t error = UINT16_MAX;
    if ((opcode & 0xFFFF) == 0x09DF && packet) {
        const auto* bytes = static_cast<const uint8_t*>(packet);
        channel = *reinterpret_cast<const uint16_t*>(bytes + 0x08);
        error = *reinterpret_cast<const uint16_t*>(bytes + 0x0C);
    }
    if (g_replyUpdateProfile && error == 0 && channel != 0) {
        const uint32_t group = FindGroupForChannel(channel);
        if (group != UINT32_MAX)
            SetSelectedChannel(group, channel);
    }
    g_service[0x1AC] = 0;
    if (g_replyAllocation) {
        g_replyPacketReleased =
            VirtualFree(g_replyAllocation, 0, MEM_RELEASE) != 0;
        g_replyAllocation = nullptr;
    }
    return 73;
}

int __fastcall FakeLobbyReply(
    void* self,
    void*,
    int,
    int,
    int,
    int opcode,
    void* packet)
{
    g_replySequenceInsideOriginal =
        g_channels
            ? g_channels->State().reply_sequence
            : UINT32_MAX;
    uint16_t channel = 0;
    uint16_t error = UINT16_MAX;
    if ((opcode & 0xFFFF) == 0x09DF && packet) {
        const auto* bytes = static_cast<const uint8_t*>(packet);
        channel = *reinterpret_cast<const uint16_t*>(bytes + 0x08);
        error = *reinterpret_cast<const uint16_t*>(bytes + 0x0C);
    }
    const bool pending =
        self == g_lobbyService &&
        *reinterpret_cast<const uint32_t*>(
            g_lobbyService + 0x68) == 4;
    if (pending &&
        g_replyUpdateProfile &&
        error == 0 &&
        channel != 0) {
        const uint32_t group = FindGroupForChannel(channel);
        if (group != UINT32_MAX)
            SetSelectedChannel(group, channel);
    }
    if (g_replyAllocation) {
        g_replyPacketReleased =
            VirtualFree(g_replyAllocation, 0, MEM_RELEASE) != 0;
        g_replyAllocation = nullptr;
    }
    return 73;
}

void* LookupSymbol(const char* name)
{
    if (std::strcmp(name, "game.app") == 0)
        return reinterpret_cast<void*>(&FakeGameApp);
    if (std::strcmp(name, "flow.login_packet_ctor") == 0)
        return reinterpret_cast<void*>(&FakeLoginPacketCtor);
    if (std::strcmp(name, "flow.password_md5_hex") == 0)
        return reinterpret_cast<void*>(&FakePasswordMd5Hex);
    if (std::strcmp(name, "flow.network") == 0)
        return reinterpret_cast<void*>(&FakeNetwork);
    if (std::strcmp(name, "flow.network_send") == 0)
        return reinterpret_cast<void*>(&FakeNetworkSend);
    if (std::strcmp(name, "flow.login_service_dispatch") == 0)
        return reinterpret_cast<void*>(&FakeLoginDispatch);
    if (std::strcmp(name, "flow.profile_pointer") == 0)
        return &g_profile;
    if (std::strcmp(name, "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(&FakeNativeWStringCtor);
    if (std::strcmp(name, "flow.native_wstring_dtor") == 0)
        return reinterpret_cast<void*>(&FakeNativeWStringDtor);
    if (std::strcmp(name, "flow.nickname_dialog_ctor") == 0)
        return reinterpret_cast<void*>(&FakeNicknameDialogCtor);
    if (std::strcmp(name, "flow.enqueue_ui_object") == 0)
        return reinterpret_cast<void*>(&FakeEnqueueUiObject);
    if (std::strcmp(name, "flow.nickname_dialog_dtor") == 0)
        return reinterpret_cast<void*>(&FakeNicknameDialogDtor);
    if (std::strcmp(name, "flow.nickname_dialog_confirm") == 0)
        return reinterpret_cast<void*>(&FakeNicknameDialogConfirm);
    if (std::strcmp(name, "flow.service_managed_pointer") == 0)
        return &g_serviceManager;
    if (std::strcmp(name, "flow.service_find_by_name") == 0)
        return reinterpret_cast<void*>(&FakeServiceFindByName);
    if (std::strcmp(name, "flow.vs_channel_vtable") == 0)
        return g_vtable;
    if (std::strcmp(name, "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(&FakeUiFindControl);
    if (std::strcmp(name, "flow.vs_channel_click_business") == 0)
        return reinterpret_cast<void*>(&FakeVsClick);
    if (std::strcmp(name, "flow.vs_channel_open_business") == 0)
        return reinterpret_cast<void*>(&FakeVsOpen);
    if (std::strcmp(name, "flow.vs_channel_reply_dispatch") == 0)
        return reinterpret_cast<void*>(&FakeVsReply);
    if (std::strcmp(name, "flow.lobby_ui_vtable") == 0)
        return g_lobbyVtable;
    if (std::strcmp(name, "flow.lobby_join_channel_business") == 0)
        return reinterpret_cast<void*>(&FakeLobbyJoinChannel);
    if (std::strcmp(
            name,
            "flow.lobby_join_channel_reply_dispatch") == 0)
        return reinterpret_cast<void*>(&FakeLobbyReply);
    return nullptr;
}

void* ApiSymbol(SdgoMod*, const char* name)
{
    return LookupSymbol(name);
}

int ApiHookCode(
    SdgoMod*,
    const char* name,
    void* detour,
    void** original)
{
    void* target = LookupSymbol(name);
    if (!target || !detour || !original)
        return 0;
    *original = target;
    if (std::strcmp(name, "flow.network_send") == 0) {
        g_networkSendDetour =
            reinterpret_cast<NetworkSendDetour>(detour);
    } else if (
        std::strcmp(name, "flow.vs_channel_reply_dispatch") == 0) {
        g_replyDetour = reinterpret_cast<ReplyDetour>(detour);
    } else if (
        std::strcmp(
            name,
            "flow.lobby_join_channel_reply_dispatch") == 0) {
        g_lobbyReplyDetour =
            reinterpret_cast<ReplyDetour>(detour);
    }
    return 1;
}

void InitializeControls()
{
    WriteNativeString(g_leftControl + 0x118, L"LeftBtn");
    WriteNativeString(g_rightControl + 0x118, L"RightBtn");
    for (unsigned i = 0; i < 7; ++i) {
        wchar_t name[16] = {};
        swprintf_s(name, L"ChannelBtn%u", i);
        WriteNativeString(g_channelControls[i] + 0x118, name);
    }
}

void DestroyControls()
{
    DestroyNativeString(g_leftControl + 0x118);
    DestroyNativeString(g_rightControl + 0x118);
    for (auto& control : g_channelControls)
        DestroyNativeString(control + 0x118);
}

uint8_t* AllocateGuardedReply(
    uint16_t channel,
    uint16_t error)
{
    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    const size_t page = systemInfo.dwPageSize;
    auto* allocation = static_cast<uint8_t*>(VirtualAlloc(
        nullptr,
        page * 2,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!allocation)
        return nullptr;
    DWORD previous = 0;
    if (!VirtualProtect(
            allocation + page,
            page,
            PAGE_NOACCESS,
            &previous)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return nullptr;
    }
    auto* packet = allocation + page - 0x0E;
    std::memset(packet, 0, 0x0E);
    *reinterpret_cast<uint16_t*>(packet + 0x08) = channel;
    *reinterpret_cast<uint16_t*>(packet + 0x0C) = error;
    g_replyAllocation = allocation;
    return packet;
}

bool SendChannelRequest(ChannelActions& channels)
{
    const ChannelActions::Result result = channels.EnsureVsGroup(2);
    return result == ChannelActions::kRequestSent;
}

void TestAlreadySelected(ChannelActions& channels)
{
    ResetFixture(channels, 2, kTargetChannel, 0);
    const auto result = channels.EnsureVsGroup(2);
    const auto state = channels.State();
    Check(
        result == ChannelActions::kAlreadySelected,
        "group 2 is treated as the native group-vector index");
    Check(
        state.selected_group == 2 &&
            state.selected_channel == kTargetChannel &&
            state.target_channel == kTargetChannel,
        "already-selected result belongs to a channel contained in group 2");
    Check(
        g_openCalls == 0 && g_clickCalls == 0,
        "already-selected path does not open or click the selector");
}

void TestOpenPageRequestAndReplyLifetime(ChannelActions& channels)
{
    ResetFixture(channels, 1, 200, 8);
    *reinterpret_cast<void**>(g_service + 0x60) = nullptr;

    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kSelectorOpened,
        "missing selector root is opened by the shipped Open method");
    Check(
        g_openCalls == 1 &&
            *reinterpret_cast<void**>(g_service + 0x60) == g_root,
        "Open installed the selector root before the adapter continued");

    Check(
        channels.EnsureVsGroup(2) == ChannelActions::kPaging,
        "target row on page 1 uses one native page click");
    Check(
        g_clickAbiValid &&
            std::wcscmp(g_lastClickName, L"RightBtn") == 0 &&
            *reinterpret_cast<uint32_t*>(g_service + 0x1A4) == 1,
        "page click used service, 0x101, control+0x118, and the real control");

    Check(
        SendChannelRequest(channels),
        "target row slot 1 sent the native channel request");
    const auto requested = channels.State();
    Check(
        g_clickAbiValid &&
            std::wcscmp(g_lastClickName, L"ChannelBtn1") == 0,
        "channel click used the control matching row modulo seven");
    Check(
        requested.request_transport_sequence != 0 &&
            requested.request_sent &&
            g_networkOriginalCalls == 1,
        "channel click reached the original transport observer exactly once");

    const uint32_t sequenceBefore = requested.reply_sequence;
    uint8_t* packet = AllocateGuardedReply(kTargetChannel, 0);
    Check(
        packet != nullptr,
        "guarded 0x09DF reply packet fixture was allocated");
    if (!packet || !g_replyDetour)
        return;
    g_replyUpdateProfile = true;
    const int handled = g_replyDetour(
        g_service,
        nullptr,
        1,
        2,
        3,
        0x09DF,
        packet);
    const auto replied = channels.State();

    Check(
        handled == 73,
        "reply detour preserved the original non-boolean int return");
    Check(
        g_replySequenceInsideOriginal == sequenceBefore,
        "reply observation was not published before the original consumer ran");
    Check(
        g_replyPacketReleased,
        "original reply consumer released the guarded packet fixture");
    Check(
        replied.reply_sequence == sequenceBefore + 1 &&
            replied.reply_opcode == 0x09DF &&
            replied.reply_error == 0 &&
            replied.reply_channel == kTargetChannel,
        "reply fields copied before packet ownership transferred");
    Check(
        replied.reply_consumed &&
            replied.selected_group == 2 &&
            replied.selected_channel == kTargetChannel,
        "success was published only after the real profile matched group 2");
    Check(
        channels.EnsureVsGroup(2) == ChannelActions::kAccepted,
        "matching consumed reply completed the channel precondition");
    const unsigned sendsBeforeReset = g_networkOriginalCalls;
    channels.Reset();
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kAlreadySelected &&
            g_networkOriginalCalls == sendsBeforeReset,
        "next mode reuses the consumed group-2 selection without a duplicate request");
}

void TestLobbyFallback(ChannelActions& channels)
{
    ResetFixture(channels);
    g_selectorAvailable = false;

    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kRequestSent,
        "absent pre-channel selector used the registered lobby business path");
    const auto requested = channels.State();
    Check(
        g_lobbyJoinCalls == 1 &&
            g_lobbyRequestedChannel == kTargetChannel &&
            g_lastJoinPacketChannel == kTargetChannel &&
            requested.target_channel == kTargetChannel &&
            requested.request_transport_sequence != 0 &&
            *reinterpret_cast<const uint32_t*>(
                g_lobbyService + 0x68) == 4,
        "lobby business sent the selected endpoint in 0x04FA and entered native state 4");
    Check(
        g_openCalls == 0 && g_clickCalls == 0,
        "lobby fallback did not fabricate or click an unloaded selector");

    uint8_t* packet = AllocateGuardedReply(kTargetChannel, 0);
    Check(
        packet != nullptr,
        "lobby fallback guarded 0x09DF reply fixture was allocated");
    if (!packet || !g_lobbyReplyDetour)
        return;
    g_replyUpdateProfile = true;
    const int handled = g_lobbyReplyDetour(
        g_lobbyService,
        nullptr,
        1,
        2,
        3,
        0x09DF,
        packet);
    Check(
        handled == 73 &&
            channels.EnsureVsGroup(2) ==
                ChannelActions::kAccepted,
        "original lobby reply consumer completed the real profile transition");
}

void TestLobbyFailureBoundaries(ChannelActions& channels)
{
    ResetFixture(channels);
    g_selectorAvailable = false;
    g_lobbyAvailable = false;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kServiceUnavailable &&
            g_networkOriginalCalls == 0,
        "missing selector and lobby services fail closed without transport");

    ResetFixture(channels);
    g_selectorAvailable = false;
    *reinterpret_cast<void***>(g_lobbyService) = g_vtable;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kServiceUnavailable &&
            g_networkOriginalCalls == 0,
        "lobby service with the wrong verified vtable fails closed");

    ResetFixture(channels);
    g_selectorAvailable = false;
    *reinterpret_cast<uint32_t*>(
        GroupChannel(2, 0) + 0x16) = 2;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kNoAvailableChannel &&
            g_networkOriginalCalls == 0,
        "lobby fallback never chooses a non-VS channel");

    ResetFixture(channels);
    g_selectorAvailable = false;
    *reinterpret_cast<uint32_t*>(
        GroupChannel(2, 0) + 0x0E) = 100;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kNoAvailableChannel &&
            g_networkOriginalCalls == 0,
        "lobby fallback never chooses a full channel");

    ResetFixture(channels);
    g_selectorAvailable = false;
    *reinterpret_cast<uint32_t*>(
        g_lobbyService + 0x68) = 2;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kNativeRequestPending &&
            g_networkOriginalCalls == 0,
        "lobby fallback does not overlap another native request state");

    ResetFixture(channels);
    g_selectorAvailable = false;
    g_lobbyWritesRequestState = false;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kNativeStateMismatch &&
            g_networkOriginalCalls == 1,
        "transport alone cannot pass without the original state-4 transition");
}

void TestReturnToNormalVsGroup(ChannelActions& channels)
{
    ResetFixture(channels,2,kTargetChannel);
    *reinterpret_cast<uint16_t*>(g_groups+kGroupStride+0x22)=2;
    PutChannel(GroupChannel(1,0),199,100,1,2);
    PutChannel(GroupChannel(1,1),200,100,1,3);
    Check(channels.EnsureVsGroup(1)==ChannelActions::kRequestSent,
        "normal modes switch back from battleship channels through native lobby business");
    Check(g_lobbyRequestedChannel==200 && g_openCalls==0 && g_clickCalls==0,
        "normal group skips its mission channel and uses the real VS channel without the battleship selector");
    uint8_t* packet=AllocateGuardedReply(200,0);
    Check(packet!=nullptr,"normal group reply allocated at guarded boundary");
    if (!packet) return;
    g_replyUpdateProfile=true;
    Check(g_lobbyReplyDetour(g_lobbyService,nullptr,1,2,3,0x09DF,packet)==73 &&
        channels.EnsureVsGroup(1)==ChannelActions::kAccepted,
        "return to normal VS requires original consumption and matching profile group");
}

void TestSemanticChannelType(ChannelActions& channels)
{
    ResetFixture(channels);
    Check(channels.EnsureVsType(2)==ChannelActions::kRequestSent && g_lobbyRequestedChannel==400 && channels.State().target_group==3,
        "explicit special type skips the second normal VS group and chooses native type2 in group3");
    uint8_t* reply=AllocateGuardedReply(400,0);
    Check(reply!=nullptr,"semantic channel reply allocated");
    if (reply) {
        g_replyUpdateProfile=true;
        Check(g_lobbyReplyDetour(g_lobbyService,nullptr,0,0,0,0x09DF,reply)==73 &&
            channels.EnsureVsType(2)==ChannelActions::kAccepted && channels.State().selected_group==3,
            "type-based selection still requires genuine native consumer and matching profile");
    }
    ResetFixture(channels);
    std::array<uint8_t,kGroupStride> temporary{};
    memcpy(temporary.data(),g_groups+2*kGroupStride,kGroupStride);
    memcpy(g_groups+2*kGroupStride,g_groups+3*kGroupStride,kGroupStride);
    memcpy(g_groups+3*kGroupStride,temporary.data(),kGroupStride);
    Check(channels.EnsureVsType(2)==ChannelActions::kRequestSent && channels.State().target_group==2 && g_lobbyRequestedChannel==400,
        "reordering the server-provided groups does not change semantic type selection");
    ResetFixture(channels);
    *reinterpret_cast<uint32_t*>(GroupChannel(3,0)+0x06)=1;
    Check(channels.EnsureVsType(2)==ChannelActions::kNoAvailableChannel && g_networkOriginalCalls==0,
        "missing requested special type cannot silently fall back to a normal VS channel");
    ResetFixture(channels);
    Check(channels.EnsureVsType(1)==ChannelActions::kAlreadySelected && g_networkOriginalCalls==0,
        "an already selected normal VS channel needs no native request");
}

void TestLobbyReplyStateGuard(ChannelActions& channels)
{
    ResetFixture(channels);
    g_selectorAvailable = false;
    Check(
        SendChannelRequest(channels),
        "lobby reply-state guard scenario sent one native request");
    *reinterpret_cast<uint32_t*>(g_lobbyService + 0x68) = 0;
    uint8_t* packet = AllocateGuardedReply(kTargetChannel, 0);
    Check(packet != nullptr, "guarded lobby reply fixture was allocated");
    if (!packet || !g_lobbyReplyDetour)
        return;
    g_replyUpdateProfile = true;
    g_lobbyReplyDetour(
        g_lobbyService,
        nullptr,
        0,
        0,
        0,
        0x09DF,
        packet);
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kReplyNotConsumed,
        "09DF cannot pass when the original lobby consumer is outside state 4");
}

void TestRejectedAndUnconsumedReplies(ChannelActions& channels)
{
    ResetFixture(channels);
    Check(
        SendChannelRequest(channels),
        "rejection scenario sent one channel request");
    uint8_t* rejected = AllocateGuardedReply(kTargetChannel, 0x0BC2);
    Check(rejected != nullptr, "rejected reply fixture was allocated");
    if (rejected && g_replyDetour) {
        g_replyUpdateProfile = false;
        g_replyDetour(
            g_service,
            nullptr,
            0,
            0,
            0,
            0x09DF,
            rejected);
        Check(
            channels.EnsureVsGroup(2) ==
                ChannelActions::kReplyRejected,
            "nonzero server error fails the pending channel request");
    }

    ResetFixture(channels);
    Check(
        SendChannelRequest(channels),
        "unconsumed scenario sent one channel request");
    uint8_t* unconsumed = AllocateGuardedReply(kTargetChannel, 0);
    Check(unconsumed != nullptr, "unconsumed reply fixture was allocated");
    if (unconsumed && g_replyDetour) {
        g_replyUpdateProfile = false;
        g_replyDetour(
            g_service,
            nullptr,
            0,
            0,
            0,
            0x09DF,
            unconsumed);
        Check(
            channels.EnsureVsGroup(2) ==
                ChannelActions::kReplyNotConsumed,
            "zero-error reply cannot pass before the original profile update");
    }

    ResetFixture(channels);
    Check(
        SendChannelRequest(channels),
        "mismatch scenario sent one channel request");
    uint8_t* mismatch = AllocateGuardedReply(9999, 0);
    Check(mismatch != nullptr, "mismatched reply fixture was allocated");
    if (mismatch && g_replyDetour) {
        g_replyUpdateProfile = false;
        g_replyDetour(
            g_service,
            nullptr,
            0,
            0,
            0,
            0x09DF,
            mismatch);
        Check(
            channels.EnsureVsGroup(2) ==
                ChannelActions::kReplyMismatch,
            "reply for another channel cannot satisfy the request");
    }
}

void TestStaleReplyBaseline(ChannelActions& channels)
{
    ResetFixture(channels);
    uint8_t* stale = AllocateGuardedReply(kTargetChannel, 0);
    Check(stale != nullptr, "stale reply fixture was allocated");
    if (!stale || !g_replyDetour)
        return;
    g_replyUpdateProfile = false;
    g_replyDetour(
        g_service,
        nullptr,
        0,
        0,
        0,
        0x09DF,
        stale);
    const uint32_t staleSequence = channels.State().reply_sequence;
    Check(
        SendChannelRequest(channels),
        "request sent after an older observed 0x09DF reply");
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kRequestSent,
        "reply at or before the request baseline remains stale");
    Check(
        channels.State().reply_sequence == staleSequence,
        "stale baseline test did not fabricate a newer reply");
}

void TestMalformedBoundaries(ChannelActions& channels)
{
    ResetFixture(channels);
    Check(
        channels.EnsureVsGroup(0) ==
            ChannelActions::kInvalidTarget,
        "cooperative group is not accepted as a VS target");

    ResetFixture(channels);
    g_profile = nullptr;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kProfileUnavailable,
        "null profile fails closed");

    ResetFixture(channels);
    *reinterpret_cast<uint8_t**>(g_profileStorage + 0x5B4) =
        g_groups + sizeof(g_groups) - 1;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kTargetGroupMalformed,
        "misaligned group vector fails before indexing");

    ResetFixture(channels);
    *reinterpret_cast<uint32_t*>(
        GroupChannel(2, 0) + 0x0E) = 100;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kNoAvailableChannel,
        "full server channel is never clicked");

    ResetFixture(channels);
    *reinterpret_cast<uint8_t**>(g_service + 0x1B4) =
        g_rows + kRowStride - 1;
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kRowsUnavailable,
        "misaligned native row vector fails before matching");

    ResetFixture(channels, 1, 200, 8);
    WriteNativeString(g_rightControl + 0x118, L"WrongBtn");
    Check(
        channels.EnsureVsGroup(2) ==
            ChannelActions::kControlNameInvalid,
        "looked-up paging control must retain its exact native name");
    Check(
        g_clickCalls == 0,
        "invalid control name prevents a native click");
    WriteNativeString(g_rightControl + 0x118, L"RightBtn");
}

void TestGuardedRowBoundary(ChannelActions& channels)
{
    ResetFixture(channels);
    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    const size_t page = systemInfo.dwPageSize;
    auto* allocation = static_cast<uint8_t*>(VirtualAlloc(
        nullptr,
        page * 2,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    Check(allocation != nullptr, "guarded native-row fixture was allocated");
    if (!allocation)
        return;
    DWORD previous = 0;
    if (!VirtualProtect(
            allocation + page,
            page,
            PAGE_NOACCESS,
            &previous)) {
        Check(false, "guard page was installed after the native row");
        VirtualFree(allocation, 0, MEM_RELEASE);
        return;
    }
    uint8_t* row = allocation + page - kRowStride;
    std::memset(row, 0, kRowStride);
    std::memcpy(
        row + 0x09,
        GroupChannel(2, 0),
        kChannelStride);
    *reinterpret_cast<uint8_t**>(g_service + 0x1B0) = row;
    *reinterpret_cast<uint8_t**>(g_service + 0x1B4) =
        row + kRowStride;

    Check(
        SendChannelRequest(channels),
        "exact 0x28-byte row at a guard boundary matched without overread");
    VirtualFree(allocation, 0, MEM_RELEASE);
}
} // namespace

int main()
{
    std::puts(
        "OFFLINE ONLY: ChannelActions ABI/boundary regression; no game, server, or database");

    InitializeControls();
    g_vtable[0x28 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeVsReply);
    g_vtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeVsClick);
    g_vtable[0x3C / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeVsOpen);
    g_lobbyVtable[0x28 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeLobbyReply);

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;

    NativeActions::Config config = {};
    config.username = "offline";
    config.password = "offline";
    config.room_title = "offline-channel";
    config.room_mode = 6;
    config.map_id = 0;

    NativeActions native;
    const bool nativeBound = native.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        config);
    Check(
        nativeBound,
        "NativeActions transport observer bound to offline stubs");

    ChannelActions channels;
    g_channels = &channels;
    ResetFixture(channels);
    const bool channelBound = channels.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        &native);
    Check(
        channelBound,
        "ChannelActions bound only after the verified VS vtable matched");
    Check(
        g_networkSendDetour != nullptr && g_replyDetour != nullptr,
        "transport and reply detours were captured offline");

    if (nativeBound && channelBound) {
        TestAlreadySelected(channels);
        TestLobbyFallback(channels);
        TestReturnToNormalVsGroup(channels);
        TestSemanticChannelType(channels);
        TestLobbyFailureBoundaries(channels);
        TestLobbyReplyStateGuard(channels);
        TestOpenPageRequestAndReplyLifetime(channels);
        TestRejectedAndUnconsumedReplies(channels);
        TestStaleReplyBaseline(channels);
        TestMalformedBoundaries(channels);
        TestGuardedRowBoundary(channels);
    }

    DestroyControls();
    if (g_failures) {
        std::fprintf(
            stderr,
            "OFFLINE CHANNEL ABI FAILURES: %d\n",
            g_failures);
        return 1;
    }
    std::puts("OFFLINE CHANNEL ABI PASS");
    return 0;
}
