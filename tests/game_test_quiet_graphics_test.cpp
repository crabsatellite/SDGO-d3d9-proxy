#include "../mods/game_test_client/quiet_graphics.h"
#include <cstdio>
#include <cwchar>
#include <cstring>

namespace {
QuietGraphics::Create9Fn detour=nullptr;
int failures=0;
bool factoryUnavailable=false;
unsigned factoryDestroyed=0;
DWORD lastWorkerThread=0;
D3DDEVTYPE lastType=D3DDEVTYPE_FORCE_DWORD;
DWORD lastFlags=0;
HWND lastWindow=nullptr;
D3DPRESENT_PARAMETERS lastParameters{};
HRESULT queryResult=D3DERR_NOTAVAILABLE;
HRESULT createResult=D3DERR_DEVICELOST;
HRESULT resetResult=static_cast<HRESULT>(0x88761230u);
HRESULT presentResult=static_cast<HRESULT>(0x08761231u);
struct WindowCount {DWORD pid;HWND proxy;unsigned proxies,ime;};

BOOL CALLBACK CountInputWindows(HWND window,LPARAM parameter) {
    auto* count=reinterpret_cast<WindowCount*>(parameter);
    DWORD pid=0;GetWindowThreadProcessId(window,&pid);
    if (pid!=count->pid)return TRUE;
    wchar_t klass[64]{};
    GetClassNameW(window,klass,64);
    if (window==count->proxy && !std::wcscmp(klass,QuietGraphics::ProxyClassName()))++count->proxies;
    if (!std::wcscmp(klass,L"IME"))++count->ime;
    return TRUE;
}

struct FakeDevice {void** table;ULONG references;};
void* deviceTable[119]{};
FakeDevice fakeDevice{deviceTable,1};

ULONG STDMETHODCALLTYPE FakeDeviceRelease(IDirect3DDevice9* raw) {
    auto* device=reinterpret_cast<FakeDevice*>(raw);return --device->references;
}
HRESULT STDMETHODCALLTYPE FakeAdditionalSwapChain(IDirect3DDevice9*,D3DPRESENT_PARAMETERS* parameters,
                                                   IDirect3DSwapChain9**) {
    lastWorkerThread=GetCurrentThreadId();
    if (parameters)lastParameters=*parameters;
    return resetResult;
}
HRESULT STDMETHODCALLTYPE FakeReset(IDirect3DDevice9*,D3DPRESENT_PARAMETERS* parameters) {
    lastWorkerThread=GetCurrentThreadId();
    if (parameters)lastParameters=*parameters;
    return resetResult;
}
HRESULT STDMETHODCALLTYPE FakePresent(IDirect3DDevice9*,const RECT*,const RECT*,HWND window,const RGNDATA*) {
    lastWindow=window;return presentResult;
}

class FakeD3D final : public IDirect3D9 {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if (!out)return E_POINTER;
        *out=nullptr;
        if (!IsEqualIID(iid,__uuidof(IUnknown)) && !IsEqualIID(iid,__uuidof(IDirect3D9)))
            return E_NOINTERFACE;
        *out=static_cast<IDirect3D9*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining=--references_;
        if (!remaining) {++factoryDestroyed;delete this;}
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void*) override {
        lastWorkerThread=GetCurrentThreadId();return queryResult;
    }
    UINT STDMETHODCALLTYPE GetAdapterCount() override {
        lastWorkerThread=GetCurrentThreadId();return 7;
    }
    HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT,DWORD,D3DADAPTER_IDENTIFIER9*) override {
        lastWorkerThread=GetCurrentThreadId();return queryResult;
    }
    UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT,D3DFORMAT) override {
        lastWorkerThread=GetCurrentThreadId();return 13;
    }
    HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT,D3DFORMAT,UINT,D3DDISPLAYMODE*) override {
        lastWorkerThread=GetCurrentThreadId();return queryResult;
    }
    HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT,D3DDISPLAYMODE*) override {
        lastWorkerThread=GetCurrentThreadId();return queryResult;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT,D3DDEVTYPE type,D3DFORMAT,D3DFORMAT,BOOL) override {
        lastWorkerThread=GetCurrentThreadId();lastType=type;return queryResult;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT,D3DDEVTYPE type,D3DFORMAT,DWORD,
                                                D3DRESOURCETYPE,D3DFORMAT) override {
        lastWorkerThread=GetCurrentThreadId();lastType=type;return queryResult;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT,D3DDEVTYPE type,D3DFORMAT,BOOL,
                                                         D3DMULTISAMPLE_TYPE,DWORD*) override {
        lastWorkerThread=GetCurrentThreadId();lastType=type;return queryResult;
    }
    HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT,D3DDEVTYPE type,D3DFORMAT,D3DFORMAT,
                                                     D3DFORMAT) override {
        lastWorkerThread=GetCurrentThreadId();lastType=type;return queryResult;
    }
    HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT,D3DDEVTYPE type,D3DFORMAT,
                                                          D3DFORMAT) override {
        lastWorkerThread=GetCurrentThreadId();lastType=type;return queryResult;
    }
    HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT,D3DDEVTYPE type,D3DCAPS9*) override {
        lastWorkerThread=GetCurrentThreadId();lastType=type;return queryResult;
    }
    HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT) override {
        lastWorkerThread=GetCurrentThreadId();
        return reinterpret_cast<HMONITOR>(static_cast<uintptr_t>(0x2468));
    }
    HRESULT STDMETHODCALLTYPE CreateDevice(UINT,D3DDEVTYPE type,HWND window,DWORD flags,
                                           D3DPRESENT_PARAMETERS* parameters,
                                           IDirect3DDevice9** returnedDevice) override {
        lastWorkerThread=GetCurrentThreadId();lastType=type;lastWindow=window;lastFlags=flags;
        if (parameters)lastParameters=*parameters;
        if (SUCCEEDED(createResult) && returnedDevice) {
            fakeDevice.references=1;
            *returnedDevice=reinterpret_cast<IDirect3DDevice9*>(&fakeDevice);
        }
        return createResult;
    }
private:
    ULONG references_=1;
};

IDirect3D9* WINAPI Create(UINT sdkVersion) {
    lastWorkerThread=GetCurrentThreadId();
    if (sdkVersion!=D3D_SDK_VERSION)++failures;
    return factoryUnavailable ? nullptr:new FakeD3D;
}
int __cdecl Hook(SdgoMod*,const char* library,const char* name,void* replacement,void** original) {
    if (std::strcmp(library,"d3d9.dll") || std::strcmp(name,"Direct3DCreate9"))return 0;
    detour=reinterpret_cast<QuietGraphics::Create9Fn>(replacement);
    *original=reinterpret_cast<void*>(&Create);return 1;
}
}

int main() {
    deviceTable[2]=reinterpret_cast<void*>(&FakeDeviceRelease);
    deviceTable[13]=reinterpret_cast<void*>(&FakeAdditionalSwapChain);
    deviceTable[16]=reinterpret_cast<void*>(&FakeReset);
    deviceTable[17]=reinterpret_cast<void*>(&FakePresent);
    SdgoApi api{};api.HookImport=&Hook;
    QuietGraphics quiet;
    if (!quiet.Install(&api,reinterpret_cast<SdgoMod*>(1)) || !detour)return 1;
    auto state=quiet.State();
    if (!state.active || !state.worker_ready || !state.ime_disabled || !state.proxy_hwnd || state.proxy_visible ||
        !state.proxy_noactivate || !state.proxy_toolwindow || state.proxy_appwindow ||
        std::wcscmp(state.proxy_class,QuietGraphics::ProxyClassName()) || !*state.proxy_desktop)++failures;
    HDESK input=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);
    WindowCount windows{GetCurrentProcessId(),state.proxy_hwnd,0,0};
    if (!input || !EnumDesktopWindows(input,&CountInputWindows,reinterpret_cast<LPARAM>(&windows)) ||
        windows.proxies!=1 || windows.ime)++failures;
    if (input)CloseDesktop(input);

    factoryUnavailable=true;
    if (detour(D3D_SDK_VERSION)!=nullptr)++failures;
    factoryUnavailable=false;
    IDirect3D9* d3d=detour(D3D_SDK_VERSION);
    if (!d3d)return 1;
    queryResult=static_cast<HRESULT>(0x88761234u);
    if (d3d->CheckDeviceFormat(3,D3DDEVTYPE_SW,D3DFMT_R5G6B5,0x55,D3DRTYPE_TEXTURE,
                               D3DFMT_A1R5G5B5)!=queryResult ||
        lastType!=D3DDEVTYPE_HAL || lastWorkerThread!=state.proxy_thread_id)++failures;

    D3DPRESENT_PARAMETERS parameters{};
    parameters.BackBufferWidth=800;parameters.BackBufferHeight=600;parameters.Windowed=TRUE;
    HWND gameWindow=reinterpret_cast<HWND>(static_cast<uintptr_t>(0x1357));
    IDirect3DDevice9* device=reinterpret_cast<IDirect3DDevice9*>(static_cast<uintptr_t>(1));
    D3DPRESENT_PARAMETERS fullscreen=parameters;fullscreen.Windowed=FALSE;
    lastWorkerThread=0;
    if (d3d->CreateDevice(8,D3DDEVTYPE_HAL,gameWindow,D3DCREATE_HARDWARE_VERTEXPROCESSING,
                          &fullscreen,&device)!=D3DERR_INVALIDCALL || device || lastWorkerThread)++failures;
    const DWORD requested=D3DCREATE_FPU_PRESERVE|D3DCREATE_PUREDEVICE|D3DCREATE_HARDWARE_VERTEXPROCESSING;
    createResult=D3DERR_DEVICELOST;
    if (d3d->CreateDevice(8,D3DDEVTYPE_REF,gameWindow,requested,&parameters,&device)!=D3DERR_DEVICELOST ||
        device || lastType!=D3DDEVTYPE_HAL || lastWindow!=state.proxy_hwnd ||
        lastParameters.hDeviceWindow!=state.proxy_hwnd ||
        !(lastFlags&D3DCREATE_MULTITHREADED) || !(lastFlags&D3DCREATE_NOWINDOWCHANGES) ||
        (lastFlags&D3DCREATE_PUREDEVICE))++failures;

    createResult=S_OK;device=nullptr;
    if (d3d->CreateDevice(9,D3DDEVTYPE_REF,gameWindow,D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                          &parameters,&device)!=S_OK || !device)++failures;
    if (device) {
        lastWindow=nullptr;
        if (device->Present(nullptr,nullptr,gameWindow,nullptr)!=presentResult ||
            lastWindow!=state.proxy_hwnd)++failures;
        D3DPRESENT_PARAMETERS reset=parameters;reset.hDeviceWindow=gameWindow;
        lastWorkerThread=0;
        if (device->Reset(&reset)!=resetResult || lastWorkerThread!=state.proxy_thread_id ||
            lastParameters.hDeviceWindow!=state.proxy_hwnd)++failures;
        if (device->Release()!=0)++failures;
    }
    if (d3d->Release()!=0 || factoryDestroyed!=1)++failures;

    state=quiet.State();
    if (state.factory_calls!=2 || state.factories!=1 || state.create_attempts!=3 ||
        state.created_devices!=1 || state.failed_devices!=2 || state.resets!=1 ||
        state.presents!=1 || state.last_create_hresult!=S_OK ||
        state.last_reset_hresult!=resetResult || state.last_present_hresult!=presentResult ||
        state.dispatch_failures || state.proxy_visible)++failures;
    std::printf("Quiet input-desktop D3D9 proxy policy: %d failures\n",failures);
    return failures ? 1:0;
}
