// OFFLINE-ONLY ABI, ownership, and boundary tests for BattleShipActions.
//
// The fixture models shipped x86 popup layouts and original reply ownership.
// It never starts GOnline, contacts a server, writes a database, grants a ship,
// or claims that a real account completed the tutorial.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../mods/game_test_client/battleship_actions.h"

namespace
{
constexpr size_t kProfileBattleShip = 0x2A6;
constexpr size_t kBattleShipPayloadSize = 0x2BC;
constexpr uint32_t kTutorialShipId = 270005;

using NetworkSendDetour =
    int(__fastcall*)(
        void*,
        void*,
        void*,
        const void*,
        int,
        char);
using CloneDetour = void*(__fastcall*)(void*, void*);
using DispatchDetour =
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
uint8_t g_network[0x60] = {};
uint8_t g_tutorialLive[0x100] = {};
uint8_t g_resultLive[0x100] = {};
uint8_t g_tutorialRoot = 0;
uint8_t g_resultRoot = 0;
uint8_t g_nextControl[0x180] = {};
uint8_t g_closeControl[0x180] = {};
uint8_t g_okControl[0x180] = {};
void* g_tutorialVtable[32] = {};
void* g_resultVtable[32] = {};

NetworkSendDetour g_networkSendDetour = nullptr;
CloneDetour g_tutorialCloneDetour = nullptr;
CloneDetour g_resultCloneDetour = nullptr;
DispatchDetour g_dispatchDetour = nullptr;
BattleShipActions* g_battleships = nullptr;

unsigned g_tutorialCtorCalls = 0;
unsigned g_tutorialDtorCalls = 0;
unsigned g_namedEnqueueCalls = 0;
unsigned g_tutorialCloneCalls = 0;
unsigned g_tutorialActionCalls = 0;
unsigned g_resultCloneCalls = 0;
unsigned g_resultActionCalls = 0;
unsigned g_networkOriginalCalls = 0;
unsigned g_dispatchOriginalCalls = 0;
bool g_enqueueAbiValid = true;
bool g_actionAbiValid = true;
bool g_resultActionAbiValid = true;
bool g_sendOnClose = true;
bool g_dispatchCopies = true;
bool g_missingNext = false;
bool g_missingClose = false;
bool g_missingOk = false;
uint16_t g_lastRequestOpcode = 0;
uint32_t g_replySequenceInsideOriginal = UINT32_MAX;
void* g_replyAllocation = nullptr;
wchar_t g_lastActionName[32] = {};

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
        std::memcpy(
            bytes,
            value,
            (length + 1) * sizeof(wchar_t));
        *reinterpret_cast<uint32_t*>(bytes + 0x14) = 7;
    } else {
        const size_t byteCount =
            (length + 1) * sizeof(wchar_t);
        auto* allocation = static_cast<wchar_t*>(
            HeapAlloc(
                GetProcessHeap(),
                HEAP_ZERO_MEMORY,
                byteCount));
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

void InitializeControls()
{
    WriteNativeString(g_nextControl + 0x118, L"NextBtn");
    WriteNativeString(g_closeControl + 0x118, L"CloseBtn");
    WriteNativeString(g_okControl + 0x118, L"OkBtn");
}

void DestroyControls()
{
    DestroyNativeString(g_nextControl + 0x118);
    DestroyNativeString(g_closeControl + 0x118);
    DestroyNativeString(g_okControl + 0x118);
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
    if (packet && size >= 2)
        g_lastRequestOpcode =
            *static_cast<const uint16_t*>(packet);
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

void* __fastcall FakeTutorialCtor(void* self, void*)
{
    ++g_tutorialCtorCalls;
    std::memset(self, 0, 0x58);
    *static_cast<void***>(self) = g_tutorialVtable;
    return self;
}

void __fastcall FakeTutorialDtor(void* self, void*)
{
    ++g_tutorialDtorCalls;
    if (self)
        *static_cast<void***>(self) = nullptr;
}

void* __fastcall FakeTutorialClone(void*, void*)
{
    ++g_tutorialCloneCalls;
    std::memset(g_tutorialLive, 0, sizeof(g_tutorialLive));
    *reinterpret_cast<void***>(g_tutorialLive) =
        g_tutorialVtable;
    return g_tutorialLive;
}

void* __fastcall FakeResultClone(void*, void*)
{
    ++g_resultCloneCalls;
    std::memset(g_resultLive, 0, sizeof(g_resultLive));
    *reinterpret_cast<void***>(g_resultLive) =
        g_resultVtable;
    return g_resultLive;
}

void __cdecl FakeNamedUiEnqueue(
    void* prototype,
    const void* name,
    int flag)
{
    ++g_namedEnqueueCalls;
    const wchar_t* value = ReadNativeString(name);
    g_enqueueAbiValid =
        prototype &&
        *static_cast<void***>(prototype) == g_tutorialVtable &&
        value &&
        std::wcscmp(value, L"BattleShipTutorial") == 0 &&
        flag == 1 &&
        g_tutorialCloneDetour;
    if (!g_enqueueAbiValid)
        return;
    void* live = g_tutorialCloneDetour(prototype, nullptr);
    if (!live)
        return;
    *reinterpret_cast<void**>(
        static_cast<uint8_t*>(live) + 0x4C) =
        &g_tutorialRoot;
    *reinterpret_cast<uint8_t*>(
        static_cast<uint8_t*>(live) + 0x50) = 0;
    *reinterpret_cast<uint32_t*>(
        static_cast<uint8_t*>(live) + 0x54) = 5;
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* name,
    uint32_t type)
{
    if (!root || type != 4)
        return nullptr;
    const wchar_t* value = ReadNativeString(name);
    if (!value)
        return nullptr;
    if (root == &g_tutorialRoot) {
        if (std::wcscmp(value, L"NextBtn") == 0)
            return g_missingNext ? nullptr : g_nextControl;
        if (std::wcscmp(value, L"CloseBtn") == 0)
            return g_missingClose ? nullptr : g_closeControl;
    }
    if (root == &g_resultRoot &&
        std::wcscmp(value, L"OkBtn") == 0)
        return g_missingOk ? nullptr : g_okControl;
    return nullptr;
}

void __stdcall FakeTutorialAction(
    void* popup,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_tutorialActionCalls;
    const wchar_t* value = ReadNativeString(nativeName);
    if (value)
        wcsncpy_s(
            g_lastActionName,
            value,
            _TRUNCATE);
    const bool next =
        value && std::wcscmp(value, L"NextBtn") == 0;
    const bool close =
        value && std::wcscmp(value, L"CloseBtn") == 0;
    g_actionAbiValid =
        g_actionAbiValid &&
        popup == g_tutorialLive &&
        event == 0x101 &&
        ((next &&
          control == g_nextControl &&
          nativeName == g_nextControl + 0x118) ||
         (close &&
          control == g_closeControl &&
          nativeName == g_closeControl + 0x118));
    if (next) {
        ++*reinterpret_cast<uint8_t*>(
            g_tutorialLive + 0x50);
        return;
    }
    if (!close || !g_sendOnClose || !g_networkSendDetour)
        return;

    uint8_t packet[0x0A] = {};
    *reinterpret_cast<uint16_t*>(packet) = 0x04FC;
    *reinterpret_cast<uint32_t*>(packet + 0x04) =
        *reinterpret_cast<uint32_t*>(
            g_profileStorage + 0xBC);
    *reinterpret_cast<uint16_t*>(packet + 0x08) =
        static_cast<uint16_t>(
            *reinterpret_cast<uint32_t*>(
                g_profileStorage + 0x138));
    g_networkSendDetour(
        g_network,
        nullptr,
        nullptr,
        packet,
        sizeof(packet),
        0);
}

void __stdcall FakeResultAction(
    void* popup,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    ++g_resultActionCalls;
    const wchar_t* value = ReadNativeString(nativeName);
    g_resultActionAbiValid =
        popup == g_resultLive &&
        event == 0x101 &&
        value &&
        std::wcscmp(value, L"OkBtn") == 0 &&
        control == g_okControl &&
        nativeName == g_okControl + 0x118;
    if (g_resultActionAbiValid)
        *reinterpret_cast<uint32_t*>(
            g_profileStorage + 0xBC) = 4;
}

int __fastcall FakeTutorialDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int opcode,
    void* packet)
{
    ++g_dispatchOriginalCalls;
    g_replySequenceInsideOriginal =
        g_battleships
            ? g_battleships->State().reply_sequence
            : UINT32_MAX;
    const uint16_t normalized =
        static_cast<uint16_t>(opcode & 0xFFFF);
    if (normalized == 0x08EF &&
        packet &&
        g_dispatchCopies) {
        std::memcpy(
            g_profileStorage + kProfileBattleShip,
            static_cast<const uint8_t*>(packet) + 0x0A,
            kBattleShipPayloadSize);
        if (g_resultCloneDetour) {
            void* live =
                g_resultCloneDetour(
                    g_tutorialLive,
                    nullptr);
            if (live) {
                *reinterpret_cast<void**>(
                    static_cast<uint8_t*>(live) + 0x4C) =
                    &g_resultRoot;
            }
        }
    }
    if (g_replyAllocation) {
        VirtualFree(g_replyAllocation, 0, MEM_RELEASE);
        g_replyAllocation = nullptr;
    }
    return 83;
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
    if (std::strcmp(name, "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(&FakeUiFindControl);
    if (std::strcmp(name, "flow.battleship_tutorial_ctor") == 0)
        return reinterpret_cast<void*>(&FakeTutorialCtor);
    if (std::strcmp(name, "flow.battleship_tutorial_dtor") == 0)
        return reinterpret_cast<void*>(&FakeTutorialDtor);
    if (std::strcmp(name, "flow.named_ui_enqueue") == 0)
        return reinterpret_cast<void*>(&FakeNamedUiEnqueue);
    if (std::strcmp(name, "flow.battleship_tutorial_clone") == 0)
        return reinterpret_cast<void*>(&FakeTutorialClone);
    if (std::strcmp(name, "flow.battleship_tutorial_action") == 0)
        return reinterpret_cast<void*>(&FakeTutorialAction);
    if (std::strcmp(name, "flow.battleship_tutorial_dispatch") == 0)
        return reinterpret_cast<void*>(&FakeTutorialDispatch);
    if (std::strcmp(name, "flow.battleship_tutorial_vtable") == 0)
        return g_tutorialVtable;
    if (std::strcmp(name, "flow.battleship_get_popup_clone") == 0)
        return reinterpret_cast<void*>(&FakeResultClone);
    if (std::strcmp(name, "flow.battleship_get_popup_action") == 0)
        return reinterpret_cast<void*>(&FakeResultAction);
    if (std::strcmp(name, "flow.battleship_get_popup_vtable") == 0)
        return g_resultVtable;
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
        std::strcmp(
            name,
            "flow.battleship_tutorial_clone") == 0) {
        g_tutorialCloneDetour =
            reinterpret_cast<CloneDetour>(detour);
    } else if (
        std::strcmp(
            name,
            "flow.battleship_tutorial_dispatch") == 0) {
        g_dispatchDetour =
            reinterpret_cast<DispatchDetour>(detour);
    } else if (
        std::strcmp(
            name,
            "flow.battleship_get_popup_clone") == 0) {
        g_resultCloneDetour =
            reinterpret_cast<CloneDetour>(detour);
    }
    return 1;
}

void InitializeVtables()
{
    std::memset(
        g_tutorialVtable,
        0,
        sizeof(g_tutorialVtable));
    std::memset(
        g_resultVtable,
        0,
        sizeof(g_resultVtable));
    g_tutorialVtable[0x28 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeTutorialDispatch);
    g_tutorialVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeTutorialAction);
    g_tutorialVtable[0x54 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeTutorialClone);
    g_resultVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeResultAction);
    g_resultVtable[0x54 / sizeof(void*)] =
        reinterpret_cast<void*>(&FakeResultClone);
}

void ResetFixture(BattleShipActions& actions)
{
    actions.Reset();
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(
        g_tutorialLive,
        0,
        sizeof(g_tutorialLive));
    std::memset(
        g_resultLive,
        0,
        sizeof(g_resultLive));
    *reinterpret_cast<uint32_t*>(
        g_app + 0x4A0) = 6;
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0x138) = 9229;
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0xBC) = 2;
    g_profile = g_profileStorage;

    g_tutorialCtorCalls = 0;
    g_tutorialDtorCalls = 0;
    g_namedEnqueueCalls = 0;
    g_tutorialCloneCalls = 0;
    g_tutorialActionCalls = 0;
    g_resultCloneCalls = 0;
    g_resultActionCalls = 0;
    g_networkOriginalCalls = 0;
    g_dispatchOriginalCalls = 0;
    g_enqueueAbiValid = true;
    g_actionAbiValid = true;
    g_resultActionAbiValid = true;
    g_sendOnClose = true;
    g_dispatchCopies = true;
    g_missingNext = false;
    g_missingClose = false;
    g_missingOk = false;
    g_lastRequestOpcode = 0;
    g_replySequenceInsideOriginal = UINT32_MAX;
    g_replyAllocation = nullptr;
    g_lastActionName[0] = L'\0';
}

uint8_t* AllocateAcceptedReply()
{
    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    const size_t page = systemInfo.dwPageSize;
    auto* allocation = static_cast<uint8_t*>(
        VirtualAlloc(
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
    auto* packet =
        allocation + page -
        (0x0A + kBattleShipPayloadSize);
    std::memset(
        packet,
        0,
        0x0A + kBattleShipPayloadSize);
    uint8_t* payload = packet + 0x0A;
    *reinterpret_cast<uint32_t*>(payload + 0x00) =
        0x11223344;
    *reinterpret_cast<uint32_t*>(payload + 0x04) =
        0x55667788;
    *reinterpret_cast<uint32_t*>(payload + 0x0C) =
        kTutorialShipId;
    *reinterpret_cast<uint32_t*>(payload + 0x1C) =
        0xAABBCCDD;
    *reinterpret_cast<uint32_t*>(payload + 0x20) =
        0x12345678;
    g_replyAllocation = allocation;
    return packet;
}

uint8_t* AllocateRejectedReply(uint16_t error)
{
    SYSTEM_INFO systemInfo = {};
    GetSystemInfo(&systemInfo);
    const size_t page = systemInfo.dwPageSize;
    auto* allocation = static_cast<uint8_t*>(
        VirtualAlloc(
            nullptr,
            page,
            MEM_RESERVE | MEM_COMMIT,
            PAGE_READWRITE));
    if (!allocation)
        return nullptr;
    auto* packet = allocation + page - 8;
    std::memset(packet, 0, 8);
    *reinterpret_cast<uint16_t*>(packet + 6) = error;
    g_replyAllocation = allocation;
    return packet;
}

bool SendTutorialRequest(BattleShipActions& actions)
{
    if (actions.OpenTutorial() !=
        BattleShipActions::kTutorialOpened)
        return false;
    for (unsigned page = 0; page < 4; ++page) {
        if (actions.DriveTutorial() !=
            BattleShipActions::kPaging)
            return false;
    }
    return actions.DriveTutorial() ==
        BattleShipActions::kRequestSent;
}

void TestEligibility(BattleShipActions& actions)
{
    ResetFixture(actions);
    *reinterpret_cast<uint32_t*>(g_app + 0x4A0) = 4;
    Check(
        actions.OpenTutorial() ==
            BattleShipActions::kWaitingForLobby,
        "tutorial acquisition waits for the real lobby app state");

    ResetFixture(actions);
    g_profile = nullptr;
    Check(
        actions.OpenTutorial() ==
            BattleShipActions::kProfileUnavailable,
        "missing real profile fails closed");

    ResetFixture(actions);
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0xBC) = 1;
    Check(
        actions.OpenTutorial() ==
            BattleShipActions::kNotEligible,
        "pre-starter tutorial level cannot request battleships");

    ResetFixture(actions);
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + 0xBC) = 4;
    *reinterpret_cast<uint32_t*>(
        g_profileStorage + kProfileBattleShip) = 1;
    Check(
        actions.OpenTutorial() ==
            BattleShipActions::kAlreadyCompleted,
        "completed account cannot repeat the one-time ship acquisition");
}

void TestAcceptedFlow(BattleShipActions& actions)
{
    ResetFixture(actions);
    Check(
        actions.OpenTutorial() ==
            BattleShipActions::kTutorialOpened,
        "original named manager path opened the tutorial");
    const auto opened = actions.State();
    Check(
        g_tutorialCtorCalls == 1 &&
            g_tutorialDtorCalls == 1 &&
            g_namedEnqueueCalls == 1 &&
            g_tutorialCloneCalls == 1 &&
            g_enqueueAbiValid,
        "tutorial used shipped ctor, named enqueue, clone, and dtor ABI");
    Check(
        opened.tutorial_popup_ready &&
            opened.page == 0 &&
            opened.page_count == 5,
        "manager-owned tutorial popup exposed its real five-page state");

    for (unsigned page = 1; page <= 4; ++page) {
        Check(
            actions.DriveTutorial() ==
                BattleShipActions::kPaging,
            "NextBtn callback advanced one native tutorial page");
        Check(
            actions.State().page == page,
            "page observation followed the original callback mutation");
    }
    Check(
        g_actionAbiValid &&
            std::wcscmp(
                g_lastActionName,
                L"NextBtn") == 0,
        "page callbacks received control+0x118 and the real control");

    Check(
        actions.DriveTutorial() ==
            BattleShipActions::kRequestSent,
        "final CloseBtn callback sent the native tutorial request");
    const auto requested = actions.State();
    Check(
        requested.request_sent &&
            requested.request_transport_sequence != 0 &&
            g_networkOriginalCalls == 1 &&
            g_lastRequestOpcode == 0x04FC &&
            g_actionAbiValid &&
            std::wcscmp(
                g_lastActionName,
                L"CloseBtn") == 0,
        "request used original 0x04FC/0x0A transport exactly once");
    Check(
        actions.DriveTutorial() ==
            BattleShipActions::kRequestSent &&
            g_networkOriginalCalls == 1,
        "pending one-time request cannot be duplicated");

    const uint32_t sequenceBefore =
        requested.reply_sequence;
    uint8_t* packet = AllocateAcceptedReply();
    Check(
        packet != nullptr,
        "guarded 0x08EF reply fixture was allocated");
    if (!packet || !g_dispatchDetour)
        return;
    const int handled = g_dispatchDetour(
        g_tutorialLive,
        nullptr,
        1,
        2,
        3,
        0x08EF,
        packet);
    const auto accepted = actions.State();
    Check(
        handled == 83,
        "reply hook preserved the original int return");
    Check(
        g_replySequenceInsideOriginal == sequenceBefore,
        "reply evidence was published only after original consumption");
    Check(
        g_replyAllocation == nullptr &&
            g_dispatchOriginalCalls == 1,
        "original consumer retained and released packet ownership");
    Check(
        accepted.result == BattleShipActions::kAccepted &&
            accepted.reply_sequence == sequenceBefore + 1 &&
            accepted.reply_opcode == 0x08EF &&
            accepted.reply_error == 0 &&
            accepted.reply_consumed,
        "accepted reply requires byte-identical real profile consumption");
    Check(
        accepted.ship_uid_low == 0x11223344 &&
            accepted.ship_uid_high == 0x55667788 &&
            accepted.ship_id == kTutorialShipId &&
            accepted.leader_uid_low == 0xAABBCCDD &&
            accepted.leader_uid_high == 0x12345678,
        "observer copied ship identity before original packet release");
    Check(
        !accepted.tutorial_popup_ready &&
            accepted.result_popup_ready &&
            g_resultCloneCalls == 1,
        "original success closed tutorial and created acquisition popup");

    *reinterpret_cast<void**>(g_resultLive + 0x4C) = nullptr;
    Check(
        actions.ConfirmAcquisitionPopup() ==
                BattleShipActions::kResultPopupUnavailable &&
            actions.State().result ==
                BattleShipActions::kAccepted,
        "result popup may finish opening without losing accepted reply state");
    *reinterpret_cast<void**>(g_resultLive + 0x4C) =
        &g_resultRoot;
    Check(
        actions.ConfirmAcquisitionPopup() ==
            BattleShipActions::kConfirmed,
        "real OkBtn callback completed tutorial-level progression");
    const auto confirmed = actions.State();
    Check(
        confirmed.tutorial_level == 4 &&
            confirmed.confirm_sequence == 1 &&
            g_resultActionCalls == 1 &&
            g_resultActionAbiValid &&
            !confirmed.result_popup_ready,
        "confirmation used the real popup control and dropped its pointer");
    Check(
        actions.OpenTutorial() ==
            BattleShipActions::kAlreadyCompleted,
        "confirmed profile cannot reacquire tutorial ships");
}

void TestRejectedReply(BattleShipActions& actions)
{
    ResetFixture(actions);
    Check(
        SendTutorialRequest(actions),
        "rejection fixture sent a genuine-form request first");
    const uint32_t sequenceBefore =
        actions.State().reply_sequence;
    uint8_t* packet = AllocateRejectedReply(0x08A6);
    Check(
        packet != nullptr,
        "guarded rejection reply fixture was allocated");
    if (!packet || !g_dispatchDetour)
        return;
    g_dispatchDetour(
        g_tutorialLive,
        nullptr,
        1,
        2,
        3,
        0x09E1,
        packet);
    const auto rejected = actions.State();
    Check(
        g_replySequenceInsideOriginal == sequenceBefore &&
            g_replyAllocation == nullptr,
        "rejected packet remained original-consumer-owned");
    Check(
        rejected.result ==
                BattleShipActions::kReplyRejected &&
            rejected.reply_opcode == 0x09E1 &&
            rejected.reply_error == 0x08A6 &&
            rejected.reply_sequence == sequenceBefore + 1 &&
            !rejected.reply_consumed,
        "real error opcode and code were observed without a fake success");
    Check(
        actions.ConfirmAcquisitionPopup() ==
            BattleShipActions::kReplyRejected,
        "rejected acquisition cannot be confirmed");
}

void TestUnconsumedReply(BattleShipActions& actions)
{
    ResetFixture(actions);
    Check(
        SendTutorialRequest(actions),
        "unconsumed fixture sent the normal request first");
    g_dispatchCopies = false;
    uint8_t* packet = AllocateAcceptedReply();
    Check(
        packet != nullptr,
        "guarded unconsumed success fixture was allocated");
    if (!packet || !g_dispatchDetour)
        return;
    g_dispatchDetour(
        g_tutorialLive,
        nullptr,
        1,
        2,
        3,
        0x08EF,
        packet);
    const auto observed = actions.State();
    Check(
        observed.result ==
                BattleShipActions::kReplyNotConsumed &&
            !observed.reply_consumed &&
            observed.ship_id == kTutorialShipId &&
            g_replyAllocation == nullptr,
        "captured packet data alone is not accepted without profile readback");
}

void TestBoundaries(BattleShipActions& actions)
{
    ResetFixture(actions);
    Check(
        actions.OpenTutorial() ==
            BattleShipActions::kTutorialOpened,
        "missing-control fixture opened tutorial");
    g_missingNext = true;
    Check(
        actions.DriveTutorial() ==
            BattleShipActions::kControlMissing,
        "missing manager-owned NextBtn fails closed");

    ResetFixture(actions);
    actions.OpenTutorial();
    *reinterpret_cast<uint32_t*>(
        g_tutorialLive + 0x54) = 0;
    Check(
        actions.DriveTutorial() ==
            BattleShipActions::kPageStateInvalid,
        "zero page count is rejected before a callback");

    ResetFixture(actions);
    actions.OpenTutorial();
    WriteNativeString(
        g_nextControl + 0x118,
        L"WrongBtn");
    Check(
        actions.DriveTutorial() ==
            BattleShipActions::kControlNameInvalid,
        "lookup result with mismatched embedded name is rejected");
    WriteNativeString(
        g_nextControl + 0x118,
        L"NextBtn");

    ResetFixture(actions);
    actions.OpenTutorial();
    for (unsigned page = 0; page < 4; ++page)
        actions.DriveTutorial();
    g_sendOnClose = false;
    Check(
        actions.DriveTutorial() ==
            BattleShipActions::kRequestNotSent &&
            g_networkOriginalCalls == 0,
        "CloseBtn callback without an observed wire send fails closed");
}
} // namespace

int main()
{
    InitializeControls();
    InitializeVtables();

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;
    auto* mod = reinterpret_cast<SdgoMod*>(1);

    NativeActions native;
    const NativeActions::Config config{
        "offline-user",
        "offline-password",
        "offline-room",
        1,
        1,
    };
    Check(
        native.Bind(&api, mod, config),
        "NativeActions transport observer bound to offline fixture");

    BattleShipActions invalid;
    void* savedClone =
        g_tutorialVtable[0x54 / sizeof(void*)];
    g_tutorialVtable[0x54 / sizeof(void*)] = nullptr;
    Check(
        !invalid.Bind(&api, mod, &native),
        "vtable mismatch rejects battleship binding");
    g_tutorialVtable[0x54 / sizeof(void*)] = savedClone;

    BattleShipActions actions;
    g_battleships = &actions;
    Check(
        actions.Bind(&api, mod, &native),
        "BattleShipActions exact ABI hooks bound");
    Check(
        g_tutorialCloneDetour &&
            g_dispatchDetour &&
            g_resultCloneDetour,
        "clone and reply observers were installed");

    TestEligibility(actions);
    TestAcceptedFlow(actions);
    TestRejectedReply(actions);
    TestUnconsumedReply(actions);
    TestBoundaries(actions);

    DestroyControls();
    std::printf(
        "SUMMARY: %s (%d failure%s)\n",
        g_failures ? "FAIL" : "PASS",
        g_failures,
        g_failures == 1 ? "" : "s");
    return g_failures ? 1 : 0;
}
