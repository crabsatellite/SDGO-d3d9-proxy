// Shared by the loader shims (d3d9.dll for the game client, winmm.dll for the battle
// server): start <exe dir>\sdgo\core.dll as early as possible, outside the loader lock.
//
// DllMain only calls EarlyBootInstall, which redirects the executable's import of
// kernel32!GetSystemTimeAsFileTime. The MSVC CRT calls it first thing in its entry
// point (__security_init_cookie); that first call restores the import and boots the
// core on the main thread before main/WinMain runs. Each shim also calls EarlyBoot from
// its forwarded exports, so the core still starts if the import is missing.

#pragma once

#include <windows.h>

void EarlyBootInstall(const char* loaderName);
void EarlyBoot();
void EarlyBootTrace(const wchar_t* message);

// The DLL a shim forwards its exports to. Normally the system copy (system32\<systemName>).
// When <exe dir>\sdgo\sdgo.ini has [loader] <chainKey>=<path>, that DLL is loaded instead
// (path relative to the exe directory, or absolute), so another wrapper installed under
// the same name keeps working. A chain that cannot be loaded, or that is this shim
// itself, falls back to the system copy.
HMODULE LoadForwardTarget(const wchar_t* chainKey, const wchar_t* systemName);
