#pragma once
#include "native_capabilities.h"

namespace battle_ai::native {
struct Shield {
    bool known=false,intact=false;
    float current=0,maximum=0;
    std::int32_t definitionCapacity=0;
    std::uint8_t type=0,mode=0,classes=0;
};
// Native shield direction classes are 1 front, 2 rear, 3/4 lateral. The
// coverage changes with the *committed* weapon slot and current form.
inline std::uint8_t ShieldClasses(std::uint8_t mode) noexcept {
    constexpr std::uint8_t modes[]={0,8,4,12,2,15,1,13,14};
    return mode<sizeof(modes)?modes[mode]:0;
}
inline bool ShieldCovers(const Shield& shield,unsigned directionClass) noexcept {
    return shield.known&&shield.intact&&directionClass>=1&&directionClass<=4&&
        (shield.classes&(1u<<(directionClass-1)))!=0;
}
inline Shield ReadShield(const std::uint8_t* unit) noexcept {
    Shield result;std::uint8_t* definition=nullptr;std::uint8_t* fsm=nullptr;std::int32_t slot=-1;
    if(!detail::ReadPointer(unit,0x9C4,definition)||!detail::ReadPointer(unit,0x9CC,fsm)||
       !detail::Read(fsm,0x240,slot)||slot<0||slot>4||
       !detail::Read(unit,0x2B70,result.current)||!detail::Read(unit,0x2B74,result.maximum)||
       !detail::Read(definition,0x145,result.definitionCapacity)||!detail::Read(definition,0x149,result.type)||
       !detail::Read(definition,0x14E+static_cast<std::size_t>(slot),result.mode)||
       !std::isfinite(result.current)||!std::isfinite(result.maximum)||result.mode>8) return {};
    result.known=true;result.classes=ShieldClasses(result.mode);
    result.intact=result.current>0&&result.maximum>0&&result.definitionCapacity>0&&result.classes!=0;
    return result;
}
}
