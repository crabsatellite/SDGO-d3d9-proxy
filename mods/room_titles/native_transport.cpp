#include "native_transport.h"

#include <windows.h>

#include <memory>
#include <new>

#include "room_metadata_common.h"

namespace sdgo::room_titles::native_transport {
namespace {
namespace metadata = room_metadata;
using SendFn = int(__thiscall*)(void*, uint32_t, const void*, uint32_t, uint32_t);
using QueueFn = uint32_t(__thiscall*)(void*, uint32_t, const void*, uint32_t, uint8_t);
using ReceiveFn = int(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, void*, uint32_t);
using DecodeFn = const uint8_t*(__thiscall*)(void*, const void*, void*);
SendFn g_send = nullptr;
QueueFn g_queue = nullptr;
ReceiveFn g_receive = nullptr;
DecodeFn g_decode = nullptr;
Callbacks g_callbacks;

struct LastErrorGuard {
    DWORD value = GetLastError();
    ~LastErrorGuard() { SetLastError(value); }
    void BeforeNative() const { SetLastError(value); }
    void AfterNative() { value = GetLastError(); }
};

struct ReceiveContext {
    const uint8_t* outer = nullptr;
    size_t outer_size = 0;
    uint32_t source_handle = 0;
    std::shared_ptr<std::vector<uint8_t>> decoded;
};
thread_local ReceiveContext* g_context = nullptr;
// The original decoder uses a global scratch buffer. Retain the most recent
// owned buffer until the next successful decode, in addition to active scopes.
thread_local std::shared_ptr<std::vector<uint8_t>> g_latestDecoded;

uint16_t Word(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

int __fastcall Send(void* self, void*, uint32_t connection, const void* packet, uint32_t size, uint32_t flags)
{
    if (!packet || !size || size > metadata::kMaximumPacketBytes || !g_callbacks.before_send)
        return g_send(self, connection, packet, size, flags);
    LastErrorGuard error;
    try {
        std::vector<uint8_t> replacement;
        const auto decision = g_callbacks.before_send(connection, static_cast<const uint8_t*>(packet), size,
                                                        flags, &replacement);
        if (decision == SendDecision::Reject)
            return 0;
        const bool replace = decision == SendDecision::Replace;
        if (replace && (replacement.size() < 2 || replacement.size() > metadata::kMaximumPacketBytes))
            return 0;
        error.BeforeNative();
        const int result = g_send(self, connection, replace ? replacement.data() : packet,
                                  replace ? static_cast<uint32_t>(replacement.size()) : size, flags);
        error.AfterNative();
        if (g_callbacks.after_send)
            g_callbacks.after_send(connection, false,
                                    replace ? replacement.data() : static_cast<const uint8_t*>(packet),
                                    replace ? replacement.size() : size, result);
        return result;
    } catch (const std::bad_alloc&) {
        return 0;
    }
}

uint32_t __fastcall Queue(void* self, void*, uint32_t connection, const void* packet, uint32_t size, uint8_t flags)
{
    if (!packet || !size || size > metadata::kMaximumPacketBytes || !g_callbacks.before_send)
        return g_queue(self, connection, packet, size, flags);
    LastErrorGuard error;
    try {
        std::vector<uint8_t> replacement;
        const auto decision = g_callbacks.before_send(connection, static_cast<const uint8_t*>(packet), size,
                                                        flags, &replacement);
        if (decision == SendDecision::Reject)
            return 0;
        const bool replace = decision == SendDecision::Replace;
        if (replace && (replacement.size() < 2 || replacement.size() > metadata::kMaximumPacketBytes))
            return 0;
        error.BeforeNative();
        const uint32_t result = g_queue(self, connection, replace ? replacement.data() : packet,
                                        replace ? static_cast<uint32_t>(replacement.size()) : size, flags);
        error.AfterNative();
        // A zero queue result means the native caller tries the immediate path;
        // it must not fail or discard the same staged title transaction here.
        if (g_callbacks.after_send)
            g_callbacks.after_send(connection, true,
                                    replace ? replacement.data() : static_cast<const uint8_t*>(packet),
                                    replace ? replacement.size() : size, static_cast<int>(result));
        return result;
    } catch (const std::bad_alloc&) {
        return 0;
    }
}

int ReceiveScoped(void* self, uint32_t argument0, uint32_t argument1, uint32_t sourceHandle,
                   void* packet, uint32_t size, ReceiveContext* context)
{
    ReceiveContext* previous = g_context;
    g_context = context;
    __try {
        return g_receive(self, argument0, argument1, sourceHandle, packet, size);
    } __finally {
        g_context = previous;
    }
}

int __fastcall Receive(void* self, void*, uint32_t argument0, uint32_t argument1, uint32_t sourceHandle,
                        void* packet, uint32_t size)
{
    if (!packet || size < 2 || size > metadata::kMaximumPacketBytes + 8)
        return g_receive(self, argument0, argument1, sourceHandle, packet, size);
    LastErrorGuard error;
    ReceiveContext context;
    context.outer = static_cast<const uint8_t*>(packet);
    context.outer_size = size;
    context.source_handle = sourceHandle;
    error.BeforeNative();
    const int result = ReceiveScoped(self, argument0, argument1, sourceHandle, packet, size, &context);
    error.AfterNative();
    return result;
}

const uint8_t* __fastcall Decode(void* self, void*, const void* packet, void* nativeOut)
{
    if (!g_context || !packet || packet != g_context->outer)
        return g_decode(self, packet, nativeOut);
    LastErrorGuard error;
    try {
        auto& context = *g_context;
        const uint16_t outerOpcode = Word(context.outer);
        const bool envelope = outerOpcode == 0x65 || outerOpcode == 0x66;
        size_t plainSize = context.outer_size;
        const uint8_t* plain = nullptr;
        if (envelope) {
            if (context.outer_size < 8)
                return nullptr;
            plainSize = Word(context.outer + 6);
            if (plainSize < 2 || plainSize > context.outer_size - 8)
                return nullptr;
            context.decoded = std::make_shared<std::vector<uint8_t>>(plainSize);
            // Use the native key/state and decoder exactly once. Never inspect
            // ciphertext for metadata, and never write plaintext into an envelope.
            error.BeforeNative();
            plain = g_decode(self, packet, context.decoded->data());
            error.AfterNative();
            if (!plain)
                return nullptr;
            g_latestDecoded = context.decoded;
        } else {
            error.BeforeNative();
            plain = g_decode(self, packet, nativeOut);
            error.AfterNative();
            if (!plain)
                return nullptr;
        }
        const size_t expected = g_callbacks.native_receive_size ?
            g_callbacks.native_receive_size(Word(plain), plain, plainSize) : 0;
        if (!expected)
            return plain;
        if (plainSize == expected) {
            if (g_callbacks.on_receive)
                g_callbacks.on_receive(context.source_handle, plain, plainSize, nullptr, 0);
            return plain;
        }
        const auto parsed = metadata::ParseTrailer(plain, plainSize);
        if (parsed.status == metadata::TrailerStatus::NoTrailer)
            return plain; // includes shorter native errors and other-Mod extensions
        if (parsed.status != metadata::TrailerStatus::Valid || parsed.view.native_size != expected)
            return nullptr;
        if (g_callbacks.on_receive)
            g_callbacks.on_receive(context.source_handle, plain, expected, parsed.view.body_data, parsed.view.body_size);
        if (!context.decoded || plain != context.decoded->data())
            context.decoded = std::make_shared<std::vector<uint8_t>>(plain, plain + expected);
        else
            context.decoded->resize(expected);
        g_latestDecoded = context.decoded;
        // The verified GameApp receiver never reads its old length after decode;
        // all native opcode handlers now see only the original native prefix.
        return context.decoded->data();
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}
} // namespace

bool Install(const SdgoApi* api, SdgoMod* mod, const Callbacks& callbacks)
{
    g_callbacks = callbacks;
    return api->HookCode(mod, "room_titles.net_send_plain", reinterpret_cast<void*>(&Send),
                          reinterpret_cast<void**>(&g_send)) &&
           api->HookCode(mod, "room_titles.net_send_queued", reinterpret_cast<void*>(&Queue),
                          reinterpret_cast<void**>(&g_queue)) &&
           api->HookCode(mod, "room_titles.net_receive", reinterpret_cast<void*>(&Receive),
                          reinterpret_cast<void**>(&g_receive)) &&
           api->HookCode(mod, "room_titles.net_decode", reinterpret_cast<void*>(&Decode),
                          reinterpret_cast<void**>(&g_decode));
}
} // namespace sdgo::room_titles::native_transport
