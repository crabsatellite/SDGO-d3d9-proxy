// Optional room-title provider lookup for other server mods. Does not load a DLL.
#pragma once

#include <windows.h>

#include <cstddef>
#include <string>

#include "room_titles.h"

namespace sdgo {
inline const SdgoRoomTitleApi* FindRoomTitleApi(const wchar_t* consumerDirectory)
{
    if (!consumerDirectory || !*consumerDirectory)
        return nullptr;
    const std::wstring relative = std::wstring(consumerDirectory) +
                                   L"\\..\\room_titles_server\\room_titles_server.dll";
    wchar_t providerPath[32768] = {};
    const DWORD length = GetFullPathNameW(relative.c_str(), static_cast<DWORD>(_countof(providerPath)),
                                         providerPath, nullptr);
    if (!length || length >= _countof(providerPath))
        return nullptr;
    HMODULE module = GetModuleHandleW(providerPath);
    if (!module)
        return nullptr;
    const auto getter = reinterpret_cast<SdgoRoomTitleApiV1Fn>(GetProcAddress(module, "SdgoRoomTitleApiV1"));
    if (!getter)
        return nullptr;
    const SdgoRoomTitleApi* api = getter();
    if (!api || api->version != SDGO_ROOM_TITLE_API_VERSION ||
        api->size < offsetof(SdgoRoomTitleApi, QueryForPilot) + sizeof(api->QueryForPilot) || !api->QueryForPilot)
        return nullptr;
    return api;
}
} // namespace sdgo
