#include <cstdio>
#include "sdgo/reward_window.h"

namespace {
    int failures = 0;
    void Check(bool result, const char* label) {
        std::printf("%s %s\n", result ? "ok  " : "FAIL", label);
        if (!result) ++failures;
    }
}
int main() {
    using namespace sdgo::rewards;
    const Window normal;
    Check(normal.Valid(), "default daily window is valid");
    Check(!normal.Contains(17 * 60 + 59), "17:59 is outside the event");
    Check(normal.Contains(18 * 60), "18:00 starts the event inclusively");
    Check(normal.Contains(21 * 60 + 59), "21:59 remains inside the event");
    Check(!normal.Contains(22 * 60), "22:00 ends the event exclusively");
    Check(!normal.Contains(0) && !normal.Contains(24 * 60), "midnight and invalid minutes are outside the default event");
    const Window overnight{23 * 60 + 30, 75};
    Check(overnight.Contains(23 * 60 + 30) && overnight.Contains(0) && overnight.Contains(74), "an owner-configured window can cross midnight");
    Check(!overnight.Contains(75) && !overnight.Contains(12 * 60), "overnight end and daytime are outside");
    Check(!Window{600, 600}.Valid() && !Window{1440, 0}.Valid(), "empty and invalid windows are rejected");
    unsigned minute = 17;
    Check(ParseTime("18:30", minute) && minute == 1110, "HH:MM configuration supports minute precision");
    for (auto bad : {"24:00", "18:60", "8:00", "18:00:00", "aa:00", ""}) {
        minute = 17;
        Check(!ParseTime(bad, minute) && minute == 17, "invalid configuration is rejected without partial output");
    }
    Check(DoubleBounded(0, 65535) == 0 && DoubleBounded(123, 65535) == 246, "zero and ordinary rewards double exactly");
    Check(DoubleBounded(32767, 65535) == 65534 && DoubleBounded(32768, 65535) == 65535, "16-bit reward overflow saturates without wraparound");
    Check(DoubleBounded(0x7fffffff, 0xffffffff) == 0xfffffffe && DoubleBounded(0x80000000, 0xffffffff) == 0xffffffff, "32-bit reward overflow saturates without wraparound");
    return failures ? 1 : 0;
}
