#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sdgo/sdgo_api.h"

namespace sdgo::room_titles::native_transport {
enum class SendDecision { Pass, Replace, Reject };

struct Callbacks {
    SendDecision (*before_send)(uint32_t connection, const uint8_t* packet, size_t size,
                                uint32_t flags, std::vector<uint8_t>* replacement) = nullptr;
    void (*after_send)(uint32_t connection, bool queued, const uint8_t* packet, size_t size, int result) = nullptr;
    // Zero means this opcode is unrelated and must retain the native path.
    size_t (*native_receive_size)(uint16_t opcode, const uint8_t* packet, size_t size) = nullptr;
    void (*on_receive)(uint32_t sourceHandle, const uint8_t* nativePacket, size_t nativeSize,
                        const uint8_t* metadata, size_t metadataSize) = nullptr;
};

// Hooks distinct from net_probe's outer net.send_packet. No socket imports,
// endpoint changes, encryption implementation, or game-specific room cache.
bool Install(const SdgoApi* api, SdgoMod* mod, const Callbacks& callbacks);
} // namespace sdgo::room_titles::native_transport
