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
HRESULT createTextureResult=S_OK;
HRESULT getBackBufferResult=S_OK;
HRESULT cooperativeResult=static_cast<HRESULT>(0x08761232u);
HRESULT resetResult=static_cast<HRESULT>(0x88761230u);
HRESULT presentResult=static_cast<HRESULT>(0x08761231u);
LONG diagnosticLogs=0;
LONG deviceReleaseCalls=0;
LONG resetCalls=0;
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
struct FakeSurface {void** table;ULONG references;};
void* surfaceTable[17]{};
FakeSurface fakeSurface{surfaceTable,1};
struct FakeTexture {void** table;ULONG references;};
void* textureTable[22]{};
FakeTexture fakeTexture{textureTable,1};

ULONG STDMETHODCALLTYPE FakeDeviceRelease(IDirect3DDevice9* raw) {
    InterlockedIncrement(&deviceReleaseCalls);
    auto* device=reinterpret_cast<FakeDevice*>(raw);return --device->references;
}
HRESULT STDMETHODCALLTYPE FakeCooperativeLevel(IDirect3DDevice9*) {
    lastWorkerThread=GetCurrentThreadId();return cooperativeResult;
}
HRESULT STDMETHODCALLTYPE FakeGetBackBuffer(IDirect3DDevice9*,UINT,UINT,D3DBACKBUFFER_TYPE,
                                             IDirect3DSurface9** surface) {
    lastWorkerThread=GetCurrentThreadId();
    fakeSurface.references=1;
    if (surface)*surface=SUCCEEDED(getBackBufferResult)
        ? reinterpret_cast<IDirect3DSurface9*>(&fakeSurface):nullptr;
    return getBackBufferResult;
}
HRESULT STDMETHODCALLTYPE FakeCreateTexture(IDirect3DDevice9*,UINT width,UINT height,UINT levels,
                                             DWORD usage,D3DFORMAT format,D3DPOOL pool,
                                             IDirect3DTexture9** texture,HANDLE*) {
    lastWorkerThread=GetCurrentThreadId();
    lastParameters.BackBufferWidth=width;lastParameters.BackBufferHeight=height;
    lastParameters.BackBufferCount=levels;lastParameters.MultiSampleQuality=usage;
    lastParameters.BackBufferFormat=format;lastParameters.SwapEffect=static_cast<D3DSWAPEFFECT>(pool);
    fakeTexture.references=1;
    if (texture)*texture=SUCCEEDED(createTextureResult)
        ? reinterpret_cast<IDirect3DTexture9*>(&fakeTexture):nullptr;
    return createTextureResult;
}
ULONG STDMETHODCALLTYPE FakeTextureRelease(IDirect3DTexture9* raw) {
    auto* texture=reinterpret_cast<FakeTexture*>(raw);
    lastWorkerThread=GetCurrentThreadId();
    return --texture->references;
}
void Log(SdgoMod*,int,const char*,...) {InterlockedIncrement(&diagnosticLogs);}
HRESULT STDMETHODCALLTYPE FakeAdditionalSwapChain(IDirect3DDevice9*,D3DPRESENT_PARAMETERS* parameters,
                                                   IDirect3DSwapChain9**) {
    lastWorkerThread=GetCurrentThreadId();
    if (parameters)lastParameters=*parameters;
    return resetResult;
}
HRESULT STDMETHODCALLTYPE FakeReset(IDirect3DDevice9*,D3DPRESENT_PARAMETERS* parameters) {
    InterlockedIncrement(&resetCalls);
    lastWorkerThread=GetCurrentThreadId();
    if (parameters)lastParameters=*parameters;
    return resetResult;
}
HRESULT STDMETHODCALLTYPE FakePresent(IDirect3DDevice9*,const RECT*,const RECT*,HWND window,const RGNDATA*) {
    lastWindow=window;return presentResult;
}
ULONG STDMETHODCALLTYPE FakeSurfaceRelease(IDirect3DSurface9* raw) {
    auto* surface=reinterpret_cast<FakeSurface*>(raw);
    lastWorkerThread=GetCurrentThreadId();
    return --surface->references;
}
HRESULT STDMETHODCALLTYPE FakeSurfaceGetDesc(IDirect3DSurface9*,D3DSURFACE_DESC* description) {
    lastWorkerThread=GetCurrentThreadId();
    if (!description)return D3DERR_INVALIDCALL;
    *description={};
    description->Format=lastParameters.BackBufferFormat;
    description->Type=D3DRTYPE_SURFACE;
    description->Usage=D3DUSAGE_RENDERTARGET;
    description->Pool=D3DPOOL_DEFAULT;
    description->Width=lastParameters.BackBufferWidth;
    description->Height=lastParameters.BackBufferHeight;
    return S_OK;
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

void InitializeDeviceTable(void** table) {
    table[2]=reinterpret_cast<void*>(&FakeDeviceRelease);
    table[3]=reinterpret_cast<void*>(&FakeCooperativeLevel);
    table[13]=reinterpret_cast<void*>(&FakeAdditionalSwapChain);
    table[16]=reinterpret_cast<void*>(&FakeReset);
    table[17]=reinterpret_cast<void*>(&FakePresent);
    table[18]=reinterpret_cast<void*>(&FakeGetBackBuffer);
    table[23]=reinterpret_cast<void*>(&FakeCreateTexture);
}

bool HasOriginalDeviceSlot(void** table) {
    return table[2]==reinterpret_cast<void*>(&FakeDeviceRelease) ||
        table[3]==reinterpret_cast<void*>(&FakeCooperativeLevel) ||
        table[13]==reinterpret_cast<void*>(&FakeAdditionalSwapChain) ||
        table[16]==reinterpret_cast<void*>(&FakeReset) ||
        table[17]==reinterpret_cast<void*>(&FakePresent) ||
        table[18]==reinterpret_cast<void*>(&FakeGetBackBuffer) ||
        table[23]==reinterpret_cast<void*>(&FakeCreateTexture);
}
}

int main() {
    InitializeDeviceTable(deviceTable);
    surfaceTable[2]=reinterpret_cast<void*>(&FakeSurfaceRelease);
    surfaceTable[12]=reinterpret_cast<void*>(&FakeSurfaceGetDesc);
    textureTable[2]=reinterpret_cast<void*>(&FakeTextureRelease);
    SdgoApi api{};api.Log=&Log;api.HookImport=&Hook;
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

    HWND sourceWindow=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,L"STATIC",L"",
        WS_POPUP|WS_DISABLED,0,0,640,480,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    D3DPRESENT_PARAMETERS automatic{};
    automatic.Windowed=TRUE;automatic.hDeviceWindow=sourceWindow;
    D3DPRESENT_PARAMETERS failedAutomatic=automatic;
    createResult=D3DERR_DEVICELOST;device=nullptr;
    if (!sourceWindow ||
        d3d->CreateDevice(9,D3DDEVTYPE_REF,sourceWindow,D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                          &failedAutomatic,&device)!=D3DERR_DEVICELOST || device ||
        failedAutomatic.BackBufferWidth || failedAutomatic.BackBufferHeight ||
        failedAutomatic.hDeviceWindow!=sourceWindow ||
        lastParameters.BackBufferWidth!=640 || lastParameters.BackBufferHeight!=480 ||
        lastParameters.hDeviceWindow!=state.proxy_hwnd)++failures;
    state=quiet.State();
    if (state.last_focus_hwnd!=sourceWindow || state.last_device_hwnd!=sourceWindow ||
        state.last_requested_width || state.last_requested_height ||
        state.last_client_width!=640 || state.last_client_height!=480 ||
        state.last_resolved_width!=640 || state.last_resolved_height!=480)++failures;

    createResult=S_OK;device=nullptr;
    if (d3d->CreateDevice(9,D3DDEVTYPE_REF,sourceWindow,D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                          &automatic,&device)!=S_OK || !device ||
        automatic.BackBufferWidth!=640 || automatic.BackBufferHeight!=480 ||
        automatic.hDeviceWindow!=sourceWindow)++failures;
    if (device) {
        lastWorkerThread=0;
        cooperativeResult=D3DERR_DEVICELOST;
        if (device->TestCooperativeLevel()!=D3DERR_DEVICELOST ||
            lastWorkerThread!=state.proxy_thread_id)++failures;
        cooperativeResult=static_cast<HRESULT>(0x08761232u);
        lastWorkerThread=0;
        if (device->TestCooperativeLevel()!=cooperativeResult ||
            lastWorkerThread!=state.proxy_thread_id)++failures;
        lastWindow=nullptr;
        if (device->Present(nullptr,nullptr,gameWindow,nullptr)!=presentResult ||
            lastWindow!=state.proxy_hwnd)++failures;
        IDirect3DSurface9* backbuffer=nullptr;
        lastWorkerThread=0;
        if (device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer)!=S_OK ||
            backbuffer!=reinterpret_cast<IDirect3DSurface9*>(&fakeSurface) ||
            lastWorkerThread!=GetCurrentThreadId())++failures;
        lastWorkerThread=0;
        if (!backbuffer || backbuffer->Release()!=0 ||
            lastWorkerThread!=GetCurrentThreadId())++failures;
        backbuffer=reinterpret_cast<IDirect3DSurface9*>(static_cast<uintptr_t>(1));
        getBackBufferResult=D3DERR_INVALIDCALL;lastWorkerThread=0;
        if (device->GetBackBuffer(2,3,D3DBACKBUFFER_TYPE_MONO,&backbuffer)!=D3DERR_INVALIDCALL ||
            backbuffer || lastWorkerThread!=GetCurrentThreadId())++failures;
        getBackBufferResult=S_OK;
        IDirect3DTexture9* texture=nullptr;
        createTextureResult=S_OK;lastWorkerThread=0;
        if (device->CreateTexture(640,480,1,D3DUSAGE_RENDERTARGET,D3DFMT_X8R8G8B8,
                                  D3DPOOL_DEFAULT,&texture,nullptr)!=S_OK ||
            texture!=reinterpret_cast<IDirect3DTexture9*>(&fakeTexture) ||
            lastWorkerThread!=GetCurrentThreadId())++failures;
        lastWorkerThread=0;
        if (!texture || texture->Release()!=0 ||
            lastWorkerThread!=GetCurrentThreadId())++failures;
        texture=reinterpret_cast<IDirect3DTexture9*>(static_cast<uintptr_t>(1));
        createTextureResult=D3DERR_INVALIDCALL;lastWorkerThread=0;
        if (device->CreateTexture(1024,768,1,D3DUSAGE_RENDERTARGET,D3DFMT_X8R8G8B8,
                                  D3DPOOL_DEFAULT,&texture,nullptr)!=D3DERR_INVALIDCALL ||
            texture || lastWorkerThread!=GetCurrentThreadId())++failures;
        D3DPRESENT_PARAMETERS reset{};
        reset.Windowed=TRUE;reset.hDeviceWindow=sourceWindow;

        void* originalClone[119]{};
        InitializeDeviceTable(originalClone);
        fakeDevice.table=originalClone;
        resetResult=D3DERR_DEVICELOST;
        lastWorkerThread=0;
        if (!quiet.RefreshDeviceHooks() || HasOriginalDeviceSlot(originalClone) ||
            device->Reset(&reset)!=resetResult || lastWorkerThread!=state.proxy_thread_id ||
            resetCalls!=1)++failures;

        void* inheritedHookClone[119]{};
        std::memcpy(inheritedHookClone,originalClone,sizeof(inheritedHookClone));
        fakeDevice.table=inheritedHookClone;
        resetResult=S_OK;
        lastWorkerThread=0;
        if (!quiet.RefreshDeviceHooks() ||
            device->Reset(&reset)!=resetResult || lastWorkerThread!=state.proxy_thread_id ||
            lastParameters.hDeviceWindow!=state.proxy_hwnd ||
            reset.BackBufferWidth!=640 || reset.BackBufferHeight!=480 ||
            reset.hDeviceWindow!=sourceWindow || resetCalls!=2)++failures;

        inheritedHookClone[2]=reinterpret_cast<void*>(&FakeDeviceRelease);
        inheritedHookClone[16]=reinterpret_cast<void*>(&FakeReset);
        lastWorkerThread=0;
        if (!quiet.RefreshDeviceHooks() ||
            inheritedHookClone[2]==reinterpret_cast<void*>(&FakeDeviceRelease) ||
            inheritedHookClone[16]==reinterpret_cast<void*>(&FakeReset) ||
            device->Reset(&reset)!=resetResult || lastWorkerThread!=state.proxy_thread_id ||
            resetCalls!=3)++failures;

        if (!quiet.RefreshDeviceHooks() || !quiet.RefreshDeviceHooks())++failures;
        lastWorkerThread=0;
        if (device->Reset(&reset)!=resetResult || lastWorkerThread!=state.proxy_thread_id ||
            resetCalls!=4)++failures;

        void* nativeTables[10][119]{};
        for (unsigned i=0;i<10;++i) {
            InitializeDeviceTable(nativeTables[i]);
            fakeDevice.table=nativeTables[i];
            lastWorkerThread=0;
            if (!quiet.RefreshDeviceHooks() || HasOriginalDeviceSlot(nativeTables[i]) ||
                device->Reset(&reset)!=resetResult ||
                lastWorkerThread!=state.proxy_thread_id ||
                resetCalls!=static_cast<LONG>(5+i))++failures;
        }
        fakeDevice.references=2;
        if (device->Release()!=1 || deviceReleaseCalls!=1 ||
            !quiet.RefreshDeviceHooks())++failures;
        if (device->Release()!=0 || deviceReleaseCalls!=2)++failures;
        void* afterRelease[119]{};
        InitializeDeviceTable(afterRelease);
        fakeDevice.table=afterRelease;
        if (!quiet.RefreshDeviceHooks() ||
            afterRelease[16]!=reinterpret_cast<void*>(&FakeReset))++failures;
    }
    if (sourceWindow)DestroyWindow(sourceWindow);
    if (d3d->Release()!=0 || factoryDestroyed!=1)++failures;

    state=quiet.State();
    if (state.factory_calls!=2 || state.factories!=1 || state.create_attempts!=4 ||
        state.created_devices!=1 || state.failed_devices!=3 || state.resets!=14 ||
        state.presents!=1 || state.cooperative_checks!=2 || state.last_create_hresult!=S_OK ||
        state.last_reset_hresult!=resetResult || state.last_present_hresult!=presentResult ||
        state.last_cooperative_hresult!=cooperativeResult || state.last_backbuffer_hresult!=S_OK ||
        state.last_actual_width!=640 || state.last_actual_height!=480 ||
        state.texture_calls!=2 || state.texture_failures!=1 || state.texture_null_results!=1 ||
        state.render_target_texture_calls!=2 || state.render_target_texture_failures!=1 ||
        state.render_target_texture_null_results!=1 ||
        state.last_render_target_width!=1024 || state.last_render_target_height!=768 ||
        state.last_render_target_levels!=1 || state.last_render_target_usage!=D3DUSAGE_RENDERTARGET ||
        state.last_render_target_format!=D3DFMT_X8R8G8B8 ||
        state.last_render_target_pool!=D3DPOOL_DEFAULT || !state.last_render_target_null ||
        state.last_render_target_hresult!=D3DERR_INVALIDCALL ||
        !state.last_render_target_return_address ||
        resetCalls!=14 || deviceReleaseCalls!=2 || diagnosticLogs!=38 ||
        state.dispatch_failures || state.proxy_visible)++failures;
    std::printf("Quiet input-desktop D3D9 proxy policy: %d failures\n",failures);
    return failures ? 1:0;
}
