#define DBINITCONSTANTS
#include <windows.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "sdgo/mod_settings.h"
#include "sdgo/sdgo_api.h"

#include "native.h"
#include "oledb_draw.h"

namespace {
using namespace sdgo::collection_capsule;

const SdgoApi* g_api = nullptr;
SdgoMod* g_mod = nullptr;
PickFunction g_original = nullptr;
std::int32_t g_machineId = kDefaultMachineId;

template<class T>
bool Read(const T* source, T& destination) {
    __try {
        if (!source) return false;
        std::memcpy(&destination, source, sizeof(destination));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ProbeWritable(void* address, std::size_t size) {
    if (!address || !size) return false;
    __try {
        const std::uintptr_t begin = reinterpret_cast<std::uintptr_t>(address);
        const std::uintptr_t end = begin + size - 1;
        if (end < begin) return false;
        SYSTEM_INFO info = {};
        GetSystemInfo(&info);
        const std::uintptr_t page = info.dwPageSize ? info.dwPageSize : 4096;
        for (std::uintptr_t current = begin; current <= end;) {
            auto* byte = reinterpret_cast<volatile BYTE*>(current);
            const BYTE value = *byte;
            *byte = value;
            const std::uintptr_t next = ((current / page) + 1) * page;
            if (next <= current || next > end) break;
            current = next;
        }
        auto* last = reinterpret_cast<volatile BYTE*>(end);
        const BYTE value = *last;
        *last = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MachineId(const sdgo::ModSettings& settings, std::int32_t& result) {
    char text[32] = {};
    const auto length = settings.String("MachineID", "990001", text, sizeof(text));
    if (!length || length >= sizeof(text) - 1) return false;
    unsigned value = 0;
    for (std::uint32_t index = 0; index < length; ++index) {
        if (text[index] < '0' || text[index] > '9') return false;
        const unsigned digit = static_cast<unsigned>(text[index] - '0');
        if (value > static_cast<unsigned>(INT_MAX) / 10 ||
            (value == static_cast<unsigned>(INT_MAX) / 10 &&
             digit > static_cast<unsigned>(INT_MAX) % 10)) return false;
        value = value * 10 + digit;
    }
    if (!value) return false;
    result = static_cast<std::int32_t>(value);
    return true;
}

int __fastcall PickHook(
    void* self, void*, void* connection, PickInput* rawInput, PickOutput* callerOutput) {
    PickInput input = {};
    if (!Read(rawInput, input)) return kDatabaseFailure;
    if (input.machine != g_machineId)
        return g_original(self, connection, rawInput, callerOutput);

    // The native caller refunds every nonzero return. Probe both publication
    // destinations before delivery so a committed unit cannot be followed by
    // a refund merely because the caller supplied an unwritable result buffer.
    if (!ProbeWritable(&rawInput->result, sizeof(rawInput->result)) ||
        !ProbeWritable(callerOutput, sizeof(*callerOutput))) return kDatabaseFailure;

    int result = kDatabaseFailure;
    PickOutput pending = {};
    DrawDatabase database;
    if (database.Open(connection)) result = database.Draw(input.account, input.machine, pending);
    if (result < 0) result = kDatabaseFailure;

    if (result == 0) {
        std::memcpy(callerOutput, &pending, sizeof(pending));
        rawInput->result = 0;
        return 0;
    }
    rawInput->result = result;
    if (g_api && g_api->Log)
        g_api->Log(g_mod, SDGO_LOG_WARN,
                   "collection capsule delivery declined: machine=%d code=%d",
                   input.machine, result);
    return result;
}
} // namespace

SDGO_MOD_ENTRY {
    if (!api || api->version != SDGO_API_VERSION ||
        api->size < offsetof(SdgoApi, ModDirectory) + sizeof(api->ModDirectory) ||
        !api->HookCode || !api->Log) return 0;
    g_api = api;
    g_mod = mod;
    if (!MachineId(sdgo::ModSettings(api, mod), g_machineId)) {
        api->Log(mod, SDGO_LOG_ERROR, "invalid MachineID; expected an integer from 1 to INT_MAX");
        return 0;
    }
    if (!api->HookCode(
            mod, "collection.pick", reinterpret_cast<void*>(&PickHook),
            reinterpret_cast<void**>(&g_original))) return 0;
    api->Log(mod, SDGO_LOG_INFO,
             "collection capsule active: machine=%d, native price registry=100000 GP",
             g_machineId);
    return 1;
}
