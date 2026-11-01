// Native client adapter for capsule catalogs compiled by capsule_client_compile.py.
//
// This GOnline build statically links the complete Zoa resource stack. GDT,
// SPR and TXR loaders all reach the same in-executable CZoaPack::OpenFile, so
// the overlay constructs only the executable's native CZoaFile with its own
// allocator and CRT.

#include <windows.h>

#include <cstdio>
#include <sstream>
#include <string>
#include <unordered_map>

#include "capsule_runtime.h"
#include "sdgo/sdgo_api.h"

namespace
{
    const char kProviderMarker[] = "SDGO_CAPSULE_REGISTRY_6TABLE_V2";

    const SdgoApi* g_api = nullptr;
    SdgoMod* g_mod = nullptr;
    capsule_catalog::Bundle g_bundle;
    volatile LONG g_priceUiOpened = 0;

    using OpenFileFn = void*(__thiscall*)(void*, const void*, bool);
    using FileCtorFn = void*(__thiscall*)(void*, void*, capsule_catalog::NativeFileInfo&, uint32_t, bool);
    using NewFn = void*(__cdecl*)(size_t);
    using WfopenFn = void*(__cdecl*)(const wchar_t*, const wchar_t*);

    OpenFileFn g_mainOpen = nullptr;
    FileCtorFn g_mainCtor = nullptr;
    NewFn g_mainNew = nullptr;
    WfopenFn g_mainWfopen = nullptr;

    using VerifyChecksumFn = bool(__thiscall*)(void*, const wchar_t*, void*);
    VerifyChecksumFn g_verifyChecksum = nullptr;
    SRWLOCK g_streamLock = SRWLOCK_INIT;
    std::unordered_map<void*, std::wstring> g_overlayGdtStreams;

    using GridLoadAllFn = bool(__thiscall*)(void*);
    using GridDataFn = void*(__cdecl*)();
    using GridFindFn = int(__thiscall*)(void*, void**, int, int, int);
    GridLoadAllFn g_gridLoadAll = nullptr;
    GridDataFn g_gridData = nullptr;
    GridFindFn g_gridFind = nullptr;
    std::wstring g_receiptPath;

    template <typename Fn>
    bool BindCode(Fn& target, const char* name)
    {
        target = reinterpret_cast<Fn>(g_api->Symbol(g_mod, name));
        return target != nullptr;
    }

    // MSVC 2010 x86 std::wstring used by GOnline.exe:
    // union { wchar_t inlineBuffer[8]; wchar_t* pointer; } at +0,
    // size at +0x10 and capacity at +0x14.
    bool TryMainName(const void* nativeString, const wchar_t** text, size_t* length)
    {
        __try {
            const auto* bytes = static_cast<const uint8_t*>(nativeString);
            size_t size = *reinterpret_cast<const uint32_t*>(bytes + 0x10);
            size_t capacity = *reinterpret_cast<const uint32_t*>(bytes + 0x14);
            if (size > 259 || capacity < size) {
                return false;
            }
            const wchar_t* value = capacity < 8
                ? reinterpret_cast<const wchar_t*>(nativeString)
                : *reinterpret_cast<const wchar_t* const*>(nativeString);
            if (!value || value[size] != L'\0')
                return false;
            *text = value;
            *length = size;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    bool TryRawName(const wchar_t* nativeString, const wchar_t** text, size_t* length)
    {
        __try {
            if (!nativeString)
                return false;
            size_t size = 0;
            while (size <= 259 && nativeString[size] != L'\0')
                ++size;
            if (size > 259)
                return false;
            *text = nativeString;
            *length = size;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }

    std::wstring NormalizeNativePath(const wchar_t* text, size_t length)
    {
        std::wstring normalized(text, length);
        for (wchar_t& value : normalized) {
            if (value == L'/')
                value = L'\\';
            if (value >= L'A' && value <= L'Z')
                value = static_cast<wchar_t>(value + (L'a' - L'A'));
        }
        return normalized;
    }

    void TrackOverlayGdt(void* stream, const std::wstring& path)
    {
        AcquireSRWLockExclusive(&g_streamLock);
        g_overlayGdtStreams[stream] = path;
        ReleaseSRWLockExclusive(&g_streamLock);
    }

    void* CallCtor(FileCtorFn ctor, void* storage, void* file,
                   const capsule_catalog::OverlayEntry* entry, uint32_t chunkSize, bool fullMemory)
    {
        __try {
            return ctor(storage, file, const_cast<capsule_catalog::NativeFileInfo&>(entry->info),
                        chunkSize, fullMemory);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return nullptr;
        }
    }

    void* CreateNativeFile(const capsule_catalog::OverlayEntry* entry, bool fullMemory,
                           size_t objectSize, NewFn allocate, WfopenFn open, FileCtorFn construct)
    {
        void* file = open(g_bundle.zpkPath.c_str(), L"rb");
        if (!file)
            return nullptr;
        void* storage = allocate(objectSize);
        if (!storage)
            return nullptr; // Mirrors the original OpenFile's extremely rare allocation-failure path.
        return CallCtor(construct, storage, file, entry, g_bundle.chunkSize, fullMemory);
    }

    void* __fastcall MainOpenDetour(void* pack, void*, const void* nativeName, bool fullMemory)
    {
        const wchar_t* text = nullptr;
        size_t length = 0;
        if (TryMainName(nativeName, &text, &length)) {
            std::wstring name = NormalizeNativePath(text, length);
            if (const auto* entry = g_bundle.Find(name)) {
                void* file = CreateNativeFile(entry, fullMemory, 0x7c, g_mainNew, g_mainWfopen, g_mainCtor);
                if (file) {
                    if (capsule_catalog::IsOverlayGdtPath(name))
                        TrackOverlayGdt(file, name);
                    if (name == L"uirs\\1161.uir" &&
                        InterlockedCompareExchange(&g_priceUiOpened, 1, 0) == 0)
                        g_api->Log(g_mod, SDGO_LOG_INFO,
                                   "capsule price UI defaults patch opened: uirs\\1161.uir");
                    return file;
                }
                g_api->Log(g_mod, SDGO_LOG_ERROR, "overlay open failed for %ls; using stock pack", name.c_str());
            }
        }
        return g_mainOpen(pack, nativeName, fullMemory);
    }

    bool __fastcall VerifyChecksumDetour(void* grid, void*, const wchar_t* filename, void* stream)
    {
        const wchar_t* text = nullptr;
        size_t length = 0;
        std::wstring actual;
        bool readable = TryRawName(filename, &text, &length);
        if (readable)
            actual = NormalizeNativePath(text, length);

        AcquireSRWLockExclusive(&g_streamLock);
        bool accepted = capsule_catalog::ConsumeOverlayGdtChecksum(
            g_overlayGdtStreams, stream, readable ? &actual : nullptr);
        ReleaseSRWLockExclusive(&g_streamLock);

        if (accepted) {
            g_api->Log(g_mod, SDGO_LOG_DEBUG, "accepted verified capsule overlay checksum for %ls", actual.c_str());
            return true;
        }
        return g_verifyChecksum(grid, filename, stream);
    }

    bool TryGridFind(void* grid, int id, int type, void** record)
    {
        __try {
            *record = nullptr;
            return g_gridFind(grid, record, id, type, 0) != 0 && *record != nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            *record = nullptr;
            return false;
        }
    }

    uint32_t CountContinuous(void* grid, int first, int type)
    {
        uint32_t count = 0;
        void* record = nullptr;
        while (count < 8192 && TryGridFind(grid, first + static_cast<int>(count), type, &record))
            ++count;
        return count;
    }

    uint32_t RecordU32(const void* record, size_t offset)
    {
        uint32_t value = 0;
        __try {
            value = *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(record) + offset);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            value = 0;
        }
        return value;
    }

    struct NativeCounts
    {
        uint32_t fixed = 0;
        uint32_t coin = 0;
        uint32_t custom = 0;
        uint32_t candidates = 0;
        uint32_t activity = 0;
        uint32_t lastActivityGrid = 0;
        uint32_t lastActivityMachine = 0;
        uint32_t firstCustomMachine = 0;
        uint32_t lastCandidateMachine = 0;
        uint32_t lastCandidateUnit = 0;
    };

    NativeCounts ReadNativeCounts(void* grid)
    {
        NativeCounts result;
        result.fixed = CountContinuous(grid, 155001, 0x17);
        result.coin = CountContinuous(grid, 135001, 0x16);
        result.custom = CountContinuous(grid, 3519001, 0x41);
        result.candidates = CountContinuous(grid, 3520001, 0x42);
        result.activity = CountContinuous(grid, 145001, 0x3C);

        void* record = nullptr;
        if (result.activity != 0) {
            result.lastActivityGrid = 145000 + result.activity;
            if (TryGridFind(grid, static_cast<int>(result.lastActivityGrid), 0x3C, &record))
                result.lastActivityMachine = RecordU32(record, 5);
        }
        if (result.custom != 0 && TryGridFind(grid, 3519001, 0x41, &record))
            result.firstCustomMachine = RecordU32(record, 5);
        if (result.candidates != 0 &&
            TryGridFind(grid, 3520001 + static_cast<int>(result.candidates - 1), 0x42, &record)) {
            result.lastCandidateMachine = RecordU32(record, 5);
            result.lastCandidateUnit = RecordU32(record, 9);
        }
        return result;
    }

    bool CountsMatch(const NativeCounts& counts)
    {
        return counts.fixed == g_bundle.fixedCount && counts.coin == g_bundle.coinCount &&
               counts.custom == g_bundle.customCount &&
               counts.candidates == g_bundle.candidateCount &&
               counts.activity == g_bundle.activityCount;
    }

    void WriteReceipt(const NativeCounts& counts, bool match)
    {
        std::ostringstream text;
        text << "[runtime]\r\nschema=1\r\ncatalog_revision=" << g_bundle.catalogRevision
             << "\r\nstatus=" << (match ? "registered" : "count_mismatch")
             << "\r\n\r\n[expected]\r\nfixed=" << g_bundle.fixedCount
             << "\r\ncoin=" << g_bundle.coinCount
             << "\r\ncustom=" << g_bundle.customCount
             << "\r\ncandidates=" << g_bundle.candidateCount
             << "\r\nactivity=" << g_bundle.activityCount
             << "\r\n\r\n[native]\r\nfixed=" << counts.fixed
             << "\r\ncoin=" << counts.coin
             << "\r\ncustom=" << counts.custom
             << "\r\ncandidates=" << counts.candidates
             << "\r\nactivity=" << counts.activity
             << "\r\n\r\n[samples]\r\nlast_activity_grid=" << counts.lastActivityGrid
             << "\r\nlast_activity_machine=" << counts.lastActivityMachine
             << "\r\nfirst_custom_machine=" << counts.firstCustomMachine
             << "\r\nlast_candidate_machine=" << counts.lastCandidateMachine
             << "\r\nlast_candidate_unit=" << counts.lastCandidateUnit << "\r\n";
        std::string bytes = text.str();
        HANDLE file = CreateFileW(g_receiptPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            g_api->Log(g_mod, SDGO_LOG_WARN, "cannot write capsule runtime receipt");
            return;
        }
        DWORD written = 0;
        bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                  written == bytes.size();
        CloseHandle(file);
        if (!ok)
            g_api->Log(g_mod, SDGO_LOG_WARN, "capsule runtime receipt write was incomplete");
    }

    bool __fastcall GridLoadAllDetour(void* gridThis, void*)
    {
        bool loaded = g_gridLoadAll(gridThis);
        if (!loaded) {
            g_api->Log(g_mod, SDGO_LOG_ERROR, "native GridData::LoadAll failed");
            return false;
        }
        void* grid = g_gridData();
        if (!grid)
            grid = gridThis;
        NativeCounts counts = ReadNativeCounts(grid);
        bool match = CountsMatch(counts);
        WriteReceipt(counts, match);
        g_api->Log(g_mod, match ? SDGO_LOG_INFO : SDGO_LOG_ERROR,
                   "capsule native registration %s: fixed %u/%u, coin %u/%u, custom %u/%u, candidates %u/%u, "
                   "activity %u/%u; last activity grid %u machine %u; first custom %u; last candidate %u/%u",
                   match ? "verified" : "mismatch",
                   counts.fixed, g_bundle.fixedCount, counts.coin, g_bundle.coinCount,
                   counts.custom, g_bundle.customCount, counts.candidates, g_bundle.candidateCount,
                   counts.activity, g_bundle.activityCount,
                   counts.lastActivityGrid, counts.lastActivityMachine, counts.firstCustomMachine,
                   counts.lastCandidateMachine, counts.lastCandidateUnit);
        return true;
    }
}

SDGO_MOD_ENTRY
{
    g_api = api;
    g_mod = mod;
    g_receiptPath = std::wstring(api->ModDirectory(mod)) + L"\\runtime-receipt.ini";

    std::string error;
    if (!capsule_catalog::LoadBundle(api->ModDirectory(mod), g_bundle, error)) {
        api->Log(mod, SDGO_LOG_ERROR, "capsule bundle rejected: %s; stock catalog remains active", error.c_str());
        return 0;
    }

    bool bound =
        BindCode(g_mainCtor, "capsule.main.file_ctor") &&
        BindCode(g_mainNew, "capsule.main.operator_new") &&
        BindCode(g_mainWfopen, "capsule.main.wfopen") &&
        BindCode(g_gridData, "grid.data") &&
        BindCode(g_gridFind, "grid.find");
    if (!bound) {
        api->Log(mod, SDGO_LOG_ERROR, "capsule native file symbols are unavailable; stock catalog remains active");
        return 0;
    }

    if (!api->HookCode(mod, "capsule.main.open_file", reinterpret_cast<void*>(&MainOpenDetour),
                       reinterpret_cast<void**>(&g_mainOpen)) ||
        !api->HookCode(mod, "capsule.grid_verify_checksum", reinterpret_cast<void*>(&VerifyChecksumDetour),
                       reinterpret_cast<void**>(&g_verifyChecksum)) ||
        !api->HookCode(mod, "capsule.grid_load_all", reinterpret_cast<void*>(&GridLoadAllDetour),
                       reinterpret_cast<void**>(&g_gridLoadAll))) {
        return 0;
    }
    api->Log(mod, SDGO_LOG_INFO, "%s capsule catalog %s ready: %u overlay resources",
             kProviderMarker, g_bundle.catalogRevision.c_str(),
             static_cast<unsigned>(g_bundle.entries.size()));
    return 1;
}
