#pragma once
#include <string>
#include <vector>
#include "sdgo/random_loadout.h"

namespace room_random_inventory {
struct Settings {
    std::wstring databaseIni;
    std::wstring driver = L"SQL Server";
    unsigned timeoutSeconds = 2;
    unsigned maxRowsPerPlayer = 4096;
};

// One read-only, parameterized query for the room. Never changes GoUserUnitNum,
// deck selections, inventory, experience, energy or any other database column.
bool Read(const Settings& settings, const std::vector<uint32_t>& owners,
          std::vector<sdgo::random_loadout::Candidate>& output, std::string& error);
}
