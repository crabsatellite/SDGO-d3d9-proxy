#pragma once
#include <initializer_list>
#include "native_capabilities.h"

namespace battle_ai::native {
struct DamageProtection {
    bool known=false;
    bool active=false;
};

// Pure observations of the supported client's authoritative hurt state and
// movement FSM. Do not call CheckHurt: that API consumes the damage latch.
inline DamageProtection ReadDamageProtection(const std::uint8_t* unit) noexcept {
    std::uint8_t invulnerable=0;
    std::uint32_t hurtState=0;
    if(!detail::Read(unit,0xEB4,invulnerable)||!detail::Read(unit,0x9D8,hurtState)) return {};
    if(invulnerable||hurtState==2) return {true,true};
    std::uint8_t* movement=nullptr;
    std::uint32_t current=0,pending=0;
    if(!detail::ReadPointer(unit,0x9C8,movement)||
       !detail::Read(movement,4,current)||!detail::Read(movement,8,pending)) return {};
    for(std::size_t offset:{std::size_t(0xFC),std::size_t(0x110)})
        if(detail::StateIs(movement,current,offset)||detail::StateIs(movement,pending,offset)) return {true,true};
    // Movement +E8 is ordinary hit-stun and deliberately remains damageable.
    return {true,false};
}
}
