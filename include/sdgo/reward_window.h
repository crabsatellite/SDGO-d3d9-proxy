#pragma once

#include <cstdint>
#include <initializer_list>
#include <string_view>

namespace sdgo::rewards {
    struct Window {
        unsigned start = 18 * 60;
        unsigned end = 22 * 60;

        constexpr bool Valid() const {
            return start < 24 * 60 && end < 24 * 60 && start != end;
        }
        constexpr bool Contains(unsigned localMinute) const {
            if (!Valid() || localMinute >= 24 * 60) return false;
            return start < end ? localMinute >= start && localMinute < end
                               : localMinute >= start || localMinute < end;
        }
    };

    inline bool ParseTime(std::string_view text, unsigned& minute) {
        if (text.size() != 5 || text[2] != ':') return false;
        for (unsigned i : {0u, 1u, 3u, 4u})
            if (text[i] < '0' || text[i] > '9') return false;
        const unsigned hour = (text[0] - '0') * 10 + text[1] - '0';
        const unsigned withinHour = (text[3] - '0') * 10 + text[4] - '0';
        if (hour >= 24 || withinHour >= 60) return false;
        minute = hour * 60 + withinHour;
        return true;
    }

    constexpr uint32_t DoubleBounded(uint32_t original, uint32_t maximum) {
        return original > maximum / 2 ? maximum : original * 2;
    }
}
