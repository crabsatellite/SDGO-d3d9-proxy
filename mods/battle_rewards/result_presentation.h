#pragma once
#include <windows.h>
#include <cstdint>
#include "native_sites.h"

namespace sdgo::battle_rewards {
inline bool WriteNativeGResult(BYTE* frame, unsigned site, uint16_t amount) {
    if (!frame || site >= kSettlementSites.size()) return false;
    const auto& fields = kSettlementSites[site];
    __try {
        *reinterpret_cast<volatile uint16_t*>(frame + fields.g_amount) = amount;
        // Native UI labels describe the old reward tables, not contribution
        // policy. Present the complete settled amount in its base row and clear
        // discarded legacy GB rows so animation and skip both show that amount.
        *reinterpret_cast<volatile uint16_t*>(frame + fields.g_display_base) = amount;
        *reinterpret_cast<volatile uint16_t*>(frame + fields.g_event) = 0;
        for (ptrdiff_t offset : fields.g_display_bonuses)
            *reinterpret_cast<volatile uint16_t*>(frame + offset) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}
