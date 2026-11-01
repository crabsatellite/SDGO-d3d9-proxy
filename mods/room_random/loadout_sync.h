#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Existing client notifications, not a private protocol. The native room
// broadcaster replaces header +2 with each recipient's pilot ID.
namespace room_random_sync {
constexpr size_t kUnitSize=0x99, kShipSize=0x2BC, kPayload=0x12;
constexpr uint16_t kUnitOpcode=0x0861, kShipOpcode=0x098B;
template<size_t N> std::array<uint8_t,kPayload+N> Packet(
    uint16_t opcode,uint32_t owner,uint32_t slot,uint32_t roomIndex,const std::array<uint8_t,N>& data) {
    std::array<uint8_t,kPayload+N> out{};
    std::memcpy(out.data(),&opcode,sizeof(opcode));
    std::memcpy(out.data()+6,&owner,sizeof(owner));
    std::memcpy(out.data()+10,&slot,sizeof(slot));
    std::memcpy(out.data()+14,&roomIndex,sizeof(roomIndex));
    std::memcpy(out.data()+kPayload,data.data(),data.size());
    return out;
}
}
