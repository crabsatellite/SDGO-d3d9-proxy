#pragma once
#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <string>
#include <vector>
#include <set>
#include "sdgo/game_test.h"

namespace sdgo_test {
inline const SdgoApi* api;
inline SdgoMod* mod;
inline HANDLE events = INVALID_HANDLE_VALUE;
inline std::string runId, role, current, suiteId;
inline std::wstring directory;
inline std::string imagePath;
inline std::vector<SdgoGameTestCaseV1> cases;
inline std::vector<std::string> requiredCases;
inline size_t index;
inline uint64_t started;
inline unsigned assertions, failures, ticks;
inline bool caseFailed, finished, discovered;

inline std::string Escape(const char* s) {
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); p && *p; ++p) {
        switch (*p) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (*p < 32) {
                char escaped[8]; sprintf_s(escaped, "\\u%04x", *p); out += escaped;
            } else out += static_cast<char>(*p);
        }
    }
    return out;
}
inline void Emit(const char* event, const char* status, const char* message) {
    std::string line = "{\"run_id\":\"" + Escape(runId.c_str()) + "\",\"role\":\"" + role +
        "\",\"event\":\"" + Escape(event) + "\",\"case\":\"" + Escape(current.c_str()) +
        "\",\"status\":\"" + Escape(status) + "\",\"message\":\"" + Escape(message) +
        "\",\"host_key\":\"" + Escape(api->host_key) + "\",\"pid\":" +
        std::to_string(GetCurrentProcessId()) + ",\"image_path\":\"" + Escape(imagePath.c_str()) + "\",\"duration_ms\":" +
        std::to_string(current.empty() ? 0 : GetTickCount64() - started) + "}\n";
    DWORD written = 0;
    if (events == INVALID_HANDLE_VALUE || !WriteFile(events, line.data(), static_cast<DWORD>(line.size()), &written, nullptr)
        || written != line.size() || !FlushFileBuffers(events)) ExitProcess(3);
}
inline void __cdecl Assert(int pass, const char* message) {
    ++assertions;
    if (!pass) { caseFailed = true; ++failures; }
    Emit("assert", pass ? "pass" : "fail", message);
}
inline void __cdecl Observe(const char* message) { Emit("observation", "info", message); }
inline void Finish(bool passed, const char* why) {
    if (finished) return;
    finished = true;
    Emit("run_end", passed && !failures ? "pass" : "fail", why);
    CloseHandle(events); events = INVALID_HANDLE_VALUE;
    ExitProcess(passed && !failures ? 0 : 1);
}
inline void Fail(const char* why) { Assert(0, why); Finish(false, why); }
inline bool RejectJob(const char* why) {
    api->Log(mod, SDGO_LOG_ERROR, "GameTest refused job: %s", why);
    // An explicitly requested test must never fall through into ordinary game
    // or server startup when its job is malformed or its receipts already exist.
    ExitProcess(3);
}

// Imported mods on an ordinary player's computer remain completely inert.
// Both the launcher environment and a matching local job must be present.
inline bool Open(const SdgoApi* a, SdgoMod* m, const char* expectedRole) {
    api = a; mod = m; role = expectedRole;
    char id[64] = {}, selectedRole[16] = {};
    if (!GetEnvironmentVariableA("SDGO_GAME_TEST_RUN", id, sizeof(id)) ||
        GetEnvironmentVariableA("SDGO_GAME_TEST_ROLE", selectedRole, sizeof(selectedRole)) != role.size() ||
        role != selectedRole) return false;
    runId = id;
    if (runId.size() != 36 || runId.find_first_not_of("0123456789abcdef-") != std::string::npos) return RejectJob("invalid run id");
    wchar_t exe[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return RejectJob("executable path unavailable");
    char utf8[MAX_PATH * 4] = {};
    if (!WideCharToMultiByte(CP_UTF8, 0, exe, -1, utf8, sizeof(utf8), nullptr, nullptr)) return RejectJob("image path encoding failed");
    imagePath = utf8;
    directory = exe;
    directory.resize(directory.find_last_of(L"\\/"));
    directory += L"\\sdgo\\gametest";
    const auto job = directory + L"\\run.ini";
    wchar_t jobId[64] = {}, jobRole[16] = {}, suite[64] = {};
    GetPrivateProfileStringW(L"run", L"id", L"", jobId, 64, job.c_str());
    GetPrivateProfileStringW(L"run", L"role", L"", jobRole, 16, job.c_str());
    GetPrivateProfileStringW(L"run", L"suite", L"", suite, 64, job.c_str());
    std::wstring wideId(runId.begin(), runId.end()), wideRole(role.begin(), role.end());
    if (wideId != jobId || wideRole != jobRole || !suite[0]) return RejectJob("job and launcher disagree");
    for (const wchar_t* p = suite; *p; ++p) {
        if (*p > 127 || !(isalnum(static_cast<unsigned char>(*p)) || *p == L'-' || *p == L'_' || *p == L'.'))
            return RejectJob("invalid suite id");
        suiteId += static_cast<char>(*p);
    }
    wchar_t required[4096] = {};
    GetPrivateProfileStringW(L"run", L"required_cases", L"", required, 4096, job.c_str());
    std::string item;
    for (const wchar_t* p = required; ; ++p) {
        if (*p > 127) return RejectJob("non-ASCII case id");
        if (*p == L',' || !*p) {
            if (!item.empty()) requiredCases.push_back(item);
            item.clear();
            if (!*p) break;
        } else item += static_cast<char>(*p);
    }
    events = CreateFileW((directory + L"\\events.jsonl").c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                         nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (events == INVALID_HANDLE_VALUE) return RejectJob("event file already exists or cannot be created");
    Emit("run_begin", "running", "normal native process; game data and state are observed without mutation");
    return true;
}
inline void Discover() {
    discovered = true;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) Fail("cannot enumerate loaded test providers");
    MODULEENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::set<std::string> ids;
    if (Module32FirstW(snapshot, &entry)) do {
        auto get = reinterpret_cast<SdgoGameTestGetProviderV1Fn>(GetProcAddress(entry.hModule, SDGO_GAME_TEST_PROVIDER_EXPORT));
        if (!get) continue;
        const auto* provider = get();
        if (!provider || provider->size < sizeof(*provider) || provider->version != SDGO_GAME_TEST_VERSION ||
            !provider->role || role != provider->role || !provider->suite ||
            (strcmp(provider->suite, suiteId.c_str()) && strcmp(provider->suite, "*"))) continue;
        if (!provider->cases || provider->count > 512) Fail("invalid provider case array");
        for (unsigned i = 0; i < provider->count; ++i) {
            const auto& test = provider->cases[i];
            if (!test.id || !*test.id || !test.poll || !test.timeout_ms || !ids.insert(test.id).second)
                Fail("invalid or duplicate native case");
            cases.push_back(test);
        }
    } while (Module32NextW(snapshot, &entry));
    CloseHandle(snapshot);
    for (const auto& required : requiredCases)
        if (!ids.count(required)) Fail(("required native case is not registered: " + required).c_str());
    // Preserve each provider's declared sequence: fixture creation may precede
    // spawn/deck assertions. Only the framework bootstrap moves to the front.
    std::stable_partition(cases.begin(), cases.end(), [](const auto& test) {
        return strcmp(test.id, "framework.bootstrap") == 0;
    });
    if (cases.empty()) Fail("no native cases registered for this host");
}
inline int GuardPoll(SdgoGameTestCallbackV1 callback, const SdgoGameTestContextV1* context, DWORD* code) {
    __try { return callback(context); }
    __except(EXCEPTION_EXECUTE_HANDLER) { *code = GetExceptionCode(); return SDGO_GAME_TEST_FAIL; }
}
// Called from a verified main-thread engine boundary, never a worker thread.
inline void Poll() {
    if (finished) return;
    if (!discovered) Discover();
    if (index == cases.size()) Finish(!failures, "all registered native cases completed");
    const auto& test = cases[index];
    if (current.empty()) {
        current = test.id; started = GetTickCount64(); ticks = assertions = 0; caseFailed = false;
        Emit("case_begin", "running", "");
    }
    SdgoGameTestContextV1 context{sizeof(context), SDGO_GAME_TEST_VERSION, api, runId.c_str(), role.c_str(),
        current.c_str(), directory.c_str(), GetTickCount64() - started, ticks++, &Assert, &Observe};
    DWORD exception = 0;
    int result = GuardPoll(test.poll, &context, &exception);
    if (exception) {
        char text[80]; sprintf_s(text, "native exception 0x%08lX; stopping this process", exception);
        Assert(0, text); Emit("case_end", "fail", text); Finish(false, text);
    }
    if (result == SDGO_GAME_TEST_WAIT && context.elapsed_ms < test.timeout_ms) return;
    if (result == SDGO_GAME_TEST_WAIT) Assert(0, "native case timed out");
    if (result == SDGO_GAME_TEST_PASS && !assertions) Assert(0, "case returned PASS without assertions");
    bool pass = result == SDGO_GAME_TEST_PASS && !caseFailed;
    if (!pass && !caseFailed) { caseFailed = true; ++failures; }
    Emit("case_end", pass ? "pass" : result == SDGO_GAME_TEST_SKIP ? "skip" : "fail", "");
    // Providers may share a native fixture across their declared sequence.
    // Continuing after a broken fixture risks operating on corrupted objects.
    if (!pass) Finish(false, "native case did not pass; dependent cases were not executed");
    ++index; current.clear();
}
}
