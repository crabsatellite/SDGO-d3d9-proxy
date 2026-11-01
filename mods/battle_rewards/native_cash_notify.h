// Native MB balance notification contract for BattleServer.exe SHA256
// f74d16037fc8f4fe49a8041488aabd1948df1b1a9f2d125de9dad4a2347b63a4.
//
// Verified shipped-artifact facts:
// - RoomPilot +0x429 is the account number used by native settlement.
// - RoomPilot +0x476 is the upstream Agent/client route used by result packets.
// - CNetFrameProc::SendToServer is VA 0x00724020 (RVA 0x00324020).
// - g_pNetFrameProc is a pointer stored at VA 0x00836DA0
//   (RVA 0x00436DA0).
// - SendToServer returns a negative value on failure; zero is a valid result.
// - The existing client consumes the packed 10-byte 0x064B packet as
//   {u16 packet_id, u32 account, u32 absolute_current_mb}.
//
// Symbol binding owns the two inline pointers below. This header does not pin
// preferred-base addresses. An isolated copied Agent accepted this message over
// its real BATTLE transport and forwarded the exact 10 bytes to a synthetic
// client's native TCP handle; authenticated client HUD acceptance is separate.
#pragma once

#ifndef _WIN32
#error native battle reward cash notification requires Windows
#endif

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace sdgo {
namespace battle_rewards {
namespace native_cash_notify {

constexpr size_t kRoomPilotAccount = 0x429;
constexpr size_t kRoomPilotAgentHandle = 0x476;
constexpr size_t kNetFrameTransport = 0xAC;
constexpr uint16_t kCashInfoPacket = 0x064B;
constexpr size_t kCashInfoPacketSize = 10;

using SendToServerFn = int(__thiscall*)(
    void* net_frame_proc, uint32_t target_handle, const void* packet,
    int packet_length, int flags);

// Resolved once during module load. net_frame_proc_global is the address of
// BattleServer's g_pNetFrameProc storage, rather than its current pointee.
inline void** net_frame_proc_global = nullptr;
inline SendToServerFn send_to_server = nullptr;

struct SessionRoute {
    // This may become dangling after capture. It is only dereferenced through
    // SEH-protected reads and both identity fields must still match.
    const void* room_pilot = nullptr;
    uint32_t account_number = 0;
    uint32_t agent_handle = 0;
};

enum class NotifyResult : uint8_t {
    Sent,
    StaleSession,
    BindingUnavailable,
    TransportFailure
};

namespace detail {

inline bool AddAddress(const void* base, size_t offset, uintptr_t* address) {
    if (!base || !address) return false;
    const uintptr_t value = reinterpret_cast<uintptr_t>(base);
    if (offset > (std::numeric_limits<uintptr_t>::max)() - value) return false;
    *address = value + offset;
    return true;
}

template <typename T>
inline bool SafeRead(const void* base, size_t offset, T* value) {
    if (!value) return false;
    uintptr_t address = 0;
    if (!AddAddress(base, offset, &address)) return false;
    __try {
        *value = *reinterpret_cast<const volatile T*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

inline void PutLe16(std::array<uint8_t, kCashInfoPacketSize>* packet,
                    size_t offset, uint16_t value) {
    (*packet)[offset] = static_cast<uint8_t>(value);
    (*packet)[offset + 1] = static_cast<uint8_t>(value >> 8);
}

inline void PutLe32(std::array<uint8_t, kCashInfoPacketSize>* packet,
                    size_t offset, uint32_t value) {
    (*packet)[offset] = static_cast<uint8_t>(value);
    (*packet)[offset + 1] = static_cast<uint8_t>(value >> 8);
    (*packet)[offset + 2] = static_cast<uint8_t>(value >> 16);
    (*packet)[offset + 3] = static_cast<uint8_t>(value >> 24);
}

inline std::array<uint8_t, kCashInfoPacketSize> CashInfoPacket(
    uint32_t account_number, uint32_t current_mb) {
    std::array<uint8_t, kCashInfoPacketSize> packet{};
    PutLe16(&packet, 0, kCashInfoPacket);
    PutLe32(&packet, 2, account_number);
    PutLe32(&packet, 6, current_mb);
    return packet;
}

inline bool InvokeSend(
    SendToServerFn callback, void* net_frame_proc, uint32_t target_handle,
    const std::array<uint8_t, kCashInfoPacketSize>& packet, int* result) {
    if (!callback || !net_frame_proc || !result) return false;
    __try {
        *result = callback(
            net_frame_proc, target_handle, packet.data(),
            static_cast<int>(packet.size()), 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace detail

inline bool Capture(const void* room_pilot, SessionRoute* route) {
    if (!route) return false;
    *route = {};
    uint32_t account_number = 0;
    uint32_t agent_handle = 0;
    if (!detail::SafeRead(
            room_pilot, kRoomPilotAccount, &account_number) ||
        !detail::SafeRead(
            room_pilot, kRoomPilotAgentHandle, &agent_handle) ||
        account_number == 0 || (account_number & 0x80000000u) != 0 ||
        agent_handle == 0) {
        return false;
    }
    route->room_pilot = room_pilot;
    route->account_number = account_number;
    route->agent_handle = agent_handle;
    return true;
}

// current_mb must be a fresh native wallet read performed after the durable
// credit. A historical ledger MbAfter value can become stale if the player
// spends or receives MB before a delayed/replayed notification.
inline NotifyResult NotifyCurrentMb(
    const SessionRoute& route, uint32_t account_number, uint32_t current_mb) {
    if (!route.room_pilot || route.account_number == 0 ||
        route.agent_handle == 0 || account_number != route.account_number) {
        return NotifyResult::StaleSession;
    }

    uint32_t live_account = 0;
    uint32_t live_handle = 0;
    if (!detail::SafeRead(
            route.room_pilot, kRoomPilotAccount, &live_account) ||
        !detail::SafeRead(
            route.room_pilot, kRoomPilotAgentHandle, &live_handle) ||
        live_account != route.account_number ||
        live_handle != route.agent_handle) {
        return NotifyResult::StaleSession;
    }

    const SendToServerFn callback = send_to_server;
    void* net_frame_proc = nullptr;
    if (!callback || !net_frame_proc_global ||
        !detail::SafeRead(net_frame_proc_global, 0, &net_frame_proc) ||
        !net_frame_proc) {
        return NotifyResult::BindingUnavailable;
    }
    void* transport = nullptr;
    if (!detail::SafeRead(
            net_frame_proc, kNetFrameTransport, &transport) ||
        !transport) {
        // The native wrapper itself returns zero when +0xAC is null. Without
        // this guard that "not initialized" path is indistinguishable from a
        // successfully queued send, because live sends may also return zero.
        return NotifyResult::BindingUnavailable;
    }

    const auto packet = detail::CashInfoPacket(account_number, current_mb);
    int native_result = -1;
    const DWORD incoming_error = GetLastError();
    const bool invoked = detail::InvokeSend(
        callback, net_frame_proc, live_handle, packet, &native_result);
    SetLastError(incoming_error);
    if (!invoked || native_result < 0) {
        return NotifyResult::TransportFailure;
    }
    return NotifyResult::Sent;
}

}  // namespace native_cash_notify
}  // namespace battle_rewards
}  // namespace sdgo
