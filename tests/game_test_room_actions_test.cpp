// OFFLINE-ONLY ABI regression tests for RoomActions detours.
//
// These tests use SdgoApi Symbol/HookCode stubs and never start GOnline,
// contact a server, write a database, or claim gameplay/runtime evidence.

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../mods/game_test_client/room_actions.h"

namespace
{
RoomActions* g_rooms = nullptr;
void* g_battleEndDetour = nullptr;
void* g_leaveDetour = nullptr;
void* g_factoryDetour = nullptr;
void* g_normalThreeDLeaveReplyDetour = nullptr;

bool g_battleOriginalCalled = false;
uint32_t g_battleSequenceInsideOriginal = UINT32_MAX;
uint8_t g_battleCompletedInsideOriginal = UINT8_MAX;

bool g_leaveOriginalCalled = false;
bool g_leavePacketReleased = false;
uint32_t g_leaveSequenceInsideOriginal = UINT32_MAX;

bool g_normalThreeDLeaveOriginalCalled = false;
bool g_normalThreeDLeavePacketReleased = false;
uint32_t g_normalThreeDLeaveSequenceInsideOriginal = UINT32_MAX;

bool g_factoryResult = false;
int g_failures = 0;
uint8_t* g_profile = nullptr;
void* g_serviceManager = nullptr;
uint8_t g_app[0x500] = {};
uint8_t g_nonNullObject = 0;

void Check(bool condition, const char* message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
        return;
    }
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++g_failures;
}

uint8_t* __cdecl FakeGameApp()
{
    return g_app;
}

bool __fastcall FakeCreateRoomInit(
    void*,
    void*,
    uint32_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    uint8_t,
    const char*,
    const char*)
{
    return true;
}

int __fastcall FakeCreateRoomDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void*)
{
    return 1;
}

bool __fastcall FakeWaitRoomFactory(void*, void*, void*)
{
    return g_factoryResult;
}

void __fastcall FakeWaitRoomAlwaysDispatch(
    void*,
    void*,
    int,
    int,
    int,
    int,
    void* packet)
{
    g_leaveOriginalCalled = true;
    g_leaveSequenceInsideOriginal =
        g_rooms ? g_rooms->LeaveState().reply_sequence : UINT32_MAX;

    // Model the original handler taking ownership of its packet. A wrapper
    // that reads packet+6 after this call will fault or lose the real error.
    g_leavePacketReleased =
        packet && VirtualFree(packet, 0, MEM_RELEASE) != 0;
}

int __fastcall FakeNormalThreeDLeaveReply(
    void* self,
    void*,
    int,
    int,
    int,
    int,
    void* packet)
{
    g_normalThreeDLeaveOriginalCalled = true;
    g_normalThreeDLeaveSequenceInsideOriginal =
        g_rooms ? g_rooms->LeaveState().reply_sequence : UINT32_MAX;

    // The real handler advances its state and owns the packet. Both mutations
    // happen before the detour is allowed to publish its saved observation.
    if (self)
        *reinterpret_cast<uint32_t*>(
            static_cast<uint8_t*>(self) + 8) = 3;
    g_normalThreeDLeavePacketReleased =
        packet && VirtualFree(packet, 0, MEM_RELEASE) != 0;
    return 29;
}

void __fastcall FakeWaitMenuUpdate(void*, void*)
{
}

void __fastcall FakeStartOrReady(void*, void*)
{
}

bool __fastcall FakeIsRoomMaster(void*, void*, void*)
{
    return true;
}

int __stdcall FakeWaitRoomPlayerCount(void*)
{
    return 1;
}

void* __fastcall FakeServiceFindByName(void*, void*, const void*)
{
    return nullptr;
}

void* __fastcall FakeNativeWStringCtor(
    void* self,
    void*,
    const wchar_t*)
{
    return self;
}

void __fastcall FakeNativeWStringDtor(void*, void*)
{
}

bool __fastcall FakeThreeDRoomHasState(void*, void*, int, int)
{
    return false;
}

void __fastcall FakeThreeDRoomAddState(
    void*,
    void*,
    int,
    void*,
    int,
    int)
{
}

void* __cdecl FakeGridData()
{
    return &g_nonNullObject;
}

int __fastcall FakeGridFind(
    void*,
    void*,
    void**,
    int,
    int,
    char)
{
    return 0;
}

int __fastcall FakeBattleEndHandler(void*, void*, void*)
{
    g_battleOriginalCalled = true;
    if (g_rooms) {
        const auto during = g_rooms->BattleEndState();
        g_battleSequenceInsideOriginal = during.sequence;
        g_battleCompletedInsideOriginal =
            during.original_handler_completed;
    }
    return 37;
}

void* LookupSymbol(const char* name)
{
    if (std::strcmp(name, "game.app") == 0)
        return reinterpret_cast<void*>(&FakeGameApp);
    if (std::strcmp(name, "flow.create_room_packet_init") == 0)
        return reinterpret_cast<void*>(&FakeCreateRoomInit);
    if (std::strcmp(name, "flow.create_room_dialog_dispatch") == 0)
        return reinterpret_cast<void*>(&FakeCreateRoomDispatch);
    if (std::strcmp(name, "flow.wait_room_object_factory") == 0)
        return reinterpret_cast<void*>(&FakeWaitRoomFactory);
    if (std::strcmp(name, "flow.waitroom_always_dispatch") == 0)
        return reinterpret_cast<void*>(&FakeWaitRoomAlwaysDispatch);
    if (std::strcmp(name, "flow.wait_menu_update") == 0)
        return reinterpret_cast<void*>(&FakeWaitMenuUpdate);
    if (std::strcmp(name, "flow.wait_menu_start_or_ready") == 0)
        return reinterpret_cast<void*>(&FakeStartOrReady);
    if (std::strcmp(name, "flow.is_room_master") == 0)
        return reinterpret_cast<void*>(&FakeIsRoomMaster);
    if (std::strcmp(name, "flow.wait_room_player_count") == 0)
        return reinterpret_cast<void*>(&FakeWaitRoomPlayerCount);
    if (std::strcmp(name, "flow.profile_pointer") == 0)
        return &g_profile;
    if (std::strcmp(name, "flow.service_managed_pointer") == 0)
        return &g_serviceManager;
    if (std::strcmp(name, "flow.service_find_by_name") == 0)
        return reinterpret_cast<void*>(&FakeServiceFindByName);
    if (std::strcmp(name, "flow.wait_menu_vtable") == 0 ||
        std::strcmp(name, "flow.three_d_wait_room_vtable") == 0 ||
        std::strcmp(name, "flow.three_d_start_command_vtable") == 0)
        return &g_nonNullObject;
    if (std::strcmp(name, "flow.three_d_room_has_state") == 0)
        return reinterpret_cast<void*>(&FakeThreeDRoomHasState);
    if (std::strcmp(name, "flow.three_d_room_add_state") == 0)
        return reinterpret_cast<void*>(&FakeThreeDRoomAddState);
    if (std::strcmp(name, "flow.native_wstring_ctor") == 0)
        return reinterpret_cast<void*>(&FakeNativeWStringCtor);
    if (std::strcmp(name, "flow.native_wstring_dtor") == 0)
        return reinterpret_cast<void*>(&FakeNativeWStringDtor);
    if (std::strcmp(name, "grid.data") == 0)
        return reinterpret_cast<void*>(&FakeGridData);
    if (std::strcmp(name, "grid.find") == 0)
        return reinterpret_cast<void*>(&FakeGridFind);
    if (std::strcmp(name, "flow.battle_end_handler") == 0)
        return reinterpret_cast<void*>(&FakeBattleEndHandler);
    if (std::strcmp(name, "flow.normal_3d_leave_business") == 0)
        return reinterpret_cast<void*>(&FakeStartOrReady);
    if (std::strcmp(name, "flow.normal_3d_leave_reply_dispatch") == 0)
        return reinterpret_cast<void*>(&FakeNormalThreeDLeaveReply);
    if (std::strcmp(name, "flow.normal_3d_main_state_vtable") == 0 ||
        std::strcmp(name, "flow.normal_3d_manager_vtable") == 0 ||
        std::strcmp(name, "flow.ai_3d_manager_vtable") == 0 ||
        std::strcmp(name, "flow.battleship_3d_manager_vtable") == 0)
        return &g_nonNullObject;
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
    if (std::strcmp(name, "flow.battle_end_handler") == 0)
        g_battleEndDetour = detour;
    else if (std::strcmp(name, "flow.waitroom_always_dispatch") == 0)
        g_leaveDetour = detour;
    else if (std::strcmp(name, "flow.wait_room_object_factory") == 0)
        g_factoryDetour = detour;
    else if (
        std::strcmp(name, "flow.normal_3d_leave_reply_dispatch") == 0)
        g_normalThreeDLeaveReplyDetour = detour;
    return 1;
}

void TestBattleEndAbi(RoomActions& rooms)
{
    using Detour = int(__fastcall*)(void*, void*, void*);
    auto detour = reinterpret_cast<Detour>(g_battleEndDetour);
    Check(detour != nullptr, "battle-end detour was captured offline");
    if (!detour)
        return;

    rooms.ResetObservations();
    const auto before = rooms.BattleEndState();
    uint8_t packet[0x20] = {};
    packet[0x12] = 9;
    g_battleOriginalCalled = false;
    g_battleSequenceInsideOriginal = UINT32_MAX;
    g_battleCompletedInsideOriginal = UINT8_MAX;

    const int handled = detour(
        reinterpret_cast<void*>(0x11110000u),
        nullptr,
        packet);
    const auto after = rooms.BattleEndState();

    Check(g_battleOriginalCalled, "battle-end detour called the original handler");
    Check(handled == 37, "battle-end detour preserved the original non-boolean int return");
    Check(
        g_battleSequenceInsideOriginal == before.sequence &&
            g_battleCompletedInsideOriginal == 0,
        "battle-end receipt was not published before the original handler completed");
    Check(
        after.sequence == before.sequence + 1 &&
            after.original_handler_completed == 1,
        "battle-end receipt was published after the original handler completed");
    Check(
        after.reply_opcode == 0x0871 && after.result_type == 9,
        "battle-end receipt retained the real packet opcode and result type");
}

void TestLeavePacketLifetime(RoomActions& rooms)
{
    using Detour =
        void(__fastcall*)(void*, void*, int, int, int, int, void*);
    auto detour = reinterpret_cast<Detour>(g_leaveDetour);
    Check(detour != nullptr, "leave-room detour was captured offline");
    if (!detour)
        return;

    rooms.ResetObservations();
    const auto before = rooms.LeaveState();
    auto* packet = static_cast<uint8_t*>(VirtualAlloc(
        nullptr,
        0x1000,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    Check(packet != nullptr, "leave-room owned packet fixture was allocated");
    if (!packet)
        return;
    std::memset(packet, 0, 0x1000);
    *reinterpret_cast<uint16_t*>(packet + 6) = 0x1234;
    g_leaveOriginalCalled = false;
    g_leavePacketReleased = false;
    g_leaveSequenceInsideOriginal = UINT32_MAX;

    detour(
        reinterpret_cast<void*>(0x22220000u),
        nullptr,
        1,
        2,
        3,
        0x057B,
        packet);
    const auto after = rooms.LeaveState();

    Check(g_leaveOriginalCalled, "leave-room detour called the original handler");
    Check(g_leavePacketReleased, "original leave handler consumed the packet fixture");
    Check(
        g_leaveSequenceInsideOriginal == before.reply_sequence,
        "leave-room receipt was not published before the original handler completed");
    Check(
        after.reply_sequence == before.reply_sequence + 1,
        "leave-room receipt was published after the original handler completed");
    Check(
        after.reply_opcode == 0x057B && after.error == 0x1234,
        "leave-room detour saved the reply error before packet ownership transfer");
    Check(
        after.result == RoomActions::kLeaveRejected,
        "leave-room saved error produced the expected rejected result");
}

void TestFactoryReturnPassthrough()
{
    using Detour = bool(__fastcall*)(void*, void*, void*);
    auto detour = reinterpret_cast<Detour>(g_factoryDetour);
    Check(detour != nullptr, "wait-room factory detour was captured offline");
    if (!detour)
        return;

    uint8_t packet[0x20] = {};
    packet[0x10] = 0;
    g_factoryResult = false;
    Check(
        !detour(reinterpret_cast<void*>(0x33330000u), nullptr, packet),
        "wait-room factory detour preserved false");
    g_factoryResult = true;
    Check(
        detour(reinterpret_cast<void*>(0x33330000u), nullptr, packet),
        "wait-room factory detour preserved true");
}

uint8_t* AllocateLeavePacket(uint16_t error)
{
    auto* packet = static_cast<uint8_t*>(VirtualAlloc(
        nullptr,
        0x1000,
        MEM_RESERVE | MEM_COMMIT,
        PAGE_READWRITE));
    if (!packet)
        return nullptr;
    std::memset(packet, 0, 0x1000);
    *reinterpret_cast<uint16_t*>(packet + 6) = error;
    return packet;
}

void TestNormalThreeDLeaveReply(RoomActions& rooms)
{
    using Detour =
        int(__fastcall*)(void*, void*, int, int, int, int, void*);
    auto detour =
        reinterpret_cast<Detour>(g_normalThreeDLeaveReplyDetour);
    Check(detour != nullptr, "normal 3D leave-reply detour was captured offline");
    if (!detour)
        return;

    rooms.ResetObservations();
    uint8_t state[0x10] = {};
    *reinterpret_cast<uint32_t*>(state + 8) = 1;
    auto* preStagePacket = AllocateLeavePacket(0x1111);
    Check(
        preStagePacket != nullptr,
        "normal 3D pre-stage packet fixture was allocated");
    if (!preStagePacket)
        return;
    const auto beforePreStage = rooms.LeaveState();
    g_normalThreeDLeaveOriginalCalled = false;
    g_normalThreeDLeavePacketReleased = false;
    g_normalThreeDLeaveSequenceInsideOriginal = UINT32_MAX;

    const int preStageHandled = detour(
        state,
        nullptr,
        1,
        2,
        3,
        0x057B,
        preStagePacket);
    const auto afterPreStage = rooms.LeaveState();

    Check(
        preStageHandled == 29,
        "normal 3D pre-stage reply preserved the original int return");
    Check(
        g_normalThreeDLeaveOriginalCalled &&
            g_normalThreeDLeavePacketReleased,
        "normal 3D pre-stage reply still ran the owning original handler");
    Check(
        g_normalThreeDLeaveSequenceInsideOriginal ==
                beforePreStage.reply_sequence &&
            afterPreStage.reply_sequence == beforePreStage.reply_sequence,
        "normal 3D reply did not publish leave before stage 2");

    rooms.ResetObservations();
    *reinterpret_cast<uint32_t*>(state + 8) = 2;
    auto* stageTwoPacket = AllocateLeavePacket(0x2345);
    Check(
        stageTwoPacket != nullptr,
        "normal 3D stage-2 packet fixture was allocated");
    if (!stageTwoPacket)
        return;
    const auto beforeStageTwo = rooms.LeaveState();
    g_normalThreeDLeaveOriginalCalled = false;
    g_normalThreeDLeavePacketReleased = false;
    g_normalThreeDLeaveSequenceInsideOriginal = UINT32_MAX;

    const int stageTwoHandled = detour(
        state,
        nullptr,
        4,
        5,
        6,
        0x057B,
        stageTwoPacket);
    const auto afterStageTwo = rooms.LeaveState();

    Check(
        stageTwoHandled == 29,
        "normal 3D stage-2 reply preserved the original int return");
    Check(
        g_normalThreeDLeaveOriginalCalled &&
            g_normalThreeDLeavePacketReleased,
        "normal 3D stage-2 reply called the original handler and transferred packet ownership");
    Check(
        g_normalThreeDLeaveSequenceInsideOriginal ==
            beforeStageTwo.reply_sequence,
        "normal 3D leave receipt was not published before the original handler completed");
    Check(
        afterStageTwo.reply_sequence ==
            beforeStageTwo.reply_sequence + 1,
        "normal 3D leave receipt was published after the original handler completed");
    Check(
        afterStageTwo.reply_opcode == 0x057B &&
            afterStageTwo.error == 0x2345,
        "normal 3D leave saved the reply error before the original released the packet");
    Check(
        afterStageTwo.result == RoomActions::kLeaveRejected,
        "normal 3D saved error produced the expected rejected result");
}
} // namespace

int main()
{
    std::puts(
        "OFFLINE ONLY: RoomActions ABI regression; no game, server, or database");

    SdgoApi api = {};
    api.size = sizeof(api);
    api.version = SDGO_API_VERSION;
    api.Symbol = &ApiSymbol;
    api.HookCode = &ApiHookCode;

    NativeActions native;
    NativeActions::Config config = {};
    config.username = "offline";
    config.password = "offline";
    config.room_title = "offline-abi";
    config.room_mode = 1;
    config.map_id = 0;

    RoomActions rooms;
    g_rooms = &rooms;
    const bool bound = rooms.Bind(
        &api,
        reinterpret_cast<SdgoMod*>(1),
        &native,
        config);
    Check(bound, "RoomActions bound to offline Symbol/HookCode stubs");
    if (bound) {
        TestBattleEndAbi(rooms);
        TestLeavePacketLifetime(rooms);
        TestNormalThreeDLeaveReply(rooms);
        TestFactoryReturnPassthrough();
    }

    if (g_failures) {
        std::fprintf(stderr, "OFFLINE ABI FAILURES: %d\n", g_failures);
        return 1;
    }
    std::puts("OFFLINE ABI PASS");
    return 0;
}
