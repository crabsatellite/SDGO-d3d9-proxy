// Behavior tests for the actual client native transport adapter. This test
// includes the implementation in the same translation unit so its injected
// native function pointers and internal detours can be exercised without a
// game process or a duplicate test implementation.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mods/room_titles/native_transport.cpp"

namespace sdgo::room_titles::native_transport {
namespace {
namespace metadata = room_metadata;

constexpr uint16_t kCreateRoom = 0x0640;
constexpr size_t kCreateRoomBytes = 0x53;
constexpr uint16_t kRoomList = 0x0617;
constexpr size_t kRoomListBytes = 0x5B5;
constexpr uint16_t kTeamList = 0x0824;
constexpr size_t kTeamListHeaderBytes = 0x13;
constexpr size_t kTeamListRowBytes = 0x123;
constexpr uint16_t kCreateAck = 0x0A33;
constexpr size_t kCreateAckBytes = 0x21FC;
constexpr uint32_t kSendResult = 0x13572468;
constexpr uint32_t kQueueResult = 0x24681357;
constexpr DWORD kTestSeh = 0xE0421001;
constexpr DWORD kSendCallerError = 0xE1010001;
constexpr DWORD kQueueCallerError = 0xE1010002;
constexpr DWORD kDecodeCallerError = 0xE1010003;
constexpr DWORD kBeforeSendCallbackError = 0xE1020001;
constexpr DWORD kAfterSendCallbackError = 0xE1020002;
constexpr DWORD kNativeSizeCallbackError = 0xE1020003;
constexpr DWORD kOnReceiveCallbackError = 0xE1020004;
constexpr DWORD kSendNativeResultError = 0xE1030001;
constexpr DWORD kQueueNativeResultError = 0xE1030002;
constexpr DWORD kDecodeNativeResultError = 0xE1030003;

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

uint16_t TestWord(const void* pointer)
{
    uint16_t value = 0;
    std::memcpy(&value, pointer, sizeof(value));
    return value;
}

uint32_t TestDword(const void* pointer)
{
    uint32_t value = 0;
    std::memcpy(&value, pointer, sizeof(value));
    return value;
}

void Put16(void* pointer, uint16_t value)
{
    std::memcpy(pointer, &value, sizeof(value));
}

void Put32(void* pointer, uint32_t value)
{
    std::memcpy(pointer, &value, sizeof(value));
}

std::vector<uint8_t> NativePacket(size_t size, uint16_t opcode,
                                  uint8_t seed)
{
    std::vector<uint8_t> packet(size);
    for (size_t index = 0; index < size; ++index)
        packet[index] = static_cast<uint8_t>(seed + index * 5u);
    Put16(packet.data(), opcode);
    return packet;
}

metadata::Message CreationMessage(const std::string& title,
                                  uint32_t request_id)
{
    metadata::Message message;
    message.kind = metadata::MessageKind::SetTitle;
    message.request_id = request_id;
    message.title = title;
    return message;
}

std::vector<uint8_t> CreationPacket(const std::vector<uint8_t>& native,
                                    const std::string& title,
                                    uint32_t request_id)
{
    std::vector<uint8_t> packet;
    Check(metadata::EncodePacket(native.data(), native.size(),
                                 CreationMessage(title, request_id),
                                 &packet),
          "common protocol encodes a client send replacement");
    return packet;
}

metadata::SnapshotBatch Batch(uint32_t request_id,
                              const std::string& title)
{
    metadata::SnapshotBatch batch;
    batch.server_epoch = 0x0102030405060708ull;
    batch.request_id = request_id;
    metadata::SnapshotRecord room;
    room.key.server_epoch = batch.server_epoch;
    room.key.room_id = 77;
    room.key.generation = 4;
    room.revision = 9;
    room.title = title;
    batch.rooms.push_back(room);
    return batch;
}

std::vector<uint8_t> BatchPacket(const std::vector<uint8_t>& native,
                                 uint32_t request_id,
                                 const std::string& title)
{
    std::vector<uint8_t> packet;
    Check(metadata::EncodeSnapshotBatchPacket(
              native.data(), native.size(), Batch(request_id, title),
              &packet),
          "common protocol encodes a client receive batch");
    return packet;
}

// Test-only envelope format matching the adapter's observable native contract:
// opcode:u16, key:u32, plaintext-size:u16, payload. A nonzero key uses a small
// reversible XOR fixture solely to prove that metadata is inspected after the
// injected native decoder. It is not evidence about the game's real cipher.
std::vector<uint8_t> Envelope(uint16_t opcode, uint32_t key,
                              const std::vector<uint8_t>& plain)
{
    std::vector<uint8_t> packet(8 + plain.size());
    Put16(packet.data(), opcode);
    Put32(packet.data() + 2, key);
    Put16(packet.data() + 6, static_cast<uint16_t>(plain.size()));
    const uint8_t key_bytes[4] = {
        static_cast<uint8_t>(key),
        static_cast<uint8_t>(key >> 8),
        static_cast<uint8_t>(key >> 16),
        static_cast<uint8_t>(key >> 24)};
    for (size_t index = 0; index < plain.size(); ++index)
        packet[8 + index] =
            key ? static_cast<uint8_t>(plain[index] ^
                                       key_bytes[index % 4])
                : plain[index];
    return packet;
}

struct SendCall
{
    void* self = nullptr;
    uint32_t connection = 0;
    const void* pointer = nullptr;
    uint32_t size = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> bytes;
};

struct QueueCall
{
    void* self = nullptr;
    uint32_t connection = 0;
    const void* pointer = nullptr;
    uint32_t size = 0;
    uint8_t flags = 0;
    std::vector<uint8_t> owned_copy;
};

struct BeforeCall
{
    uint32_t connection = 0;
    const uint8_t* pointer = nullptr;
    size_t size = 0;
    uint32_t flags = 0;
    std::vector<uint8_t> bytes;
};

struct AfterCall
{
    uint32_t connection = 0;
    bool queued = false;
    const uint8_t* pointer = nullptr;
    size_t size = 0;
    int result = 0;
    std::vector<uint8_t> bytes;
};

std::vector<SendCall> g_send_calls;
std::vector<QueueCall> g_queue_calls;
std::vector<BeforeCall> g_before_calls;
std::vector<AfterCall> g_after_calls;
std::vector<uint8_t> g_send_replacement;
bool g_reject_target = false;
uint32_t g_fake_send_result = kSendResult;
uint32_t g_fake_queue_result = kQueueResult;
bool g_track_send_last_error = false;
DWORD g_send_native_input_error = 0;
DWORD g_queue_native_input_error = 0;

int __fastcall FakeSend(void* self, void*, uint32_t connection,
                        const void* packet, uint32_t size, uint32_t flags)
{
    const DWORD input_error = GetLastError();
    if (g_track_send_last_error)
        g_send_native_input_error = input_error;
    SendCall call;
    call.self = self;
    call.connection = connection;
    call.pointer = packet;
    call.size = size;
    call.flags = flags;
    if (packet && size) {
        const auto* bytes = static_cast<const uint8_t*>(packet);
        call.bytes.assign(bytes, bytes + size);
    }
    g_send_calls.push_back(std::move(call));
    if (g_track_send_last_error)
        SetLastError(kSendNativeResultError);
    return static_cast<int>(g_fake_send_result);
}

uint32_t __fastcall FakeQueue(void* self, void*, uint32_t connection,
                              const void* packet, uint32_t size,
                              uint8_t flags)
{
    const DWORD input_error = GetLastError();
    if (g_track_send_last_error)
        g_queue_native_input_error = input_error;
    QueueCall call;
    call.self = self;
    call.connection = connection;
    call.pointer = packet;
    call.size = size;
    call.flags = flags;
    if (packet && size) {
        const auto* bytes = static_cast<const uint8_t*>(packet);
        // This models the documented native pre-defer copy. The adapter's
        // temporary replacement may be destroyed as soon as FakeQueue returns.
        call.owned_copy.assign(bytes, bytes + size);
    }
    g_queue_calls.push_back(std::move(call));
    if (g_track_send_last_error)
        SetLastError(kQueueNativeResultError);
    return g_fake_queue_result;
}

SendDecision BeforeSend(uint32_t connection, const uint8_t* packet,
                        size_t size, uint32_t flags,
                        std::vector<uint8_t>* replacement)
{
    BeforeCall call;
    call.connection = connection;
    call.pointer = packet;
    call.size = size;
    call.flags = flags;
    if (packet && size)
        call.bytes.assign(packet, packet + size);
    g_before_calls.push_back(std::move(call));

    SendDecision decision = SendDecision::Pass;
    if (packet && size >= 2 && TestWord(packet) == kCreateRoom) {
        if (g_reject_target) {
            decision = SendDecision::Reject;
        } else {
            *replacement = g_send_replacement;
            decision = SendDecision::Replace;
        }
    }
    if (g_track_send_last_error)
        SetLastError(kBeforeSendCallbackError);
    return decision;
}

void AfterSend(uint32_t connection, bool queued, const uint8_t* packet,
               size_t size, int result)
{
    AfterCall call;
    call.connection = connection;
    call.queued = queued;
    call.pointer = packet;
    call.size = size;
    call.result = result;
    if (packet && size)
        call.bytes.assign(packet, packet + size);
    g_after_calls.push_back(std::move(call));
    if (g_track_send_last_error)
        SetLastError(kAfterSendCallbackError);
}

void ResetSendFixture()
{
    g_send = reinterpret_cast<SendFn>(&FakeSend);
    g_queue = reinterpret_cast<QueueFn>(&FakeQueue);
    g_send_calls.clear();
    g_queue_calls.clear();
    g_before_calls.clear();
    g_after_calls.clear();
    g_send_replacement.clear();
    g_reject_target = false;
    g_fake_send_result = kSendResult;
    g_fake_queue_result = kQueueResult;
    g_track_send_last_error = false;
    g_send_native_input_error = 0;
    g_queue_native_input_error = 0;
    g_callbacks = {};
    g_callbacks.before_send = &BeforeSend;
    g_callbacks.after_send = &AfterSend;
}

void TestSendAdapter()
{
    ResetSendFixture();
    std::array<uint8_t, 16> self = {};
    auto native = NativePacket(kCreateRoomBytes, kCreateRoom, 0x11);
    g_send_replacement =
        CreationPacket(native, "mission send replacement", 101);
    const auto original = native;
    auto send = reinterpret_cast<SendFn>(&Send);
    const int result =
        send(self.data(), 0x11223344, native.data(),
             static_cast<uint32_t>(native.size()), 0x55667788);
    Check(result == static_cast<int>(kSendResult) &&
              g_before_calls.size() == 1 &&
              g_before_calls[0].pointer == native.data() &&
              g_before_calls[0].bytes == original &&
              g_send_calls.size() == 1 &&
              g_send_calls[0].self == self.data() &&
              g_send_calls[0].connection == 0x11223344 &&
              g_send_calls[0].flags == 0x55667788 &&
              g_send_calls[0].bytes == g_send_replacement &&
              g_send_calls[0].pointer != native.data() &&
              native == original,
          "Send replaces only the target payload while preserving native call arguments and caller storage");
    Check(g_after_calls.size() == 1 &&
              !g_after_calls[0].queued &&
              g_after_calls[0].connection == 0x11223344 &&
              g_after_calls[0].pointer ==
                  g_send_calls[0].pointer &&
              g_after_calls[0].size ==
                  g_send_replacement.size() &&
              g_after_calls[0].bytes == g_send_replacement &&
              g_after_calls[0].result ==
                  static_cast<int>(kSendResult),
          "Send reports the actual extended lower-layer packet and return through after_send");

    ResetSendFixture();
    auto unrelated = NativePacket(24, 0x1234, 0x22);
    const auto unrelated_original = unrelated;
    const int unrelated_result =
        send(self.data(), 0xABCDEF01, unrelated.data(),
             static_cast<uint32_t>(unrelated.size()), 0x10203040);
    Check(unrelated_result == static_cast<int>(kSendResult) &&
              g_before_calls.size() == 1 &&
              g_send_calls.size() == 1 &&
              g_send_calls[0].pointer == unrelated.data() &&
              g_send_calls[0].bytes == unrelated_original &&
              g_send_calls[0].connection == 0xABCDEF01 &&
              g_send_calls[0].flags == 0x10203040 &&
              unrelated == unrelated_original,
          "unrelated Send payload, pointer, destination, flags and return remain native");
    Check(g_after_calls.size() == 1 &&
              !g_after_calls[0].queued &&
              g_after_calls[0].pointer == unrelated.data() &&
              g_after_calls[0].size == unrelated.size() &&
              g_after_calls[0].bytes == unrelated_original &&
              TestWord(g_after_calls[0].bytes.data()) == 0x1234,
          "unrelated keepalive callback retains its original non-title packet shape");

    ResetSendFixture();
    g_reject_target = true;
    native = NativePacket(kCreateRoomBytes, kCreateRoom, 0x33);
    Check(send(self.data(), 7, native.data(),
               static_cast<uint32_t>(native.size()), 8) == 0 &&
              g_before_calls.size() == 1 &&
              g_send_calls.empty() && g_after_calls.empty(),
          "rejected target Send never reaches the lower layer");
}

void TestQueueAdapter()
{
    ResetSendFixture();
    std::array<uint8_t, 16> self = {};
    auto native = NativePacket(kCreateRoomBytes, kCreateRoom, 0x41);
    g_send_replacement =
        CreationPacket(native, "pvp queued replacement", 102);
    auto queue = reinterpret_cast<QueueFn>(&Queue);
    const uint32_t result =
        queue(self.data(), 0x01020304, native.data(),
              static_cast<uint32_t>(native.size()), 0xA5);
    Check(result == kQueueResult &&
              g_queue_calls.size() == 1 &&
              g_queue_calls[0].self == self.data() &&
              g_queue_calls[0].connection == 0x01020304 &&
              g_queue_calls[0].flags == 0xA5 &&
              g_queue_calls[0].pointer != native.data() &&
              g_queue_calls[0].owned_copy == g_send_replacement,
          "Queue hands the complete replacement to the native pre-defer copy");
    Check(g_after_calls.size() == 1 &&
              g_after_calls[0].queued &&
              g_after_calls[0].pointer ==
                  g_queue_calls[0].pointer &&
              g_after_calls[0].size ==
                  g_send_replacement.size() &&
              g_after_calls[0].bytes == g_send_replacement &&
              g_after_calls[0].result ==
                  static_cast<int>(kQueueResult),
          "Queue reports the actual extended pre-defer packet and native result");

    ResetSendFixture();
    auto unrelated = NativePacket(18, 0x4567, 0x52);
    const auto unrelated_original = unrelated;
    const uint32_t unrelated_result =
        queue(self.data(), 0x05060708, unrelated.data(),
              static_cast<uint32_t>(unrelated.size()), 0x3C);
    Check(unrelated_result == kQueueResult &&
              g_queue_calls.size() == 1 &&
              g_queue_calls[0].pointer == unrelated.data() &&
              g_queue_calls[0].owned_copy == unrelated_original &&
              g_queue_calls[0].connection == 0x05060708 &&
              g_queue_calls[0].flags == 0x3C,
          "unrelated Queue payload, pointer, destination, flags and return remain native");
    Check(g_after_calls.size() == 1 &&
              g_after_calls[0].queued &&
              g_after_calls[0].pointer == unrelated.data() &&
              g_after_calls[0].size == unrelated.size() &&
              g_after_calls[0].bytes == unrelated_original &&
              TestWord(g_after_calls[0].bytes.data()) == 0x4567,
          "unrelated queued callback retains its original non-title packet shape");

    ResetSendFixture();
    native = NativePacket(kCreateRoomBytes, kCreateRoom, 0x63);
    g_send_replacement =
        CreationPacket(native, "queue zero fallback", 103);
    g_fake_queue_result = 0;
    auto send = reinterpret_cast<SendFn>(&Send);
    const uint32_t queued =
        queue(self.data(), 0x0A0B0C0D, native.data(),
              static_cast<uint32_t>(native.size()), 0x7E);
    const int sent =
        send(self.data(), 0x0A0B0C0D, native.data(),
             static_cast<uint32_t>(native.size()), 0x7E);
    Check(queued == 0 && sent == static_cast<int>(kSendResult) &&
              g_before_calls.size() == 2 &&
              g_before_calls[0].pointer == native.data() &&
              g_before_calls[1].pointer == native.data() &&
              g_queue_calls.size() == 1 &&
              g_queue_calls[0].owned_copy == g_send_replacement &&
              g_send_calls.size() == 1 &&
              g_send_calls[0].bytes == g_send_replacement,
          "queue-zero fallback can replace the same request again on immediate Send");
    Check(g_after_calls.size() == 2 &&
              g_after_calls[0].queued &&
              g_after_calls[0].bytes == g_send_replacement &&
              g_after_calls[0].size ==
                  g_send_replacement.size() &&
              g_after_calls[0].result == 0 &&
              !g_after_calls[1].queued &&
              g_after_calls[1].bytes == g_send_replacement &&
              g_after_calls[1].size ==
                  g_send_replacement.size() &&
              g_after_calls[1].result ==
                  static_cast<int>(kSendResult),
          "queue-zero and immediate fallback callbacks remain distinct and ordered");
}

void TestSendQueueLastError()
{
    std::array<uint8_t, 16> self = {};

    ResetSendFixture();
    auto native = NativePacket(kCreateRoomBytes, kCreateRoom, 0x6A);
    g_send_replacement =
        CreationPacket(native, "mission LastError send", 104);
    g_track_send_last_error = true;
    auto send = reinterpret_cast<SendFn>(&Send);
    SetLastError(kSendCallerError);
    const int send_result =
        send(self.data(), 0x11112222, native.data(),
             static_cast<uint32_t>(native.size()), 0x33334444);
    const DWORD send_final_error = GetLastError();
    Check(send_result == static_cast<int>(kSendResult) &&
              g_before_calls.size() == 1 &&
              g_send_calls.size() == 1 &&
              g_after_calls.size() == 1 &&
              g_send_native_input_error == kSendCallerError &&
              send_final_error == kSendNativeResultError,
          "Send restores the caller LastError before native code and preserves the native result across callbacks");

    ResetSendFixture();
    native = NativePacket(kCreateRoomBytes, kCreateRoom, 0x7B);
    g_send_replacement =
        CreationPacket(native, "pvp LastError queue", 105);
    g_track_send_last_error = true;
    auto queue = reinterpret_cast<QueueFn>(&Queue);
    SetLastError(kQueueCallerError);
    const uint32_t queue_result =
        queue(self.data(), 0x55556666, native.data(),
              static_cast<uint32_t>(native.size()), 0x77);
    const DWORD queue_final_error = GetLastError();
    Check(queue_result == kQueueResult &&
              g_before_calls.size() == 1 &&
              g_queue_calls.size() == 1 &&
              g_after_calls.size() == 1 &&
              g_queue_native_input_error == kQueueCallerError &&
              queue_final_error == kQueueNativeResultError,
          "Queue restores the caller LastError before native code and preserves the native result across callbacks");
}

enum class ReceiveMode
{
    DecodeOuter,
    DecodeDifferent,
    Nested,
    RaiseSeh
};

struct DecodeCall
{
    void* self = nullptr;
    const void* packet = nullptr;
    void* native_out = nullptr;
};

struct ReceiveCall
{
    void* self = nullptr;
    uint32_t argument0 = 0;
    uint32_t argument1 = 0;
    uint32_t source_handle = 0;
    void* packet = nullptr;
    uint32_t size = 0;
    bool canary_unchanged = false;
    const uint8_t* decoded = nullptr;
    std::vector<uint8_t> decoded_native;
};

struct OnReceiveCall
{
    uint32_t source_handle = 0;
    std::vector<uint8_t> native;
    std::vector<uint8_t> metadata_body;
};

std::vector<DecodeCall> g_decode_calls;
std::vector<ReceiveCall> g_receive_calls;
std::vector<OnReceiveCall> g_on_receive_calls;
ReceiveMode g_receive_mode = ReceiveMode::DecodeOuter;
const void* g_different_decode_packet = nullptr;
std::vector<uint8_t> g_nested_packet;
uint32_t g_nested_source_handle = 0;
LONG g_receive_depth = 0;
std::array<uint8_t, 32> g_decode_self = {};
const uint8_t* g_latest_receiver_pointer = nullptr;
bool g_track_receive_last_error = false;
DWORD g_decode_native_input_error = 0;

size_t NativeReceiveSize(uint16_t opcode, const uint8_t* packet,
                         size_t size)
{
    const size_t result =
        opcode == kRoomList ? kRoomListBytes :
        opcode == kCreateAck ? kCreateAckBytes :
        opcode == kTeamList
            ? (packet && size >= kTeamListHeaderBytes &&
                       packet[0x12] <= 10
                   ? kTeamListHeaderBytes +
                         static_cast<size_t>(packet[0x12]) *
                             kTeamListRowBytes
                   : kTeamListHeaderBytes)
            : 0;
    if (g_track_receive_last_error)
        SetLastError(kNativeSizeCallbackError);
    return result;
}

void OnReceive(uint32_t source_handle, const uint8_t* native_packet,
               size_t native_size, const uint8_t* body,
               size_t body_size)
{
    OnReceiveCall call;
    call.source_handle = source_handle;
    if (native_packet && native_size)
        call.native.assign(native_packet, native_packet + native_size);
    if (body && body_size)
        call.metadata_body.assign(body, body + body_size);
    g_on_receive_calls.push_back(std::move(call));
    if (g_track_receive_last_error)
        SetLastError(kOnReceiveCallbackError);
}

const uint8_t* __fastcall FakeDecode(void* self, void*, const void* packet,
                                     void* native_out)
{
    const DWORD input_error = GetLastError();
    if (g_track_receive_last_error)
        g_decode_native_input_error = input_error;
    g_decode_calls.push_back({self, packet, native_out});
    const uint8_t* result = nullptr;
    if (packet) {
        const auto* bytes = static_cast<const uint8_t*>(packet);
        const uint16_t opcode = TestWord(bytes);
        if (opcode != 0x65 && opcode != 0x66) {
            result = bytes;
        } else {
            const uint32_t key = TestDword(bytes + 2);
            const size_t size = TestWord(bytes + 6);
            auto* output = static_cast<uint8_t*>(native_out);
            const uint8_t key_bytes[4] = {
                static_cast<uint8_t>(key),
                static_cast<uint8_t>(key >> 8),
                static_cast<uint8_t>(key >> 16),
                static_cast<uint8_t>(key >> 24)};
            for (size_t index = 0; index < size; ++index)
                output[index] =
                    key ? static_cast<uint8_t>(
                              bytes[8 + index] ^
                              key_bytes[index % 4])
                        : bytes[8 + index];
            result = output;
        }
    }
    if (g_track_receive_last_error)
        SetLastError(kDecodeNativeResultError);
    return result;
}

int __fastcall FakeReceive(void* self, void*, uint32_t argument0,
                           uint32_t argument1, uint32_t source_handle,
                           void* packet, uint32_t size)
{
    ReceiveCall call;
    call.self = self;
    call.argument0 = argument0;
    call.argument1 = argument1;
    call.source_handle = source_handle;
    call.packet = packet;
    call.size = size;
    if (g_receive_mode == ReceiveMode::RaiseSeh)
        RaiseException(kTestSeh, 0, 0, nullptr);

    const bool outer_nested =
        g_receive_mode == ReceiveMode::Nested &&
        InterlockedCompareExchange(&g_receive_depth, 0, 0) == 0;
    if (outer_nested) {
        InterlockedIncrement(&g_receive_depth);
        auto receive = reinterpret_cast<ReceiveFn>(&Receive);
        receive(self, 0xD1D1D1D1, 0xD2D2D2D2,
                g_nested_source_handle, g_nested_packet.data(),
                static_cast<uint32_t>(g_nested_packet.size()));
        InterlockedDecrement(&g_receive_depth);
    }

    std::array<uint8_t, 8> native_out;
    native_out.fill(0xA5);
    const auto before = native_out;
    const void* decode_packet =
        g_receive_mode == ReceiveMode::DecodeDifferent
            ? g_different_decode_packet
            : packet;
    auto decode = reinterpret_cast<DecodeFn>(&Decode);
    const uint8_t* decoded =
        decode(g_decode_self.data(), decode_packet, native_out.data());
    call.canary_unchanged = native_out == before;
    call.decoded = decoded;
    const size_t expected =
        decoded ? NativeReceiveSize(TestWord(decoded), decoded, size) : 0;
    if (expected && (decoded != packet || size >= expected))
        call.decoded_native.assign(decoded, decoded + expected);
    g_latest_receiver_pointer = decoded;
    g_receive_calls.push_back(std::move(call));
    return decoded ? static_cast<int>(0x41000000u |
                                      (source_handle & 0xFFFFu))
                   : 0;
}

void ResetReceiveFixture()
{
    g_receive = reinterpret_cast<ReceiveFn>(&FakeReceive);
    g_decode = reinterpret_cast<DecodeFn>(&FakeDecode);
    g_decode_calls.clear();
    g_receive_calls.clear();
    g_on_receive_calls.clear();
    g_receive_mode = ReceiveMode::DecodeOuter;
    g_different_decode_packet = nullptr;
    g_nested_packet.clear();
    g_nested_source_handle = 0;
    InterlockedExchange(&g_receive_depth, 0);
    g_latest_receiver_pointer = nullptr;
    g_track_receive_last_error = false;
    g_decode_native_input_error = 0;
    g_context = nullptr;
    g_latestDecoded.reset();
    g_callbacks = {};
    g_callbacks.native_receive_size = &NativeReceiveSize;
    g_callbacks.on_receive = &OnReceive;
}

int InvokeReceive(void* self, uint32_t argument0, uint32_t argument1,
                  uint32_t source_handle, std::vector<uint8_t>* packet)
{
    auto receive = reinterpret_cast<ReceiveFn>(&Receive);
    return receive(self, argument0, argument1, source_handle,
                   packet->data(), static_cast<uint32_t>(packet->size()));
}

bool DecodeBatchBody(const std::vector<uint8_t>& body,
                     uint32_t expected_request)
{
    metadata::SnapshotBatch batch;
    return metadata::DecodeSnapshotBatchBody(body.data(), body.size(),
                                             &batch) &&
           batch.request_id == expected_request;
}

void TestRawReceive()
{
    ResetReceiveFixture();
    std::array<uint8_t, 24> self = {};
    const auto native = NativePacket(kRoomListBytes, kRoomList, 0x71);
    auto packet = BatchPacket(native, 201, "mission raw receive");
    const auto original = packet;
    const int result =
        InvokeReceive(self.data(), 0x11111111, 0x22222222,
                      0x33333333, &packet);
    Check(result == static_cast<int>(0x41003333) &&
              g_receive_calls.size() == 1 &&
              g_receive_calls[0].self == self.data() &&
              g_receive_calls[0].argument0 == 0x11111111 &&
              g_receive_calls[0].argument1 == 0x22222222 &&
              g_receive_calls[0].source_handle == 0x33333333 &&
              g_receive_calls[0].packet == packet.data() &&
              g_receive_calls[0].size == packet.size(),
          "Receive uses the exact thiscall five-stack-argument contract and third source handle");
    Check(g_decode_calls.size() == 1 &&
              g_decode_calls[0].self == g_decode_self.data() &&
              g_decode_calls[0].packet == packet.data() &&
              g_receive_calls[0].canary_unchanged,
          "raw receive calls the exact thiscall two-stack-argument decoder contract once");
    Check(g_on_receive_calls.size() == 1 &&
              g_on_receive_calls[0].source_handle == 0x33333333 &&
              g_on_receive_calls[0].native == native &&
              DecodeBatchBody(g_on_receive_calls[0].metadata_body, 201) &&
              g_receive_calls[0].decoded_native == native &&
              packet == original,
          "raw extended receive reports metadata and returns only the native prefix");
    Check(g_latest_receiver_pointer &&
              std::memcmp(g_latest_receiver_pointer, native.data(),
                          native.size()) == 0,
          "owned stripped raw buffer remains valid after Receive returns");

    ResetReceiveFixture();
    auto exact = native;
    const int exact_result =
        InvokeReceive(self.data(), 1, 2, 3, &exact);
    Check(exact_result == static_cast<int>(0x41000003) &&
              g_decode_calls.size() == 1 &&
              g_on_receive_calls.size() == 1 &&
              g_on_receive_calls[0].metadata_body.empty() &&
              g_receive_calls[0].decoded == exact.data(),
          "exact-size native room list remains on the raw native pointer");

    ResetReceiveFixture();
    auto unknown = NativePacket(40, 0x4321, 0x82);
    const auto unknown_original = unknown;
    const int unknown_result =
        InvokeReceive(self.data(), 4, 5, 6, &unknown);
    Check(unknown_result == static_cast<int>(0x41000006) &&
              g_decode_calls.size() == 1 &&
              g_on_receive_calls.empty() &&
              g_receive_calls[0].decoded == unknown.data() &&
              unknown == unknown_original,
          "unknown decoded opcode passes through without metadata handling");
}

void TestEnvelopeReceive()
{
    std::array<uint8_t, 24> self = {};
    const auto native = NativePacket(kRoomListBytes, kRoomList, 0x91);
    const auto plain =
        BatchPacket(native, 202, "pvp envelope key zero");

    ResetReceiveFixture();
    auto key_zero = Envelope(0x65, 0, plain);
    const int zero_result =
        InvokeReceive(self.data(), 7, 8, 0x9001, &key_zero);
    Check(zero_result == static_cast<int>(0x41009001) &&
              g_decode_calls.size() == 1 &&
              g_decode_calls[0].packet == key_zero.data() &&
              g_decode_calls[0].native_out !=
                  g_receive_calls[0].packet &&
              g_receive_calls[0].canary_unchanged &&
              g_on_receive_calls.size() == 1 &&
              g_on_receive_calls[0].native == native &&
              DecodeBatchBody(g_on_receive_calls[0].metadata_body, 202),
          "0x65 key-zero envelope uses one native copy decode into owned storage before trailer parsing");
    Check(g_latest_receiver_pointer &&
              std::memcmp(g_latest_receiver_pointer, native.data(),
                          native.size()) == 0,
          "owned key-zero decoded prefix remains valid after Receive returns");

    ResetReceiveFixture();
    const auto cipher_plain =
        BatchPacket(native, 203, "mission fake cipher envelope");
    auto keyed = Envelope(0x66, 0xA1B2C3D4, cipher_plain);
    Check(std::search(keyed.begin(), keyed.end(),
                      reinterpret_cast<const uint8_t*>("RTMD"),
                      reinterpret_cast<const uint8_t*>("RTMD") + 4) ==
              keyed.end(),
          "test ciphertext does not expose trailer magic before native decode");
    const int keyed_result =
        InvokeReceive(self.data(), 9, 10, 0x9002, &keyed);
    Check(keyed_result == static_cast<int>(0x41009002) &&
              g_decode_calls.size() == 1 &&
              g_receive_calls[0].canary_unchanged &&
              g_on_receive_calls.size() == 1 &&
              g_on_receive_calls[0].native == native &&
              DecodeBatchBody(g_on_receive_calls[0].metadata_body, 203),
          "0x66 nonzero-key fixture recognizes metadata only after one fake-native decrypt");
}

void TestVariableTeamListReceive()
{
    std::array<uint8_t, 24> self = {};
    const uint8_t counts[] = {0, 6, 10};
    for (uint8_t count : counts) {
        ResetReceiveFixture();
        const size_t native_size =
            kTeamListHeaderBytes +
            static_cast<size_t>(count) * kTeamListRowBytes;
        auto native =
            NativePacket(native_size, kTeamList,
                         static_cast<uint8_t>(0x40 + count));
        native[0x12] = count;
        auto packet =
            BatchPacket(native, 220 + count,
                        "mission Team-list variable native prefix");
        const int result =
            InvokeReceive(self.data(), 0x31, 0x32,
                          0xA000 + count, &packet);
        Check(result ==
                  static_cast<int>(0x41000000u |
                                   ((0xA000u + count) & 0xFFFFu)) &&
                  g_decode_calls.size() == 1 &&
                  g_on_receive_calls.size() == 1 &&
                  g_on_receive_calls[0].native == native &&
                  DecodeBatchBody(g_on_receive_calls[0].metadata_body,
                                  220 + count) &&
                  g_receive_calls.size() == 1 &&
                  g_receive_calls[0].decoded_native == native,
              count == 0
                  ? "zero-row 0x824 Team list strips metadata at its computed 0x13-byte prefix"
                  : count == 6
                        ? "six-row 0x824 Team list strips metadata after six 0x123-byte rows"
                        : "ten-row 0x824 Team list strips metadata at the native page maximum");
    }
}

void TestReceiveRejectionsAndPointerGate()
{
    std::array<uint8_t, 24> self = {};
    const auto native = NativePacket(kRoomListBytes, kRoomList, 0xA1);

    ResetReceiveFixture();
    auto invalid_length = Envelope(0x65, 0, std::vector<uint8_t>{1, 2, 3});
    Put16(invalid_length.data() + 6, 10);
    Check(InvokeReceive(self.data(), 11, 12, 13,
                        &invalid_length) == 0 &&
              g_decode_calls.empty() && g_on_receive_calls.empty() &&
              g_receive_calls.size() == 1 &&
              g_receive_calls[0].canary_unchanged,
          "invalid envelope plaintext length drops before native decoder access");

    ResetReceiveFixture();
    auto unrelated_suffix = native;
    unrelated_suffix.insert(unrelated_suffix.end(),
                            {0x99, 0x18, 0xA7, 0x26, 0xB5});
    const auto unrelated_original = unrelated_suffix;
    const int unrelated_result =
        InvokeReceive(self.data(), 14, 15, 16,
                      &unrelated_suffix);
    Check(unrelated_result == static_cast<int>(0x41000010) &&
              g_decode_calls.size() == 1 &&
              g_decode_calls[0].packet == unrelated_suffix.data() &&
              g_receive_calls.size() == 1 &&
              g_receive_calls[0].decoded == unrelated_suffix.data() &&
              g_latest_receiver_pointer == unrelated_suffix.data() &&
              std::memcmp(g_latest_receiver_pointer,
                          unrelated_original.data(),
                          unrelated_original.size()) == 0 &&
              unrelated_suffix == unrelated_original &&
              g_on_receive_calls.empty(),
          "known oversized plaintext without RTMD passes through complete and unchanged");

    ResetReceiveFixture();
    auto short_native_error = NativePacket(6, kCreateAck, 0x9A);
    const auto short_error_original = short_native_error;
    const int short_error_result =
        InvokeReceive(self.data(), 19, 20, 0x9004,
                      &short_native_error);
    Check(short_error_result == static_cast<int>(0x41009004) &&
              g_decode_calls.size() == 1 &&
              g_receive_calls.size() == 1 &&
              g_receive_calls[0].decoded == short_native_error.data() &&
              g_receive_calls[0].decoded_native.empty() &&
              g_latest_receiver_pointer == short_native_error.data() &&
              g_on_receive_calls.empty() &&
              short_native_error == short_error_original,
          "short bare native failure for a known success opcode passes through without a metadata callback");

    ResetReceiveFixture();
    auto malformed_trailer =
        BatchPacket(native, 205, "malformed RTMD candidate");
    Put16(malformed_trailer.data() + malformed_trailer.size() -
              metadata::kTrailerFooterBytes,
          static_cast<uint16_t>(metadata::kProtocolVersion + 1));
    Check(metadata::ParseTrailer(malformed_trailer.data(),
                                 malformed_trailer.size())
                  .status == metadata::TrailerStatus::Invalid &&
              InvokeReceive(self.data(), 21, 22, 23,
                            &malformed_trailer) == 0 &&
              g_decode_calls.size() == 1 &&
              g_receive_calls.size() == 1 &&
              g_receive_calls[0].decoded == nullptr &&
              g_latest_receiver_pointer == nullptr &&
              g_on_receive_calls.empty(),
          "known oversized plaintext with a malformed RTMD footer drops after native decode");

    ResetReceiveFixture();
    auto outer = BatchPacket(native, 204, "outer pointer gate");
    auto different = NativePacket(32, 0x7777, 0xB2);
    g_receive_mode = ReceiveMode::DecodeDifferent;
    g_different_decode_packet = different.data();
    const int different_result =
        InvokeReceive(self.data(), 17, 18, 0x9003, &outer);
    Check(different_result == static_cast<int>(0x41009003) &&
              g_decode_calls.size() == 1 &&
              g_decode_calls[0].packet == different.data() &&
              g_receive_calls[0].decoded == different.data() &&
              g_on_receive_calls.empty(),
          "Decode on a different packet pointer inside Receive keeps the native path");
}

void TestDecodeLastError()
{
    ResetReceiveFixture();
    const auto native = NativePacket(kRoomListBytes, kRoomList, 0xB3);
    auto packet =
        BatchPacket(native, 206, "mission LastError receive");
    ReceiveContext context;
    context.outer = packet.data();
    context.outer_size = packet.size();
    context.source_handle = 0xA0B0C0D0;
    std::array<uint8_t, 8> native_out = {};
    ReceiveContext* previous = g_context;
    g_context = &context;
    g_track_receive_last_error = true;
    auto decode = reinterpret_cast<DecodeFn>(&Decode);
    SetLastError(kDecodeCallerError);
    const uint8_t* decoded =
        decode(g_decode_self.data(), packet.data(), native_out.data());
    const DWORD final_error = GetLastError();
    g_context = previous;

    Check(decoded != nullptr &&
              g_decode_calls.size() == 1 &&
              g_decode_native_input_error == kDecodeCallerError &&
              g_on_receive_calls.size() == 1 &&
              g_on_receive_calls[0].source_handle ==
                  context.source_handle &&
              g_on_receive_calls[0].native == native &&
              DecodeBatchBody(g_on_receive_calls[0].metadata_body, 206) &&
              final_error == kDecodeNativeResultError,
          "Decode restores the caller LastError before native code and preserves the native result across receive callbacks");
}

void TestNestedReceive()
{
    ResetReceiveFixture();
    std::array<uint8_t, 24> self = {};
    const auto outer_native =
        NativePacket(kRoomListBytes, kRoomList, 0xC1);
    const auto inner_native =
        NativePacket(kRoomListBytes, kRoomList, 0xD2);
    const auto outer_plain =
        BatchPacket(outer_native, 301, "outer nested batch");
    auto outer = Envelope(0x66, 0x11223344, outer_plain);
    g_nested_packet =
        BatchPacket(inner_native, 302, "inner nested batch");
    g_nested_source_handle = 0xCAFE0002;
    g_receive_mode = ReceiveMode::Nested;

    const int result =
        InvokeReceive(self.data(), 0xC1C1C1C1, 0xC2C2C2C2,
                      0xCAFE0001, &outer);
    Check(result == static_cast<int>(0x41000001) &&
              g_decode_calls.size() == 2 &&
              g_on_receive_calls.size() == 2 &&
              g_on_receive_calls[0].source_handle ==
                  g_nested_source_handle &&
              DecodeBatchBody(g_on_receive_calls[0].metadata_body, 302) &&
              g_on_receive_calls[1].source_handle == 0xCAFE0001 &&
              DecodeBatchBody(g_on_receive_calls[1].metadata_body, 301),
          "nested Receive restores the enclosing TLS context and source handle");
    Check(g_receive_calls.size() == 2 &&
              g_receive_calls[0].decoded_native == inner_native &&
              g_receive_calls[1].decoded_native == outer_native,
          "nested and enclosing decodes retain their independent native prefixes");
}

DWORD InvokeReceiveCatchingSeh(ReceiveFn receive, void* self,
                               uint32_t argument0, uint32_t argument1,
                               uint32_t source_handle, void* packet,
                               uint32_t size, int* result)
{
    __try {
        *result = receive(self, argument0, argument1, source_handle,
                          packet, size);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return GetExceptionCode();
    }
}

void TestSehRestoresContext()
{
    ResetReceiveFixture();
    std::array<uint8_t, 24> self = {};
    auto packet = NativePacket(kRoomListBytes, kRoomList, 0xE3);
    g_receive_mode = ReceiveMode::RaiseSeh;
    auto receive = reinterpret_cast<ReceiveFn>(&Receive);
    int ignored = 0;
    const DWORD exception =
        InvokeReceiveCatchingSeh(
            receive, self.data(), 1, 2, 3, packet.data(),
            static_cast<uint32_t>(packet.size()), &ignored);
    Check(exception == kTestSeh && g_context == nullptr,
          "SEH from the native receiver restores the TLS context");

    g_receive_mode = ReceiveMode::DecodeOuter;
    std::array<uint8_t, 8> native_out = {};
    auto decode = reinterpret_cast<DecodeFn>(&Decode);
    const uint8_t* decoded =
        decode(g_decode_self.data(), packet.data(), native_out.data());
    Check(decoded == packet.data() && g_decode_calls.size() == 1 &&
              g_on_receive_calls.empty(),
          "Decode after exceptional Receive no longer sees the expired scope");
}

void RunTests()
{
    TestSendAdapter();
    TestQueueAdapter();
    TestSendQueueLastError();
    TestRawReceive();
    TestEnvelopeReceive();
    TestVariableTeamListReceive();
    TestReceiveRejectionsAndPointerGate();
    TestDecodeLastError();
    TestNestedReceive();
    TestSehRestoresContext();
}
}  // namespace
}  // namespace sdgo::room_titles::native_transport

int main()
{
    sdgo::room_titles::native_transport::RunTests();
    std::puts(sdgo::room_titles::native_transport::g_failures
                  ? "ROOM TITLES TRANSPORT CHECKS FAILED"
                  : "ALL ROOM TITLES TRANSPORT CHECKS PASSED");
    return sdgo::room_titles::native_transport::g_failures ? 1 : 0;
}
