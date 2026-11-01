// Static native contract for BattleServer.exe SHA256
// f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4:
// effective-HP-loss helper VA 0x0047DA90, prologue
// 55 8B EC 83 EC 34 89 4D CC 83 7D 08 00 74 06, ret 0x14.
// This provenance identifies the shipped artifact inspected offline. It is not
// evidence that a live server loaded the hook or delivered a reward.
#pragma once

#ifndef _WIN32
#error native battle reward combat hook requires Windows
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <limits>

namespace sdgo {
namespace battle_rewards {
namespace native_combat_hook {

constexpr size_t kRoomMode = 0x05;
constexpr size_t kRoomMission = 0x1B;
constexpr size_t kSlotInfo = 0x533;
constexpr size_t kSlotInfoStride = 0x48E;
constexpr size_t kSlotAccount = 0x429;
constexpr size_t kBattlePilots = 0x3CE4;
constexpr size_t kBattlePilotStride = 0x1F54;
constexpr size_t kPilotEntity = 0x00;
constexpr size_t kPilotTeam = 0x04;
// The first unit dword is populated with NpcDataID by the verified AI builder
// at 0x00485720 (modes 11/12). Its meaning for ordinary mission units is not
// established, so the damage hook exposes it only as an opaque key.
constexpr size_t kUnitKey = 0x00;
constexpr size_t kUnitDataPointer = 0x6C;
constexpr int32_t kHumanRoomSlots = 12;
// In mission modes, native entity lookup 0x0047ACB0 searches 22 BattlePilot
// records. The AI attack path at 0x004C4596 resolves an entity through that
// lookup, forms room+0x3CE4+slot*0x1F54 at 0x004C45E3, and passes that exact
// BattlePilot pointer through 0x004A0300 to the effective-loss helper.
constexpr int32_t kNativePilotSlots = 22;
constexpr uint32_t kNpcEntityBit = 0x80000000u;

struct NativeDamageSnapshot {
    void* room = nullptr;
    void* victim_pilot = nullptr;
    void* attacker_pilot = nullptr;
    void* victim_unit = nullptr;
    uint32_t attacker_account = 0;
    uint32_t attacker_entity = 0;
    uint32_t victim_account = 0;
    uint32_t victim_entity = 0;
    uint32_t mission_id = 0;
    uint32_t victim_unit_key = 0;
    uintptr_t victim_unit_data = 0;
    uint32_t effective_damage = 0;
    int32_t attacker_slot = -1;
    int32_t victim_slot = -1;
    int32_t death_flag = 0;
    uint8_t room_mode = 0;
    uint8_t attacker_team = 0;
    uint8_t victim_team = 0;
    bool attacker_native_npc = false;
};

struct VictimClassification {
    bool accepted = false;
    bool npc = false;
    bool boss = false;
};

enum class DiagnosticReason : uint8_t {
    None,
    MissingOriginal,
    NativeGuardRejected,
    NoEffectiveDamage,
    SnapshotFault,
    AttackerNotMember,
    EmptyAttackerAccount,
    AttackerIdentityMismatch,
    VictimIdentityMismatch,
    FriendlyDamage,
    MissingEventGate,
    EventGateRejected,
    MissingClassifier,
    ClassificationRejected,
    InvalidClassification,
    MissingRecordSink,
    CallbackFault
};

using OriginalFn = uint32_t(__thiscall*)(
    void* room, void* victim_pilot, void* attacker_pilot, void* victim_unit,
    uint32_t effective_damage, int32_t death_flag);
using EventGate = bool(__stdcall*)(const NativeDamageSnapshot* snapshot);
using ClassifyVictim = bool(__stdcall*)(
    const NativeDamageSnapshot* snapshot, VictimClassification* classification);
using RecordSink = void(__stdcall*)(
    const NativeDamageSnapshot* snapshot,
    const VictimClassification* classification);
using DiagnosticSink = void(__stdcall*)(
    DiagnosticReason reason, const NativeDamageSnapshot* snapshot);

// HookCode writes the verified trampoline here. The other callbacks are set
// once during mod load before battle threads can reach the detour.
inline OriginalFn original = nullptr;
inline EventGate event_gate = nullptr;
inline ClassifyVictim classify_victim = nullptr;
inline RecordSink record_sink = nullptr;
inline DiagnosticSink diagnostic_sink = nullptr;

namespace detail {

inline bool AddAddress(const void* base, size_t offset, uintptr_t* address) {
    if (!base || !address) return false;
    const uintptr_t value = reinterpret_cast<uintptr_t>(base);
    if (offset > (std::numeric_limits<uintptr_t>::max)() - value) return false;
    *address = value + offset;
    return true;
}

inline bool IsReadableRange(uintptr_t address, size_t size) {
    if (size == 0 ||
        size > (std::numeric_limits<uintptr_t>::max)() - address) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &memory,
                     sizeof(memory)) != sizeof(memory) ||
        memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD protection = memory.Protect & 0xFF;
    if (protection != PAGE_READONLY && protection != PAGE_READWRITE &&
        protection != PAGE_WRITECOPY && protection != PAGE_EXECUTE_READ &&
        protection != PAGE_EXECUTE_READWRITE &&
        protection != PAGE_EXECUTE_WRITECOPY) {
        return false;
    }
    const uintptr_t region = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    if (memory.RegionSize >
        (std::numeric_limits<uintptr_t>::max)() - region) {
        return false;
    }
    return address >= region && address + size <= region + memory.RegionSize;
}

template <typename T>
inline bool SafeRead(const void* base, size_t offset, T* value) {
    if (!value) return false;
    uintptr_t address = 0;
    if (!AddAddress(base, offset, &address)) return false;
#if defined(__clang__)
    // clang-cl's 32-bit SEH does not reliably resume from a bad volatile read.
    // Production uses MSVC SEH directly; this preflight keeps the independent
    // clang ABI test from dereferencing a clearly inaccessible page.
    if (!IsReadableRange(address, sizeof(T))) return false;
#endif
    __try {
        *value = *reinterpret_cast<const volatile T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline int32_t MemberSlot(const void* room, const void* pilot) {
    uintptr_t first = 0;
    if (!AddAddress(room, kBattlePilots, &first) || !pilot) return -1;
    const uintptr_t candidate = reinterpret_cast<uintptr_t>(pilot);
    for (int32_t slot = 0; slot < kHumanRoomSlots; ++slot) {
        const uintptr_t offset =
            static_cast<uintptr_t>(slot) * kBattlePilotStride;
        if (offset > (std::numeric_limits<uintptr_t>::max)() - first) return -1;
        if (candidate == first + offset) return slot;
    }
    return -1;
}

inline int32_t MissionNpcSlot(const void* room, const void* pilot) {
    uintptr_t first = 0;
    if (!AddAddress(room, kBattlePilots, &first) || !pilot) return -1;
    const uintptr_t candidate = reinterpret_cast<uintptr_t>(pilot);
    for (int32_t slot = kHumanRoomSlots; slot < kNativePilotSlots; ++slot) {
        const uintptr_t offset =
            static_cast<uintptr_t>(slot) * kBattlePilotStride;
        if (offset > (std::numeric_limits<uintptr_t>::max)() - first) return -1;
        if (candidate == first + offset) return slot;
    }
    return -1;
}

inline bool SlotAccount(const void* room, int32_t slot, uint32_t* account) {
    if (slot < 0 || slot >= kHumanRoomSlots) return false;
    const size_t offset =
        kSlotInfo + static_cast<size_t>(slot) * kSlotInfoStride + kSlotAccount;
    return SafeRead(room, offset, account);
}

inline bool IsMissionMode(uint8_t mode) {
    return mode == 5 || mode == 8 || mode == 10;
}

inline DiagnosticReason Capture(
    void* room, void* victim_pilot, void* attacker_pilot, void* victim_unit,
    uint32_t effective_damage, int32_t death_flag,
    NativeDamageSnapshot* snapshot) {
    if (!snapshot) return DiagnosticReason::SnapshotFault;
    *snapshot = {};
    snapshot->room = room;
    snapshot->victim_pilot = victim_pilot;
    snapshot->attacker_pilot = attacker_pilot;
    snapshot->victim_unit = victim_unit;
    snapshot->effective_damage = effective_damage;
    snapshot->death_flag = death_flag;

    if (!victim_pilot || !attacker_pilot) {
        return DiagnosticReason::NativeGuardRejected;
    }
    if (!SafeRead(room, kRoomMode, &snapshot->room_mode) ||
        !SafeRead(room, kRoomMission, &snapshot->mission_id)) {
        return DiagnosticReason::SnapshotFault;
    }
    if (!victim_unit && !IsMissionMode(snapshot->room_mode)) {
        return DiagnosticReason::NativeGuardRejected;
    }
    if (effective_damage == 0) return DiagnosticReason::NoEffectiveDamage;

    snapshot->attacker_slot = MemberSlot(room, attacker_pilot);
    if (snapshot->attacker_slot < 0) {
        if (!IsMissionMode(snapshot->room_mode)) {
            return DiagnosticReason::AttackerNotMember;
        }
        snapshot->attacker_slot = MissionNpcSlot(room, attacker_pilot);
        if (snapshot->attacker_slot < 0) {
            return DiagnosticReason::AttackerNotMember;
        }
        snapshot->attacker_native_npc = true;
    }
    snapshot->victim_slot = MemberSlot(room, victim_pilot);
    if (!SafeRead(attacker_pilot, kPilotEntity,
                  &snapshot->attacker_entity) ||
        !SafeRead(attacker_pilot, kPilotTeam,
                  &snapshot->attacker_team) ||
        !SafeRead(victim_pilot, kPilotEntity,
                  &snapshot->victim_entity) ||
        !SafeRead(victim_pilot, kPilotTeam,
                  &snapshot->victim_team)) {
        return DiagnosticReason::SnapshotFault;
    }
    if (snapshot->attacker_native_npc) {
        if ((snapshot->attacker_entity & kNpcEntityBit) == 0) {
            return DiagnosticReason::AttackerIdentityMismatch;
        }
        // Mission-native slots 12..21 have no corresponding player-account
        // slot. Preserve their native entity identity so their effective
        // damage can close a Boss life without ever becoming a wallet target.
        snapshot->attacker_account = snapshot->attacker_entity;
    } else if (!SlotAccount(room, snapshot->attacker_slot,
                            &snapshot->attacker_account)) {
        return DiagnosticReason::SnapshotFault;
    }
    if (snapshot->attacker_account == 0) {
        return DiagnosticReason::EmptyAttackerAccount;
    }
    if (snapshot->attacker_account != snapshot->attacker_entity) {
        return DiagnosticReason::AttackerIdentityMismatch;
    }
    if (snapshot->victim_slot >= 0) {
        if (!SlotAccount(room, snapshot->victim_slot,
                         &snapshot->victim_account)) {
            return DiagnosticReason::SnapshotFault;
        }
        if (snapshot->victim_account == 0 ||
            snapshot->victim_account != snapshot->victim_entity) {
            return DiagnosticReason::VictimIdentityMismatch;
        }
    }
    if (victim_unit &&
        (!SafeRead(victim_unit, kUnitKey,
                   &snapshot->victim_unit_key) ||
         !SafeRead(victim_unit, kUnitDataPointer,
                   &snapshot->victim_unit_data))) {
        return DiagnosticReason::SnapshotFault;
    }
    if (snapshot->attacker_team != 0 &&
        snapshot->attacker_team == snapshot->victim_team) {
        return DiagnosticReason::FriendlyDamage;
    }
    return DiagnosticReason::None;
}

inline bool InvokeGateSeh(EventGate callback,
                          const NativeDamageSnapshot* snapshot,
                          bool* accepted) {
    __try {
        *accepted = callback(snapshot);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool InvokeClassifierSeh(
    ClassifyVictim callback, const NativeDamageSnapshot* snapshot,
    VictimClassification* classification, bool* accepted) {
    __try {
        *accepted = callback(snapshot, classification);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline bool InvokeRecordSeh(
    RecordSink callback, const NativeDamageSnapshot* snapshot,
    const VictimClassification* classification) {
    __try {
        callback(snapshot, classification);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void EmitDiagnostic(
    DiagnosticSink callback, DiagnosticReason reason,
    const NativeDamageSnapshot* snapshot) {
    if (!callback || reason == DiagnosticReason::None) return;
    __try {
        callback(reason, snapshot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

inline DiagnosticReason PrepareCallbacks(
    const NativeDamageSnapshot& snapshot, EventGate gate,
    ClassifyVictim classifier, VictimClassification* classification) {
    if (!gate) return DiagnosticReason::MissingEventGate;
    bool accepted = false;
    try {
        if (!InvokeGateSeh(gate, &snapshot, &accepted)) {
            return DiagnosticReason::CallbackFault;
        }
    } catch (...) {
        return DiagnosticReason::CallbackFault;
    }
    if (!accepted) return DiagnosticReason::EventGateRejected;

    if (!classifier) return DiagnosticReason::MissingClassifier;
    accepted = false;
    try {
        if (!InvokeClassifierSeh(
                classifier, &snapshot, classification, &accepted)) {
            return DiagnosticReason::CallbackFault;
        }
    } catch (...) {
        return DiagnosticReason::CallbackFault;
    }
    if (!accepted || !classification->accepted) {
        return DiagnosticReason::ClassificationRejected;
    }
    if (classification->boss && !classification->npc) {
        return DiagnosticReason::InvalidClassification;
    }
    return DiagnosticReason::None;
}

inline bool DispatchRecord(
    RecordSink sink, const NativeDamageSnapshot& snapshot,
    const VictimClassification& classification) {
    if (!sink) return false;
    try {
        return InvokeRecordSeh(sink, &snapshot, &classification);
    } catch (...) {
        return false;
    }
}

}  // namespace detail

inline uint32_t __fastcall Detour(
    void* room, void* /*unused_edx*/, void* victim_pilot,
    void* attacker_pilot, void* victim_unit, uint32_t effective_damage,
    int32_t death_flag) {
    const DWORD incoming_error = GetLastError();
    const OriginalFn call_original = original;
    const EventGate gate = event_gate;
    const ClassifyVictim classifier = classify_victim;
    const RecordSink sink = record_sink;
    const DiagnosticSink diagnostics = diagnostic_sink;

    NativeDamageSnapshot snapshot;
    DiagnosticReason reason = detail::Capture(
        room, victim_pilot, attacker_pilot, victim_unit, effective_damage,
        death_flag, &snapshot);
    VictimClassification classification;
    if (reason == DiagnosticReason::None) {
        reason =
            detail::PrepareCallbacks(snapshot, gate, classifier, &classification);
    }

    SetLastError(incoming_error);
    if (!call_original) {
        detail::EmitDiagnostic(
            diagnostics, DiagnosticReason::MissingOriginal, &snapshot);
        SetLastError(incoming_error);
        return 0;
    }
    if (reason == DiagnosticReason::None) {
        if (!sink) {
            reason = DiagnosticReason::MissingRecordSink;
        } else if (!detail::DispatchRecord(sink, snapshot, classification)) {
            reason = DiagnosticReason::CallbackFault;
        }
    }
    detail::EmitDiagnostic(diagnostics, reason, &snapshot);
    // This helper's death path can synchronously advance battle/mission state.
    // Record the already-applied effective HP loss first so any settlement
    // reached inside the native original includes the final hit and Boss kill.
    SetLastError(incoming_error);
    const uint32_t result = call_original(
        room, victim_pilot, attacker_pilot, victim_unit, effective_damage,
        death_flag);
    const DWORD original_error = GetLastError();
    SetLastError(original_error);
    return result;
}

}  // namespace native_combat_hook
}  // namespace battle_rewards
}  // namespace sdgo
