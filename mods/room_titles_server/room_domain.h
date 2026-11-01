#pragma once

#include <cstdint>

namespace sdgo {
namespace room_titles_server {

enum class RoomDomain : std::uint8_t {
    Team,
    Battle
};

constexpr std::uint32_t kBattleDomainBit = 0x80000000u;
constexpr std::uint32_t kNativeRoomIdMask = 0x7FFFFFFFu;
constexpr std::uint32_t kNativeRoomPoolLimit = 400u;

constexpr bool IsValidNativeRoomId(std::uint32_t nativeId)
{
    return nativeId != 0 && (nativeId & kBattleDomainBit) == 0 &&
           nativeId <= kNativeRoomPoolLimit;
}

constexpr std::uint32_t EncodeRoomId(RoomDomain domain, std::uint32_t nativeId)
{
    return IsValidNativeRoomId(nativeId) ?
           (domain == RoomDomain::Battle ? nativeId | kBattleDomainBit : nativeId) : 0;
}

constexpr RoomDomain DecodeRoomDomain(std::uint32_t wireId)
{
    return (wireId & kBattleDomainBit) != 0 ? RoomDomain::Battle : RoomDomain::Team;
}

constexpr std::uint32_t DecodeNativeRoomId(std::uint32_t wireId)
{
    return wireId & kNativeRoomIdMask;
}

constexpr bool IsValidWireRoomId(std::uint32_t wireId)
{
    return IsValidNativeRoomId(DecodeNativeRoomId(wireId));
}

constexpr std::uint32_t ResponseRequestId(std::uint32_t actorPilot,
                                          std::uint32_t recipientPilot,
                                          std::uint32_t requestId)
{
    return actorPilot != 0 && actorPilot == recipientPilot ? requestId : 0;
}

} // namespace room_titles_server
} // namespace sdgo
