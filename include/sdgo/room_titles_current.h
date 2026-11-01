// Optional confirmed current client title. No DLL loading or shared allocation.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include "room_titles.h"
namespace sdgo {
inline uint32_t CurrentRoomTitle(const wchar_t* directory,char* output,uint32_t capacity) {
    if(!directory||!output||capacity<2) return 0;
    const std::wstring relative=std::wstring(directory)+L"\\..\\room_titles\\room_titles.dll";
    wchar_t path[32768]={};auto n=GetFullPathNameW(relative.c_str(),_countof(path),path,nullptr);
    if(!n||n>=_countof(path)) return 0;
    auto module=GetModuleHandleW(path);if(!module) return 0;
    using Query=uint32_t(__cdecl*)(char*,uint32_t);
    auto query=reinterpret_cast<Query>(GetProcAddress(module,"SdgoRoomTitleCurrentV1"));
    return query?query(output,capacity):0;
}
}
