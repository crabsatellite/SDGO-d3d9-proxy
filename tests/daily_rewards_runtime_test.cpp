// Real core + production Mod + x86 settlement sites. Never executes a game.
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "sdgo/sdgo_api.h"
#include "daily_rewards_native_contract.h"

namespace {
    constexpr size_t kFrameBytes = 0xC00;
    BYTE g_code[10][16];
    void* g_sites[10];
    __declspec(align(16)) BYTE g_beforeFx[10][512], g_afterFx[10][512];
    __declspec(align(16)) BYTE g_xmm[8][16];
    bool g_active;
    bool g_clamp;
    int g_failures = 0;
    BYTE g_beforeFrame[10][kFrameBytes];
    DWORD g_beforeFlags[10];
    const DWORD g_expectedError = 0x5A6391;
    void Check(bool value, const char* message) {
        printf("%s %s\n", value ? "ok  " : "FAIL", message);
        if (!value) ++g_failures;
    }
    std::wstring Root() {
        wchar_t path[32768]; GetModuleFileNameW(nullptr, path, 32768);
        std::wstring result = path; return result.substr(0, result.find_last_of(L"\\/"));
    }
    void Write(const std::wstring& path, const std::string& text) {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        Check(file != INVALID_HANDLE_VALUE && WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) && written == text.size(), "write isolated fixture data");
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
    uint16_t Read(const BYTE* frame, ptrdiff_t offset) {
        uint16_t result; memcpy(&result, frame + offset, sizeof(result)); return result;
    }
    void __stdcall Prepare(unsigned index, BYTE* frame) {
        // Keep the three saved nonvolatile registers just below EBP untouched.
        memset(frame - kFrameBytes, 0xA5, kFrameBytes - 16);
        const uint16_t totals[3] = {uint16_t(g_clamp ? 32768 : 101), uint16_t(g_clamp ? 50000 : 103), uint16_t(g_clamp ? 65535 : 107)};
        const uint16_t events[3] = {7, 11, 13};
        for (unsigned i = 0; i < 3; ++i) {
            memcpy(frame + native_reward_contract::sites[index].fields[i], &totals[i], 2);
            memcpy(frame + native_reward_contract::sites[index].fields[i + 3], &events[i], 2);
        }
        memcpy(g_beforeFrame[index], frame - kFrameBytes, kFrameBytes);
        SetLastError(g_expectedError);
    }
    void __stdcall Inspect(unsigned index, BYTE* frame, const DWORD* registers) {
        const DWORD error = GetLastError();
        bool fields = true, decomposition = true;
        const auto& offsets = native_reward_contract::sites[index].fields;
        for (unsigned i = 0; i < 3; ++i) {
            uint16_t old = 0, previousEvent = 0;
            memcpy(&old, g_beforeFrame[index] + kFrameBytes + offsets[i], 2);
            memcpy(&previousEvent, g_beforeFrame[index] + kFrameBytes + offsets[i + 3], 2);
            const uint32_t scaled = g_active ? (old > 32767 ? 65535 : old * 2u) : old;
            fields = fields && Read(frame, offsets[i]) == scaled;
            const uint32_t event = previousEvent + scaled - old;
            fields = fields && Read(frame, offsets[i + 3]) == event;
            decomposition = decomposition && uint32_t(old - previousEvent) + Read(frame, offsets[i + 3]) == Read(frame, offsets[i]);
        }
        Check(fields, "all three final rewards and native event buckets use one bounded double");
        Check(decomposition, "unchanged base/other bonuses plus the event bucket equal DB and client totals");
        bool guards = true;
        for (ptrdiff_t offset = -ptrdiff_t(kFrameBytes); offset < -12; ++offset) {
            bool reward = false;
            for (auto field : offsets) if (offset == field || offset == field + 1) reward = true;
            if (!reward && frame[offset] != g_beforeFrame[index][kFrameBytes + offset]) guards = false;
        }
        Check(guards, "all unrelated frame data, costs and counters stay untouched");
        Check(registers[0] == 0x77777777 && registers[1] == 0x66666666 && registers[2] == reinterpret_cast<uintptr_t>(frame) &&
              registers[4] == 0x22222222 && registers[5] == 0x44444444 && registers[6] == (native_reward_contract::sites[index].writesEcx ? 0xA5A5A5A5 : 0x33333333) && registers[7] == (native_reward_contract::sites[index].writesEcx ? 0x11111111 : 0xA5A5A5A5),
              "site trampoline preserves registers while replaying the captured native MOV");
        Check((registers[8] & 0xCD5) == (g_beforeFlags[index] & 0xCD5), "site trampoline preserves direction and carry flags");
        Check(!memcmp(g_beforeFx[index], g_afterFx[index], 512), "x87 stack/control, MXCSR and all SIMD state are preserved");
        Check(error == g_expectedError, "Win32 last-error state survives the clock and logging callback");
    }

#define SETTLEMENT(N) \
    __declspec(naked) void Settle##N() { \
        __asm { push ebp } \
        __asm { mov ebp, esp } \
        __asm { push ebx } \
        __asm { push esi } \
        __asm { push edi } \
        __asm { sub esp, 0xC00 } \
        __asm { push ebp } \
        __asm { push N } \
        __asm { call Prepare } \
        __asm { fninit } \
        __asm { fld1 } \
        __asm { fldpi } \
        __asm { movdqu xmm0, [g_xmm] } \
        __asm { movdqu xmm1, [g_xmm + 16] } \
        __asm { movdqu xmm2, [g_xmm + 32] } \
        __asm { movdqu xmm3, [g_xmm + 48] } \
        __asm { movdqu xmm4, [g_xmm + 64] } \
        __asm { movdqu xmm5, [g_xmm + 80] } \
        __asm { movdqu xmm6, [g_xmm + 96] } \
        __asm { movdqu xmm7, [g_xmm + 112] } \
        __asm { fxsave [g_beforeFx + N * 512] } \
        __asm { mov eax, 0x11111111 } \
        __asm { mov ebx, 0x22222222 } \
        __asm { mov ecx, 0x33333333 } \
        __asm { mov edx, 0x44444444 } \
        __asm { mov esi, 0x66666666 } \
        __asm { mov edi, 0x77777777 } \
        __asm { stc } \
        __asm { std } \
        __asm { pushfd } \
        __asm { pop dword ptr [g_beforeFlags + N * 4] } \
        __asm { call dword ptr [g_sites + N * 4] } \
        __asm { fxsave [g_afterFx + N * 512] } \
        __asm { pushfd } \
        __asm { pushad } \
        __asm { mov eax, esp } \
        __asm { cld } \
        __asm { push eax } \
        __asm { push ebp } \
        __asm { push N } \
        __asm { call Inspect } \
        __asm { popad } \
        __asm { popfd } \
        __asm { cld } \
        __asm { fninit } \
        __asm { lea esp, [ebp - 12] } \
        __asm { pop edi } \
        __asm { pop esi } \
        __asm { pop ebx } \
        __asm { mov esp, ebp } \
        __asm { pop ebp } \
        __asm { ret } \
    }
    SETTLEMENT(0) SETTLEMENT(1) SETTLEMENT(2) SETTLEMENT(3) SETTLEMENT(4)
    SETTLEMENT(5) SETTLEMENT(6) SETTLEMENT(7) SETTLEMENT(8) SETTLEMENT(9)
#undef SETTLEMENT
    void (*kSettle[])() = {Settle0, Settle1, Settle2, Settle3, Settle4, Settle5, Settle6, Settle7, Settle8, Settle9};
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::wstring mode = argc > 1 ? argv[1] : L"active";
    g_active = mode == L"active" || mode == L"clamp";
    g_clamp = mode == L"clamp";
    for (unsigned i = 0; i < sizeof(g_xmm); ++i) reinterpret_cast<BYTE*>(g_xmm)[i] = static_cast<BYTE>(i ^ 0x5A);
    memset(g_beforeFx, 0, sizeof(g_beforeFx)); memset(g_afterFx, 0, sizeof(g_afterFx));
    const auto root = Root(); const auto host = GetModuleHandleW(nullptr);
    DWORD old = 0;
    Check(VirtualProtect(g_code, sizeof(g_code), PAGE_EXECUTE_READWRITE, &old) != 0, "reserve owned executable site fixtures");
    for (unsigned i = 0; i < 10; ++i) { memset(g_code[i], 0x90, 15); memcpy(g_code[i], native_reward_contract::sites[i].instruction.data(), 6); g_code[i][15] = 0xC3; g_sites[i] = g_code[i]; }
    FlushInstructionCache(GetCurrentProcess(), g_code, sizeof(g_code));
    auto* base = reinterpret_cast<BYTE*>(host);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    wchar_t key[128]; swprintf_s(key, L"daily_rewards_runtime_test-%08X-%08X", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    char header[256]; sprintf_s(header, "[host]\ntimestamp=0x%08X\nsize_of_image=0x%08X\n", nt->FileHeader.TimeDateStamp, nt->OptionalHeader.SizeOfImage);
    std::string symbols = header;
    for (unsigned i = 0; i < 10; ++i) {
        char expected[64] = {};
        for (unsigned byte = 0; byte < 6; ++byte)
            sprintf_s(expected + byte * 3, sizeof(expected) - byte * 3, "%02X ", native_reward_contract::sites[i].instruction[byte]);
        char text[256]; sprintf_s(text, "[%s]\nkind=%s\nrva=0x%08X\nexpect=%s\n", native_reward_contract::sites[i].symbol,
            mode == L"wrongkind" && i == 9 ? "code" : "site", static_cast<unsigned>(g_code[i] - base),
            mode == L"badbytes" && i == 9 ? "CC CC CC CC CC" : expected);
        symbols += text;
        std::string layout = native_reward_contract::sites[i].symbol;
        layout.replace(layout.find(".final."), 7, ".layout.");
        sprintf_s(text, "[%s]\nkind=site\nrva=0x%08X\nexpect=%s\n", layout.c_str(),
                  static_cast<unsigned>(g_code[i] - base), mode == L"badlayout" && i == 9 ? "CC CC CC CC CC" : expected);
        symbols += text;
    }
    CreateDirectoryW((root + L"\\sdgo\\symbols").c_str(), nullptr);
    Write(root + L"\\sdgo\\symbols\\" + key + L".ini", symbols);
    SYSTEMTIME time = {}; GetLocalTime(&time);
    const unsigned now = time.wHour * 60u + time.wMinute;
    const bool configuredActive = mode != L"inactive";
    const unsigned start = configuredActive ? (now + 1380) % 1440 : (now + 60) % 1440;
    const unsigned end = configuredActive ? (now + 60) % 1440 : (now + 120) % 1440;
    char settings[256]; sprintf_s(settings, "[settings]\nStartTime=%02u:%02u\nEndTime=%02u:%02u\nTrace=1\n", start / 60, start % 60, end / 60, end % 60);
    Write(root + L"\\sdgo\\mods\\daily_rewards\\config.ini", mode == L"invalid" ? "[settings]\nStartTime=18:99\nEndTime=22:00\n" : settings);
    HMODULE core = LoadLibraryW((root + L"\\sdgo\\core.dll").c_str());
    auto init = core ? reinterpret_cast<SdgoCoreInitFn>(GetProcAddress(core, "SdgoCoreInit")) : nullptr;
    Check(init != nullptr, "load the real core"); if (!init) return 2;
    SdgoBoot boot = {sizeof(boot), "daily_rewards_test", host}; Check(init(&boot) != 0, "initialize real core and daily reward Mod");
    for (auto settle : kSettle) settle();
    puts(g_failures ? "DAILY REWARD RUNTIME CHECKS FAILED" : "ALL DAILY REWARD RUNTIME CHECKS PASSED");
    return g_failures ? 1 : 0;
}
