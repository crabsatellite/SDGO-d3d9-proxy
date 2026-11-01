#pragma once
#include <windows.h>
#include <d3d9.h>
#include <cstdint>
#include <cwchar>
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
        uint32_t query_calls,create_attempts,created_devices,failed_devices,resets,presents;
        HRESULT last_create_hresult,last_reset_hresult,last_present_hresult;
        HWND proxy_hwnd;
        DWORD proxy_thread_id;
        uint32_t proxy_visible,proxy_noactivate,proxy_toolwindow,proxy_appwindow;
        const wchar_t* proxy_class;
        const wchar_t* proxy_desktop;
    };

    bool Install(const SdgoApi* api,SdgoMod* mod) {
        if (installed_)return true;
        if (!api || !api->HookImport || (active_ && active_!=this))return false;
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
            Read(resets_),Read(presents_),ReadResult(lastCreateResult_),ReadResult(lastResetResult_),
            ReadResult(lastPresentResult_),proxy,proxyThreadId_,proxy && IsWindowVisible(proxy) ? 1u:0u,
            (style&WS_EX_NOACTIVATE) ? 1u:0u,(style&WS_EX_TOOLWINDOW) ? 1u:0u,
            (style&WS_EX_APPWINDOW) ? 1u:0u,ProxyClassName(),proxyDesktop_
        };
    }

private:
    static constexpr DWORD DispatchTimeoutMs=30000;
    using DispatchFn=void(*)(QuietGraphics*,void*);
    struct DispatchItem {
        DispatchFn invoke;
        void* payload;
        HANDLE done;
        volatile LONG references;
        void(*destroy)(void*);
    };
    using DeviceSwapChainFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*,IDirect3DSwapChain9**);
    using DeviceResetFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
    using DevicePresentFn=HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*,const RECT*,const RECT*,HWND,const RGNDATA*);
    struct DeviceVtable {
        void** table;
        DeviceSwapChainFn swapChain;
        DeviceResetFn reset;
        DevicePresentFn present;
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
    static D3DPRESENT_PARAMETERS ProxyParameters(const D3DPRESENT_PARAMETERS& input,HWND proxy) {
        D3DPRESENT_PARAMETERS value=input;value.hDeviceWindow=proxy;return value;
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
        HRESULT STDMETHODCALLTYPE CreateDevice(UINT adapter,D3DDEVTYPE,HWND,DWORD flags,
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
            };
            R r{real_,adapter,flags,parameters!=nullptr,{},nullptr,D3DERR_INVALIDCALL};
            if (parameters)r.parameters=ProxyParameters(*parameters,owner_->proxyWindow_);
            if (!owner_->Dispatch(r,[](QuietGraphics* owner,R* v){
                owner->SizeProxy(v->hasParameters ? &v->parameters:nullptr);
                const DWORD vertex=D3DCREATE_SOFTWARE_VERTEXPROCESSING|D3DCREATE_HARDWARE_VERTEXPROCESSING|
                                   D3DCREATE_MIXED_VERTEXPROCESSING;
                const DWORD quiet=(v->flags&~D3DCREATE_PUREDEVICE)|D3DCREATE_MULTITHREADED|D3DCREATE_NOWINDOWCHANGES;
                v->result=v->real->CreateDevice(v->adapter,D3DDEVTYPE_HAL,owner->proxyWindow_,
                    (quiet&~vertex)|(v->flags&vertex),v->hasParameters ? &v->parameters:nullptr,&v->device);
                if (SUCCEEDED(v->result) && v->device && !owner->PatchDevice(v->device)) {
                    v->device->Release();v->device=nullptr;v->result=E_OUTOFMEMORY;
                }
            }))r.result=D3DERR_NOTAVAILABLE;
            InterlockedExchange(&owner_->lastCreateResult_,r.result);
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
    bool PatchDevice(IDirect3DDevice9* device) {
        void** table=*reinterpret_cast<void***>(device);
        for (uint32_t i=0;i<deviceVtableCount_;++i)if (deviceVtables_[i].table==table)return true;
        if (deviceVtableCount_>=4)return false;
        DeviceVtable record{table,reinterpret_cast<DeviceSwapChainFn>(table[13]),
            reinterpret_cast<DeviceResetFn>(table[16]),reinterpret_cast<DevicePresentFn>(table[17])};
        if (!WriteVtableSlot(&table[13],reinterpret_cast<void*>(&DeviceCreateAdditionalSwapChain)))
            return false;
        if (!WriteVtableSlot(&table[16],reinterpret_cast<void*>(&DeviceReset))) {
            WriteVtableSlot(&table[13],reinterpret_cast<void*>(record.swapChain));return false;
        }
        if (!WriteVtableSlot(&table[17],reinterpret_cast<void*>(&DevicePresent))) {
            WriteVtableSlot(&table[16],reinterpret_cast<void*>(record.reset));
            WriteVtableSlot(&table[13],reinterpret_cast<void*>(record.swapChain));return false;
        }
        deviceVtables_[deviceVtableCount_++]=record;
        return true;
    }
    DeviceVtable* FindDeviceVtable(IDirect3DDevice9* device) {
        void** table=*reinterpret_cast<void***>(device);
        for (uint32_t i=0;i<deviceVtableCount_;++i)
            if (deviceVtables_[i].table==table)return &deviceVtables_[i];
        return nullptr;
    }
    static HRESULT STDMETHODCALLTYPE DeviceCreateAdditionalSwapChain(
        IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* parameters,IDirect3DSwapChain9** swapChain) {
        QuietGraphics* owner=active_;
        DeviceVtable* record=owner ? owner->FindDeviceVtable(device):nullptr;
        if (!record)return D3DERR_INVALIDCALL;
        if (swapChain)*swapChain=nullptr;
        if (parameters && !parameters->Windowed)return D3DERR_INVALIDCALL;
        struct R{IDirect3DDevice9* device;DeviceSwapChainFn original;bool hasParameters;D3DPRESENT_PARAMETERS parameters;IDirect3DSwapChain9* swapChain;HRESULT result;};
        R r{device,record->swapChain,parameters!=nullptr,{},nullptr,D3DERR_INVALIDCALL};
        if (parameters)r.parameters=ProxyParameters(*parameters,owner->proxyWindow_);
        if (!owner->Dispatch(r,[](QuietGraphics* context,R* v){
            context->SizeProxy(v->hasParameters ? &v->parameters:nullptr);
            v->result=v->original(v->device,v->hasParameters ? &v->parameters:nullptr,&v->swapChain);
        }))return D3DERR_NOTAVAILABLE;
        if (SUCCEEDED(r.result) && swapChain)*swapChain=r.swapChain;return r.result;
    }
    static HRESULT STDMETHODCALLTYPE DeviceReset(IDirect3DDevice9* device,D3DPRESENT_PARAMETERS* parameters) {
        QuietGraphics* owner=active_;
        DeviceVtable* record=owner ? owner->FindDeviceVtable(device):nullptr;
        if (!record)return D3DERR_INVALIDCALL;
        InterlockedIncrement(&owner->resets_);
        if (parameters && !parameters->Windowed) {
            InterlockedExchange(&owner->lastResetResult_,D3DERR_INVALIDCALL);
            return D3DERR_INVALIDCALL;
        }
        struct R{IDirect3DDevice9* device;DeviceResetFn original;bool hasParameters;D3DPRESENT_PARAMETERS parameters;HRESULT result;};
        R r{device,record->reset,parameters!=nullptr,{},D3DERR_INVALIDCALL};
        if (parameters)r.parameters=ProxyParameters(*parameters,owner->proxyWindow_);
        if (!owner->Dispatch(r,[](QuietGraphics* context,R* v){
            context->SizeProxy(v->hasParameters ? &v->parameters:nullptr);
            v->result=v->original(v->device,v->hasParameters ? &v->parameters:nullptr);
        }))r.result=D3DERR_NOTAVAILABLE;
        InterlockedExchange(&owner->lastResetResult_,r.result);
        return r.result;
    }
    static HRESULT STDMETHODCALLTYPE DevicePresent(IDirect3DDevice9* device,const RECT* source,
                                                   const RECT* destination,HWND,const RGNDATA* dirty) {
        QuietGraphics* owner=active_;
        DeviceVtable* record=owner ? owner->FindDeviceVtable(device):nullptr;
        if (!record)return D3DERR_INVALIDCALL;
        InterlockedIncrement(&owner->presents_);
        const HRESULT result=record->present(device,source,destination,owner->proxyWindow_,dirty);
        InterlockedExchange(&owner->lastPresentResult_,result);return result;
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
    DeviceVtable deviceVtables_[4]{};
    uint32_t deviceVtableCount_=0;
    mutable volatile LONG dispatchFailures_=0,factoryCalls_=0,factories_=0,queryCalls_=0;
    mutable volatile LONG createAttempts_=0,createdDevices_=0,failedDevices_=0,resets_=0,presents_=0;
    mutable volatile LONG lastCreateResult_=E_PENDING,lastResetResult_=E_PENDING,lastPresentResult_=E_PENDING;
};
