// OFFLINE-ONLY ABI, ownership, and boundary tests for
// BattleShipDeckActions.
//
// The fixture models the shipped x86 object layouts and original callback
// ownership. It never starts GOnline, contacts a server, writes a database,
// grants a unit, or claims that a real account configured a battleship deck.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include "../mods/game_test_client/battleship_deck_actions.h"

namespace
{
constexpr size_t kFullUnitSize = 0x99;
constexpr size_t kShipPayloadSize = 0x2BC;
constexpr size_t kPacketSize = 6 + kShipPayloadSize;
constexpr size_t kEditorUnits = 0x1E8;
constexpr size_t kManagerSelectedShip = 0x261C;
constexpr size_t kManagerEditableLeader = 0x2BC8;
constexpr size_t kManagerEditableUnits = 0x2BD0;
constexpr size_t kProfileBattleShip = 0x2A6;
constexpr size_t kShipLeader = 0x1C;
constexpr size_t kShipUnitUids = 0x34;
constexpr uint64_t kShipUid = 60057;
constexpr uint64_t kAlternativeShipUid = 60058;
constexpr uint64_t kTargetUids[4] = {
    169384,
    169339,
    97175,
    118323,
};
constexpr uint32_t kTargetIds[4] = {
    22040,
    22042,
    22097,
    24402,
};

using NetworkSendDetour =
    int(__fastcall*)(
        void*,
        void*,
        void*,
        const void*,
        int,
        char);
using LoginDispatchDetour =
    int(__fastcall*)(
        void*,
        void*,
        int,
        int,
        int,
        int,
        void*);
using ReplyDetour =
    void(__fastcall*)(void*, void*, const void*);

int g_failures = 0;
uint8_t g_app[0x500] = {};
uint8_t g_profileStorage[0x6000] = {};
uint8_t* g_profile = g_profileStorage;
uint8_t g_network[0x60] = {};
uint8_t g_stream = 0;
uint8_t g_manager[0x2E50] = {};
uint8_t g_lobbyManager[0x200] = {};
uint8_t g_editor[0x500] = {};
uint8_t g_lobbyRoot = 0;
uint8_t g_lobbyControl[0x180] = {};
uint8_t g_unitInfo[0x100] = {};
uint8_t g_inventory[8][kFullUnitSize] = {};
uint8_t g_grids[4][0x140] = {};
uint8_t g_serviceManagerObject = 0;
void* g_serviceManager = &g_serviceManagerObject;
void* g_lobbyVtable[32] = {};
void* g_managerVtable[16] = {};
void* g_editorVtable[32] = {};

NetworkSendDetour g_networkSendDetour = nullptr;
LoginDispatchDetour g_loginDispatchDetour = nullptr;
ReplyDetour g_saveReplyDetour = nullptr;
ReplyDetour g_goOutReplyDetour = nullptr;
BattleShipDeckActions* g_decks = nullptr;

unsigned g_lobbyCalls = 0;
unsigned g_removeCalls = 0;
unsigned g_selectCalls = 0;
unsigned g_refreshCalls = 0;
unsigned g_queueCalls = 0;
unsigned g_leaderCalls = 0;
unsigned g_saveCalls = 0;
unsigned g_goOutCalls = 0;
int g_lastGoOutIndex = -1;
bool g_serviceDescriptorValid = true;
bool g_selectCopiedFullRecord = true;
bool g_replyReleased = false;
void* g_replyAllocation = nullptr;
uint32_t g_sequenceInsideSaveOriginal = UINT32_MAX;
uint32_t g_sequenceInsideGoOutOriginal = UINT32_MAX;

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

uint64_t ReadUid(const void* storage)
{
    uint64_t value = 0;
    if (storage)
        std::memcpy(&value, storage, sizeof(value));
    return value;
}

void WriteUid(void* storage, uint64_t value)
{
    if (storage)
        std::memcpy(storage, &value, sizeof(value));
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
    const auto* bytes =
        static_cast<const uint8_t*>(storage);
    const uint32_t capacity =
        *reinterpret_cast<const uint32_t*>(
            bytes + 0x14);
    return capacity < 8
        ? reinterpret_cast<const wchar_t*>(bytes)
        : *reinterpret_cast<wchar_t* const*>(bytes);
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

char* __cdecl FakePasswordMd5Hex(
    const void*,
    char* output,
    int)
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
    const void*,
    int,
    char)
{
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

void __fastcall FakeNativeWStringDtor(
    void* self,
    void*)
{
    DestroyNativeString(self);
}

void* __fastcall FakeNicknameDialogCtor(
    void* self,
    void*)
{
    return self;
}

void* __cdecl FakeEnqueueUiObject(
    void* object,
    int)
{
    return object;
}

void __fastcall FakeNicknameDialogDtor(
    void*,
    void*)
{
}

void __fastcall FakeNicknameDialogConfirm(
    void*,
    void*)
{
}

void* __fastcall FakeServiceFindByName(
    void* self,
    void*,
    const void* descriptor)
{
    if (self != g_serviceManager || !descriptor)
        return nullptr;
    const auto* bytes =
        static_cast<const uint8_t*>(descriptor);
    if (*reinterpret_cast<const uint32_t*>(bytes) != 1) {
        g_serviceDescriptorValid = false;
        return nullptr;
    }
    const wchar_t* name =
        ReadNativeString(bytes + sizeof(uint32_t));
    if (!name) {
        g_serviceDescriptorValid = false;
        return nullptr;
    }
    if (std::wcscmp(
            name,
            L"SelectBattleShipUnitList") == 0)
        return g_editor;
    if (std::wcscmp(name, L"unitmgr") == 0)
        return g_lobbyManager;
    return nullptr;
}

void* __fastcall FakeUiFindControl(
    void* root,
    void*,
    const void* nativeName,
    uint32_t type)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (root == &g_lobbyRoot && type == 4 &&
        name &&
        std::wcscmp(
            name,
            L"SelectBattleShipBtn") == 0)
        return g_lobbyControl;
    return nullptr;
}

void __stdcall FakeLobbySelectBattleShip(
    void* self,
    uint32_t event,
    const void* nativeName,
    void* control)
{
    const wchar_t* name =
        ReadNativeString(nativeName);
    if (self == g_lobbyManager &&
        event == 0x101 &&
        control == g_lobbyControl &&
        name &&
        std::wcscmp(
            name,
            L"SelectBattleShipBtn") == 0)
        ++g_lobbyCalls;
}

void* __fastcall FakeUnitBriefBySlot(
    void* self,
    void*,
    void* output,
    uint16_t slot)
{
    if (!output)
        return output;
    std::memset(output, 0, 0x19);
    if (self == g_unitInfo &&
        slot < sizeof(g_inventory) /
            sizeof(g_inventory[0]))
        std::memcpy(output, g_inventory[slot], 0x19);
    return output;
}

void* __fastcall FakeUnitFullBySlot(
    void* self,
    void*,
    void* output,
    uint16_t slot)
{
    if (!output)
        return output;
    std::memset(output, 0, kFullUnitSize);
    if (self == g_unitInfo &&
        slot < sizeof(g_inventory) /
            sizeof(g_inventory[0]))
        std::memcpy(
            output,
            g_inventory[slot],
            kFullUnitSize);
    return output;
}

void* __cdecl FakeGridData()
{
    return g_grids;
}

bool __fastcall FakeGridFind(
    void* self,
    void*,
    void** output,
    uint32_t id,
    uint32_t type,
    bool log)
{
    if (!output)
        return false;
    *output = nullptr;
    if (self != g_grids || type != 2 || log)
        return false;
    for (unsigned i = 0; i < 4; ++i) {
        if (*reinterpret_cast<uint32_t*>(
                g_grids[i]) == id) {
            *output = g_grids[i];
            return true;
        }
    }
    return false;
}

void __fastcall FakeEditorRemoveUnit(
    void* self,
    void*,
    const void* record)
{
    if (self != g_editor || !record)
        return;
    const auto* bytes =
        static_cast<const uint8_t*>(record);
    const ptrdiff_t offset =
        bytes - (g_editor + kEditorUnits);
    if (offset < 0 ||
        offset % static_cast<ptrdiff_t>(
            kFullUnitSize) != 0 ||
        offset >= static_cast<ptrdiff_t>(
            4 * kFullUnitSize))
        return;
    ++g_removeCalls;
    std::memset(
        const_cast<uint8_t*>(bytes),
        0,
        kFullUnitSize);
}

void __fastcall FakeEditorSelectUnit(
    void* self,
    void*,
    uint32_t slot,
    const void* record)
{
    if (self != g_editor ||
        slot >= 4 || !record)
        return;
    ++g_selectCalls;
    std::memcpy(
        g_editor + kEditorUnits +
            slot * kFullUnitSize,
        record,
        kFullUnitSize);
    g_selectCopiedFullRecord =
        g_selectCopiedFullRecord &&
        std::memcmp(
            g_editor + kEditorUnits +
                slot * kFullUnitSize,
            record,
            kFullUnitSize) == 0;
}

void __fastcall FakeEditorRefresh(
    void* self,
    void*)
{
    if (self != g_editor)
        return;
    ++g_refreshCalls;
    if (!ReadUid(g_editor + 0x1E0)) {
        WriteUid(
            g_editor + 0x1E0,
            ReadUid(
                g_editor + kEditorUnits + 0x08));
    }
}

void __fastcall FakeEditorQueueState(
    void* self,
    void*,
    uint32_t state)
{
    if (self != g_editor || state != 4)
        return;
    ++g_queueCalls;
    std::memcpy(
        g_manager + kManagerEditableUnits,
        g_editor + kEditorUnits,
        4 * kFullUnitSize);
    WriteUid(
        g_manager + kManagerEditableLeader,
        ReadUid(g_editor + 0x1E0));
}

void __fastcall FakeEditorSetLeader(
    void* self,
    void*,
    int slot)
{
    if (self != g_editor ||
        slot < 0 || slot >= 4)
        return;
    ++g_leaderCalls;
    const uint64_t uid =
        ReadUid(
            g_editor + kEditorUnits +
            static_cast<size_t>(slot) *
                kFullUnitSize +
            0x08);
    WriteUid(g_editor + 0x1E0, uid);
    WriteUid(
        g_manager + kManagerEditableLeader,
        uid);
}

void __fastcall FakeSaveDeck(
    void* self,
    void*)
{
    if (self == g_manager)
        ++g_saveCalls;
}

void __fastcall FakeGoOut(
    void* self,
    void*,
    int carousel)
{
    if (self != g_manager)
        return;
    ++g_goOutCalls;
    g_lastGoOutIndex = carousel;
}

void ApplyShipPayload(
    const uint8_t* ship,
    bool save)
{
    if (!ship)
        return;
    if (save) {
        std::memcpy(
            g_manager + kManagerSelectedShip,
            ship,
            kShipPayloadSize);
    }
    std::memcpy(
        g_profileStorage + kProfileBattleShip,
        ship,
        kShipPayloadSize);
    if (!save) {
        WriteUid(
            g_manager + 0x20D8,
            ReadUid(ship));
    }
}

void ReleaseReply()
{
    if (!g_replyAllocation)
        return;
    g_replyReleased =
        VirtualFree(
            g_replyAllocation,
            0,
            MEM_RELEASE) != 0;
    g_replyAllocation = nullptr;
}

void __fastcall FakeSaveReply(
    void* self,
    void*,
    const void* packet)
{
    g_sequenceInsideSaveOriginal =
        g_decks
            ? g_decks->State().save_reply_sequence
            : UINT32_MAX;
    if (self == g_manager && packet) {
        const auto* bytes =
            static_cast<const uint8_t*>(packet);
        ApplyShipPayload(bytes + 6, true);
        for (unsigned i = 0; i < 4; ++i)
            g_inventory[i + 2][0x30] ^= 0x5A;
    }
    ReleaseReply();
}

void __fastcall FakeGoOutReply(
    void* self,
    void*,
    const void* packet)
{
    g_sequenceInsideGoOutOriginal =
        g_decks
            ? g_decks->State().goout_reply_sequence
            : UINT32_MAX;
    if (self == g_manager && packet) {
        const auto* bytes =
            static_cast<const uint8_t*>(packet);
        ApplyShipPayload(bytes + 6, false);
    }
    ReleaseReply();
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
    if (std::strcmp(name, "flow.native_ui_find_control") == 0)
        return reinterpret_cast<void*>(&FakeUiFindControl);
    if (std::strcmp(
            name,
            "flow.battleship_lobby_manager_vtable") == 0)
        return g_lobbyVtable;
    if (std::strcmp(
            name,
            "flow.battleship_lobby_select_business") == 0)
        return reinterpret_cast<void*>(
            &FakeLobbySelectBattleShip);
    if (std::strcmp(
            name,
            "flow.battleship_room_manager_vtable") == 0)
        return g_managerVtable;
    if (std::strcmp(
            name,
            "flow.battleship_editor_vtable") == 0)
        return g_editorVtable;
    if (std::strcmp(name, "flow.unit_info_pointer") == 0) {
        static void* pointer = g_unitInfo;
        pointer = g_unitInfo;
        return &pointer;
    }
    if (std::strcmp(name, "flow.unit_brief_by_slot") == 0)
        return reinterpret_cast<void*>(
            &FakeUnitBriefBySlot);
    if (std::strcmp(name, "flow.unit_full_by_slot") == 0)
        return reinterpret_cast<void*>(
            &FakeUnitFullBySlot);
    if (std::strcmp(name, "flow.grid_data") == 0)
        return reinterpret_cast<void*>(&FakeGridData);
    if (std::strcmp(name, "flow.grid_find") == 0)
        return reinterpret_cast<void*>(&FakeGridFind);
    if (std::strcmp(
            name,
            "flow.battleship_editor_remove_unit") == 0)
        return reinterpret_cast<void*>(
            &FakeEditorRemoveUnit);
    if (std::strcmp(
            name,
            "flow.battleship_editor_select_unit") == 0)
        return reinterpret_cast<void*>(
            &FakeEditorSelectUnit);
    if (std::strcmp(
            name,
            "flow.battleship_editor_refresh") == 0)
        return reinterpret_cast<void*>(
            &FakeEditorRefresh);
    if (std::strcmp(
            name,
            "flow.battleship_editor_queue_state") == 0)
        return reinterpret_cast<void*>(
            &FakeEditorQueueState);
    if (std::strcmp(
            name,
            "flow.battleship_editor_set_leader") == 0)
        return reinterpret_cast<void*>(
            &FakeEditorSetLeader);
    if (std::strcmp(
            name,
            "flow.battleship_save_deck") == 0)
        return reinterpret_cast<void*>(&FakeSaveDeck);
    if (std::strcmp(
            name,
            "flow.battleship_goout") == 0)
        return reinterpret_cast<void*>(&FakeGoOut);
    if (std::strcmp(
            name,
            "flow.battleship_save_reply") == 0)
        return reinterpret_cast<void*>(&FakeSaveReply);
    if (std::strcmp(
            name,
            "flow.battleship_goout_reply") == 0)
        return reinterpret_cast<void*>(&FakeGoOutReply);
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
            "flow.login_service_dispatch") == 0) {
        g_loginDispatchDetour =
            reinterpret_cast<LoginDispatchDetour>(detour);
    } else if (
        std::strcmp(
            name,
            "flow.battleship_save_reply") == 0) {
        g_saveReplyDetour =
            reinterpret_cast<ReplyDetour>(detour);
    } else if (
        std::strcmp(
            name,
            "flow.battleship_goout_reply") == 0) {
        g_goOutReplyDetour =
            reinterpret_cast<ReplyDetour>(detour);
    }
    return 1;
}

void InitializeInventory()
{
    std::memset(g_inventory, 0, sizeof(g_inventory));
    *reinterpret_cast<uint32_t*>(g_inventory[1]) =
        270005;
    WriteUid(g_inventory[1] + 0x08, 90001);
    for (unsigned i = 0; i < 4; ++i) {
        auto* full = g_inventory[i + 2];
        *reinterpret_cast<uint32_t*>(full) =
            kTargetIds[i];
        *reinterpret_cast<uint32_t*>(full + 0x04) =
            i + 2;
        WriteUid(full + 0x08, kTargetUids[i]);
        for (size_t at = 0x10;
             at < kFullUnitSize;
             ++at)
            full[at] =
                static_cast<uint8_t>(
                    i * 17 + at);
    }
}

void InitializeGrids()
{
    std::memset(g_grids, 0, sizeof(g_grids));
    for (unsigned i = 0; i < 4; ++i) {
        *reinterpret_cast<uint32_t*>(g_grids[i]) =
            kTargetIds[i];
        g_grids[i][0x139] = 2;
    }
}

void ResetFixture(
    BattleShipDeckActions& decks,
    bool includeAlternative = true)
{
    decks.Reset();
    std::memset(g_app, 0, sizeof(g_app));
    std::memset(
        g_profileStorage,
        0,
        sizeof(g_profileStorage));
    std::memset(g_network, 0, sizeof(g_network));
    std::memset(g_manager, 0, sizeof(g_manager));
    std::memset(
        g_lobbyManager,
        0,
        sizeof(g_lobbyManager));
    std::memset(g_editor, 0, sizeof(g_editor));
    std::memset(g_unitInfo, 0, sizeof(g_unitInfo));
    InitializeInventory();
    InitializeGrids();

    g_profile = g_profileStorage;
    g_serviceManager = &g_serviceManagerObject;
    *reinterpret_cast<uint32_t*>(g_app + 0x4A0) = 20;
    *reinterpret_cast<void**>(g_app + 0x4A4) =
        g_manager;
    *reinterpret_cast<void***>(g_manager) =
        g_managerVtable;
    *reinterpret_cast<void***>(g_editor) =
        g_editorVtable;
    g_editor[0x120] = 1;
    g_editor[0x129] = 8;
    *reinterpret_cast<void**>(g_editor + 0x454) =
        g_editor + 0x40;
    *reinterpret_cast<int32_t*>(g_unitInfo + 0x0C) =
        7;
    g_unitInfo[0x20] = 1;

    WriteUid(g_manager + kManagerSelectedShip, kShipUid);
    *reinterpret_cast<uint16_t*>(
        g_manager + 0x20CE) =
        includeAlternative ? 2 : 1;
    WriteUid(g_manager + 0x20E0, kShipUid);
    if (includeAlternative)
        WriteUid(g_manager + 0x20E0 + 0x58,
                 kAlternativeShipUid);
    WriteUid(g_manager + 0x20D8, kShipUid);
    *reinterpret_cast<float*>(
        g_manager + 0x2080) = 70.0f;

    g_lobbyCalls = 0;
    g_removeCalls = 0;
    g_selectCalls = 0;
    g_refreshCalls = 0;
    g_queueCalls = 0;
    g_leaderCalls = 0;
    g_saveCalls = 0;
    g_goOutCalls = 0;
    g_lastGoOutIndex = -1;
    g_serviceDescriptorValid = true;
    g_selectCopiedFullRecord = true;
    g_replyReleased = false;
    g_sequenceInsideSaveOriginal = UINT32_MAX;
    g_sequenceInsideGoOutOriginal = UINT32_MAX;
    if (g_replyAllocation) {
        VirtualFree(g_replyAllocation, 0, MEM_RELEASE);
        g_replyAllocation = nullptr;
    }
}

uint8_t* AllocateReply(
    uint16_t opcode,
    uint64_t shipUid,
    const uint64_t unitUids[4])
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
        allocation + page - kPacketSize;
    std::memset(packet, 0, kPacketSize);
    *reinterpret_cast<uint16_t*>(packet) = opcode;
    auto* ship = packet + 6;
    WriteUid(ship, shipUid);
    WriteUid(ship + kShipLeader, unitUids[0]);
    for (unsigned i = 0; i < 4; ++i) {
        WriteUid(
            ship + kShipUnitUids + i * 8,
            unitUids[i]);
    }
    g_replyAllocation = allocation;
    g_replyReleased = false;
    return packet;
}

bool AdvanceToSaveRequest(
    BattleShipDeckActions& decks)
{
    for (unsigned attempt = 0;
         attempt < 16;
         ++attempt) {
        const auto result =
            decks.EnsureDeck(kTargetUids);
        if (result == BattleShipDeckActions::kSaving &&
            g_saveCalls == 1)
            return true;
        if (result >=
            BattleShipDeckActions::kInvalidTarget)
            return false;
    }
    return false;
}

bool DeliverSaveReply(
    BattleShipDeckActions& decks,
    uint64_t shipUid = kShipUid)
{
    const uint32_t before =
        decks.State().save_reply_sequence;
    uint8_t* packet =
        AllocateReply(
            0x0A27,
            shipUid,
            kTargetUids);
    if (!packet || !g_saveReplyDetour)
        return false;
    g_saveReplyDetour(
        g_manager,
        nullptr,
        packet);
    const auto after = decks.State();
    return g_replyReleased &&
           g_sequenceInsideSaveOriginal == before &&
           after.save_reply_sequence == before + 1 &&
           after.save_reply_opcode == 0x0A27;
}

bool DeliverGoOutReply(
    BattleShipDeckActions& decks,
    uint64_t shipUid,
    const uint64_t unitUids[4])
{
    const uint32_t before =
        decks.State().goout_reply_sequence;
    uint8_t* packet =
        AllocateReply(
            0x04A6,
            shipUid,
            unitUids);
    if (!packet || !g_goOutReplyDetour)
        return false;
    g_goOutReplyDetour(
        g_manager,
        nullptr,
        packet);
    const auto after = decks.State();
    return g_replyReleased &&
           g_sequenceInsideGoOutOriginal == before &&
           after.goout_reply_sequence == before + 1 &&
           after.goout_reply_opcode == 0x04A6;
}

void TestLobbyEntry(BattleShipDeckActions& decks)
{
    ResetFixture(decks);
    *reinterpret_cast<uint32_t*>(g_app + 0x4A0) = 6;
    *reinterpret_cast<void***>(g_lobbyManager) =
        g_lobbyVtable;
    *reinterpret_cast<void**>(
        g_lobbyManager + 0x64) =
        &g_lobbyRoot;
    *reinterpret_cast<void**>(
        g_lobbyControl + 0x15C) =
        &g_lobbyRoot;

    const auto result = decks.EnsureDeck(kTargetUids);
    Check(
        result ==
            BattleShipDeckActions::kOpeningBattleShip &&
            g_lobbyCalls == 1,
        "app 6 resolves unitmgr and invokes the verified SelectBattleShipBtn callback");
}

void TestFullRecordSaveAndDoubleGoOut(
    BattleShipDeckActions& decks)
{
    ResetFixture(decks);
    Check(
        AdvanceToSaveRequest(decks),
        "four genuine inventory records reached original Save");
    Check(
        g_serviceDescriptorValid,
        "service lookup used type=1 plus native wstring descriptor ABI");
    Check(
        g_selectCalls == 4 &&
            g_refreshCalls == 4 &&
            g_queueCalls == 4 &&
            g_selectCopiedFullRecord,
        "each Getter record was copied as the complete 0x99 Full99 object");
    for (unsigned i = 0; i < 4; ++i) {
        Check(
            std::memcmp(
                g_editor + kEditorUnits +
                    i * kFullUnitSize,
                g_inventory[i + 2],
                kFullUnitSize) == 0,
            "editor slot exactly matches its original FullGetter record");
    }
    Check(
        ReadUid(g_editor + kEditorUnits + 0x08) ==
            kTargetUids[0],
        "editor readback uses Full99 UID offset +0x08");

    Check(
        DeliverSaveReply(decks),
        "guarded 0A27 bytes were retained until original consumption");
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kGoingOut &&
            g_goOutCalls == 1 &&
            g_lastGoOutIndex == 1,
        "already-current target first switches through the real alternate carousel ship");

    const uint64_t alternativeUnits[4] = {
        701,
        702,
        703,
        704,
    };
    Check(
        DeliverGoOutReply(
            decks,
            kAlternativeShipUid,
            alternativeUnits),
        "first guarded 04A6 was published after original ownership transfer");
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kGoingOut &&
            g_goOutCalls == 2 &&
            g_lastGoOutIndex == 0,
        "consumed alternate 04A6 triggers original GoOut back to target");
    Check(
        DeliverGoOutReply(
            decks,
            kShipUid,
            kTargetUids),
        "target guarded 04A6 was consumed normally");
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kAccepted,
        "acceptance requires target Save plus a new target 04A6");

    const auto state = decks.State();
    Check(
        state.ship_uid == kShipUid &&
            state.unit_uids[0] == kTargetUids[0] &&
            state.unit_uids[1] == kTargetUids[1] &&
            state.unit_uids[2] == kTargetUids[2] &&
            state.unit_uids[3] == kTargetUids[3],
        "observation exposes target ship and all four real unit UIDs");
    Check(
        state.editor_matches &&
            state.manager_matches &&
            state.profile_matches &&
            state.save_reply_sequence != 0 &&
            state.goout_reply_sequence != 0,
        "final observation requires editor, manager, profile, Save, and GoOut agreement");
    Check(
        g_inventory[2][0x30] ==
            static_cast<uint8_t>(0x30 ^ 0x5A),
        "original Save mutation of nonidentity inventory flags was retained");
}

void TestExistingDeckCleaningUsesUidOffset(
    BattleShipDeckActions& decks)
{
    ResetFixture(decks);
    auto* occupied = g_editor + kEditorUnits;
    *reinterpret_cast<uint32_t*>(occupied) = 0;
    *reinterpret_cast<uint32_t*>(occupied + 0x04) = 0;
    WriteUid(occupied + 0x08, 444444);
    const auto result = decks.EnsureDeck(kTargetUids);
    Check(
        result ==
            BattleShipDeckActions::kCleaningEditor &&
            g_removeCalls == 1 &&
            ReadUid(occupied + 0x08) == 0,
        "cleaning detects an occupied Full99 slot from UID +0x08");
}

void TestNoAlternativeFailsClosed(
    BattleShipDeckActions& decks)
{
    ResetFixture(decks, false);
    Check(
        AdvanceToSaveRequest(decks) &&
            DeliverSaveReply(decks),
        "single-ship boundary reached a consumed Save");
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kGoOutAlternativeMissing &&
            g_goOutCalls == 0,
        "already-current ship cannot pass without a new 04A6 or a real alternate ship");
}

void TestSaveReplyMismatch(
    BattleShipDeckActions& decks)
{
    ResetFixture(decks);
    Check(
        AdvanceToSaveRequest(decks),
        "save-mismatch scenario reached original Save");
    Check(
        DeliverSaveReply(decks, 99999),
        "mismatched guarded Save reply was consumed");
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kSaveReplyMismatch,
        "new 0A27 for another ship fails explicitly");
}

void TestGoOutReplyMismatch(
    BattleShipDeckActions& decks)
{
    ResetFixture(decks);
    Check(
        AdvanceToSaveRequest(decks) &&
            DeliverSaveReply(decks) &&
            decks.EnsureDeck(kTargetUids) ==
                BattleShipDeckActions::kGoingOut,
        "goout-mismatch scenario reached alternate request");
    const uint64_t units[4] = { 1, 2, 3, 4 };
    Check(
        DeliverGoOutReply(decks, 99999, units),
        "mismatched guarded GoOut reply was consumed");
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kGoOutReplyMismatch,
        "new 04A6 for an unexpected ship fails explicitly");
}

void TestReplyObserverGuards(
    BattleShipDeckActions& decks)
{
    ResetFixture(decks);
    Check(
        AdvanceToSaveRequest(decks),
        "reply-guard scenario reached original Save");
    uint32_t before =
        decks.State().save_reply_sequence;
    uint8_t* packet =
        AllocateReply(
            0x0A27,
            kShipUid,
            kTargetUids);
    g_saveReplyDetour(g_editor, nullptr, packet);
    Check(
        g_replyReleased &&
            decks.State().save_reply_sequence == before,
        "Save observer rejects a stale non-manager receiver");

    ResetFixture(decks);
    Check(
        AdvanceToSaveRequest(decks),
        "wrong-opcode scenario reached original Save");
    before = decks.State().save_reply_sequence;
    packet =
        AllocateReply(
            0x0A28,
            kShipUid,
            kTargetUids);
    g_saveReplyDetour(g_manager, nullptr, packet);
    Check(
        g_replyReleased &&
            decks.State().save_reply_sequence == before,
        "Save observer rejects a packet outside 0A27 dispatch");

    ResetFixture(decks);
    Check(
        AdvanceToSaveRequest(decks) &&
            DeliverSaveReply(decks) &&
            decks.EnsureDeck(kTargetUids) ==
                BattleShipDeckActions::kGoingOut,
        "GoOut receiver-guard scenario reached original request");
    before = decks.State().goout_reply_sequence;
    const uint64_t alternativeUnits[4] = {
        701,
        702,
        703,
        704,
    };
    packet =
        AllocateReply(
            0x04A6,
            kAlternativeShipUid,
            alternativeUnits);
    g_goOutReplyDetour(g_editor, nullptr, packet);
    Check(
        g_replyReleased &&
            decks.State().goout_reply_sequence == before,
        "GoOut observer rejects a stale non-manager receiver");

    ResetFixture(decks);
    Check(
        AdvanceToSaveRequest(decks) &&
            DeliverSaveReply(decks) &&
            decks.EnsureDeck(kTargetUids) ==
                BattleShipDeckActions::kGoingOut,
        "GoOut opcode-guard scenario reached original request");
    before = decks.State().goout_reply_sequence;
    packet =
        AllocateReply(
            0x04A7,
            kAlternativeShipUid,
            alternativeUnits);
    g_goOutReplyDetour(g_manager, nullptr, packet);
    Check(
        g_replyReleased &&
            decks.State().goout_reply_sequence == before,
        "GoOut observer rejects a packet outside 04A6 dispatch");
}

void TestInventoryAndCarouselBoundaries(
    BattleShipDeckActions& decks)
{
    ResetFixture(decks);
    WriteUid(g_inventory[5] + 0x08, 0);
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kRequiredUnitMissing,
        "missing required real UID fails before editor mutation");

    ResetFixture(decks);
    g_grids[3][0x139] = 3;
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kRequiredUnitCostMismatch,
        "deck cost must equal the verified native four-C total");

    ResetFixture(decks);
    g_editor[0x129] = 7;
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kEditorCostLimitMismatch,
        "native editor maximum below total cost fails closed");

    ResetFixture(decks);
    Check(
        AdvanceToSaveRequest(decks) &&
            DeliverSaveReply(decks),
        "carousel-boundary scenario reached consumed Save");
    *reinterpret_cast<uint16_t*>(
        g_manager + 0x20CE) = 16;
    Check(
        decks.EnsureDeck(kTargetUids) ==
            BattleShipDeckActions::kCarouselMismatch,
        "carousel count cannot read beyond the verified 15-record region");
}
} // namespace

int main()
{
    std::puts(
        "OFFLINE ONLY: BattleShipDeckActions ABI/boundary regression; no game, server, or database");

    WriteNativeString(
        g_lobbyControl + 0x118,
        L"SelectBattleShipBtn");
    g_lobbyVtable[0x38 / sizeof(void*)] =
        reinterpret_cast<void*>(
            &FakeLobbySelectBattleShip);

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;

    NativeActions::Config config = {};
    config.username = "offline";
    config.password = "offline";
    config.room_title = "offline-deck";
    config.room_mode = 6;
    config.map_id = 0;

    *reinterpret_cast<void**>(g_network + 0x14) =
        &g_stream;
    NativeActions native;
    const bool nativeBound = native.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        config);
    Check(
        nativeBound,
        "NativeActions observer bound to offline originals");

    BattleShipDeckActions decks;
    g_decks = &decks;
    const bool deckBound = decks.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        &native);
    Check(
        deckBound,
        "deck adapter bound after all verified native symbols matched");
    Check(
        g_networkSendDetour &&
            g_loginDispatchDetour &&
            g_saveReplyDetour &&
            g_goOutReplyDetour,
        "transport, login, Save, and GoOut detours were captured");

    if (nativeBound && deckBound) {
        TestLobbyEntry(decks);
        TestExistingDeckCleaningUsesUidOffset(decks);
        TestFullRecordSaveAndDoubleGoOut(decks);
        TestNoAlternativeFailsClosed(decks);
        TestSaveReplyMismatch(decks);
        TestGoOutReplyMismatch(decks);
        TestReplyObserverGuards(decks);
        TestInventoryAndCarouselBoundaries(decks);
    }

    DestroyNativeString(g_lobbyControl + 0x118);
    if (g_replyAllocation)
        VirtualFree(g_replyAllocation, 0, MEM_RELEASE);
    if (g_failures) {
        std::fprintf(
            stderr,
            "OFFLINE BATTLESHIP DECK ABI FAILURES: %d\n",
            g_failures);
        return 1;
    }
    std::puts("OFFLINE BATTLESHIP DECK ABI PASS");
    return 0;
}
