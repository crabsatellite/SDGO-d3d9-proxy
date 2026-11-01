#pragma once
#include <cstdint>
#include <cstring>

namespace sdgo::inventory {
// CGameApp::SetState (0048E6D0): +4A0 is the discriminant; +4A4 is
// a DIFFERENT service class for each state. Lobby/room services may be only
// four/eight bytes. Their +0C is not a game-mode pointer and must not be read.
inline bool IsMenuState(uint32_t state) {
    return (state>=5 && state<=20) || (state>=26 && state<=29);
}
inline uint32_t AppState(const void* app) {
    uint32_t state=UINT32_MAX;
    if(app)memcpy(&state,static_cast<const uint8_t*>(app)+0x4A0,sizeof(state));
    return state;
}
inline bool MenuAvailable(const void* app){return IsMenuState(AppState(app));}
}
