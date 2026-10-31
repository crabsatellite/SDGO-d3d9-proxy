// Test fixture for findwindow_fix_test: built twice, as SDPlus.dll and as
// generic_input_method.dll, so FindWindowW is called from inside a module of that name.
#include <windows.h>

extern "C" __declspec(dllexport) HWND WINAPI Lookup(LPCWSTR className, LPCWSTR title)
{
    // The volatile local prevents a tail call, so the return address is in this DLL.
    HWND volatile result = FindWindowW(className, title);
    return result;
}

extern "C" __declspec(dllexport) HWND WINAPI DynamicLookup(LPCWSTR className, LPCWSTR title)
{
    using FindFn = HWND(WINAPI*)(LPCWSTR, LPCWSTR);
    auto find = reinterpret_cast<FindFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "FindWindowW"));
    HWND volatile result = find(className, title);
    return result;
}
