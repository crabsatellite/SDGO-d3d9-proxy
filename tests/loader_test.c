/*
 * loader_test - checks where the d3d9.dll loader shim forwards Direct3DCreate9.
 *
 * Run from a folder that holds this exe, the shim as d3d9.dll and optionally sdgo\ (core.dll,
 * sdgo.ini with [loader] chain_d3d9=...) and a chained DLL. tests/loader_test.ps1 builds
 * those folders. Prints the result and every loaded module whose file name starts with
 * "d3d9" (the shim itself plus the DLL it forwarded to).
 * Exit code: 0 when Direct3DCreate9 returned an IDirect3D9, otherwise non-zero.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

typedef void*(WINAPI* Direct3DCreate9Fn)(UINT);
typedef ULONG(WINAPI* ReleaseFn)(void*);

int main(void)
{
    wchar_t dir[MAX_PATH];
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(NULL, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = L'\0';
    swprintf_s(path, MAX_PATH, L"%s\\d3d9.dll", dir);

    HMODULE shim = LoadLibraryW(path);
    if (!shim) {
        printf("FAIL: d3d9.dll (the shim) did not load\n");
        return 1;
    }
    Direct3DCreate9Fn create = (Direct3DCreate9Fn)GetProcAddress(shim, "Direct3DCreate9");
    void* d3d = create ? create(32 /* D3D_SDK_VERSION */) : NULL;

    printf("Direct3DCreate9: %s; d3d9 modules:", d3d ? "IDirect3D9" : "NULL");
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (snapshot != INVALID_HANDLE_VALUE && Module32FirstW(snapshot, &entry)) {
        do {
            if (_wcsnicmp(entry.szModule, L"d3d9", 4) == 0)
                printf(" [%ls]", entry.szExePath);
        } while (Module32NextW(snapshot, &entry));
        CloseHandle(snapshot);
    }
    printf("\n");
    if (d3d)
        ((ReleaseFn)(*(void***)d3d)[2])(d3d);
    return d3d ? 0 : 2;
}
