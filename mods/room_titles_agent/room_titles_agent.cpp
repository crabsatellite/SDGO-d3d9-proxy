// Preserve a validated room-title extension when the native Agent rebuilds a
// create-room request. Other packets retain the native path and payload.
#include <windows.h>

#include <cstdint>
#include <new>
#include <vector>

#include "sdgo/sdgo_api.h"
#include "../room_metadata/common/room_metadata_common.h"

namespace {
namespace metadata = sdgo::room_metadata;
using DispatchFn = int(__cdecl*)(DWORD, DWORD, DWORD, void*, int);
using SendFn = int(__thiscall*)(void*, DWORD, const void*, int, DWORD);
DispatchFn g_dispatch = nullptr;
SendFn g_send = nullptr;
DispatchFn g_serverDispatch = nullptr;
SendFn g_sendClient = nullptr;

constexpr uint16_t kCreateRoom = 0x0640;
constexpr size_t kCreateRoomBytes = 0x53;
constexpr uint16_t kListRooms = 0x07FB;
constexpr size_t kListRoomsBytes = 0x16;

size_t RequestBytes(uint16_t opcode)
{
    switch (opcode) {
    case kCreateRoom: return kCreateRoomBytes;
    case kListRooms: return kListRoomsBytes;
    case 0x0A08: return kListRoomsBytes; // Team / mission lobby list
    case 0x050F: return 0x37; // battle room settings
    case 0x089E: return 0x1D; // battle title change, subtype 1
    case 0x09ED: return 0x3E; // lobby / mission room settings
    default: return 0;
    }
}

struct ForwardContext {
    std::vector<uint8_t> body;
    const void* native_packet = nullptr;
    size_t native_size = 0;
    uint16_t native_opcode = 0;
    bool sending = false;
};
thread_local ForwardContext* g_forward = nullptr;
thread_local ForwardContext* g_reverse = nullptr;

uint16_t Opcode(const void* packet)
{
    const auto* bytes = static_cast<const uint8_t*>(packet);
    return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
}

// Keep SEH cleanup separate from C++ storage. The native dispatcher owns its
// own exception behavior; restoring the thread context must happen on any exit.
int DispatchScoped(DispatchFn original, ForwardContext** active, DWORD argument0, DWORD argument1, DWORD handle,
                   void* data, int size, ForwardContext* context)
{
    ForwardContext* previous = *active;
    *active = context;
    __try {
        return original(argument0, argument1, handle, data, size);
    } __finally {
        *active = previous;
    }
}

int __cdecl Dispatch(DWORD argument0, DWORD argument1, DWORD handle, void* packet, int size)
{
    if (!packet || size < 2)
        return g_dispatch(argument0, argument1, handle, packet, size);
    const uint16_t opcode = Opcode(packet);
    const size_t nativeSize = RequestBytes(opcode);
    if (!nativeSize || size <= static_cast<int>(nativeSize))
        return g_dispatch(argument0, argument1, handle, packet, size);
    try {
        const auto parsed = metadata::ParseTrailer(static_cast<const uint8_t*>(packet), static_cast<size_t>(size));
        if (parsed.status == metadata::TrailerStatus::NoTrailer)
            return g_dispatch(argument0, argument1, handle, packet, size);
        if (parsed.status != metadata::TrailerStatus::Valid || parsed.view.native_size != nativeSize)
            return 0;
        metadata::Message message;
        if (!metadata::DecodeMessageBody(parsed.view.body_data, parsed.view.body_size, &message))
            return 0;
        const bool create = opcode == kCreateRoom && message.kind == metadata::MessageKind::SetTitle &&
                            !message.server_epoch && !message.room_id && !message.generation && !message.revision;
        const bool hello = (opcode == kListRooms || opcode == 0x0A08 || opcode == kCreateRoom) &&
                           message.kind == metadata::MessageKind::Hello;
        const bool titleChange = opcode == 0x050F || opcode == 0x09ED ||
                                 (opcode == 0x089E && (parsed.view.native_data[6] == 1 ||
                                                     parsed.view.native_data[6] == 2));
        const bool update = titleChange && message.kind == metadata::MessageKind::SetTitle &&
                            message.server_epoch && message.room_id && message.generation && message.revision;
        if (!create && !hello && !update)
            return 0;
        ForwardContext context;
        context.body.assign(parsed.view.body_data, parsed.view.body_data + parsed.view.body_size);
        // The Agent writes the authenticated pilot ID into its request; its native
        // helper must receive writable storage, without modifying the caller's frame.
        std::vector<uint8_t> native(parsed.view.native_data, parsed.view.native_data + parsed.view.native_size);
        context.native_packet = native.data();
        context.native_size = native.size();
        context.native_opcode = opcode;
        return DispatchScoped(g_dispatch, &g_forward, argument0, argument1, handle, native.data(),
                               static_cast<int>(native.size()), &context);
    } catch (const std::bad_alloc&) {
        // An extension must fail as a unit, never escape into a foreign CRT or
        // retry the original request with its title silently removed.
        return 0;
    }
}

int SendScoped(SendFn original, ForwardContext* context, void* self, DWORD destination,
               const void* packet, int size, DWORD flags)
{
    const bool previous = context->sending;
    context->sending = true;
    __try {
        return original(self, destination, packet, size, flags);
    } __finally {
        context->sending = previous;
    }
}

int Forward(SendFn original, ForwardContext* context, void* self, DWORD destination,
            const void* packet, int size, DWORD flags)
{
    if (!context || !packet || packet != context->native_packet ||
        size != static_cast<int>(context->native_size) || Opcode(packet) != context->native_opcode)
        return original(self, destination, packet, size, flags);
    // A synchronous re-entry must not send an unextended native packet. A later
    // native retry after either success or failure receives the same extension.
    if (context->sending)
        return 0;
    try {
        std::vector<uint8_t> extended;
        if (!metadata::AppendTrailer(static_cast<const uint8_t*>(packet), static_cast<size_t>(size),
                                     context->body.data(), context->body.size(), &extended))
            return 0;
        return SendScoped(original, context, self, destination, extended.data(), static_cast<int>(extended.size()), flags);
    } catch (const std::bad_alloc&) {
        return 0;
    }
}

int __fastcall Send(void* self, void*, DWORD destination, const void* packet, int size, DWORD flags)
{
    return Forward(g_send, g_forward, self, destination, packet, size, flags);
}

size_t ResponseBytes(uint16_t opcode, const uint8_t* packet, size_t size)
{
    switch (opcode) {
    case 0x054E: case 0x09F4: case 0x07AC: case 0x08FB:
    case 0x0709: case 0x0916: return 8; // native request failures
    case 0x0824:
        return size >= 0x13 && packet[0x12] <= 10 ?
               0x13 + static_cast<size_t>(packet[0x12]) * 0x123 : 0x13;
    case 0x0617: return 0x5B5; // private room-list response
    case 0x0A33: return 0x21FC; // successful create-room response
    case 0x06C1: return 0x21FC; // Team room details
    case 0x0550: return 0x0B;   // Battle room join confirmation
    case 0x0680: return 0x3CE1; // mission Team-to-Battle conversion
    case 0x086F: return 0x3CE1; // PvP Team-to-Battle conversion
    case 0x0902: return 0x3CE2; // battle room settings notification
    case 0x0809: return 6;     // lobby room settings confirmation
    case 0x0717: return 0x13E; // lobby room information notification
    case 0x091F: return 0x13E; // room info / join snapshot
    case 0x06BA: return 0x1C;  // room title notification
    default: return 0;
    }
}

bool ServerBody(const metadata::TrailerView& view)
{
    metadata::Message message;
    if (metadata::DecodeMessageBody(view.body_data, view.body_size, &message))
        return message.kind == metadata::MessageKind::HelloAck || message.kind == metadata::MessageKind::Snapshot ||
               message.kind == metadata::MessageKind::Error;
    metadata::SnapshotBatch batch;
    return metadata::DecodeSnapshotBatchBody(view.body_data, view.body_size, &batch);
}

int __cdecl ServerDispatch(DWORD argument0, DWORD argument1, DWORD handle, void* packet, int size)
{
    if (!packet || size < 2)
        return g_serverDispatch(argument0, argument1, handle, packet, size);
    const uint16_t opcode = Opcode(packet);
    const size_t nativeSize = ResponseBytes(opcode, static_cast<const uint8_t*>(packet), static_cast<size_t>(size));
    if (!nativeSize || size <= static_cast<int>(nativeSize))
        return g_serverDispatch(argument0, argument1, handle, packet, size);
    try {
        const auto parsed = metadata::ParseTrailer(static_cast<const uint8_t*>(packet), static_cast<size_t>(size));
        if (parsed.status == metadata::TrailerStatus::NoTrailer)
            return g_serverDispatch(argument0, argument1, handle, packet, size);
        if (parsed.status != metadata::TrailerStatus::Valid || parsed.view.native_size != nativeSize || !ServerBody(parsed.view))
            return 0;
        ForwardContext context;
        context.body.assign(parsed.view.body_data, parsed.view.body_data + parsed.view.body_size);
        std::vector<uint8_t> native(parsed.view.native_data, parsed.view.native_data + parsed.view.native_size);
        context.native_packet = native.data();
        context.native_size = native.size();
        context.native_opcode = opcode;
        return DispatchScoped(g_serverDispatch, &g_reverse, argument0, argument1, handle, native.data(),
                               static_cast<int>(native.size()), &context);
    } catch (const std::bad_alloc&) {
        return 0;
    }
}

int __fastcall SendClient(void* self, void*, DWORD destination, const void* packet, int size, DWORD flags)
{
    return Forward(g_sendClient, g_reverse, self, destination, packet, size, flags);
}
} // namespace

SDGO_MOD_ENTRY
{
    if (!api->HookCode(mod, "room_titles.agent_client_dispatch", reinterpret_cast<void*>(&Dispatch),
                       reinterpret_cast<void**>(&g_dispatch)) ||
        !api->HookCode(mod, "room_titles.agent_send_server", reinterpret_cast<void*>(&Send),
                       reinterpret_cast<void**>(&g_send)) ||
        !api->HookCode(mod, "room_titles.agent_server_dispatch", reinterpret_cast<void*>(&ServerDispatch),
                       reinterpret_cast<void**>(&g_serverDispatch)) ||
        !api->HookCode(mod, "room_titles.agent_send_client", reinterpret_cast<void*>(&SendClient),
                       reinterpret_cast<void**>(&g_sendClient)))
        return 0;
    api->Log(mod, SDGO_LOG_INFO, "ready: preserve validated room-title metadata on native request and response forwarding");
    return 1;
}
