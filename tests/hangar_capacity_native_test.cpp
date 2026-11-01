#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "../mods/hangar_capacity/hangar_capacity.cpp"

namespace fixture {

constexpr DWORD kIncomingError = 0x10203040;
constexpr DWORD kPolicyError = 0x50607080;
constexpr DWORD kNativeError = 0x90A0B0C0;

int checks = 0;
int failures = 0;
int ensureCalls = 0;
int originalCalls = 0;
bool ensureResult = true;
bool ensureThrows = false;
bool originalWrites = true;
int originalResult = 0;
std::int16_t originalExtra = sdgo::hangar_capacity::kTargetExtra;
std::int32_t originalProcedureResult = 0;
void* expectedSelf = reinterpret_cast<void*>(0x11223344);
void* expectedConnection = reinterpret_cast<void*>(0x55667788);
CountArgs* expectedArgs = nullptr;
std::int32_t expectedAccount = 0;
std::vector<int> order;

void Check(bool condition, const char* expression, int line) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL line %d: %s\n", line, expression);
    }
}

#define CHECK(expression) ::fixture::Check(!!(expression), #expression, __LINE__)

template <class T>
T Read(const void* source) {
    T value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

bool Ensure(void* connection, std::int32_t account) {
    ++ensureCalls;
    order.push_back(1);
    CHECK(connection == expectedConnection);
    CHECK(account == expectedAccount);
    SetLastError(kPolicyError);
    if (ensureThrows) throw std::runtime_error("fixture policy failure");
    return ensureResult;
}

int __fastcall Original(void* self, void*, void* connection, CountArgs* args) {
    ++originalCalls;
    order.push_back(2);
    CHECK(self == expectedSelf);
    CHECK(connection == expectedConnection);
    CHECK(args == expectedArgs);
    CHECK(GetLastError() == kIncomingError);
    CHECK(Read<std::int32_t>(args) == expectedAccount);
    if (originalWrites) {
        std::memcpy(reinterpret_cast<BYTE*>(args) + 4, &originalExtra, sizeof(originalExtra));
        std::memcpy(reinterpret_cast<BYTE*>(args) + 6, &originalProcedureResult,
                    sizeof(originalProcedureResult));
    }
    SetLastError(kNativeError);
    return originalResult;
}

void Reset() {
    ensureCalls = 0;
    originalCalls = 0;
    ensureResult = true;
    ensureThrows = false;
    originalWrites = true;
    originalResult = 0;
    originalExtra = sdgo::hangar_capacity::kTargetExtra;
    originalProcedureResult = 0;
    expectedArgs = nullptr;
    expectedAccount = 0;
    order.clear();
    g_original = reinterpret_cast<GetCountFn>(&Original);
    g_ensure = &Ensure;
    g_api = nullptr;
    g_mod = nullptr;
    g_errors.store(0);
    g_announced.store(false);
}

CountArgs MakeArgs(std::int32_t account, std::int16_t extra = -1,
                   std::int32_t procedureResult = -1) {
    CountArgs args{};
    std::memcpy(&args.account, &account, sizeof(account));
    std::memcpy(&args.rawExtra, &extra, sizeof(extra));
    std::memcpy(&args.procedureResult, &procedureResult, sizeof(procedureResult));
    return args;
}

void SuccessPreservesNativeReply() {
    Reset();
    auto args = MakeArgs(71, -7, -9);
    expectedArgs = &args;
    expectedAccount = 71;
    originalExtra = sdgo::hangar_capacity::kTargetExtra;
    originalProcedureResult = 0;
    SetLastError(kIncomingError);

    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, &args) == 0);
    CHECK(ensureCalls == 1);
    CHECK(originalCalls == 1);
    CHECK(order == (std::vector<int>{1, 2}));
    CHECK(args.account == 71);
    CHECK(args.rawExtra == sdgo::hangar_capacity::kTargetExtra);
    CHECK(args.procedureResult == 0);
    CHECK(GetLastError() == kNativeError);
}

void NativeFailurePassesThrough() {
    Reset();
    auto args = MakeArgs(72, 3, 4);
    expectedArgs = &args;
    expectedAccount = 72;
    originalResult = 37;
    originalExtra = 15;
    originalProcedureResult = 37;
    SetLastError(kIncomingError);

    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, &args) == 37);
    CHECK(ensureCalls == 1);
    CHECK(originalCalls == 1);
    CHECK(order == (std::vector<int>{1, 2}));
    CHECK(args.account == 72);
    CHECK(args.rawExtra == 15);
    CHECK(args.procedureResult == 37);
    CHECK(GetLastError() == kNativeError);
}

void StaleNativeSuccessFailsClosedWithoutRewriting() {
    Reset();
    auto args = MakeArgs(73, 8, 9);
    expectedArgs = &args;
    expectedAccount = 73;
    originalExtra = static_cast<std::int16_t>(sdgo::hangar_capacity::kTargetExtra - 1);
    originalProcedureResult = 0;
    SetLastError(kIncomingError);

    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, &args) == 1);
    CHECK(ensureCalls == 1);
    CHECK(originalCalls == 1);
    CHECK(args.account == 73);
    CHECK(args.rawExtra == sdgo::hangar_capacity::kTargetExtra - 1);
    CHECK(args.procedureResult == 0);
    CHECK(GetLastError() == kNativeError);
}

void PolicyFailuresLeaveNativeArgumentsUntouched() {
    for (int throwing = 0; throwing != 2; ++throwing) {
        Reset();
        auto args = MakeArgs(74 + throwing, 0x1234, 0x55667788);
        expectedArgs = &args;
        expectedAccount = args.account;
        ensureResult = false;
        ensureThrows = throwing != 0;
        std::array<BYTE, sizeof(CountArgs)> before{};
        std::memcpy(before.data(), &args, before.size());
        SetLastError(kIncomingError);

        CHECK(GetCount(expectedSelf, nullptr, expectedConnection, &args) == 1);
        CHECK(ensureCalls == 1);
        CHECK(originalCalls == 0);
        CHECK(order == (std::vector<int>{1}));
        CHECK(std::memcmp(before.data(), &args, before.size()) == 0);
        CHECK(GetLastError() == kIncomingError);
    }
}

void InvalidInputsStopBeforePolicy() {
    Reset();
    auto zero = MakeArgs(0, 1, 2);
    SetLastError(kIncomingError);
    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, &zero) == 1);
    CHECK(ensureCalls == 0);
    CHECK(originalCalls == 0);
    CHECK(GetLastError() == kIncomingError);

    Reset();
    auto negative = MakeArgs(-1, 3, 4);
    SetLastError(kIncomingError);
    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, &negative) == 1);
    CHECK(ensureCalls == 0);
    CHECK(originalCalls == 0);
    CHECK(GetLastError() == kIncomingError);

    Reset();
    SetLastError(kIncomingError);
    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, nullptr) == 1);
    CHECK(ensureCalls == 0);
    CHECK(originalCalls == 0);
    CHECK(GetLastError() == kIncomingError);

    Reset();
    auto valid = MakeArgs(76, 5, 6);
    SetLastError(kIncomingError);
    CHECK(GetCount(expectedSelf, nullptr, nullptr, &valid) == 1);
    CHECK(ensureCalls == 0);
    CHECK(originalCalls == 0);
    CHECK(GetLastError() == kIncomingError);
}

void ReadOnlyAndInaccessibleNativeArguments() {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    auto* page = static_cast<BYTE*>(VirtualAlloc(
        nullptr, system.dwPageSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    CHECK(page != nullptr);
    if (!page) return;

    auto args = MakeArgs(77, 0x1234, 0x55667788);
    std::memcpy(page, &args, sizeof(args));
    expectedArgs = reinterpret_cast<CountArgs*>(page);
    expectedAccount = 77;
    ensureResult = false;
    DWORD previous = 0;
    CHECK(VirtualProtect(page, system.dwPageSize, PAGE_READONLY, &previous) != 0);
    SetLastError(kIncomingError);
    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, expectedArgs) == 1);
    CHECK(ensureCalls == 1);
    CHECK(originalCalls == 0);
    CHECK(GetLastError() == kIncomingError);

    DWORD ignored = 0;
    CHECK(VirtualProtect(page, system.dwPageSize, previous, &ignored) != 0);
    CHECK(std::memcmp(page, &args, sizeof(args)) == 0);

    Reset();
    std::memcpy(page, &args, sizeof(args));
    expectedArgs = reinterpret_cast<CountArgs*>(page);
    expectedAccount = 77;
    originalWrites = false;
    originalResult = 19;
    CHECK(VirtualProtect(page, system.dwPageSize, PAGE_READONLY, &previous) != 0);
    SetLastError(kIncomingError);
    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, expectedArgs) == 19);
    CHECK(ensureCalls == 1);
    CHECK(originalCalls == 1);
    CHECK(GetLastError() == kNativeError);
    CHECK(VirtualProtect(page, system.dwPageSize, previous, &ignored) != 0);
    CHECK(std::memcmp(page, &args, sizeof(args)) == 0);

    Reset();
    expectedArgs = reinterpret_cast<CountArgs*>(page);
    CHECK(VirtualProtect(page, system.dwPageSize, PAGE_NOACCESS, &previous) != 0);
    SetLastError(kIncomingError);
    CHECK(GetCount(expectedSelf, nullptr, expectedConnection, expectedArgs) == 1);
    CHECK(ensureCalls == 0);
    CHECK(originalCalls == 0);
    CHECK(GetLastError() == kIncomingError);
    CHECK(VirtualProtect(page, system.dwPageSize, previous, &ignored) != 0);
    CHECK(VirtualFree(page, 0, MEM_RELEASE) != 0);
}

}  // namespace fixture

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    static_assert(sizeof(CountArgs) == 10);
    static_assert(offsetof(CountArgs, account) == 0);
    static_assert(offsetof(CountArgs, rawExtra) == 4);
    static_assert(offsetof(CountArgs, procedureResult) == 6);

    fixture::SuccessPreservesNativeReply();
    fixture::NativeFailurePassesThrough();
    fixture::StaleNativeSuccessFailsClosedWithoutRewriting();
    fixture::PolicyFailuresLeaveNativeArgumentsUntouched();
    fixture::InvalidInputsStopBeforePolicy();
    fixture::Reset();
    fixture::ReadOnlyAndInaccessibleNativeArguments();

    if (fixture::failures) {
        std::printf("HANGAR CAPACITY NATIVE CHECKS FAILED: %d/%d\n",
                    fixture::failures, fixture::checks);
        return 1;
    }
    std::printf(
        "ALL HANGAR CAPACITY NATIVE CHECKS PASSED: %d checks; "
        "production wrapper, no server, network or SQL execution\n",
        fixture::checks);
    return 0;
}
