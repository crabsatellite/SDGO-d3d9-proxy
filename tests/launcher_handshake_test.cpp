#include <windows.h>
#include <cstdio>
#include "../loader/common/launcher_handshake.h"

int main() {
    int failures = 0, checks = 0;
    auto check = [&](bool value, const char* name) {
        ++checks;
        if (!value) { ++failures; std::printf("FAIL %s\n", name); }
    };
    wchar_t name[100];
    swprintf_s(name, L"Local\\SDGOHandshakeTest_%lu", GetCurrentProcessId());
    DWORD before = 0, after = 0;
    check(GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE, "handle count before");
    {
        sdgo::loader::LauncherHandshake handshake;
        check(!handshake.Acquire(0x548A891D, 0x0046C000, name), "Battle servers are untouched");
        check(!handshake.Acquire(0x54BE1568, 0x00C98000, name), "other client builds are untouched");
        HANDLE absent = OpenEventW(EVENT_ALL_ACCESS, FALSE, name);
        check(absent == nullptr, "unsupported builds create no object");
        if (absent) CloseHandle(absent);
        check(handshake.Acquire(0x54BE1568, 0x00C97000, name), "missing launcher event is supplied");
        HANDLE native = OpenEventW(EVENT_ALL_ACCESS, FALSE, name);
        check(native != nullptr, "native WinMain can open with its original full access mask");
        if (native) {
            check(SetEvent(native) != FALSE, "native WinMain can signal its launcher");
            check(WaitForSingleObject(native, 0) == WAIT_OBJECT_0, "normal event signalling works");
            CloseHandle(native);
        }
        DWORD first = 0, repeated = 0;
        GetProcessHandleCount(GetCurrentProcess(), &first);
        bool reused = true;
        for (int i = 0; i < 100; ++i)
            reused = handshake.Acquire(0x54BE1568, 0x00C97000, name) && reused;
        check(reused, "repeated boot shares the retained handle");
        GetProcessHandleCount(GetCurrentProcess(), &repeated);
        check(first == repeated, "repeated boot does not leak handles");
    }
    HANDLE gone = OpenEventW(EVENT_ALL_ACCESS, FALSE, name);
    check(gone == nullptr, "last loader handle is released on destruction");
    if (gone) CloseHandle(gone);
    HANDLE launcher = CreateEventW(nullptr, TRUE, TRUE, name);
    check(launcher != nullptr, "original launcher event created");
    {
        sdgo::loader::LauncherHandshake handshake;
        check(handshake.Acquire(0x54BE1568, 0x00C97000, name), "existing launcher event is retained");
        check(WaitForSingleObject(launcher, 0) == WAIT_OBJECT_0, "existing state remains signalled");
        check(WaitForSingleObject(launcher, 0) == WAIT_OBJECT_0, "existing manual-reset mode is preserved");
        CloseHandle(launcher);
        HANDLE native = OpenEventW(EVENT_ALL_ACCESS, FALSE, name);
        check(native != nullptr, "launcher exit during Mod loading does not remove the event");
        handshake.Release();
        if (native) {
            check(SetEvent(native) != FALSE, "native handle survives releasing the loader guard");
            CloseHandle(native);
        }
        HANDLE stale = OpenEventW(EVENT_ALL_ACCESS, FALSE, name);
        check(stale == nullptr, "completed handshake leaves no stale event for another launch");
        if (stale) CloseHandle(stale);
    }
    check(GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE, "handle count after");
    check(before == after, "all test handles released");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
