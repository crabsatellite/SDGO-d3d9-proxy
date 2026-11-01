#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cwchar>
#include <intrin.h>
#include <new>
#include "sdgo/sdgo_api.h"

// Direct3D 9 cannot enumerate or create a device on a non-input WinSta0
// desktop on current Windows. Keep the game there, while one owned worker is
// attached to the input desktop and owns one never-shown graphics proxy window.
// It performs real HAL factory queries, CreateDevice and Reset calls. The
// genuine multithreaded device and genuine HRESULTs are returned to the game.
class QuietGraphics {
public:
    using Create9Fn=IDirect3D9*(WINAPI*)(UINT);
    static constexpr const wchar_t* ProxyClassName() {return L"SDGO.GameTest.GraphicsProxy";}
    struct Observation {
        uint32_t active,worker_ready,ime_disabled,dispatch_failures,factory_calls,factories;
        uint32_t query_calls,create_attempts,created_devices,failed_devices,resets,presents,cooperative_checks;
        HRESULT last_create_hresult,last_reset_hresult,last_present_hresult,last_cooperative_hresult;
        HWND proxy_hwnd;
        DWORD proxy_thread_id;
        uint32_t proxy_visible,proxy_noactivate,proxy_toolwindow,proxy_appwindow;
        const wchar_t* proxy_class;
        const wchar_t* proxy_desktop;
        HWND last_focus_hwnd,last_device_hwnd;
        uint32_t last_requested_width,last_requested_height,last_client_width,last_client_height;
        uint32_t last_resolved_width,last_resolved_height,last_actual_width,last_actual_height;
        HRESULT last_backbuffer_hresult;
        uint32_t texture_calls,texture_failures,texture_null_results;
        uint32_t render_target_texture_calls,render_target_texture_failures,render_target_texture_null_results;
        uint32_t last_render_target_width,last_render_target_height,last_render_target_levels;
        uint32_t last_render_target_usage,last_render_target_format,last_render_target_pool;
        uint32_t last_render_target_null;
        HRESULT last_render_target_hresult;
        uintptr_t last_render_target_return_address;
    };

    bool Install(const SdgoApi* api,SdgoMod* mod) {
        if (installed_)return true;
        if (!api || !api->HookImport || (active_ && active_!=this))return false;
        api_=api;mod_=mod;
        active_=this;
        if (!api->HookImport(mod,"d3d9.dll","Direct3DCreate9",reinterpret_cast<void*>(&Create9),
                            reinterpret_cast<void**>(&original_)) || !original_) {
            active_=nullptr;original_=nullptr;return false;
        }
        InitializeCriticalSection(&dispatchLock_);
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(reinterpret_cast<uintptr_t>(&WorkerEntry)),&pinnedModule_))
            return FailInstall();
        requestEvent_=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        workerReadyEvent_=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!requestEvent_ || !workerReadyEvent_)return FailInstall();
        worker_=CreateThread(nullptr,0,&WorkerEntry,this,0,&proxyThreadId_);
        if (!worker_)return FailInstall();
        HANDLE waits[2]={workerReadyEvent_,worker_};
        const DWORD wait=WaitForMultipleObjects(2,waits,FALSE,15000);
        if (wait!=WAIT_OBJECT_0 || !workerReady_ || !proxyWindow_)return FailInstall();
        installed_=true;return true;
    }

    Observation State() const {
        const HWND proxy=proxyWindow_;
        const LONG_PTR style=proxy ? GetWindowLongPtrW(proxy,GWL_EXSTYLE):0;
        return {
            installed_ ? 1u:0u,Read(workerReady_),Read(imeDisabled_),Read(dispatchFailures_),Read(factoryCalls_),Read(factories_),
            Read(queryCalls_),Read(createAttempts_),Read(createdDevices_),Read(failedDevices_),
            Read(resets_),Read(presents_),Read(cooperativeChecks_),ReadResult(lastCreateResult_),ReadResult(lastResetResult_),
            ReadResult(lastPresentResult_),ReadResult(lastCooperativeResult_),proxy,proxyThreadId_,
            proxy && IsWindowVisible(proxy) ? 1u:0u,
            (style&WS_EX_NOACTIVATE) ? 1u:0u,(style&WS_EX_TOOLWINDOW) ? 1u:0u,
            (style&WS_EX_APPWINDOW) ? 1u:0u,ProxyClassName(),proxyDesktop_,
            reinterpret_cast<HWND>(InterlockedCompareExchangePointer(&lastFocusWindow_,nullptr,nullptr)),
            reinterpret_cast<HWND>(InterlockedCompareExchangePointer(&lastDeviceWindow_,nullptr,nullptr)),
            Read(lastRequestedWidth_),Read(lastRequestedHeight_),Read(lastClientWidth_),Read(lastClientHeight_),
            Read(lastResolvedWidth_),Read(lastResolvedHeight_),Read(lastActualWidth_),Read(lastActualHeight_),
            ReadResult(lastBackbufferResult_),Read(textureCalls_),Read(textureFailures_),Read(textureNullResults_),
            Read(renderTargetTextureCalls_),Read(renderTargetTextureFailures_),Read(renderTargetTextureNullResults_),
            Read(lastRenderTargetWidth_),Read(lastRenderTargetHeight_),Read(lastRenderTargetLevels_),
            Read(lastRenderTargetUsage_),Read(lastRenderTargetFormat_),Read(lastRenderTargetPool_),
            Read(lastRenderTargetNull_),ReadResult(lastRenderTargetResult_),
            reinterpret_cast<uintptr_t>(InterlockedCompareExchangePointer(
                &lastRenderTargetReturnAddress_,nullptr,nullptr))
        };
    }

    bool RefreshDeviceHooks() {
        if (!installed_)return false;
        AcquireSRWLockExclusive(&deviceLock_);
        bool result=true;
        for (uint32_t i=0;i<deviceBindingCount_;++i) {
            if (deviceBindings_[i].device && !PatchDeviceLocked(deviceBindings_[i].device))
                result=false;
        }
        ReleaseSRWLockExclusive(&deviceLock_);
        return result;
    }

private:
    static constexpr DWORD DispatchTimeoutMs=30000;
    static constexpr uint32_t DeviceHookCount=7;
    static constexpr uint32_t DeviceVtableCapacity=64;
    static constexpr uint32_t DeviceBindingCapacity=8;
    using DispatchFn=void(*)(QuietGraphics*,void*);
    struct DispatchItem {
        DispatchFn invoke;
        void* payload;
        HANDLE done;
        volatile LONG references;
        void(*destroy)(void*);
    };
    using DeviceReleaseFn=ULONG(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    using DeviceCooperativeFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
    using DeviceSwapChainFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*,IDirect3DSwapChain9**);
    using DeviceResetFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
    using DevicePresentFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
    using DeviceGetBackBufferFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,UINT,UINT,
                                                            D3DBACKBUFFER_TYPE,IDirect3DSurface9**);
    using DeviceCreateTextureFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,UINT,UINT,UINT,DWORD,
                                                            D3DFORMAT,D3DPOOL,IDirect3DTexture9**,HANDLE*);
    using TextureReleaseFn=ULONG(STDMETHODCALLTYPE*)(IDirect3DTexture9*);
    using SurfaceReleaseFn=ULONG(STDMETHODCALLTYPE*)(IDirect3DSurface9*);
    struct DeviceVtable {
        void** table;
        DeviceReleaseFn release;
        DeviceCooperativeFn cooperative;
        DeviceSwapChainFn swapChain;
        DeviceResetFn reset;
        DevicePresentFn present;
        DeviceGetBackBufferFn getBackBuffer;
        DeviceCreateTextureFn createTexture;
    };
    struct DeviceBinding {
        IDirect3DDevice9* device;
        void** table;
    };
    struct TextureVtable {
        void** table;
        TextureReleaseFn release;
    };
    struct SurfaceVtable {
        void** table;
        SurfaceReleaseFn release;
    };

    template<class Request,class Invoke>
    bool Dispatch(Request& request,Invoke invoke) {
        if (!worker_ || !workerReady_ || dispatchBroken_)return false;
        if (GetCurrentThreadId()==proxyThreadId_) {invoke(this,&request);return true;}
        Request* payload=new(std::nothrow) Request(request);
        DispatchItem* item=new(std::nothrow) DispatchItem{};
        HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if (!payload || !item || !done) {
            delete payload;delete item;if (done)CloseHandle(done);
            InterlockedIncrement(&dispatchFailures_);return false;
        }
        void(*typedInvoke)(QuietGraphics*,Request*)=invoke;
        item->invoke=reinterpret_cast<DispatchFn>(typedInvoke);
        item->payload=payload;item->done=done;item->references=2;
        item->destroy=[](void* value){delete static_cast<Request*>(value);};
        EnterCriticalSection(&dispatchLock_);
        pending_=item;SetEvent(requestEvent_);
        HANDLE waits[2]={done,worker_};
        const DWORD wait=WaitForMultipleObjects(2,waits,FALSE,DispatchTimeoutMs);
        const bool completed=wait==WAIT_OBJECT_0;
        if (completed)request=*payload;
        else {
            InterlockedExchange(&dispatchBroken_,1);
            InterlockedIncrement(&dispatchFailures_);
        }
        LeaveCriticalSection(&dispatchLock_);
        ReleaseItem(item);
        return completed;
    }
    static void ReleaseItem(DispatchItem* item) {
        if (InterlockedDecrement(&item->references)==0) {
            item->destroy(item->payload);CloseHandle(item->done);delete item;
        }
    }
    static void Complete(DispatchItem* item) {
        SetEvent(item->done);
        ReleaseItem(item);
    }
    static LRESULT CALLBACK ProxyWindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
        return DefWindowProcW(window,message,wparam,lparam);
    }
    static DWORD WINAPI WorkerEntry(void* context) {
        return static_cast<QuietGraphics*>(context)->Worker();
    }
    bool DisableWorkerIme() {
        wchar_t path[MAX_PATH]{};
        const UINT length=GetSystemDirectoryW(path,MAX_PATH);
        if (!length || length>=MAX_PATH || length+11>=MAX_PATH)return false;
        if (wcscpy_s(path+length,MAX_PATH-length,L"\\imm32.dll"))return false;
        imm32Module_=LoadLibraryW(path);
        if (!imm32Module_)return false;
        using DisableFn=BOOL(WINAPI*)(DWORD);
        const auto disable=reinterpret_cast<DisableFn>(GetProcAddress(imm32Module_,"ImmDisableIME"));
        if (!disable || !disable(GetCurrentThreadId()))return false;
        InterlockedExchange(&imeDisabled_,1);
        return true;
    }
    DWORD Worker() {
        HDESK desktop=OpenInputDesktop(0,FALSE,GENERIC_ALL);
        if (!desktop || !SetThreadDesktop(desktop)) {
            if (desktop)CloseDesktop(desktop);
            SetEvent(workerReadyEvent_);return 1;
        }
        // ImmDisableIME must run before this thread's first top-level window
        // receives WM_CREATE. It is scoped to this graphics worker only.
        if (!DisableWorkerIme()) {
            CloseDesktop(desktop);SetEvent(workerReadyEvent_);return 4;
        }
        DWORD length=sizeof(proxyDesktop_);
        if (!GetUserObjectInformationW(desktop,UOI_NAME,proxyDesktop_,length,&length))
            proxyDesktop_[0]=L'\0';
        WNDCLASSEXW klass{sizeof(klass)};
        klass.lpfnWndProc=&ProxyWindowProc;klass.hInstance=GetModuleHandleW(nullptr);
        klass.lpszClassName=ProxyClassName();
        if (!RegisterClassExW(&klass) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) {
            CloseDesktop(desktop);SetEvent(workerReadyEvent_);return 2;
        }
        proxyWindow_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,ProxyClassName(),L"",
            WS_POPUP|WS_DISABLED,0,0,1,1,nullptr,nullptr,klass.hInstance,nullptr);
        if (!proxyWindow_) {CloseDesktop(desktop);SetEvent(workerReadyEvent_);return 3;}
        InterlockedExchange(&workerReady_,1);SetEvent(workerReadyEvent_);
        HANDLE handles[1]={requestEvent_};MSG message{};
        for (;;) {
            const DWORD wait=MsgWaitForMultipleObjects(1,handles,FALSE,INFINITE,QS_ALLINPUT);
            if (wait==WAIT_OBJECT_0) {
                DispatchItem* item=const_cast<DispatchItem*>(pending_);
                pending_=nullptr;
                if (item) {item->invoke(this,item->payload);Complete(item);}
            } else if (wait==WAIT_OBJECT_0+1) {
                while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
                    TranslateMessage(&message);DispatchMessageW(&message);
                }
            } else break;
        }
        return 0;
    }
    bool FailInstall() {
        active_=nullptr;InterlockedExchange(&dispatchBroken_,1);return false;
    }
    static uint32_t Read(volatile LONG& value) {
        return static_cast<uint32_t>(InterlockedCompareExchange(&value,0,0));
    }
    static HRESULT ReadResult(volatile LONG& value) {
        return static_cast<HRESULT>(InterlockedCompareExchange(&value,0,0));
    }
    void CountQuery() {InterlockedIncrement(&queryCalls_);}
    D3DPRESENT_PARAMETERS ProxyParameters(const D3DPRESENT_PARAMETERS& input,HWND fallbackWindow,
                                          bool observeCreate) {
        D3DPRESENT_PARAMETERS value=input;
        const HWND sourceWindow=input.hDeviceWindow ? input.hDeviceWindow:fallbackWindow;
        RECT client{};
        UINT clientWidth=0,clientHeight=0;
        if (sourceWindow && GetClientRect(sourceWindow,&client)) {
            const LONG width=client.right-client.left;
            const LONG height=client.bottom-client.top;
            if (width>0)clientWidth=static_cast<UINT>(width);
            if (height>0)clientHeight=static_cast<UINT>(height);
        }
        if (value.Windowed) {
            if (!value.BackBufferWidth && clientWidth)value.BackBufferWidth=clientWidth;
            if (!value.BackBufferHeight && clientHeight)value.BackBufferHeight=clientHeight;
        }
        value.hDeviceWindow=proxyWindow_;
        if (observeCreate) {
            InterlockedExchangePointer(&lastFocusWindow_,fallbackWindow);
            InterlockedExchangePointer(&lastDeviceWindow_,input.hDeviceWindow);
            InterlockedExchange(&lastRequestedWidth_,static_cast<LONG>(input.BackBufferWidth));
            InterlockedExchange(&lastRequestedHeight_,static_cast<LONG>(input.BackBufferHeight));
            InterlockedExchange(&lastClientWidth_,static_cast<LONG>(clientWidth));
            InterlockedExchange(&lastClientHeight_,static_cast<LONG>(clientHeight));
            InterlockedExchange(&lastResolvedWidth_,static_cast<LONG>(value.BackBufferWidth));
            InterlockedExchange(&lastResolvedHeight_,static_cast<LONG>(value.BackBufferHeight));
        }
        return value;
    }
    void SizeProxy(const D3DPRESENT_PARAMETERS* parameters) {
        if (!parameters || !proxyWindow_)return;
        const int width=parameters->BackBufferWidth ? static_cast<int>(parameters->BackBufferWidth):1;
        const int height=parameters->BackBufferHeight ? static_cast<int>(parameters->BackBufferHeight):1;
        SetWindowPos(proxyWindow_,nullptr,0,0,width,height,
                     SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE|SWP_NOSENDCHANGING|SWP_HIDEWINDOW);
    }

    class Factory final : public IDirect3D9 {
    public:
        Factory(QuietGraphics* owner,IDirect3D9* real):owner_(owner),real_(real) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
            if (!out)return E_POINTER;
            *out=nullptr;
            if (!IsEqualIID(iid,__uuidof(IUnknown)) && !IsEqualIID(iid,__uuidof(IDirect3D9)))
                return E_NOINTERFACE;
            *out=static_cast<IDirect3D9*>(this);AddRef();return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override {
            return static_cast<ULONG>(InterlockedIncrement(&references_));
        }
        ULONG STDMETHODCALLTYPE Release() override {
            const ULONG remaining=static_cast<ULONG>(InterlockedDecrement(&references_));
            if (!remaining)delete this;
            return remaining;
        }
        HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void* initialize) override {
            struct R{IDirect3D9* real;void* initialize;HRESULT result;};
            R r{real_,initialize,D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->RegisterSoftwareDevice(v->initialize);}))
                return D3DERR_NOTAVAILABLE;
            return r.result;
        }
        UINT STDMETHODCALLTYPE GetAdapterCount() override {
            struct R{IDirect3D9* real;UINT result;};R r{real_,0};owner_->CountQuery();
            owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->GetAdapterCount();});return r.result;
        }
        HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT adapter,DWORD flags,D3DADAPTER_IDENTIFIER9* out) override {
            struct R{IDirect3D9* real;UINT adapter;DWORD flags;D3DADAPTER_IDENTIFIER9 value;HRESULT result;};
            R r{real_,adapter,flags,{},D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->GetAdapterIdentifier(v->adapter,v->flags,&v->value);}))
                return D3DERR_NOTAVAILABLE;
            if (SUCCEEDED(r.result) && out)*out=r.value;return r.result;
        }
        UINT STDMETHODCALLTYPE GetAdapterModeCount(UINT adapter,D3DFORMAT format) override {
            struct R{IDirect3D9* real;UINT adapter;D3DFORMAT format;UINT result;};
            R r{real_,adapter,format,0};owner_->CountQuery();
            owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->GetAdapterModeCount(v->adapter,v->format);});
            return r.result;
        }
        HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT adapter,D3DFORMAT format,UINT mode,D3DDISPLAYMODE* out) override {
            struct R{IDirect3D9* real;UINT adapter;D3DFORMAT format;UINT mode;D3DDISPLAYMODE value;HRESULT result;};
            R r{real_,adapter,format,mode,{},D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->EnumAdapterModes(v->adapter,v->format,v->mode,&v->value);}))
                return D3DERR_NOTAVAILABLE;
            if (SUCCEEDED(r.result) && out)*out=r.value;return r.result;
        }
        HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT adapter,D3DDISPLAYMODE* out) override {
            struct R{IDirect3D9* real;UINT adapter;D3DDISPLAYMODE value;HRESULT result;};
            R r{real_,adapter,{},D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->GetAdapterDisplayMode(v->adapter,&v->value);}))
                return D3DERR_NOTAVAILABLE;
            if (SUCCEEDED(r.result) && out)*out=r.value;return r.result;
        }
        HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT adapter,D3DDEVTYPE,D3DFORMAT adapterFormat,
                                                  D3DFORMAT backBufferFormat,BOOL windowed) override {
            struct R{IDirect3D9* real;UINT adapter;D3DFORMAT adapterFormat,backBufferFormat;BOOL windowed;HRESULT result;};
            R r{real_,adapter,adapterFormat,backBufferFormat,windowed,D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->CheckDeviceType(v->adapter,D3DDEVTYPE_HAL,v->adapterFormat,v->backBufferFormat,v->windowed);}))
                return D3DERR_NOTAVAILABLE;
            return r.result;
        }
        HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT adapter,D3DDEVTYPE,D3DFORMAT adapterFormat,
                                                    DWORD usage,D3DRESOURCETYPE resourceType,
                                                    D3DFORMAT checkFormat) override {
            struct R{IDirect3D9* real;UINT adapter;D3DFORMAT adapterFormat;DWORD usage;D3DRESOURCETYPE type;D3DFORMAT checkFormat;HRESULT result;};
            R r{real_,adapter,adapterFormat,usage,resourceType,checkFormat,D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->CheckDeviceFormat(v->adapter,D3DDEVTYPE_HAL,v->adapterFormat,v->usage,v->type,v->checkFormat);}))
                return D3DERR_NOTAVAILABLE;
            return r.result;
        }
        HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT adapter,D3DDEVTYPE,D3DFORMAT format,
                                                             BOOL windowed,D3DMULTISAMPLE_TYPE type,
                                                             DWORD* quality) override {
            struct R{IDirect3D9* real;UINT adapter;D3DFORMAT format;BOOL windowed;D3DMULTISAMPLE_TYPE type;DWORD quality;HRESULT result;};
            R r{real_,adapter,format,windowed,type,0,D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->CheckDeviceMultiSampleType(v->adapter,D3DDEVTYPE_HAL,v->format,v->windowed,v->type,&v->quality);}))
                return D3DERR_NOTAVAILABLE;
            if (SUCCEEDED(r.result) && quality)*quality=r.quality;return r.result;
        }
        HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT adapter,D3DDEVTYPE,D3DFORMAT adapterFormat,
                                                         D3DFORMAT renderTargetFormat,D3DFORMAT depthFormat) override {
            struct R{IDirect3D9* real;UINT adapter;D3DFORMAT adapterFormat,renderTargetFormat,depthFormat;HRESULT result;};
            R r{real_,adapter,adapterFormat,renderTargetFormat,depthFormat,D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->CheckDepthStencilMatch(v->adapter,D3DDEVTYPE_HAL,v->adapterFormat,v->renderTargetFormat,v->depthFormat);}))
                return D3DERR_NOTAVAILABLE;
            return r.result;
        }
        HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT adapter,D3DDEVTYPE,D3DFORMAT source,
                                                              D3DFORMAT target) override {
            struct R{IDirect3D9* real;UINT adapter;D3DFORMAT source,target;HRESULT result;};
            R r{real_,adapter,source,target,D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->CheckDeviceFormatConversion(v->adapter,D3DDEVTYPE_HAL,v->source,v->target);}))
                return D3DERR_NOTAVAILABLE;
            return r.result;
        }
        HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT adapter,D3DDEVTYPE,D3DCAPS9* out) override {
            struct R{IDirect3D9* real;UINT adapter;D3DCAPS9 value;HRESULT result;};
            R r{real_,adapter,{},D3DERR_NOTAVAILABLE};owner_->CountQuery();
            if (!owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->GetDeviceCaps(v->adapter,D3DDEVTYPE_HAL,&v->value);}))
                return D3DERR_NOTAVAILABLE;
            if (SUCCEEDED(r.result) && out)*out=r.value;return r.result;
        }
        HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT adapter) override {
            struct R{IDirect3D9* real;UINT adapter;HMONITOR result;};R r{real_,adapter,nullptr};owner_->CountQuery();
            owner_->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->real->GetAdapterMonitor(v->adapter);});return r.result;
        }
        HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter,D3DDEVTYPE,HWND focusWindow,DWORD flags,
                                               D3DPRESENT_PARAMETERS* parameters,
                                               IDirect3DDevice9** returnedDevice) override {
            if (returnedDevice)*returnedDevice=nullptr;
            InterlockedIncrement(&owner_->createAttempts_);
            if (parameters && !parameters->Windowed) {
                InterlockedExchange(&owner_->lastCreateResult_,D3DERR_INVALIDCALL);
                InterlockedIncrement(&owner_->failedDevices_);
                return D3DERR_INVALIDCALL;
            }
            struct R {
                IDirect3D9* real;UINT adapter;DWORD flags;bool hasParameters;
                D3DPRESENT_PARAMETERS parameters;IDirect3DDevice9* device;HRESULT result;
                HRESULT backbufferResult;UINT actualWidth,actualHeight;
            };
            const HWND originalDeviceWindow=parameters ? parameters->hDeviceWindow:nullptr;
            R r{real_,adapter,flags,parameters!=nullptr,{},nullptr,D3DERR_INVALIDCALL,
                E_PENDING,0,0};
            if (parameters)r.parameters=owner_->ProxyParameters(*parameters,focusWindow,true);
            const bool completed=owner_->Dispatch(r,[](QuietGraphics* owner,R* v){
                owner->SizeProxy(v->hasParameters ? &v->parameters:nullptr);
                const DWORD vertex=D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_HARDWARE_VERTEXPROCESSING|
                                   D3DCREATE_MIXED_VERTEXPROCESSING;
                const DWORD quiet=(v->flags&~D3DCREATE_PUREDEVICE)|D3DCREATE_MULTITHREADED|D3DCREATE_NOWINDOWCHANGES;
                v->result=v->real->CreateDevice(v->adapter,D3DDEVTYPE_HAL,owner->proxyWindow_,
                    (quiet&~vertex)|(v->flags&vertex),v->hasParameters ? &v->parameters:nullptr,&v->device);
                if (SUCCEEDED(v->result) && v->device) {
                    IDirect3DSurface9* backbuffer=nullptr;
                    v->backbufferResult=v->device->GetBackBuffer(
                        0,0,D3DBACKBUFFER_TYPE_MONO,&backbuffer);
                    if (SUCCEEDED(v->backbufferResult) && backbuffer) {
                        D3DSURFACE_DESC description{};
                        const HRESULT descriptionResult=backbuffer->GetDesc(&description);
                        if (SUCCEEDED(descriptionResult)) {
                            v->actualWidth=description.Width;v->actualHeight=description.Height;
                        } else v->backbufferResult=descriptionResult;
                        backbuffer->Release();
                    }
                    if (!owner->PatchDevice(v->device)) {
                        v->device->Release();v->device=nullptr;v->result=E_OUTOFMEMORY;
                    }
                }
            });
            if (!completed)r.result=D3DERR_NOTAVAILABLE;
            if (completed && SUCCEEDED(r.result) && parameters) {
                const HWND logicalWindow=originalDeviceWindow;
                *parameters=r.parameters;
                parameters->hDeviceWindow=logicalWindow;
            }
            InterlockedExchange(&owner_->lastCreateResult_,r.result);
            InterlockedExchange(&owner_->lastBackbufferResult_,r.backbufferResult);
            InterlockedExchange(&owner_->lastActualWidth_,static_cast<LONG>(r.actualWidth));
            InterlockedExchange(&owner_->lastActualHeight_,static_cast<LONG>(r.actualHeight));
            if (SUCCEEDED(r.result) && r.device) {
                InterlockedIncrement(&owner_->createdDevices_);
                if (returnedDevice)*returnedDevice=r.device;
                else r.device->Release();
            } else InterlockedIncrement(&owner_->failedDevices_);
            return r.result;
        }
    private:
        ~Factory() {
            struct R{IDirect3D9* real;};R r{real_};
            owner_->Dispatch(r,[](QuietGraphics*,R* v){v->real->Release();});
        }
        QuietGraphics* owner_;
        IDirect3D9* real_;
        volatile LONG references_=1;
    };

    static bool WriteVtableSlot(void** slot,void* value) {
        DWORD oldProtection=0;
        if (!VirtualProtect(slot,sizeof(void*),PAGE_EXECUTE_READWRITE,&oldProtection))return false;
        InterlockedExchangePointer(slot,value);
        DWORD ignored=0;
        VirtualProtect(slot,sizeof(void*),oldProtection,&ignored);
        FlushInstructionCache(GetCurrentProcess(),slot,sizeof(void*));
        return true;
    }
    static uint32_t DeviceSlot(uint32_t index) {
        static constexpr uint32_t slots[DeviceHookCount]={2,3,13,16,17,18,23};
        return slots[index];
    }
    static void* DeviceHook(uint32_t index) {
        switch(index) {
            case 0:return reinterpret_cast<void*>(&DeviceRelease);
            case 1:return reinterpret_cast<void*>(&DeviceTestCooperativeLevel);
            case 2:return reinterpret_cast<void*>(&DeviceCreateAdditionalSwapChain);
            case 3:return reinterpret_cast<void*>(&DeviceReset);
            case 4:return reinterpret_cast<void*>(&DevicePresent);
            case 5:return reinterpret_cast<void*>(&DeviceGetBackBuffer);
            case 6:return reinterpret_cast<void*>(&DeviceCreateTexture);
            default:return nullptr;
        }
    }
    static void* DeviceOriginal(const DeviceVtable& record,uint32_t index) {
        switch(index) {
            case 0:return reinterpret_cast<void*>(record.release);
            case 1:return reinterpret_cast<void*>(record.cooperative);
            case 2:return reinterpret_cast<void*>(record.swapChain);
            case 3:return reinterpret_cast<void*>(record.reset);
            case 4:return reinterpret_cast<void*>(record.present);
            case 5:return reinterpret_cast<void*>(record.getBackBuffer);
            case 6:return reinterpret_cast<void*>(record.createTexture);
            default:return nullptr;
        }
    }
    static void SetDeviceOriginal(DeviceVtable& record,uint32_t index,void* original) {
        switch(index) {
            case 0:record.release=reinterpret_cast<DeviceReleaseFn>(original);break;
            case 1:record.cooperative=reinterpret_cast<DeviceCooperativeFn>(original);break;
            case 2:record.swapChain=reinterpret_cast<DeviceSwapChainFn>(original);break;
            case 3:record.reset=reinterpret_cast<DeviceResetFn>(original);break;
            case 4:record.present=reinterpret_cast<DevicePresentFn>(original);break;
            case 5:record.getBackBuffer=reinterpret_cast<DeviceGetBackBufferFn>(original);break;
            case 6:record.createTexture=reinterpret_cast<DeviceCreateTextureFn>(original);break;
        }
    }
    static bool IsDeviceHook(void* value) {
        for (uint32_t i=0;i<DeviceHookCount;++i)if (value==DeviceHook(i))return true;
        return false;
    }
    DeviceVtable* FindDeviceVtableLocked(void** table) {
        for (uint32_t i=0;i<deviceVtableCount_;++i)
            if (deviceVtables_[i].table==table)return &deviceVtables_[i];
        return nullptr;
    }
    DeviceBinding* FindDeviceBindingLocked(IDirect3DDevice9* device) {
        for (uint32_t i=0;i<deviceBindingCount_;++i)
            if (deviceBindings_[i].device==device)return &deviceBindings_[i];
        return nullptr;
    }
    bool FindUniqueOriginalLocked(uint32_t index,void** original) {
        void* found=nullptr;
        for (uint32_t i=0;i<deviceVtableCount_;++i) {
            void* const candidate=DeviceOriginal(deviceVtables_[i],index);
            if (!candidate || IsDeviceHook(candidate))continue;
            if (found && found!=candidate)return false;
            found=candidate;
        }
        *original=found;
        return found!=nullptr;
    }
    bool ResolveDeviceOriginalLocked(uint32_t index,void* current,const DeviceVtable* known,
                                     const DeviceVtable* previous,void** original) {
        if (current!=DeviceHook(index)) {
            if (!current || IsDeviceHook(current))return false;
            *original=current;return true;
        }
        void* candidate=known ? DeviceOriginal(*known,index):nullptr;
        if ((!candidate || IsDeviceHook(candidate)) && previous)
            candidate=DeviceOriginal(*previous,index);
        if ((!candidate || IsDeviceHook(candidate)) &&
            !FindUniqueOriginalLocked(index,&candidate))return false;
        if (!candidate || IsDeviceHook(candidate))return false;
        *original=candidate;return true;
    }
    bool PatchDeviceLocked(IDirect3DDevice9* device) {
        if (!device)return false;
        void** const table=*reinterpret_cast<void***>(device);
        if (!table)return false;
        DeviceBinding* binding=FindDeviceBindingLocked(device);
        if (!binding && deviceBindingCount_>=DeviceBindingCapacity)return false;
        DeviceVtable* known=FindDeviceVtableLocked(table);
        DeviceVtable* previous=binding ? FindDeviceVtableLocked(binding->table):nullptr;
        if (!known && deviceVtableCount_>=DeviceVtableCapacity)return false;

        DeviceVtable candidate=known ? *known:DeviceVtable{};
        candidate.table=table;
        void* oldValues[DeviceHookCount]{};
        bool changed[DeviceHookCount]{};
        for (uint32_t i=0;i<DeviceHookCount;++i) {
            oldValues[i]=table[DeviceSlot(i)];
            void* original=nullptr;
            if (!ResolveDeviceOriginalLocked(i,oldValues[i],known,previous,&original))
                return false;
            SetDeviceOriginal(candidate,i,original);
        }
        for (uint32_t i=0;i<DeviceHookCount;++i) {
            if (oldValues[i]==DeviceHook(i))continue;
            if (!WriteVtableSlot(&table[DeviceSlot(i)],DeviceHook(i))) {
                for (uint32_t j=0;j<i;++j) {
                    if (changed[j])
                        WriteVtableSlot(&table[DeviceSlot(j)],oldValues[j]);
                }
                return false;
            }
            changed[i]=true;
        }
        if (known)*known=candidate;
        else deviceVtables_[deviceVtableCount_++]=candidate;
        if (binding)binding->table=table;
        else deviceBindings_[deviceBindingCount_++]={device,table};
        return true;
    }
    bool PatchDevice(IDirect3DDevice9* device) {
        AcquireSRWLockExclusive(&deviceLock_);
        const bool result=PatchDeviceLocked(device);
        ReleaseSRWLockExclusive(&deviceLock_);
        return result;
    }
    bool CopyDeviceVtable(IDirect3DDevice9* device,DeviceVtable* copied) {
        if (!device || !copied)return false;
        AcquireSRWLockShared(&deviceLock_);
        void** const table=*reinterpret_cast<void***>(device);
        DeviceVtable* record=FindDeviceVtableLocked(table);
        if (!record) {
            DeviceBinding* const binding=FindDeviceBindingLocked(device);
            record=binding ? FindDeviceVtableLocked(binding->table):nullptr;
        }
        if (record)*copied=*record;
        ReleaseSRWLockShared(&deviceLock_);
        return record!=nullptr;
    }
    void ForgetDeviceLocked(IDirect3DDevice9* device) {
        for (uint32_t i=0;i<deviceBindingCount_;++i) {
            if (deviceBindings_[i].device!=device)continue;
            deviceBindings_[i]=deviceBindings_[deviceBindingCount_-1];
            deviceBindings_[deviceBindingCount_-1]={};
            --deviceBindingCount_;
            return;
        }
    }
    bool PatchTexture(IDirect3DTexture9* texture) {
        void** table=*reinterpret_cast<void***>(texture);
        for (uint32_t i=0;i<textureVtableCount_;++i)
            if (textureVtables_[i].table==table)return true;
        if (textureVtableCount_>=4)return false;
        TextureVtable& record=textureVtables_[textureVtableCount_];
        record={table,reinterpret_cast<TextureReleaseFn>(table[2])};
        if (!WriteVtableSlot(&table[2],reinterpret_cast<void*>(&TextureRelease))) {
            record={};return false;
        }
        ++textureVtableCount_;
        return true;
    }
    TextureVtable* FindTextureVtable(IDirect3DTexture9* texture) {
        void** table=*reinterpret_cast<void***>(texture);
        for (uint32_t i=0;i<textureVtableCount_;++i)
            if (textureVtables_[i].table==table)return &textureVtables_[i];
        return nullptr;
    }
    bool PatchSurface(IDirect3DSurface9* surface) {
        void** table=*reinterpret_cast<void***>(surface);
        for (uint32_t i=0;i<surfaceVtableCount_;++i)
            if (surfaceVtables_[i].table==table)return true;
        if (surfaceVtableCount_>=4)return false;
        SurfaceVtable& record=surfaceVtables_[surfaceVtableCount_];
        record={table,reinterpret_cast<SurfaceReleaseFn>(table[2])};
        if (!WriteVtableSlot(&table[2],reinterpret_cast<void*>(&SurfaceRelease))) {
            record={};return false;
        }
        ++surfaceVtableCount_;
        return true;
    }
    SurfaceVtable* FindSurfaceVtable(IDirect3DSurface9* surface) {
        void** table=*reinterpret_cast<void***>(surface);
        for (uint32_t i=0;i<surfaceVtableCount_;++i)
            if (surfaceVtables_[i].table==table)return &surfaceVtables_[i];
        return nullptr;
    }
    bool TrackRenderTarget(IDirect3DTexture9* texture) {
        for (PVOID volatile& slot:renderTargetTextures_) {
            if (InterlockedCompareExchangePointer(&slot,nullptr,nullptr)==texture)return true;
        }
        for (PVOID volatile& slot:renderTargetTextures_) {
            if (!InterlockedCompareExchangePointer(&slot,texture,nullptr)) {
                if (PatchTexture(texture))return true;
                InterlockedCompareExchangePointer(&slot,nullptr,texture);
                return false;
            }
        }
        return false;
    }
    bool IsTrackedRenderTarget(IDirect3DTexture9* texture) const {
        for (PVOID volatile const& slot:renderTargetTextures_) {
            if (InterlockedCompareExchangePointer(
                    const_cast<PVOID volatile*>(&slot),nullptr,nullptr)==texture)return true;
        }
        return false;
    }
    void ForgetRenderTarget(IDirect3DTexture9* texture) {
        for (PVOID volatile& slot:renderTargetTextures_)
            InterlockedCompareExchangePointer(&slot,nullptr,texture);
    }
    bool TrackBackBuffer(IDirect3DSurface9* surface,IDirect3DDevice9* device) {
        for (uint32_t i=0;i<8;++i) {
            if (InterlockedCompareExchangePointer(&backBufferSurfaces_[i],nullptr,nullptr)==surface) {
                InterlockedExchangePointer(&backBufferDevices_[i],device);
                return true;
            }
        }
        for (uint32_t i=0;i<8;++i) {
            if (!InterlockedCompareExchangePointer(&backBufferSurfaces_[i],surface,nullptr)) {
                InterlockedExchangePointer(&backBufferDevices_[i],device);
                if (PatchSurface(surface))return true;
                InterlockedExchangePointer(&backBufferDevices_[i],nullptr);
                InterlockedCompareExchangePointer(&backBufferSurfaces_[i],nullptr,surface);
                return false;
            }
        }
        return false;
    }
    bool IsTrackedBackBuffer(IDirect3DSurface9* surface) const {
        for (PVOID volatile const& slot:backBufferSurfaces_) {
            if (InterlockedCompareExchangePointer(
                    const_cast<PVOID volatile*>(&slot),nullptr,nullptr)==surface)return true;
        }
        return false;
    }
    IDirect3DDevice9* BackBufferDevice(IDirect3DSurface9* surface) const {
        for (uint32_t i=0;i<8;++i) {
            if (InterlockedCompareExchangePointer(
                    const_cast<PVOID volatile*>(&backBufferSurfaces_[i]),nullptr,nullptr)==surface)
                return static_cast<IDirect3DDevice9*>(InterlockedCompareExchangePointer(
                    const_cast<PVOID volatile*>(&backBufferDevices_[i]),nullptr,nullptr));
        }
        return nullptr;
    }
    void ForgetBackBuffer(IDirect3DSurface9* surface) {
        for (uint32_t i=0;i<8;++i) {
            if (InterlockedCompareExchangePointer(&backBufferSurfaces_[i],nullptr,surface)==surface)
                InterlockedExchangePointer(&backBufferDevices_[i],nullptr);
        }
    }
    static ULONG STDMETHODCALLTYPE DeviceRelease(IDirect3DDevice9* device) {
        QuietGraphics* owner=active_;
        if (!owner || !device)return 0;
        AcquireSRWLockExclusive(&owner->deviceLock_);
        void** const table=*reinterpret_cast<void***>(device);
        DeviceVtable* record=owner->FindDeviceVtableLocked(table);
        if (!record) {
            DeviceBinding* const binding=owner->FindDeviceBindingLocked(device);
            record=binding ? owner->FindDeviceVtableLocked(binding->table):nullptr;
        }
        DeviceReleaseFn const original=record ? record->release:nullptr;
        if (!original || IsDeviceHook(reinterpret_cast<void*>(original))) {
            ReleaseSRWLockExclusive(&owner->deviceLock_);
            return 0;
        }
        void** const previousTable=record->table;
        owner->ForgetDeviceLocked(device);
        ReleaseSRWLockExclusive(&owner->deviceLock_);
        const ULONG references=original(device);
        if (references) {
            AcquireSRWLockExclusive(&owner->deviceLock_);
            if (!owner->FindDeviceBindingLocked(device) &&
                owner->deviceBindingCount_<DeviceBindingCapacity) {
                owner->deviceBindings_[owner->deviceBindingCount_++]={device,previousTable};
                owner->PatchDeviceLocked(device);
            }
            ReleaseSRWLockExclusive(&owner->deviceLock_);
        }
        return references;
    }
    static HRESULT STDMETHODCALLTYPE DeviceTestCooperativeLevel(IDirect3DDevice9* device) {
        QuietGraphics* owner=active_;
        DeviceVtable record{};
        if (!owner || !owner->CopyDeviceVtable(device,&record))return D3DERR_INVALIDCALL;
        const LONG callNumber=InterlockedIncrement(&owner->cooperativeChecks_);
        const uintptr_t returnAddress=reinterpret_cast<uintptr_t>(_ReturnAddress());
        struct R{IDirect3DDevice9* device;DeviceCooperativeFn original;HRESULT result;};
        R r{device,record.cooperative,D3DERR_NOTAVAILABLE};
        if (!owner->Dispatch(r,[](QuietGraphics*,R* v){v->result=v->original(v->device);}))
            r.result=D3DERR_NOTAVAILABLE;
        InterlockedExchange(&owner->lastCooperativeResult_,r.result);
        if (FAILED(r.result) && owner->api_ && owner->api_->Log)
            owner->api_->Log(owner->mod_,SDGO_LOG_WARN,
                "quiet D3D9 TestCooperativeLevel #%ld caller=%p thread=%lu hr=0x%08lX",
                callNumber,reinterpret_cast<void*>(returnAddress),
                static_cast<unsigned long>(GetCurrentThreadId()),
                static_cast<unsigned long>(r.result));
        return r.result;
    }
    static HRESULT STDMETHODCALLTYPE DeviceGetBackBuffer(
        IDirect3DDevice9* device,UINT swapChain,UINT backBuffer,D3DBACKBUFFER_TYPE type,
        IDirect3DSurface9** returnedSurface) {
        QuietGraphics* owner=active_;
        DeviceVtable record{};
        if (!owner || !owner->CopyDeviceVtable(device,&record))return D3DERR_INVALIDCALL;
        const uintptr_t returnAddress=reinterpret_cast<uintptr_t>(_ReturnAddress());
        const HRESULT result=record.getBackBuffer(
            device,swapChain,backBuffer,type,returnedSurface);
        IDirect3DSurface9* const surface=returnedSurface ? *returnedSurface:nullptr;
        const bool nullResult=returnedSurface && !surface;
        if (surface && !owner->TrackBackBuffer(surface,device) && owner->api_ && owner->api_->Log)
            owner->api_->Log(owner->mod_,SDGO_LOG_WARN,
                "quiet D3D9 backbuffer Release observation unavailable surface=%p output=%p",
                surface,returnedSurface);
        if (owner->api_ && owner->api_->Log)
            owner->api_->Log(owner->mod_,FAILED(result) ? SDGO_LOG_WARN:SDGO_LOG_INFO,
                "quiet D3D9 GetBackBuffer caller=%p thread=%lu swapchain=%u index=%u "
                "type=%d hr=0x%08lX surface=%p output=%p null=%u",
                reinterpret_cast<void*>(returnAddress),
                static_cast<unsigned long>(GetCurrentThreadId()),swapChain,backBuffer,
                static_cast<int>(type),static_cast<unsigned long>(result),surface,
                returnedSurface,nullResult ? 1u:0u);
        return result;
    }
    static HRESULT STDMETHODCALLTYPE DeviceCreateTexture(
        IDirect3DDevice9* device,UINT width,UINT height,UINT levels,DWORD usage,D3DFORMAT format,
        D3DPOOL pool,IDirect3DTexture9** returnedTexture,HANDLE* sharedHandle) {
        QuietGraphics* owner=active_;
        DeviceVtable record{};
        if (!owner || !owner->CopyDeviceVtable(device,&record))return D3DERR_INVALIDCALL;
        const uintptr_t returnAddress=reinterpret_cast<uintptr_t>(_ReturnAddress());
        const LONG callNumber=InterlockedIncrement(&owner->textureCalls_);
        const HRESULT result=record.createTexture(
            device,width,height,levels,usage,format,pool,returnedTexture,sharedHandle);
        IDirect3DTexture9* const texture=returnedTexture ? *returnedTexture:nullptr;
        const bool nullResult=returnedTexture && !texture;
        if (FAILED(result))InterlockedIncrement(&owner->textureFailures_);
        if (nullResult)InterlockedIncrement(&owner->textureNullResults_);
        if (usage&D3DUSAGE_RENDERTARGET) {
            InterlockedIncrement(&owner->renderTargetTextureCalls_);
            if (FAILED(result))InterlockedIncrement(&owner->renderTargetTextureFailures_);
            if (nullResult)
                InterlockedIncrement(&owner->renderTargetTextureNullResults_);
            InterlockedExchange(&owner->lastRenderTargetWidth_,static_cast<LONG>(width));
            InterlockedExchange(&owner->lastRenderTargetHeight_,static_cast<LONG>(height));
            InterlockedExchange(&owner->lastRenderTargetLevels_,static_cast<LONG>(levels));
            InterlockedExchange(&owner->lastRenderTargetUsage_,static_cast<LONG>(usage));
            InterlockedExchange(&owner->lastRenderTargetFormat_,static_cast<LONG>(format));
            InterlockedExchange(&owner->lastRenderTargetPool_,static_cast<LONG>(pool));
            InterlockedExchange(&owner->lastRenderTargetNull_,nullResult ? 1:0);
            InterlockedExchange(&owner->lastRenderTargetResult_,result);
            InterlockedExchangePointer(&owner->lastRenderTargetReturnAddress_,
                reinterpret_cast<PVOID>(returnAddress));
            if (texture && !owner->TrackRenderTarget(texture) && owner->api_ && owner->api_->Log)
                owner->api_->Log(owner->mod_,SDGO_LOG_WARN,
                    "quiet D3D9 RT Release observation unavailable texture=%p output=%p",
                    texture,returnedTexture);
        }
        if (owner->api_ && owner->api_->Log &&
            (callNumber<=8 || FAILED(result) || nullResult)) {
            owner->api_->Log(owner->mod_,FAILED(result) ? SDGO_LOG_WARN:SDGO_LOG_INFO,
                "quiet D3D9 CreateTexture #%ld caller=%p width=%u height=%u levels=%u "
                "usage=0x%08lX format=%d pool=%d hr=0x%08lX texture=%p output=%p null=%u",
                callNumber,reinterpret_cast<void*>(returnAddress),width,height,levels,
                static_cast<unsigned long>(usage),static_cast<int>(format),static_cast<int>(pool),
                static_cast<unsigned long>(result),texture,returnedTexture,nullResult ? 1u:0u);
        }
        return result;
    }
    static ULONG STDMETHODCALLTYPE TextureRelease(IDirect3DTexture9* texture) {
        QuietGraphics* owner=active_;
        TextureVtable* record=owner ? owner->FindTextureVtable(texture):nullptr;
        if (!record)return 0;
        const bool tracked=owner->IsTrackedRenderTarget(texture);
        const uintptr_t returnAddress=reinterpret_cast<uintptr_t>(_ReturnAddress());
        const DWORD thread=GetCurrentThreadId();
        PVOID stack[6]{};
        const USHORT frames=tracked ? CaptureStackBackTrace(0,6,stack,nullptr):0;
        if (tracked && owner->api_ && owner->api_->Log)
            owner->api_->Log(owner->mod_,SDGO_LOG_INFO,
                "quiet D3D9 RT Release enter caller=%p texture=%p thread=%lu "
                "frames=%u stack=%p/%p/%p/%p/%p/%p",
                reinterpret_cast<void*>(returnAddress),texture,
                static_cast<unsigned long>(thread),static_cast<unsigned>(frames),
                stack[0],stack[1],stack[2],stack[3],stack[4],stack[5]);
        const ULONG references=record->release(texture);
        if (tracked) {
            if (owner->api_ && owner->api_->Log)
                owner->api_->Log(owner->mod_,SDGO_LOG_INFO,
                    "quiet D3D9 RT Release exit caller=%p texture=%p refs=%lu thread=%lu",
                    reinterpret_cast<void*>(returnAddress),texture,
                    static_cast<unsigned long>(references),
                    static_cast<unsigned long>(thread));
            if (!references)owner->ForgetRenderTarget(texture);
        }
        return references;
    }
    static ULONG STDMETHODCALLTYPE SurfaceRelease(IDirect3DSurface9* surface) {
        QuietGraphics* owner=active_;
        SurfaceVtable* record=owner ? owner->FindSurfaceVtable(surface):nullptr;
        if (!record)return 0;
        const bool tracked=owner->IsTrackedBackBuffer(surface);
        const uintptr_t returnAddress=reinterpret_cast<uintptr_t>(_ReturnAddress());
        const DWORD thread=GetCurrentThreadId();
        IDirect3DDevice9* const device=tracked ? owner->BackBufferDevice(surface):nullptr;
        void** const deviceTable=device ? *reinterpret_cast<void***>(device):nullptr;
        void* const resetSlot=deviceTable ? deviceTable[16]:nullptr;
        DeviceVtable deviceRecord{};
        const bool deviceRecordFound=device && owner->CopyDeviceVtable(device,&deviceRecord);
        PVOID stack[6]{};
        const USHORT frames=tracked ? CaptureStackBackTrace(0,6,stack,nullptr):0;
        if (tracked && owner->api_ && owner->api_->Log)
            owner->api_->Log(owner->mod_,SDGO_LOG_INFO,
                "quiet D3D9 backbuffer Release enter caller=%p surface=%p thread=%lu "
                "device=%p table=%p reset_slot=%p reset_hooked=%u record=%u "
                "frames=%u stack=%p/%p/%p/%p/%p/%p",
                reinterpret_cast<void*>(returnAddress),surface,
                static_cast<unsigned long>(thread),device,deviceTable,resetSlot,
                resetSlot==reinterpret_cast<void*>(&DeviceReset) ? 1u:0u,
                deviceRecordFound ? 1u:0u,static_cast<unsigned>(frames),
                stack[0],stack[1],stack[2],stack[3],stack[4],stack[5]);
        const ULONG references=record->release(surface);
        if (tracked) {
            if (owner->api_ && owner->api_->Log)
                owner->api_->Log(owner->mod_,SDGO_LOG_INFO,
                    "quiet D3D9 backbuffer Release exit caller=%p surface=%p refs=%lu thread=%lu",
                    reinterpret_cast<void*>(returnAddress),surface,
                    static_cast<unsigned long>(references),
                    static_cast<unsigned long>(thread));
            if (!references)owner->ForgetBackBuffer(surface);
        }
        return references;
    }
    static HRESULT STDMETHODCALLTYPE DeviceCreateAdditionalSwapChain(
        IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* parameters,IDirect3DSwapChain9** swapChain) {
        QuietGraphics* owner=active_;
        DeviceVtable record{};
        if (!owner || !owner->CopyDeviceVtable(device,&record))return D3DERR_INVALIDCALL;
        if (swapChain)*swapChain=nullptr;
        if (parameters && !parameters->Windowed)return D3DERR_INVALIDCALL;
        struct R{IDirect3DDevice9* device;DeviceSwapChainFn original;bool hasParameters;D3DPRESENT_PARAMETERS parameters;IDirect3DSwapChain9* swapChain;HRESULT result;};
        R r{device,record.swapChain,parameters!=nullptr,{},nullptr,D3DERR_INVALIDCALL};
        const HWND originalDeviceWindow=parameters ? parameters->hDeviceWindow:nullptr;
        if (parameters)r.parameters=owner->ProxyParameters(*parameters,nullptr,false);
        const bool completed=owner->Dispatch(r,[](QuietGraphics* context,R* v){
            context->SizeProxy(v->hasParameters ? &v->parameters:nullptr);
            v->result=v->original(v->device,v->hasParameters ? &v->parameters:nullptr,&v->swapChain);
        });
        if (!completed)return D3DERR_NOTAVAILABLE;
        if (SUCCEEDED(r.result) && parameters) {
            *parameters=r.parameters;parameters->hDeviceWindow=originalDeviceWindow;
        }
        if (SUCCEEDED(r.result) && swapChain)*swapChain=r.swapChain;return r.result;
    }
    static HRESULT STDMETHODCALLTYPE DeviceReset(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* parameters) {
        QuietGraphics* owner=active_;
        void** const table=device ? *reinterpret_cast<void***>(device):nullptr;
        DeviceVtable record{};
        const bool recordFound=owner && owner->CopyDeviceVtable(device,&record);
        if (!owner)return D3DERR_INVALIDCALL;
        const LONG resetNumber=InterlockedIncrement(&owner->resets_);
        const uintptr_t returnAddress=reinterpret_cast<uintptr_t>(_ReturnAddress());
        const UINT requestedWidth=parameters ? parameters->BackBufferWidth:0;
        const UINT requestedHeight=parameters ? parameters->BackBufferHeight:0;
        const D3DFORMAT requestedFormat=parameters ? parameters->BackBufferFormat:D3DFMT_UNKNOWN;
        const BOOL requestedWindowed=parameters ? parameters->Windowed:FALSE;
        if (owner->api_ && owner->api_->Log)
            owner->api_->Log(owner->mod_,SDGO_LOG_INFO,
                "quiet D3D9 Reset enter #%ld caller=%p thread=%lu device=%p table=%p "
                "slot16=%p record=%u width=%u height=%u format=%d windowed=%u",
                resetNumber,reinterpret_cast<void*>(returnAddress),
                static_cast<unsigned long>(GetCurrentThreadId()),device,table,
                table ? table[16]:nullptr,recordFound ? 1u:0u,requestedWidth,requestedHeight,
                static_cast<int>(requestedFormat),static_cast<unsigned>(requestedWindowed));
        if (!recordFound) {
            InterlockedExchange(&owner->lastResetResult_,D3DERR_INVALIDCALL);
            if (owner->api_ && owner->api_->Log)
                owner->api_->Log(owner->mod_,SDGO_LOG_WARN,
                    "quiet D3D9 Reset exit #%ld caller=%p thread=%lu device=%p table=%p "
                    "record=0 hr=0x%08lX",resetNumber,reinterpret_cast<void*>(returnAddress),
                    static_cast<unsigned long>(GetCurrentThreadId()),device,table,
                    static_cast<unsigned long>(D3DERR_INVALIDCALL));
            return D3DERR_INVALIDCALL;
        }
        if (parameters && !parameters->Windowed) {
            InterlockedExchange(&owner->lastResetResult_,D3DERR_INVALIDCALL);
            if (owner->api_ && owner->api_->Log)
                owner->api_->Log(owner->mod_,SDGO_LOG_WARN,
                    "quiet D3D9 Reset exit #%ld caller=%p thread=%lu width=%u height=%u "
                    "format=%d windowed=%u hr=0x%08lX",resetNumber,
                    reinterpret_cast<void*>(returnAddress),
                    static_cast<unsigned long>(GetCurrentThreadId()),requestedWidth,
                    requestedHeight,static_cast<int>(requestedFormat),
                    static_cast<unsigned>(requestedWindowed),
                    static_cast<unsigned long>(D3DERR_INVALIDCALL));
            return D3DERR_INVALIDCALL;
        }
        struct R{IDirect3DDevice9* device;DeviceResetFn original;bool hasParameters;D3DPRESENT_PARAMETERS parameters;HRESULT result;};
        R r{device,record.reset,parameters!=nullptr,{},D3DERR_INVALIDCALL};
        const HWND originalDeviceWindow=parameters ? parameters->hDeviceWindow:nullptr;
        if (parameters)r.parameters=owner->ProxyParameters(*parameters,nullptr,false);
        const bool completed=owner->Dispatch(r,[](QuietGraphics* context,R* v){
            context->SizeProxy(v->hasParameters ? &v->parameters:nullptr);
            v->result=v->original(v->device,v->hasParameters ? &v->parameters:nullptr);
        });
        if (!completed)r.result=D3DERR_NOTAVAILABLE;
        if (completed && SUCCEEDED(r.result) && parameters) {
            *parameters=r.parameters;parameters->hDeviceWindow=originalDeviceWindow;
        }
        InterlockedExchange(&owner->lastResetResult_,r.result);
        if (owner->api_ && owner->api_->Log)
            owner->api_->Log(owner->mod_,FAILED(r.result) ? SDGO_LOG_WARN:SDGO_LOG_INFO,
                "quiet D3D9 Reset exit #%ld caller=%p thread=%lu width=%u height=%u "
                "format=%d windowed=%u hr=0x%08lX",resetNumber,
                reinterpret_cast<void*>(returnAddress),
                static_cast<unsigned long>(GetCurrentThreadId()),requestedWidth,
                requestedHeight,static_cast<int>(requestedFormat),
                static_cast<unsigned>(requestedWindowed),static_cast<unsigned long>(r.result));
        return r.result;
    }
    static HRESULT STDMETHODCALLTYPE DevicePresent(IDirect3DDevice9* device,const RECT* source,
                                                   const RECT* destination,HWND,const RGNDATA* dirty) {
        QuietGraphics* owner=active_;
        DeviceVtable record{};
        if (!owner || !owner->CopyDeviceVtable(device,&record))return D3DERR_INVALIDCALL;
        const LONG presentNumber=InterlockedIncrement(&owner->presents_);
        const uintptr_t returnAddress=reinterpret_cast<uintptr_t>(_ReturnAddress());
        const HRESULT result=record.present(device,source,destination,owner->proxyWindow_,dirty);
        InterlockedExchange(&owner->lastPresentResult_,result);
        if (owner->api_ && owner->api_->Log && (presentNumber<=4 || FAILED(result)))
            owner->api_->Log(owner->mod_,FAILED(result) ? SDGO_LOG_WARN:SDGO_LOG_INFO,
                "quiet D3D9 Present #%ld caller=%p hr=0x%08lX",presentNumber,
                reinterpret_cast<void*>(returnAddress),static_cast<unsigned long>(result));
        return result;
    }

    static IDirect3D9* WINAPI Create9(UINT sdkVersion) {
        QuietGraphics* self=active_;
        if (!self || !self->original_)return nullptr;
        InterlockedIncrement(&self->factoryCalls_);
        struct R{Create9Fn original;UINT sdk;IDirect3D9* real;};
        R r{self->original_,sdkVersion,nullptr};
        if (!self->Dispatch(r,[](QuietGraphics*,R* v){v->real=v->original(v->sdk);}) || !r.real)
            return nullptr;
        Factory* wrapped=new(std::nothrow) Factory(self,r.real);
        if (!wrapped) {
            struct Release{IDirect3D9* real;};Release release{r.real};
            self->Dispatch(release,[](QuietGraphics*,Release* v){v->real->Release();});return nullptr;
        }
        InterlockedIncrement(&self->factories_);return wrapped;
    }

    inline static QuietGraphics* active_=nullptr;
    const SdgoApi* api_=nullptr;
    SdgoMod* mod_=nullptr;
    Create9Fn original_=nullptr;
    bool installed_=false;
    HMODULE pinnedModule_=nullptr;
    HMODULE imm32Module_=nullptr;
    CRITICAL_SECTION dispatchLock_{};
    HANDLE requestEvent_=nullptr,workerReadyEvent_=nullptr,worker_=nullptr;
    volatile DispatchItem* pending_=nullptr;
    mutable volatile LONG workerReady_=0,imeDisabled_=0;
    volatile LONG dispatchBroken_=0;
    HWND proxyWindow_=nullptr;
    DWORD proxyThreadId_=0;
    wchar_t proxyDesktop_[64]{};
    SRWLOCK deviceLock_=SRWLOCK_INIT;
    DeviceVtable deviceVtables_[DeviceVtableCapacity]{};
    uint32_t deviceVtableCount_=0;
    DeviceBinding deviceBindings_[DeviceBindingCapacity]{};
    uint32_t deviceBindingCount_=0;
    TextureVtable textureVtables_[4]{};
    uint32_t textureVtableCount_=0;
    PVOID volatile renderTargetTextures_[8]{};
    SurfaceVtable surfaceVtables_[4]{};
    uint32_t surfaceVtableCount_=0;
    PVOID volatile backBufferSurfaces_[8]{};
    PVOID volatile backBufferDevices_[8]{};
    mutable volatile LONG dispatchFailures_=0,factoryCalls_=0,factories_=0,queryCalls_=0;
    mutable volatile LONG createAttempts_=0,createdDevices_=0,failedDevices_=0,resets_=0,presents_=0;
    mutable volatile LONG cooperativeChecks_=0;
    mutable PVOID volatile lastFocusWindow_=nullptr,lastDeviceWindow_=nullptr;
    mutable volatile LONG lastRequestedWidth_=0,lastRequestedHeight_=0,lastClientWidth_=0,lastClientHeight_=0;
    mutable volatile LONG lastResolvedWidth_=0,lastResolvedHeight_=0;
    mutable volatile LONG lastActualWidth_=0,lastActualHeight_=0;
    mutable volatile LONG textureCalls_=0,textureFailures_=0,textureNullResults_=0;
    mutable volatile LONG renderTargetTextureCalls_=0,renderTargetTextureFailures_=0;
    mutable volatile LONG renderTargetTextureNullResults_=0;
    mutable volatile LONG lastRenderTargetWidth_=0,lastRenderTargetHeight_=0,lastRenderTargetLevels_=0;
    mutable volatile LONG lastRenderTargetUsage_=0,lastRenderTargetFormat_=0,lastRenderTargetPool_=0;
    mutable volatile LONG lastRenderTargetNull_=0;
    mutable PVOID volatile lastRenderTargetReturnAddress_=nullptr;
    mutable volatile LONG lastCreateResult_=E_PENDING,lastResetResult_=E_PENDING,lastPresentResult_=E_PENDING;
    mutable volatile LONG lastCooperativeResult_=E_PENDING,lastBackbufferResult_=E_PENDING;
    mutable volatile LONG lastRenderTargetResult_=E_PENDING;
};
