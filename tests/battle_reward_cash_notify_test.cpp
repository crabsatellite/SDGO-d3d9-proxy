#include <windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "mods/battle_rewards/native_cash_notify.h"

namespace cash = sdgo::battle_rewards::native_cash_notify;

namespace {

int checks = 0;
int failures = 0;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL line " << line << ": " << expression << "\n";
    }
}

#define CHECK(expression) Check(!!(expression), #expression, __LINE__)

std::array<uint8_t, 0x600> room_pilot{};
std::array<uint8_t, 0xB0> fake_net_object{};
void* fake_net = nullptr;
void* received_net = nullptr;
uint32_t received_handle = 0;
std::array<uint8_t, cash::kCashInfoPacketSize> received_packet{};
int received_length = 0;
int received_flags = -1;
int send_calls = 0;
int send_result = 0;

template <typename T>
void Put(void* base, size_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(base) + offset, &value, sizeof(value));
}

int __fastcall FakeSend(
    void* net_frame_proc, void* /*unused_edx*/, uint32_t target_handle,
    const void* packet, int packet_length, int flags) {
    ++send_calls;
    received_net = net_frame_proc;
    received_handle = target_handle;
    received_length = packet_length;
    received_flags = flags;
    if (packet && packet_length == static_cast<int>(received_packet.size())) {
        std::memcpy(received_packet.data(), packet, received_packet.size());
    }
    SetLastError(0x88776655);
    return send_result;
}

void Reset(uint32_t account = 0x01020304, uint32_t handle = 0x55667788) {
    room_pilot.fill(0);
    fake_net_object.fill(0);
    Put(room_pilot.data(), cash::kRoomPilotAccount, account);
    Put(room_pilot.data(), cash::kRoomPilotAgentHandle, handle);
    Put(fake_net_object.data(), cash::kNetFrameTransport,
        reinterpret_cast<void*>(0x12340000));
    fake_net = fake_net_object.data();
    received_net = nullptr;
    received_handle = 0;
    received_packet.fill(0);
    received_length = 0;
    received_flags = -1;
    send_calls = 0;
    send_result = 0;
    cash::net_frame_proc_global = &fake_net;
    cash::send_to_server =
        reinterpret_cast<cash::SendToServerFn>(&FakeSend);
}

void PacketAndNativeResultTest() {
    Reset();
    cash::SessionRoute route;
    CHECK(cash::Capture(room_pilot.data(), &route));
    CHECK(route.room_pilot == room_pilot.data());
    CHECK(route.account_number == 0x01020304);
    CHECK(route.agent_handle == 0x55667788);

    SetLastError(0x11223344);
    CHECK(cash::NotifyCurrentMb(route, 0x01020304, 0xA1B2C3D4) ==
          cash::NotifyResult::Sent);
    CHECK(GetLastError() == 0x11223344);
    CHECK(send_calls == 1);
    CHECK(received_net == fake_net);
    CHECK(received_handle == 0x55667788);
    CHECK(received_length == 10);
    CHECK(received_flags == 0);
    const std::array<uint8_t, 10> expected = {
        0x4B, 0x06,
        0x04, 0x03, 0x02, 0x01,
        0xD4, 0xC3, 0xB2, 0xA1,
    };
    CHECK(received_packet == expected);

    // The native wrapper and its callers define only negative values as
    // failure. A zero return is therefore a successful queued send.
    CHECK(send_result == 0);
    send_result = -4;
    CHECK(cash::NotifyCurrentMb(route, 0x01020304, 77) ==
          cash::NotifyResult::TransportFailure);
    CHECK(send_calls == 2);
}

void StaleSessionTests() {
    Reset();
    cash::SessionRoute route;
    CHECK(cash::Capture(room_pilot.data(), &route));

    Put(room_pilot.data(), cash::kRoomPilotAccount, 0x01020305u);
    CHECK(cash::NotifyCurrentMb(route, 0x01020304, 200) ==
          cash::NotifyResult::StaleSession);
    CHECK(send_calls == 0);

    Reset();
    CHECK(cash::Capture(room_pilot.data(), &route));
    Put(room_pilot.data(), cash::kRoomPilotAgentHandle, 0x55667789u);
    CHECK(cash::NotifyCurrentMb(route, 0x01020304, 200) ==
          cash::NotifyResult::StaleSession);
    CHECK(send_calls == 0);

    Reset();
    CHECK(cash::Capture(room_pilot.data(), &route));
    CHECK(cash::NotifyCurrentMb(route, 0x01020305, 200) ==
          cash::NotifyResult::StaleSession);
    CHECK(send_calls == 0);
}

void NoAccessAndBindingTests() {
    Reset();
    auto* page = static_cast<uint8_t*>(VirtualAlloc(
        nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(page != nullptr);
    if (page) {
        Put(page, cash::kRoomPilotAccount, 0x01020304u);
        Put(page, cash::kRoomPilotAgentHandle, 0x55667788u);
        cash::SessionRoute route;
        CHECK(cash::Capture(page, &route));
        DWORD old_protect = 0;
        CHECK(VirtualProtect(page, 0x1000, PAGE_NOACCESS, &old_protect) != 0);
        CHECK(cash::NotifyCurrentMb(route, 0x01020304, 300) ==
              cash::NotifyResult::StaleSession);
        CHECK(send_calls == 0);
        DWORD ignored = 0;
        CHECK(VirtualProtect(page, 0x1000, old_protect, &ignored) != 0);
        CHECK(VirtualFree(page, 0, MEM_RELEASE) != 0);
    }

    Reset();
    cash::SessionRoute route;
    CHECK(cash::Capture(room_pilot.data(), &route));
    cash::net_frame_proc_global = nullptr;
    CHECK(cash::NotifyCurrentMb(route, route.account_number, 400) ==
          cash::NotifyResult::BindingUnavailable);
    CHECK(send_calls == 0);

    Reset();
    CHECK(cash::Capture(room_pilot.data(), &route));
    fake_net = nullptr;
    CHECK(cash::NotifyCurrentMb(route, route.account_number, 400) ==
          cash::NotifyResult::BindingUnavailable);
    CHECK(send_calls == 0);

    Reset();
    CHECK(cash::Capture(room_pilot.data(), &route));
    Put(fake_net_object.data(), cash::kNetFrameTransport,
        static_cast<void*>(nullptr));
    CHECK(cash::NotifyCurrentMb(route, route.account_number, 400) ==
          cash::NotifyResult::BindingUnavailable);
    CHECK(send_calls == 0);

    Reset(0, 5);
    CHECK(!cash::Capture(room_pilot.data(), &route));
    CHECK(route.room_pilot == nullptr);
    Reset(0x80000005u, 5);
    CHECK(!cash::Capture(room_pilot.data(), &route));
    CHECK(route.room_pilot == nullptr);
    Reset(5, 0);
    CHECK(!cash::Capture(room_pilot.data(), &route));
    CHECK(route.room_pilot == nullptr);
}

}  // namespace

int main() {
    PacketAndNativeResultTest();
    StaleSessionTests();
    NoAccessAndBindingTests();
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
