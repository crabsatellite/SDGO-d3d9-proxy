#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

// Compile the production implementation into this isolated test executable so
// the checks exercise its anonymous-namespace room parser and optional bridge
// directly instead of duplicating either policy in a fixture.
#include "../mods/match_ai/match_ai.cpp"

namespace {

struct FakeRoom {
    alignas(void*) std::uint8_t bytes[0x20]{};
    std::wstring heap;

    void Set(std::wstring value) {
        std::memset(bytes, 0, sizeof(bytes));
        heap = std::move(value);
        *reinterpret_cast<std::uint32_t*>(bytes + 0x10) =
            static_cast<std::uint32_t>(heap.size());
        if (heap.size() < 8) {
            *reinterpret_cast<std::uint32_t*>(bytes + 0x14) = 7;
            std::memcpy(bytes, heap.c_str(),
                        (heap.size() + 1) * sizeof(wchar_t));
        } else {
            *reinterpret_cast<const wchar_t**>(bytes) = heap.c_str();
            *reinterpret_cast<std::uint32_t*>(bytes + 0x14) =
                static_cast<std::uint32_t>(heap.size());
        }
    }
};

std::wstring ExecutableDirectory() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    std::wstring result(path, length);
    const auto separator = result.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
        return {};
    result.resize(separator + 1);
    return result;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::cerr
            << "usage: match_ai_loadout_bridge_test.exe "
               "<battle_ai_bridge_fixture.dll>\n";
        return 2;
    }

    int checks = 0;
    int failures = 0;
    auto check = [&](bool value, const char* label) {
        ++checks;
        std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
        if (!value)
            ++failures;
    };
    auto expectDifficulty = [&](const wchar_t* title, int expected) {
        FakeRoom room;
        room.Set(title);
        check(BattleDifficulty(room.bytes) == expected,
              "production room-title difficulty parser");
    };

    expectDifficulty(L"!ai easy", 0);
    expectDifficulty(L"Duel !AI medium", 1);
    expectDifficulty(L"4v4 !ai hard", 2);
    expectDifficulty(L"normal", 1);

    FakeRoom malformed;
    malformed.Set(L"!ai hard");
    *reinterpret_cast<const wchar_t**>(malformed.bytes) = nullptr;
    check(BattleDifficulty(malformed.bytes) == 1,
          "null heap title defaults to Medium");
    *reinterpret_cast<std::uint32_t*>(malformed.bytes + 0x10) = 21;
    check(BattleDifficulty(malformed.bytes) == 1,
          "oversize title defaults to Medium");
    check(BattleDifficulty(nullptr) == 1,
          "null room defaults to Medium");

    const std::wstring directory = ExecutableDirectory();
    if (directory.empty()) {
        std::cerr << "cannot resolve isolated test directory\n";
        return 2;
    }
    const std::wstring stagedDll = directory + L"battle_ai.dll";
    DeleteFileW(stagedDll.c_str());

    std::uint8_t unit[16]{};
    Bot bot{};
    bot.unit = unit;
    bot.pilotId = 0x1234;
    bot.generation = 7;
    g_roomDifficulty = 2;

    // Nothing named battle_ai.dll is loaded or present yet.
    InitializeBattleAiUnit(bot, true);
    check(g_battleAiModule == nullptr &&
              g_battleAiInitializeUnit == nullptr,
          "absent battle_ai DLL is a no-op");

    if (!CopyFileW(argv[1], stagedDll.c_str(), FALSE)) {
        std::cerr << "cannot stage fixture as battle_ai.dll: "
                  << GetLastError() << '\n';
        return 2;
    }
    HMODULE fixture = LoadLibraryW(stagedDll.c_str());
    check(fixture != nullptr, "isolated fake battle_ai DLL loads");
    if (!fixture) {
        DeleteFileW(stagedDll.c_str());
        return 1;
    }

    using GetIntFn = int(__cdecl*)();
    using GetPointerFn = void*(__cdecl*)();
    auto calls = reinterpret_cast<GetIntFn>(
        GetProcAddress(fixture, "TestCalls"));
    auto difficulty = reinterpret_cast<GetIntFn>(
        GetProcAddress(fixture, "TestDifficulty"));
    auto generation = reinterpret_cast<GetIntFn>(
        GetProcAddress(fixture, "TestGeneration"));
    auto seenUnit = reinterpret_cast<GetPointerFn>(
        GetProcAddress(fixture, "TestUnit"));
    check(calls && difficulty && generation && seenUnit,
          "fixture probe exports resolve");
    if (!calls || !difficulty || !generation || !seenUnit) {
        FreeLibrary(fixture);
        DeleteFileW(stagedDll.c_str());
        return 1;
    }

    InitializeBattleAiUnit(bot, true);
    check(calls() == 1 && difficulty() == 0x102 &&
              generation() == 7 && seenUnit() == unit,
          "initial spawn carries Hard plus private 0x100 flag");

    bot.generation = 8;
    InitializeBattleAiUnit(bot);
    check(calls() == 2 && difficulty() == 2 &&
              generation() == 8 && seenUnit() == unit,
          "respawn carries Hard without the initial-spawn flag");

    FreeLibrary(fixture);
    InitializeBattleAiUnit(bot);
    check(g_battleAiModule == nullptr &&
              g_battleAiInitializeUnit == nullptr,
          "unloaded optional DLL clears the cached bridge");
    DeleteFileW(stagedDll.c_str());

    const std::uint32_t first = NextUnitGeneration();
    const std::uint32_t second = NextUnitGeneration();
    check(first != 0 && second == first + 1,
          "unit generations are monotonic and nonzero");

    std::cout << "MATCH AI LOADOUT BRIDGE CHECKS "
              << (failures ? "FAILED " : "PASSED ")
              << (checks - failures) << '/' << checks << '\n';
    return failures ? 1 : 0;
}
