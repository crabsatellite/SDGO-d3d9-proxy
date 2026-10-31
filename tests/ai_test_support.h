#pragma once

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "sdgo/sdgo_api.h"

// These tests include one production mod per executable. All game calls are
// replaced with fixtures; neither a game image nor an installed client is loaded.
namespace ai_test {
inline int checks = 0;
inline int failures = 0;
inline const char* scenario = "setup";
inline std::vector<std::string> logs;

inline void Check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL [%s] line %d: %s\n", scenario, line, expression);
    }
}
inline void Log(SdgoMod*, int, const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    logs.emplace_back(text);
}
inline SdgoApi api = [] { SdgoApi a{}; a.size = sizeof(a); a.version = SDGO_API_VERSION; a.Log = &Log; return a; }();
template<class T> void Put(uint8_t* bytes, size_t offset, T value) { std::memcpy(bytes + offset, &value, sizeof(value)); }
template<class T> T Get(const uint8_t* bytes, size_t offset) { T value; std::memcpy(&value, bytes + offset, sizeof(value)); return value; }
inline int Finish() {
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
}
#define CHECK(expression) ai_test::Check(!!(expression), #expression, __LINE__)
#define SCENARIO(name) (ai_test::scenario = name)
