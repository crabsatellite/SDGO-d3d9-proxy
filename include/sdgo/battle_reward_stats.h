#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace sdgo::battle_rewards::native_stats {

static_assert(sizeof(void*) == 4, "BattleServer native statistics require an x86 build");

constexpr size_t kParticipantSlots = 12;
constexpr size_t kRoomMode = 0x05;
constexpr size_t kRoomPilotBase = 0x533;
constexpr size_t kRoomPilotStride = 0x48E;
constexpr size_t kRoomPilotId = 0x429;
constexpr size_t kRoomPilotTeam = 0x42D;
constexpr size_t kBattlePilotBase = 0x3CE4;
constexpr size_t kBattlePilotStride = 0x1F54;
constexpr size_t kBattlePilotStars = 0x0E;
constexpr size_t kBattlePilotKills = 0x12;
constexpr size_t kBattlePilotDeaths = 0x1A;
constexpr size_t kBattlePilotState = 0x22;
constexpr size_t kBattlePilotEligibilityFlags = 0x1D04;
constexpr size_t kBattlePilotOutcome = 0x1D28;
constexpr size_t kRoomStartedAtUnix = 0x2EEB4;
constexpr uint32_t kAiIdBit = 0x80000000u;

// Verified against BattleServer.exe SHA-256
// f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4.
// Every offset is relative to EBP at the named post-daily settlement hook.
// eligibility_flags is a 12-entry DWORD array indexed by native_rank.
// mission_outcomes is a 12-entry DWORD array used only by the mission site;
// the mission function writes BP+1D28 after this hook.
struct SettlementFrameContract {
    const char* name;
    ptrdiff_t room;
    ptrdiff_t duration_seconds;
    ptrdiff_t room_pilot;
    ptrdiff_t battle_pilot;
    ptrdiff_t physical_slot;
    ptrdiff_t native_rank;
    ptrdiff_t eligibility_flags;
    ptrdiff_t mission_outcomes;
};

inline constexpr std::array<SettlementFrameContract, 10> kSettlementFrames = {{
    {"personal",   -0xB48, -0x880, -0x244, -0x7E0, -0x88C, -0x914, -0x7B8, 0},
    {"team",       -0xBBC, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {"leader",     -0xBBC, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {"ai",         -0xBBC, -0x840, -0x23C, -0x7C8, -0x8FC, -0x984, -0x7A0, 0},
    {"ai_ship",    -0xBD0, -0x850, -0x23C, -0x7CC, -0x90C, -0x994, -0x7A4, 0},
    {"mode13",     -0xBD0, -0x840, -0x234, -0x7C8, -0x8FC, -0x990, -0x7A0, 0},
    {"mode14",     -0xBD0, -0x840, -0x234, -0x7C8, -0x8FC, -0x990, -0x7A0, 0},
    {"tag",        -0xBC0, -0x838, -0x234, -0x7C0, -0x8F4, -0x980, -0x798, 0},
    {"death_grid", -0xBC8, -0x838, -0x234, -0x7BC, -0x8FC, -0x984, -0x798, 0},
    {"mission",    -0xB58, -0x844, -0x234, -0x7C8, -0x854, -0x8D8, -0x7A0, -0x828},
}};

enum class ParticipantKind : uint8_t {
    Human,
    Ai
};

enum class NativeOutcome : uint8_t {
    Draw = 0,
    WinOrClear = 1,
    LoseOrFail = 2
};

enum class CaptureError : uint8_t {
    None,
    InvalidSite,
    InvalidArgument,
    UnreadableFrame,
    InvalidSlot,
    InvalidRank,
    UnreadableRoom,
    PointerMismatch,
    EmptyParticipant,
    NativeIneligiblePath,
    InvalidOutcome,
    BufferTooSmall
};

// Facts for the participant whose native settlement calculation is currently
// executing. The hook is bypassed by native code when eligibility_flags != 0,
// so a nonzero flag here means the native frame no longer matches the verified
// control-flow contract and must fail closed.
struct NativeFrame {
    BYTE* room = nullptr;
    BYTE* room_pilot = nullptr;
    BYTE* battle_pilot = nullptr;
    uint32_t room_id = 0;
    uint32_t room_started_at_unix = 0;
    uint32_t account = 0;
    uint32_t native_id = 0;
    uint32_t duration_seconds = 0;
    uint32_t physical_slot = 0;
    uint32_t native_rank = 0;
    uint32_t eligibility_flags = 0;
    uint32_t stars = 0;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    uint8_t room_mode = 0;
    uint8_t team = 0;
    uint8_t pilot_state = 0;
    NativeOutcome outcome = NativeOutcome::Draw;
    ParticipantKind kind = ParticipantKind::Human;
    bool native_eligible = false;
    bool native_rank_first = false;
};

// Stable end-of-match facts that are safe to read for every occupied slot
// before the native settlement loop has visited all slots. Per-slot outcome
// and BP+1D04 are intentionally absent because native code writes them during
// that loop and future entries can contain stale data.
struct Participant {
    BYTE* room_pilot = nullptr;
    BYTE* battle_pilot = nullptr;
    uint32_t account = 0;
    uint32_t native_id = 0;
    uint32_t physical_slot = 0;
    uint32_t stars = 0;
    uint32_t kills = 0;
    uint32_t deaths = 0;
    uint8_t team = 0;
    uint8_t pilot_state = 0;
    ParticipantKind kind = ParticipantKind::Human;
    bool active = false;
};

// Effective-damage convergence point. Its return type is deliberately not
// declared here: only the entry address, five stack arguments and ret 14h
// cleanup were verified.
struct EffectiveDamageHookContract {
    static constexpr uintptr_t address = 0x0047DA90;
    static constexpr uintptr_t end_exclusive = 0x0047DD2B;
    static constexpr size_t argument_bytes = 0x14;
};

inline bool IsReadable(const void* address, size_t size) {
    if (!address || size == 0) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(address);
    if (begin > (std::numeric_limits<uintptr_t>::max)() - size) return false;
    const uintptr_t end = begin + size;
    uintptr_t cursor = begin;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info))) {
            return false;
        }
        if (info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
            return false;
        }
        const DWORD access = info.Protect & 0xFFu;
        if (access != PAGE_READONLY && access != PAGE_READWRITE &&
            access != PAGE_WRITECOPY && access != PAGE_EXECUTE_READ &&
            access != PAGE_EXECUTE_READWRITE && access != PAGE_EXECUTE_WRITECOPY) {
            return false;
        }
        const uintptr_t region = reinterpret_cast<uintptr_t>(info.BaseAddress);
        if (region > (std::numeric_limits<uintptr_t>::max)() - info.RegionSize) {
            return false;
        }
        const uintptr_t next = region + info.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

inline bool AddOffset(const void* base, ptrdiff_t offset, const BYTE** result) {
    if (!base || !result) return false;
    const uintptr_t value = reinterpret_cast<uintptr_t>(base);
    if (offset < 0) {
        const uintptr_t magnitude = static_cast<uintptr_t>(-(offset + 1)) + 1;
        if (value < magnitude) return false;
        *result = reinterpret_cast<const BYTE*>(value - magnitude);
        return true;
    }
    const uintptr_t magnitude = static_cast<uintptr_t>(offset);
    if (value > (std::numeric_limits<uintptr_t>::max)() - magnitude) return false;
    *result = reinterpret_cast<const BYTE*>(value + magnitude);
    return true;
}

template <typename T>
inline bool Read(const void* base, ptrdiff_t offset, T* value) {
    static_assert(std::is_trivially_copyable_v<T>, "native reads require POD values");
    const BYTE* address = nullptr;
    if (!value || !AddOffset(base, offset, &address) || !IsReadable(address, sizeof(T))) {
        return false;
    }
#if defined(_MSC_VER)
    __try {
        memcpy(value, address, sizeof(T));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    memcpy(value, address, sizeof(T));
#endif
    return true;
}

inline bool ExpectedPointer(BYTE* room, size_t base, size_t stride,
                            uint32_t slot, BYTE** value) {
    if (!room || !value || slot >= kParticipantSlots) return false;
    const uintptr_t start = reinterpret_cast<uintptr_t>(room);
    const size_t offset = base + stride * slot;
    if (start > (std::numeric_limits<uintptr_t>::max)() - offset) return false;
    *value = reinterpret_cast<BYTE*>(start + offset);
    return true;
}

inline bool ReadSettlementFrame(unsigned site, BYTE* frame, NativeFrame* snapshot,
                                CaptureError* error = nullptr) {
    const auto fail = [error](CaptureError value) {
        if (error) *error = value;
        return false;
    };
    if (error) *error = CaptureError::None;
    if (!snapshot || !frame) return fail(CaptureError::InvalidArgument);
    if (site >= kSettlementFrames.size()) return fail(CaptureError::InvalidSite);

    const SettlementFrameContract& contract = kSettlementFrames[site];
    uintptr_t roomValue = 0;
    uintptr_t roomPilotValue = 0;
    uintptr_t battlePilotValue = 0;
    int32_t slotValue = -1;
    int32_t rankValue = -1;
    uint32_t duration = 0;
    if (!Read(frame, contract.room, &roomValue) ||
        !Read(frame, contract.room_pilot, &roomPilotValue) ||
        !Read(frame, contract.battle_pilot, &battlePilotValue) ||
        !Read(frame, contract.physical_slot, &slotValue) ||
        !Read(frame, contract.native_rank, &rankValue) ||
        !Read(frame, contract.duration_seconds, &duration)) {
        return fail(CaptureError::UnreadableFrame);
    }
    if (slotValue < 0 || slotValue >= static_cast<int32_t>(kParticipantSlots)) {
        return fail(CaptureError::InvalidSlot);
    }
    if (rankValue < 0 || rankValue >= static_cast<int32_t>(kParticipantSlots)) {
        return fail(CaptureError::InvalidRank);
    }

    BYTE* room = reinterpret_cast<BYTE*>(roomValue);
    BYTE* roomPilot = reinterpret_cast<BYTE*>(roomPilotValue);
    BYTE* battlePilot = reinterpret_cast<BYTE*>(battlePilotValue);
    BYTE* expectedRoomPilot = nullptr;
    BYTE* expectedBattlePilot = nullptr;
    if (!ExpectedPointer(room, kRoomPilotBase, kRoomPilotStride,
                         static_cast<uint32_t>(slotValue), &expectedRoomPilot) ||
        !ExpectedPointer(room, kBattlePilotBase, kBattlePilotStride,
                         static_cast<uint32_t>(slotValue), &expectedBattlePilot) ||
        roomPilot != expectedRoomPilot || battlePilot != expectedBattlePilot) {
        return fail(CaptureError::PointerMismatch);
    }

    NativeFrame result{};
    result.room = room;
    result.room_pilot = roomPilot;
    result.battle_pilot = battlePilot;
    result.duration_seconds = duration;
    result.physical_slot = static_cast<uint32_t>(slotValue);
    result.native_rank = static_cast<uint32_t>(rankValue);
    const ptrdiff_t rankedFlags =
        contract.eligibility_flags + static_cast<ptrdiff_t>(result.native_rank * sizeof(uint32_t));
    uint32_t nativeOutcome = 0;
    if (!Read(frame, rankedFlags, &result.eligibility_flags)) {
        return fail(CaptureError::UnreadableFrame);
    }
    if (result.eligibility_flags != 0) {
        return fail(CaptureError::NativeIneligiblePath);
    }
    if (!Read(room, 0, &result.room_id) ||
        !Read(room, kRoomMode, &result.room_mode) ||
        !Read(room, kRoomStartedAtUnix, &result.room_started_at_unix) ||
        !Read(roomPilot, kRoomPilotId, &result.native_id) ||
        !Read(roomPilot, kRoomPilotTeam, &result.team) ||
        !Read(battlePilot, kBattlePilotStars, &result.stars) ||
        !Read(battlePilot, kBattlePilotKills, &result.kills) ||
        !Read(battlePilot, kBattlePilotDeaths, &result.deaths) ||
        !Read(battlePilot, kBattlePilotState, &result.pilot_state)) {
        return fail(CaptureError::UnreadableRoom);
    }
    if (result.native_id == 0) return fail(CaptureError::EmptyParticipant);

    if (contract.mission_outcomes != 0) {
        const ptrdiff_t rankedOutcome =
            contract.mission_outcomes + static_cast<ptrdiff_t>(result.native_rank * sizeof(uint32_t));
        uint32_t missionClear = 0;
        if (!Read(frame, rankedOutcome, &missionClear)) {
            return fail(CaptureError::UnreadableFrame);
        }
        if (missionClear > 1) return fail(CaptureError::InvalidOutcome);
        nativeOutcome = missionClear ? static_cast<uint32_t>(NativeOutcome::WinOrClear)
                                     : static_cast<uint32_t>(NativeOutcome::LoseOrFail);
    } else {
        uint8_t storedOutcome = 0;
        if (!Read(battlePilot, kBattlePilotOutcome, &storedOutcome)) {
            return fail(CaptureError::UnreadableRoom);
        }
        nativeOutcome = storedOutcome;
    }
    if (nativeOutcome > static_cast<uint32_t>(NativeOutcome::LoseOrFail)) {
        return fail(CaptureError::InvalidOutcome);
    }

    result.outcome = static_cast<NativeOutcome>(nativeOutcome);
    result.kind = (result.native_id & kAiIdBit)
        ? ParticipantKind::Ai : ParticipantKind::Human;
    result.account = result.native_id & ~kAiIdBit;
    result.native_eligible = result.pilot_state >= 3;
    result.native_rank_first = result.native_rank == 0;
    *snapshot = result;
    return true;
}

inline bool ReadParticipants(BYTE* room, Participant* participants, unsigned capacity,
                             unsigned* count, CaptureError* error = nullptr) {
    const auto fail = [error](CaptureError value) {
        if (error) *error = value;
        return false;
    };
    if (error) *error = CaptureError::None;
    if (!room || !participants || !count) return fail(CaptureError::InvalidArgument);
    *count = 0;
    if (capacity < kParticipantSlots) return fail(CaptureError::BufferTooSmall);

    Participant staged[kParticipantSlots]{};
    unsigned used = 0;
    for (uint32_t slot = 0; slot < kParticipantSlots; ++slot) {
        BYTE* roomPilot = nullptr;
        BYTE* battlePilot = nullptr;
        uint32_t nativeId = 0;
        if (!ExpectedPointer(room, kRoomPilotBase, kRoomPilotStride, slot, &roomPilot) ||
            !ExpectedPointer(room, kBattlePilotBase, kBattlePilotStride, slot, &battlePilot) ||
            !Read(roomPilot, kRoomPilotId, &nativeId)) {
            return fail(CaptureError::UnreadableRoom);
        }
        if (nativeId == 0) continue;

        Participant one{};
        one.room_pilot = roomPilot;
        one.battle_pilot = battlePilot;
        one.native_id = nativeId;
        one.account = nativeId & ~kAiIdBit;
        one.physical_slot = slot;
        one.kind = (nativeId & kAiIdBit) ? ParticipantKind::Ai : ParticipantKind::Human;
        if (!Read(roomPilot, kRoomPilotTeam, &one.team) ||
            !Read(battlePilot, kBattlePilotStars, &one.stars) ||
            !Read(battlePilot, kBattlePilotKills, &one.kills) ||
            !Read(battlePilot, kBattlePilotDeaths, &one.deaths) ||
            !Read(battlePilot, kBattlePilotState, &one.pilot_state)) {
            return fail(CaptureError::UnreadableRoom);
        }
        one.active = one.pilot_state >= 3;
        staged[used++] = one;
    }
    for (unsigned i = 0; i < used; ++i) participants[i] = staged[i];
    *count = used;
    return true;
}

inline unsigned CountActiveHumans(const Participant* participants, unsigned count) {
    if (!participants) return 0;
    unsigned humans = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (participants[i].kind == ParticipantKind::Human && participants[i].active) {
            ++humans;
        }
    }
    return humans;
}

inline int SlotFromBattlePilot(BYTE* room, const BYTE* battlePilot) {
    if (!room || !battlePilot) return -1;
    const uintptr_t roomValue = reinterpret_cast<uintptr_t>(room);
    if (roomValue > (std::numeric_limits<uintptr_t>::max)() - kBattlePilotBase) return -1;
    const uintptr_t first = roomValue + kBattlePilotBase;
    const uintptr_t value = reinterpret_cast<uintptr_t>(battlePilot);
    if (value < first) return -1;
    const uintptr_t delta = value - first;
    if (delta % kBattlePilotStride != 0) return -1;
    const uintptr_t slot = delta / kBattlePilotStride;
    return slot < kParticipantSlots ? static_cast<int>(slot) : -1;
}

} // namespace sdgo::battle_rewards::native_stats
