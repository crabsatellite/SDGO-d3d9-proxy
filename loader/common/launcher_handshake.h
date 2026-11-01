#pragma once
#include <windows.h>
#include <cstdint>

namespace sdgo::loader {
// WinMain in the supported client opens "9YOU SDGO", signals it, and immediately
// closes its own handle. A missing event returns zero before window/data loading.
// Keep the coordination object alive across synchronous Mod initialization and
// support launchers that do not create it. No login or native executable changes.
class LauncherHandshake {
public:
    LauncherHandshake() = default;
    LauncherHandshake(const LauncherHandshake&) = delete;
    LauncherHandshake& operator=(const LauncherHandshake&) = delete;
    ~LauncherHandshake() { Release(); }
    void Release() noexcept { if (event_) { CloseHandle(event_); event_ = nullptr; } }

    bool Acquire(std::uint32_t timestamp, std::uint32_t imageSize,
                 const wchar_t* name = L"9YOU SDGO") noexcept {
        if (timestamp != 0x54BE1568 || imageSize != 0x00C97000)
            return false;
        if (event_)
            return true;
        // CreateEvent also opens an existing launcher's event without resetting
        // its signalled state or changing its reset mode/security descriptor.
        event_ = CreateEventW(nullptr, FALSE, FALSE, name);
        return event_ != nullptr;
    }
private:
    HANDLE event_ = nullptr;
};
}
