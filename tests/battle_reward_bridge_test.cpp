// Exercise the actual production assembly bridge without executing game code.
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "mods/battle_rewards/site_bridge.h"
#include "mods/battle_rewards/daily_schedule.h"

namespace {
int failures = 0;
BYTE frame[0xC00];
uint32_t beforeFlags = 0, afterFlags = 0;
uint32_t regs[7] = {};
alignas(16) BYTE beforeFx[512], afterFx[512];
alignas(16) uint32_t xmmValues[4] = {0x12345678, 0xA0B0C0D0, 0x0BADF00D, 0xFFEEDDCC};
BYTE* expectedFrame = nullptr;
unsigned selected = 0, callbacks = 0;
DWORD lastError = 0;

void Check(bool ok, const char* message) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", message);
    if (!ok) ++failures;
}

void __stdcall Callback(unsigned index, BYTE* address) {
    Check(index == selected && address == expectedFrame, "collector receives exact site and native frame");
    ++callbacks;
    const uint16_t amount = 26620;
    memcpy(address - 0x20, &amount, sizeof(amount));
    SetLastError(9999); // bridge must restore the caller's error
    __asm { fninit }
    __asm { pxor xmm0, xmm0 }
}

__declspec(naked) void Resume() { __asm { ret } }

__declspec(naked) void __stdcall Invoke(void*, BYTE*) {
    __asm {
        push ebp
        push ebx
        push esi
        push edi
        mov ebp, [esp + 24]
        fninit
        fld1
        fldpi
        movdqu xmm0, xmmword ptr [xmmValues]
        fxsave [beforeFx]
        mov eax, 0x11111111
        mov ebx, 0x22222222
        mov ecx, 0x33333333
        mov edx, 0x44444444
        mov esi, 0x66666666
        mov edi, 0x77777777
        stc
        std
        pushfd
        pop dword ptr [beforeFlags]
        call dword ptr [esp + 20]
        mov regs[0], eax
        mov regs[4], ebx
        mov regs[8], ecx
        mov regs[12], edx
        mov regs[16], esi
        mov regs[20], edi
        mov regs[24], ebp
        pushfd
        pop dword ptr [afterFlags]
        cld
        fxsave [afterFx]
        pop edi
        pop esi
        pop ebx
        pop ebp
        ret 8
    }
}
}

int main() {
    using namespace sdgo::battle_rewards;
    bridge::callback = &Callback;
    expectedFrame = frame + sizeof(frame);
    for (unsigned index = 0; index < bridge::detours.size(); ++index) {
        selected = index;
        *bridge::originals[index] = reinterpret_cast<void*>(&Resume);
        SetLastError(1234);
        Invoke(bridge::detours[index], expectedFrame);
        lastError = GetLastError();
        uint16_t amount = 0; memcpy(&amount, expectedFrame - 0x20, sizeof(amount));
        Check(amount == 26620, "collector updates only the intended reward value");
        Check(regs[0] == 0x11111111 && regs[1] == 0x22222222 && regs[2] == 0x33333333 &&
              regs[3] == 0x44444444 && regs[4] == 0x66666666 && regs[5] == 0x77777777 &&
              regs[6] == reinterpret_cast<uintptr_t>(expectedFrame), "general-purpose native registers survive the bridge");
        Check((beforeFlags & 0xCD5) == (afterFlags & 0xCD5), "native direction/carry/arithmetic flags survive the bridge");
        Check(!memcmp(beforeFx, afterFx, 512), "x87 control/stack and SIMD state survive the bridge");
        Check(lastError == 1234, "Win32 last error survives the collector");
    }
    Check(callbacks == 10, "all ten settlement bridges execute once");
    DailySchedule schedule;
    Check(schedule.Factor(18 * 60) == 1, "no double multiplier when the daily component is absent");
    schedule.module_loaded = true;
    Check(schedule.Factor(18 * 60 - 1) == 1 && schedule.Factor(18 * 60) == 2 &&
          schedule.Factor(22 * 60 - 1) == 2 && schedule.Factor(22 * 60) == 1, "daily schedule has inclusive start and exclusive end");
    schedule.start = 23 * 60; schedule.end = 60;
    Check(schedule.Factor(0) == 2 && schedule.Factor(60) == 1, "daily schedule can cross midnight");
    unsigned minute = 99;
    Check(!DailySchedule::Parse(L"24:00", &minute) && minute == 99 &&
          DailySchedule::Parse(L"18:30", &minute) && minute == 1110, "daily schedule parses exact HH:MM");
    printf("%s\n", failures ? "BRIDGE CHECKS FAILED" : "ALL NATIVE BRIDGE CHECKS PASSED");
    return failures ? 1 : 0;
}
