#include <cstdint>

namespace {

int g_calls = 0;
void* g_unit = nullptr;
int g_difficulty = 0;
std::uint32_t g_generation = 0;

}  // namespace

extern "C" __declspec(dllexport) void __cdecl
SdgoBattleAiInitializeUnit(std::uint8_t* unit, int roomDifficulty,
                           std::uint32_t generation) {
    ++g_calls;
    g_unit = unit;
    g_difficulty = roomDifficulty;
    g_generation = generation;
}

extern "C" __declspec(dllexport) int __cdecl TestCalls() {
    return g_calls;
}

extern "C" __declspec(dllexport) int __cdecl TestDifficulty() {
    return g_difficulty;
}

extern "C" __declspec(dllexport) int __cdecl TestGeneration() {
    return static_cast<int>(g_generation);
}

extern "C" __declspec(dllexport) void* __cdecl TestUnit() {
    return g_unit;
}
