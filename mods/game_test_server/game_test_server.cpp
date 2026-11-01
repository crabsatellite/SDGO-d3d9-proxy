// Passive receipt writer for real PvP AI battles.
//
// This Mod never replaces BattleServer's main function and never creates rooms,
// pilots or packets. A separately driven, normally authenticated client creates
// and starts the room. pvp_ai snapshots its existing real hook boundaries; this
// observer correlates those snapshots and writes durable test receipts.

#include <windows.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sdgo/pvp_ai_observer.h"
#include "sdgo/sdgo_api.h"

namespace
{
    constexpr unsigned kQueueCapacity = 256;
    constexpr unsigned kDrainBatch = 64;

    struct RunConfig
    {
        char runId[37] = {};
        char suite[32] = {};
        char nonce[9] = {};
        uint32_t hostPilotId = 0;
        uint8_t mode = 0;
        uint8_t minimumAi = 1;
        bool requireEnd = false;
    };

    struct Lifecycle
    {
        enum Outcome { Running, Pass, Fail } outcome = Running;
        uint32_t roomCookie = 0;
        uint32_t hostPilotId = 0;
        uint8_t mode = 0;
        unsigned phase = 0;
        uint64_t eventCount = 0;
        std::string reason = "waiting for authenticated host";
    };

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    RunConfig g_config;
    std::wstring g_directory;
    std::wstring g_runIni;
    std::wstring g_expectIni;
    std::wstring g_statusPath;
    HANDLE g_eventFile = INVALID_HANDLE_VALUE;
    HANDLE g_startGate = nullptr;
    CRITICAL_SECTION g_queueLock;
    SdgoPvpAiObserverEventV1 g_queue[kQueueCapacity] = {};
    unsigned g_queueRead = 0;
    unsigned g_queueWrite = 0;
    unsigned g_queueCount = 0;
    volatile LONG g_expectedHost = 0;
    volatile LONG g_dropped = 0;
    volatile LONG g_abortWorker = 0;
    volatile LONG g_writeErrorLogged = 0;

    std::string EscapeJson(const char* text)
    {
        std::string output;
        if (!text)
            return output;
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
            switch (*p) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (*p < 0x20) {
                    char escaped[8] = {};
                    sprintf_s(escaped, "\\u%04x", static_cast<unsigned>(*p));
                    output += escaped;
                } else {
                    output += static_cast<char>(*p);
                }
            }
        }
        return output;
    }

    bool IsUuid(const char* value)
    {
        if (!value || strlen(value) != 36)
            return false;
        for (size_t i = 0; i < 36; ++i) {
            if (i == 8 || i == 13 || i == 18 || i == 23) {
                if (value[i] != '-')
                    return false;
            } else if (!((value[i] >= '0' && value[i] <= '9') ||
                         (value[i] >= 'a' && value[i] <= 'f'))) {
                return false;
            }
        }
        return true;
    }

    bool ReadAscii(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                   char* output, size_t capacity)
    {
        if (!output || capacity == 0)
            return false;
        wchar_t wide[128] = {};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", wide, static_cast<DWORD>(_countof(wide)), path.c_str());
        if (length == 0 || static_cast<size_t>(length) >= capacity)
            return false;
        for (DWORD i = 0; i < length; ++i) {
            if (wide[i] > 0x7f)
                return false;
            output[i] = static_cast<char>(wide[i]);
        }
        output[length] = '\0';
        return true;
    }

    enum class NumberResult { Missing, Valid, Invalid };

    NumberResult ReadNumber(
        const std::wstring& path, const wchar_t* section, const wchar_t* key, uint32_t* value)
    {
        wchar_t text[64] = {};
        const DWORD length = GetPrivateProfileStringW(
            section, key, L"", text, static_cast<DWORD>(_countof(text)), path.c_str());
        if (length == 0)
            return NumberResult::Missing;
        for (DWORD i = 0; i < length; ++i)
            if (text[i] < L'0' || text[i] > L'9')
                return NumberResult::Invalid;
        errno = 0;
        wchar_t* end = nullptr;
        const unsigned long parsed = wcstoul(text, &end, 10);
        if (errno == ERANGE || !end || *end != L'\0')
            return NumberResult::Invalid;
        *value = static_cast<uint32_t>(parsed);
        return NumberResult::Valid;
    }

    bool ReadConfig(std::string* error)
    {
        char environmentRun[64] = {};
        char environmentRole[16] = {};
        const DWORD runLength = GetEnvironmentVariableA(
            "SDGO_GAME_TEST_RUN", environmentRun, static_cast<DWORD>(sizeof(environmentRun)));
        const DWORD roleLength = GetEnvironmentVariableA(
            "SDGO_GAME_TEST_ROLE", environmentRole, static_cast<DWORD>(sizeof(environmentRole)));
        if (runLength == 0 && roleLength == 0)
            return false;
        if (runLength == 0 || runLength >= sizeof(environmentRun) ||
            roleLength == 0 || roleLength >= sizeof(environmentRole) ||
            strcmp(environmentRole, "server") != 0 || !IsUuid(environmentRun)) {
            *error = "launcher environment is invalid";
            return false;
        }

        wchar_t executable[32768] = {};
        const DWORD executableLength = GetModuleFileNameW(
            nullptr, executable, static_cast<DWORD>(_countof(executable)));
        if (executableLength == 0 || executableLength >= _countof(executable)) {
            *error = "BattleServer image path is unavailable";
            return false;
        }
        g_directory.assign(executable, executableLength);
        const size_t slash = g_directory.find_last_of(L"\\/");
        if (slash == std::wstring::npos) {
            *error = "BattleServer image path has no directory";
            return false;
        }
        g_directory.resize(slash);
        g_directory += L"\\sdgo\\gametest";
        g_runIni = g_directory + L"\\run.ini";
        g_expectIni = g_directory + L"\\expect.ini";
        g_statusPath = g_directory + L"\\server-observer-result.json";

        char configuredRun[64] = {};
        char configuredRole[16] = {};
        if (!ReadAscii(g_runIni, L"run", L"id", configuredRun, sizeof(configuredRun)) ||
            !ReadAscii(g_runIni, L"run", L"role", configuredRole, sizeof(configuredRole)) ||
            !ReadAscii(g_runIni, L"run", L"suite", g_config.suite, sizeof(g_config.suite)) ||
            strcmp(configuredRun, environmentRun) != 0 ||
            strcmp(configuredRole, "server") != 0 ||
            (strcmp(g_config.suite, "pvp-ai-real") != 0 &&
             strcmp(g_config.suite, "pvp-ai") != 0)) {
            *error = "run.ini and launcher do not describe the same supported server job";
            return false;
        }
        strcpy_s(g_config.runId, environmentRun);
        if (!ReadAscii(g_runIni, L"observer", L"nonce", g_config.nonce, sizeof(g_config.nonce))) {
            *error = "observer nonce is missing or too long";
            return false;
        }
        const size_t nonceLength = strlen(g_config.nonce);
        if (nonceLength < 3 || nonceLength > 8) {
            *error = "observer nonce must contain 3 to 8 ASCII letters or digits";
            return false;
        }
        for (size_t i = 0; i < nonceLength; ++i)
            if (!((g_config.nonce[i] >= 'a' && g_config.nonce[i] <= 'z') ||
                  (g_config.nonce[i] >= 'A' && g_config.nonce[i] <= 'Z') ||
                  (g_config.nonce[i] >= '0' && g_config.nonce[i] <= '9'))) {
                *error = "observer nonce must contain 3 to 8 ASCII letters or digits";
                return false;
            }

        uint32_t number = 0;
        NumberResult result = ReadNumber(
            g_runIni, L"observer", L"host_pilot_id", &number);
        if (result == NumberResult::Invalid) {
            *error = "observer host_pilot_id is invalid";
            return false;
        }
        if (result == NumberResult::Valid)
            g_config.hostPilotId = number;

        number = 0;
        result = ReadNumber(g_runIni, L"observer", L"mode", &number);
        if (result == NumberResult::Invalid || number > 255) {
            *error = "observer mode is invalid";
            return false;
        }
        if (result == NumberResult::Valid)
            g_config.mode = static_cast<uint8_t>(number);

        number = 1;
        result = ReadNumber(g_runIni, L"observer", L"minimum_ai", &number);
        if (result == NumberResult::Invalid || number == 0 || number > 11) {
            *error = "observer minimum_ai must be between 1 and 11";
            return false;
        }
        if (result == NumberResult::Valid)
            g_config.minimumAi = static_cast<uint8_t>(number);

        number = 0;
        result = ReadNumber(g_runIni, L"observer", L"require_end", &number);
        if (result == NumberResult::Invalid || number > 1) {
            *error = "observer require_end must be 0 or 1";
            return false;
        }
        if (result == NumberResult::Valid)
            g_config.requireEnd = number != 0;
        return true;
    }

    uint32_t ExpectedHost()
    {
        return static_cast<uint32_t>(
            InterlockedCompareExchange(&g_expectedHost, 0, 0));
    }

    bool TitleContainsNonce(const char title[21])
    {
        const size_t nonceLength = strlen(g_config.nonce);
        for (size_t offset = 0; offset + nonceLength <= 20; ++offset) {
            bool equal = true;
            for (size_t i = 0; i < nonceLength; ++i) {
                if (title[offset + i] == '\0' || title[offset + i] != g_config.nonce[i]) {
                    equal = false;
                    break;
                }
            }
            if (equal)
                return true;
            if (title[offset] == '\0')
                break;
        }
        return false;
    }

    void __cdecl ObservePvpAi(const SdgoPvpAiObserverEventV1* event, void*)
    {
        if (!event || event->size < sizeof(*event) ||
            event->version != SDGO_PVP_AI_OBSERVER_VERSION)
            return;
        const uint32_t expectedHost = ExpectedHost();
        if (expectedHost == 0 || event->host_pilot_id != expectedHost ||
            (g_config.mode != 0 && event->mode != g_config.mode) ||
            !TitleContainsNonce(event->title))
            return;

        EnterCriticalSection(&g_queueLock);
        if (g_queueCount == kQueueCapacity) {
            InterlockedIncrement(&g_dropped);
        } else {
            SdgoPvpAiObserverEventV1 copy = *event;
            copy.title[sizeof(copy.title) - 1] = '\0';
            g_queue[g_queueWrite] = copy;
            g_queueWrite = (g_queueWrite + 1) % kQueueCapacity;
            ++g_queueCount;
        }
        LeaveCriticalSection(&g_queueLock);
    }

    unsigned Drain(SdgoPvpAiObserverEventV1* output)
    {
        EnterCriticalSection(&g_queueLock);
        unsigned count = 0;
        while (count < kDrainBatch && g_queueCount != 0) {
            output[count++] = g_queue[g_queueRead];
            g_queueRead = (g_queueRead + 1) % kQueueCapacity;
            --g_queueCount;
        }
        LeaveCriticalSection(&g_queueLock);
        return count;
    }

    bool WriteReceipt(const std::string& line)
    {
        DWORD written = 0;
        const bool ok = g_eventFile != INVALID_HANDLE_VALUE &&
            line.size() <= MAXDWORD &&
            WriteFile(g_eventFile, line.data(), static_cast<DWORD>(line.size()),
                      &written, nullptr) != 0 &&
            written == line.size() &&
            FlushFileBuffers(g_eventFile) != 0;
        if (!ok && InterlockedCompareExchange(&g_writeErrorLogged, 1, 0) == 0)
            g_api->Log(g_mod, SDGO_LOG_ERROR, "GameTest server observer could not flush its JSONL receipt");
        return ok;
    }

    const char* EventName(uint32_t type)
    {
        switch (type) {
        case SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED: return "request_accepted";
        case SDGO_PVP_AI_EVENT_SETUP_COMPLETE: return "setup_complete";
        case SDGO_PVP_AI_EVENT_BATTLE_ACTIVE: return "battle_active";
        case SDGO_PVP_AI_EVENT_BATTLE_END: return "battle_end";
        case SDGO_PVP_AI_EVENT_SETUP_FAILED: return "setup_failed";
        default: return "unknown";
        }
    }

    std::string EventJson(const SdgoPvpAiObserverEventV1& event)
    {
        std::string line =
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" + EscapeJson(g_config.suite) +
            "\",\"event\":\"" + EventName(event.type) +
            "\",\"status_code\":" + std::to_string(event.status) +
            ",\"sequence\":" + std::to_string(event.sequence) +
            ",\"qpc\":" + std::to_string(event.qpc) +
            ",\"thread_id\":" + std::to_string(event.thread_id) +
            ",\"room_cookie\":" + std::to_string(event.room_cookie) +
            ",\"host_pilot_id\":" + std::to_string(event.host_pilot_id) +
            ",\"room_type\":" + std::to_string(event.room_type) +
            ",\"mode\":" + std::to_string(event.mode) +
            ",\"title\":\"" + EscapeJson(event.title) +
            "\",\"humans_team_1\":" + std::to_string(event.humans_team_1) +
            ",\"humans_team_2\":" + std::to_string(event.humans_team_2) +
            ",\"ai_team_1\":" + std::to_string(event.ai_team_1) +
            ",\"ai_team_2\":" + std::to_string(event.ai_team_2) +
            ",\"active_slots\":" + std::to_string(event.active_slots) +
            ",\"score_team_1\":" + std::to_string(event.score_team_1) +
            ",\"score_team_2\":" + std::to_string(event.score_team_2) +
            ",\"slots\":[";
        bool first = true;
        for (const auto& slot : event.slots) {
            if (slot.pilot_id == 0)
                continue;
            if (!first)
                line += ',';
            first = false;
            line += "{\"slot\":" + std::to_string(slot.slot) +
                ",\"pilot_id\":" + std::to_string(slot.pilot_id) +
                ",\"unit_or_npc_id\":" + std::to_string(slot.unit_or_npc_id) +
                ",\"team\":" + std::to_string(slot.team) +
                ",\"colour\":" + std::to_string(slot.colour) +
                ",\"is_ai\":" + std::string(slot.is_ai ? "true" : "false") + '}';
        }
        line += "]}\n";
        return line;
    }

    const char* OutcomeName(Lifecycle::Outcome outcome)
    {
        switch (outcome) {
        case Lifecycle::Pass: return "pass";
        case Lifecycle::Fail: return "fail";
        default: return "running";
        }
    }

    bool WriteStatus(const Lifecycle& lifecycle)
    {
        const std::wstring temporary = g_statusPath + L".tmp";
        const std::string body =
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" + EscapeJson(g_config.suite) +
            "\",\"status\":\"" + OutcomeName(lifecycle.outcome) +
            "\",\"reason\":\"" + EscapeJson(lifecycle.reason.c_str()) +
            "\",\"normal_server_main\":true,\"expected_host_pilot_id\":" +
            std::to_string(ExpectedHost()) +
            ",\"room_cookie\":" + std::to_string(lifecycle.roomCookie) +
            ",\"mode\":" + std::to_string(lifecycle.mode) +
            ",\"phase\":" + std::to_string(lifecycle.phase) +
            ",\"events\":" + std::to_string(lifecycle.eventCount) +
            ",\"dropped\":" +
            std::to_string(static_cast<unsigned long>(
                InterlockedCompareExchange(&g_dropped, 0, 0))) +
            "}\n";
        HANDLE file = CreateFileW(
            temporary.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        const bool wrote = body.size() <= MAXDWORD &&
            WriteFile(file, body.data(), static_cast<DWORD>(body.size()),
                      &written, nullptr) != 0 &&
            written == body.size() &&
            FlushFileBuffers(file) != 0;
        const bool closed = CloseHandle(file) != 0;
        if (!wrote || !closed)
            return false;
        return MoveFileExW(
            temporary.c_str(), g_statusPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }

    void Fail(Lifecycle* lifecycle, const char* reason)
    {
        if (lifecycle->outcome == Lifecycle::Fail)
            return;
        lifecycle->outcome = Lifecycle::Fail;
        lifecycle->reason = reason;
    }

    bool SameBattle(const Lifecycle& lifecycle, const SdgoPvpAiObserverEventV1& event)
    {
        return lifecycle.roomCookie == event.room_cookie &&
            lifecycle.hostPilotId == event.host_pilot_id &&
            lifecycle.mode == event.mode;
    }

    void ProcessEvent(Lifecycle* lifecycle, const SdgoPvpAiObserverEventV1& event)
    {
        ++lifecycle->eventCount;
        if (lifecycle->outcome == Lifecycle::Fail)
            return;
        if (lifecycle->outcome == Lifecycle::Pass) {
            if (event.type == SDGO_PVP_AI_EVENT_BATTLE_END &&
                lifecycle->phase == 3 && SameBattle(*lifecycle, event)) {
                lifecycle->phase = 4;
                lifecycle->reason =
                    "real battle completed after verified PvP AI activity";
            }
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_SETUP_FAILED) {
            lifecycle->roomCookie = event.room_cookie;
            lifecycle->hostPilotId = event.host_pilot_id;
            lifecycle->mode = event.mode;
            Fail(lifecycle, "PvP AI setup raised a native exception");
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_REQUEST_ACCEPTED) {
            if (lifecycle->phase == 0) {
                lifecycle->roomCookie = event.room_cookie;
                lifecycle->hostPilotId = event.host_pilot_id;
                lifecycle->mode = event.mode;
                lifecycle->phase = 1;
                lifecycle->reason = "matching real battle request accepted";
            } else if (!SameBattle(*lifecycle, event)) {
                Fail(lifecycle, "multiple matching battle rooms made the receipt ambiguous");
            }
            return;
        }
        if (lifecycle->phase == 0 || !SameBattle(*lifecycle, event)) {
            Fail(lifecycle, "observer lifecycle event arrived without its matching request");
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_SETUP_COMPLETE) {
            if (lifecycle->phase != 1) {
                Fail(lifecycle, "PvP AI setup completed out of order");
                return;
            }
            const unsigned aiCount =
                static_cast<unsigned>(event.ai_team_1) + event.ai_team_2;
            if (aiCount < g_config.minimumAi) {
                Fail(lifecycle, "PvP AI setup produced fewer AI pilots than required");
                return;
            }
            lifecycle->phase = 2;
            lifecycle->reason = "matching real room contains the required AI pilots";
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_BATTLE_ACTIVE) {
            if (lifecycle->phase != 2) {
                Fail(lifecycle, "battle became active before verified PvP AI setup");
                return;
            }
            lifecycle->phase = 3;
            lifecycle->reason = "real battle tick observed with AI pilots active";
            if (!g_config.requireEnd)
                lifecycle->outcome = Lifecycle::Pass;
            return;
        }
        if (event.type == SDGO_PVP_AI_EVENT_BATTLE_END) {
            if (lifecycle->phase != 3) {
                Fail(lifecycle, "battle end arrived before an active battle tick");
                return;
            }
            lifecycle->phase = 4;
            lifecycle->reason = "real battle completed after verified PvP AI activity";
            lifecycle->outcome = Lifecycle::Pass;
        }
    }

    bool RefreshExpectedHost(uint32_t* host, std::string* error)
    {
        uint32_t selected = g_config.hostPilotId;
        uint32_t fromFile = 0;
        const NumberResult result = ReadNumber(
            g_expectIni, L"observer", L"host_pilot_id", &fromFile);
        if (result == NumberResult::Invalid) {
            *error = "expect.ini observer host_pilot_id is invalid";
            return false;
        }
        if (result == NumberResult::Valid)
            selected = fromFile;
        if (*host != selected) {
            *host = selected;
            InterlockedExchange(&g_expectedHost, static_cast<LONG>(selected));
        }
        return true;
    }

    DWORD WINAPI Worker(void*)
    {
        WaitForSingleObject(g_startGate, INFINITE);
        if (InterlockedCompareExchange(&g_abortWorker, 0, 0) != 0) {
            CloseHandle(g_eventFile);
            g_eventFile = INVALID_HANDLE_VALUE;
            return 0;
        }

        Lifecycle lifecycle;
        uint32_t selectedHost = UINT32_MAX;
        std::string hostError;
        if (!RefreshExpectedHost(&selectedHost, &hostError))
            Fail(&lifecycle, hostError.c_str());
        else if (selectedHost != 0)
            lifecycle.reason = "waiting for matching room request";

        const std::string ready =
            "{\"run_id\":\"" + EscapeJson(g_config.runId) +
            "\",\"role\":\"server\",\"suite\":\"" + EscapeJson(g_config.suite) +
            "\",\"event\":\"observer_ready\",\"status\":\"running\"" +
            ",\"normal_server_main\":true,\"pid\":" +
            std::to_string(GetCurrentProcessId()) +
            ",\"host_key\":\"" + EscapeJson(g_api->host_key) +
            "\",\"nonce\":\"" + EscapeJson(g_config.nonce) +
            "\",\"expected_host_pilot_id\":" + std::to_string(ExpectedHost()) +
            ",\"mode\":" + std::to_string(g_config.mode) +
            ",\"minimum_ai\":" + std::to_string(g_config.minimumAi) +
            ",\"require_end\":" + (g_config.requireEnd ? "true" : "false") +
            "}\n";
        if (!WriteReceipt(ready))
            Fail(&lifecycle, "observer_ready receipt could not be flushed");
        if (!WriteStatus(lifecycle))
            g_api->Log(g_mod, SDGO_LOG_ERROR, "GameTest server observer could not publish its status file");

        ULONGLONG nextHostRefresh = 0;
        for (;;) {
            bool statusChanged = false;
            const ULONGLONG now = GetTickCount64();
            if (now >= nextHostRefresh) {
                nextHostRefresh = now + 250;
                const uint32_t previousHost = selectedHost;
                hostError.clear();
                if (!RefreshExpectedHost(&selectedHost, &hostError)) {
                    Fail(&lifecycle, hostError.c_str());
                    statusChanged = true;
                } else if (selectedHost != previousHost) {
                    if (lifecycle.phase != 0)
                        Fail(&lifecycle, "expected host changed after battle correlation began");
                    else
                        lifecycle.reason = selectedHost == 0
                            ? "waiting for authenticated host"
                            : "waiting for matching room request";
                    statusChanged = true;
                }
            }

            if (InterlockedCompareExchange(&g_dropped, 0, 0) != 0 &&
                lifecycle.outcome != Lifecycle::Fail) {
                Fail(&lifecycle, "observer queue overflowed; lifecycle evidence was lost");
                statusChanged = true;
            }

            SdgoPvpAiObserverEventV1 batch[kDrainBatch] = {};
            unsigned count = 0;
            while ((count = Drain(batch)) != 0) {
                for (unsigned i = 0; i < count; ++i) {
                    if (!WriteReceipt(EventJson(batch[i]))) {
                        Fail(&lifecycle, "observer event receipt could not be flushed");
                        statusChanged = true;
                        continue;
                    }
                    const Lifecycle before = lifecycle;
                    ProcessEvent(&lifecycle, batch[i]);
                    if (before.outcome != lifecycle.outcome ||
                        before.phase != lifecycle.phase ||
                        before.eventCount != lifecycle.eventCount ||
                        before.reason != lifecycle.reason)
                        statusChanged = true;
                }
            }
            if (statusChanged && !WriteStatus(lifecycle))
                g_api->Log(g_mod, SDGO_LOG_ERROR, "GameTest server observer could not update its status file");
            Sleep(25);
        }
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;

    std::string error;
    if (!ReadConfig(&error)) {
        if (!error.empty())
            api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: %s", error.c_str());
        return 0;
    }

    HMODULE pvpAi = GetModuleHandleW(L"pvp_ai.dll");
    auto registerObserver = pvpAi
        ? reinterpret_cast<SdgoPvpAiRegisterObserverV1Fn>(
            GetProcAddress(pvpAi, SDGO_PVP_AI_OBSERVER_EXPORT))
        : nullptr;
    if (!registerObserver) {
        api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: pvp_ai observer ABI unavailable");
        return 0;
    }

    InitializeCriticalSection(&g_queueLock);
    const std::wstring eventPath = g_directory + L"\\server-observer-events.jsonl";
    g_eventFile = CreateFileW(
        eventPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_eventFile == INVALID_HANDLE_VALUE) {
        api->Log(mod, SDGO_LOG_ERROR,
                 "GameTest server observer inactive: event receipt already exists or cannot be created");
        DeleteCriticalSection(&g_queueLock);
        return 0;
    }
    g_startGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_startGate) {
        api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: worker gate creation failed");
        CloseHandle(g_eventFile);
        g_eventFile = INVALID_HANDLE_VALUE;
        DeleteCriticalSection(&g_queueLock);
        return 0;
    }
    HANDLE worker = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!worker) {
        api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: worker creation failed");
        CloseHandle(g_startGate);
        g_startGate = nullptr;
        CloseHandle(g_eventFile);
        g_eventFile = INVALID_HANDLE_VALUE;
        DeleteCriticalSection(&g_queueLock);
        return 0;
    }

    const SdgoPvpAiObserverSinkV1 sink = {
        sizeof(sink), SDGO_PVP_AI_OBSERVER_VERSION, &ObservePvpAi, nullptr};
    if (!registerObserver(&sink)) {
        InterlockedExchange(&g_abortWorker, 1);
        SetEvent(g_startGate);
        WaitForSingleObject(worker, INFINITE);
        CloseHandle(worker);
        CloseHandle(g_startGate);
        g_startGate = nullptr;
        if (g_eventFile != INVALID_HANDLE_VALUE) {
            CloseHandle(g_eventFile);
            g_eventFile = INVALID_HANDLE_VALUE;
        }
        DeleteCriticalSection(&g_queueLock);
        api->Log(mod, SDGO_LOG_ERROR, "GameTest server observer inactive: pvp_ai rejected registration");
        return 0;
    }

    SetEvent(g_startGate);
    CloseHandle(worker);
    api->Log(mod, SDGO_LOG_INFO,
             "ready: passive GameTest observer follows normal BattleServer startup and real PvP AI rooms");
    return 1;
}
