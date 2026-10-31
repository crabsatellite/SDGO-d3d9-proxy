#pragma once

namespace sdgo::ai {
// Native grid-match totals for 1..6 players per team. The HUD accepts one
// shared full-bar value, including when an AI room has unequal team sizes.
inline int GridGauge(int perTeam)
{
    switch (perTeam) {
    case 1: case 2: case 4: return 0x50;
    case 3: return 0x3C;
    case 5: return 0x78;
    case 6: return 0xA0;
    default: return 0;
    }
}

inline int GridGaugeForTeams(int first, int second)
{
    return GridGauge(first > second ? first : second);
}
}
