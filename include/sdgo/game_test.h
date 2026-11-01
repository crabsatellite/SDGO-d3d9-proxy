// Optional in-process test ABI. A provider runs only in an explicitly owned
// GameTest instance, on the host's main thread. No change to the normal Mod ABI.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include "sdgo_api.h"

#define SDGO_GAME_TEST_VERSION 1
#define SDGO_GAME_TEST_PROVIDER_EXPORT "SdgoGameTestGetProviderV1"

enum SdgoGameTestResult {
    SDGO_GAME_TEST_FAIL = -1,
    SDGO_GAME_TEST_WAIT = 0,
    SDGO_GAME_TEST_PASS = 1,
    SDGO_GAME_TEST_SKIP = 2
};

#pragma pack(push, 8)

struct SdgoGameTestContextV1 {
    uint32_t size;
    uint32_t version;
    const SdgoApi* api;
    const char* run_id;
    const char* role;
    const char* case_id;
    const wchar_t* directory; // <isolated exe>/sdgo/gametest
    uint64_t elapsed_ms;
    uint32_t tick;
    // Assertions are persisted immediately. A failed assertion cannot be
    // erased by returning PASS; a case with no assertions cannot pass either.
    void (__cdecl* Assert)(int passed, const char* message);
    void (__cdecl* Observe)(const char* message);
};

typedef int (__cdecl* SdgoGameTestCallbackV1)(const SdgoGameTestContextV1*);
struct SdgoGameTestCaseV1 {
    const char* id;
    uint32_t timeout_ms;
    SdgoGameTestCallbackV1 poll;
};
struct SdgoGameTestProviderV1 {
    uint32_t size;
    uint32_t version;
    const char* suite;
    const char* role;
    uint32_t count;
    const SdgoGameTestCaseV1* cases;
};

#pragma pack(pop)

static_assert(sizeof(void*) == 4, "the native GameTest ABI requires a Win32 Mod");
static_assert(sizeof(SdgoGameTestCaseV1) == 12, "unexpected GameTest case layout");
static_assert(sizeof(SdgoGameTestProviderV1) == 24, "unexpected GameTest provider layout");
static_assert(sizeof(SdgoGameTestContextV1) == 56, "unexpected GameTest context layout");
static_assert(offsetof(SdgoGameTestContextV1, elapsed_ms) == 32, "unexpected GameTest elapsed offset");
static_assert(offsetof(SdgoGameTestContextV1, Assert) == 44, "unexpected GameTest assertion callback offset");
typedef const SdgoGameTestProviderV1* (__cdecl* SdgoGameTestGetProviderV1Fn)();

#define SDGO_GAME_TEST_PROVIDER_ENTRY \
    extern "C" __declspec(dllexport) const SdgoGameTestProviderV1* __cdecl SdgoGameTestGetProviderV1()
