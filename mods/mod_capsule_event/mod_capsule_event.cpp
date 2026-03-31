// ============================================================
// Capsule Machine Event Mod
//
// Patches ALL capsule machine UI entries at runtime, replacing
// each unit with a random same-grade unit from the server.
//
// Data source: HTTP fetch from command_processor.py on VPS,
// with fallback to local capsule_event.json.
//
// JSON format: { "machineId": { "units": [...], "sprs": [[s0,s1],...], "rrank": [0,1,...] }, ... }
//
// Config: capsule_event.ini in the game directory.
//   url=http://10.0.0.1:8099/capsule_event.json
//
// Hook target: CapsuleMachineUI::LoadUnitCapsuleData
//   GOnline.exe Ghidra VA 0x007A6070, RVA 0x003A6070
//   Prologue: 55 8B EC 6A FF (5 bytes)
// ============================================================
#include "mod_api.h"
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <windows.h>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

static SdgoModAPI* g_api = nullptr;

// ---- Capsule entry layout (GridData type 0x17, 1019 bytes) ----
//
// Offset  Size    Field
// 0x000   4       int32   grid_data_id
// 0x004   1       byte    security_level
// 0x005   4       int32   machine_identifier (101..7801)
// 0x009   2       bytes   padding
// 0x00B   1       byte    unit_count (6)
// 0x00C   3       bytes   padding
// 0x00F   4       int32   unknown_1
// 0x013   8       byte[8] rrank_flags (1 = R-rank: show "?" name + silhouette)
// 0x01B   32      int32[8] unit_ids (GridData type 2 = UnitNumber)
// 0x03B   960     SPR paths: 16 x 60 bytes (wchar_t[30] each)
// Total = 1019 = 0x3FB

static const int ENTRY_SIZE      = 0x3FB;  // 1019 bytes
static const int OFF_MACHINE_ID  = 0x05;
static const int OFF_UNIT_COUNT  = 0x0B;
static const int OFF_RRANK_FLAGS = 0x13;
static const int OFF_UNIT_IDS    = 0x1B;
static const int OFF_SPR_PATHS   = 0x3B;
static const int SPR_SLOT_BYTES  = 60;     // 30 wchar_t per path

// ---- Config ----

static char g_serverUrl[512] = "http://10.0.0.1:8099/capsule_event.json";

static void LoadConfig()
{
    FILE* f = fopen("capsule_event.ini", "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "url=", 4) == 0) {
            char* val = line + 4;
            size_t len = strlen(val);
            while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r' || val[len - 1] == ' '))
                val[--len] = '\0';
            if (len > 0)
                strncpy(g_serverUrl, val, sizeof(g_serverUrl) - 1);
        }
    }
    fclose(f);
}

// ---- Per-machine replacement data ----

struct MachineData {
    int machineId;
    int unitCount;
    int units[8];
    char sprs[8][2][64];  // [slot][0=spr0, 1=spr1][path]
    int rrank[8];
};

static const int MAX_MACHINES = 160;
static MachineData g_machines[MAX_MACHINES];
static int g_machineCount = 0;
static bool g_active = false;

// ---- JSON parser helpers ----

static const char* SkipWs(const char* p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
    return p;
}

static const char* ParseString(const char* p, char* out, int maxLen)
{
    if (*p != '"') { out[0] = '\0'; return p; }
    p++;
    int i = 0;
    while (*p && *p != '"' && i < maxLen - 1) {
        if (*p == '\\' && *(p+1)) {
            p++;
            if (*p == '\\')      { out[i++] = '\\'; p++; }
            else if (*p == '/')  { out[i++] = '/'; p++; }
            else if (*p == '"')  { out[i++] = '"'; p++; }
            else if (*p == 'n')  { out[i++] = '\n'; p++; }
            else if (*p == 't')  { out[i++] = '\t'; p++; }
            else { out[i++] = *p++; }  // unknown escape: keep char
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    if (*p == '"') p++;
    return p;
}

static const char* ParseInt(const char* p, int* out)
{
    *out = 0;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    while (*p >= '0' && *p <= '9') {
        *out = *out * 10 + (*p - '0');
        p++;
    }
    if (neg) *out = -*out;
    return p;
}

static const char* SkipValue(const char* p)
{
    p = SkipWs(p);
    if (*p == '"') {
        p++;
        while (*p && *p != '"') { if (*p == '\\' && *(p+1)) p++; p++; }
        if (*p == '"') p++;
    } else if (*p == '[') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && *(p+1)) p++; p++; } }
            p++;
        }
    } else if (*p == '{') {
        int depth = 1; p++;
        while (*p && depth > 0) {
            if (*p == '{') depth++;
            else if (*p == '}') depth--;
            else if (*p == '"') { p++; while (*p && *p != '"') { if (*p == '\\' && *(p+1)) p++; p++; } }
            p++;
        }
    } else {
        while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    }
    return p;
}

// Parse int array: [1, 2, 3, ...]
static const char* ParseIntArray(const char* p, int* out, int maxCount, int* outCount)
{
    *outCount = 0;
    p = SkipWs(p);
    if (*p != '[') return p;
    p++;
    while (*outCount < maxCount) {
        p = SkipWs(p);
        if (*p == ']' || !*p) break;
        int val = 0;
        p = ParseInt(p, &val);
        out[(*outCount)++] = val;
        p = SkipWs(p);
    }
    while (*p && *p != ']') p++;
    if (*p == ']') p++;
    return p;
}

// Parse string array: ["a", "b", ...]
static const char* ParseStringArray(const char* p, char out[][64], int maxCount, int* outCount)
{
    *outCount = 0;
    p = SkipWs(p);
    if (*p != '[') return p;
    p++;
    while (*outCount < maxCount) {
        p = SkipWs(p);
        if (*p == ']' || !*p) break;
        p = ParseString(p, out[*outCount], 64);
        (*outCount)++;
        p = SkipWs(p);
    }
    while (*p && *p != ']') p++;
    if (*p == ']') p++;
    return p;
}

// Parse SPR array: [["s0","s1"], ["s0","s1"], ...]
static const char* ParseSprArray(const char* p, char out[][2][64], int maxCount, int* outCount)
{
    *outCount = 0;
    p = SkipWs(p);
    if (*p != '[') return p;
    p++;  // skip outer [
    while (*outCount < maxCount) {
        p = SkipWs(p);
        if (*p == ']' || !*p) break;
        if (*p != '[') { p++; continue; }
        p++;  // skip inner [
        p = SkipWs(p);
        p = ParseString(p, out[*outCount][0], 64);
        p = SkipWs(p);
        p = ParseString(p, out[*outCount][1], 64);
        while (*p && *p != ']') p++;
        if (*p == ']') p++;
        (*outCount)++;
        p = SkipWs(p);
    }
    while (*p && *p != ']') p++;
    if (*p == ']') p++;
    return p;
}

// Parse: { "machineId": { "units": [...], "sprs": [...], "rrank": [...] }, ... }
static bool ParseEventJson(const char* buf, MachineData* machines, int maxMachines, int* outCount)
{
    *outCount = 0;
    const char* p = buf;

    // Find opening '{'
    while (*p && *p != '{') p++;
    if (!*p) return false;
    p++;

    while (*outCount < maxMachines) {
        p = SkipWs(p);
        if (*p == '}' || !*p) break;

        // Parse machine ID key (as string)
        char keyBuf[32];
        p = ParseString(p, keyBuf, sizeof(keyBuf));
        int machineId = 0;
        ParseInt(keyBuf, &machineId);

        // Skip ':'
        p = SkipWs(p);
        if (*p == ':') p++;
        p = SkipWs(p);

        if (*p != '{') { p = SkipValue(p); continue; }
        p++;  // skip '{'

        MachineData& m = machines[*outCount];
        memset(&m, 0, sizeof(MachineData));
        m.machineId = machineId;
        m.unitCount = 0;

        // Parse object fields: units, sprs, rrank
        while (true) {
            p = SkipWs(p);
            if (*p == '}' || !*p) break;

            char fieldName[32];
            p = ParseString(p, fieldName, sizeof(fieldName));
            p = SkipWs(p);
            if (*p == ':') p++;
            p = SkipWs(p);

            if (strcmp(fieldName, "units") == 0) {
                p = ParseIntArray(p, m.units, 8, &m.unitCount);
            } else if (strcmp(fieldName, "sprs") == 0) {
                int sprCount = 0;
                p = ParseSprArray(p, m.sprs, 8, &sprCount);
            } else if (strcmp(fieldName, "rrank") == 0) {
                int rrCount = 0;
                p = ParseIntArray(p, m.rrank, 8, &rrCount);
            } else {
                p = SkipValue(p);
            }
            p = SkipWs(p);
        }
        if (*p == '}') p++;

        if (m.machineId > 0 && m.unitCount > 0)
            (*outCount)++;
    }

    return *outCount > 0;
}

// ---- Data loading ----

static bool FetchJsonHttp(const char* url, char* buf, int bufSize)
{
    HINTERNET hNet = InternetOpenA("SDGO-CapsuleEvent/3.0",
                                   INTERNET_OPEN_TYPE_DIRECT, NULL, NULL, 0);
    if (!hNet) return false;

    DWORD timeout = 5000;
    InternetSetOptionA(hNet, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionA(hNet, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    HINTERNET hUrl = InternetOpenUrlA(hNet, url, NULL, 0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        InternetCloseHandle(hNet);
        return false;
    }

    DWORD totalRead = 0, bytesRead = 0;
    while (InternetReadFile(hUrl, buf + totalRead,
           (DWORD)(bufSize - 1 - totalRead), &bytesRead)
           && bytesRead > 0 && totalRead < (DWORD)(bufSize - 1)) {
        totalRead += bytesRead;
    }
    buf[totalRead] = '\0';

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hNet);

    return totalRead > 0;
}

static bool LoadEventData()
{
    static char buf[131072];  // 128KB for all machines

    if (FetchJsonHttp(g_serverUrl, buf, sizeof(buf))) {
        int count = 0;
        if (ParseEventJson(buf, g_machines, MAX_MACHINES, &count)) {
            g_machineCount = count;
            g_active = true;
            return true;
        }
    }

    // Fallback to local file
    FILE* f = fopen("capsule_event.json", "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        int count = 0;
        if (ParseEventJson(buf, g_machines, MAX_MACHINES, &count)) {
            g_machineCount = count;
            g_active = true;
            return true;
        }
    }

    g_active = false;
    g_machineCount = 0;
    return false;
}

// ---- Hook ----

typedef void (__thiscall *LoadUnitCapsuleData_t)(void* thisPtr);
static LoadUnitCapsuleData_t g_origLoad = nullptr;

static void WriteSprPath(uint8_t* entry, int unitIdx, int sprIdx, const char* path)
{
    int slotIdx = unitIdx * 2 + sprIdx;
    if (slotIdx >= 16) return;
    wchar_t* dst = (wchar_t*)(entry + OFF_SPR_PATHS + slotIdx * SPR_SLOT_BYTES);
    memset(dst, 0, SPR_SLOT_BYTES);
    if (path[0]) {
        for (int k = 0; k < 29 && path[k]; k++)
            dst[k] = (wchar_t)path[k];
    }
}

// Find machine data by ID
static MachineData* FindMachine(int machineId)
{
    for (int i = 0; i < g_machineCount; i++) {
        if (g_machines[i].machineId == machineId)
            return &g_machines[i];
    }
    return nullptr;
}

static void PatchAllMachines(void* thisPtr)
{
    if (!g_active || g_machineCount == 0)
        return;

    uint8_t** pStart = (uint8_t**)((uint8_t*)thisPtr + 0x64);
    uint8_t** pEnd   = (uint8_t**)((uint8_t*)thisPtr + 0x68);

    uint8_t* start = *pStart;
    uint8_t* end   = *pEnd;
    if (!start || !end || end <= start)
        return;

    int entryCount = (int)(end - start) / ENTRY_SIZE;
    int patchedMachines = 0;

    for (int i = 0; i < entryCount; i++) {
        uint8_t* entry = start + i * ENTRY_SIZE;
        int machineId = *(int*)(entry + OFF_MACHINE_ID);

        MachineData* md = FindMachine(machineId);
        if (!md) continue;

        int unitCount = entry[OFF_UNIT_COUNT];
        if (unitCount <= 0 || unitCount > 8) continue;

        // Replace units by position (same approach as working machine-78 mod)
        int replaceCount = (md->unitCount < unitCount) ? md->unitCount : unitCount;
        int* unitSlots = (int*)(entry + OFF_UNIT_IDS);

        for (int j = 0; j < replaceCount; j++) {
            unitSlots[j] = md->units[j];
            entry[OFF_RRANK_FLAGS + j] = (uint8_t)(md->rrank[j] ? 1 : 0);
            WriteSprPath(entry, j, 0, md->sprs[j][0]);
            WriteSprPath(entry, j, 1, md->sprs[j][1]);
        }

        patchedMachines++;
    }

    if (g_api)
        g_api->Log("[CapsuleEvent] Patched %d/%d machines (%d machine data loaded)",
                   patchedMachines, entryCount, g_machineCount);
}

static void __fastcall Hook_LoadUnitCapsuleData(void* thisPtr, void* /*edx*/)
{
    // Fetch event data each time capsule UI opens
    if (LoadEventData()) {
        if (g_api) g_api->Log("[CapsuleEvent] Loaded %d machines", g_machineCount);
    } else {
        if (g_api) g_api->Log("[CapsuleEvent] No event data (inactive)");
    }

    // Call original loader
    g_origLoad(thisPtr);

    // Patch all machines
    PatchAllMachines(thisPtr);
}

// ---- Mod entry points ----

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;

    LoadConfig();
    api->Log("[CapsuleEvent] Server URL: %s", g_serverUrl);

    // Try initial load
    if (LoadEventData())
        api->Log("[CapsuleEvent] Initial load: %d machines", g_machineCount);
    else
        api->Log("[CapsuleEvent] No event data at startup (will retry on UI open)");

    // Install hook on LoadUnitCapsuleData
    HMODULE hExe = (HMODULE)api->GetModuleHandle(nullptr);
    if (!hExe) {
        api->Log("[CapsuleEvent] Failed to get GOnline.exe base");
        return false;
    }

    void* target = (void*)((uint8_t*)hExe + 0x003A6070);

    if (!api->HookInline(target, (void*)&Hook_LoadUnitCapsuleData,
                         (void**)&g_origLoad, 5)) {
        api->Log("[CapsuleEvent] Hook install FAILED at %p", target);
        return false;
    }

    api->Log("[CapsuleEvent] Hook installed at %p (RVA 0x3A6070)", target);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_origLoad) {
        g_api->Unhook((void**)&g_origLoad);
        g_origLoad = nullptr;
    }
    g_api = nullptr;
}

SDGO_MOD_EXPORT const char* ModName()    { return "Capsule Event"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "3.0"; }
