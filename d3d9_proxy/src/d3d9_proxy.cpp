#include "d3d9_proxy.h"
#include "d3d9_device.h"
#include "config.h"

Direct3D9Proxy::Direct3D9Proxy(IDirect3D9* pReal)
    : m_pReal(pReal), m_refCount(1)
{
}

Direct3D9Proxy::~Direct3D9Proxy()
{
    if (m_pReal)
        m_pReal->Release();
}

// IUnknown
HRESULT STDMETHODCALLTYPE Direct3D9Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    HRESULT hr = m_pReal->QueryInterface(riid, ppvObj);
    if (SUCCEEDED(hr))
        *ppvObj = this;
    return hr;
}

ULONG STDMETHODCALLTYPE Direct3D9Proxy::AddRef()
{
    return ++m_refCount;
}

ULONG STDMETHODCALLTYPE Direct3D9Proxy::Release()
{
    ULONG count = --m_refCount;
    if (count == 0)
        delete this;
    return count;
}

// IDirect3D9 - passthrough methods
HRESULT STDMETHODCALLTYPE Direct3D9Proxy::RegisterSoftwareDevice(void* pInitializeFunction)
{ return m_pReal->RegisterSoftwareDevice(pInitializeFunction); }

UINT STDMETHODCALLTYPE Direct3D9Proxy::GetAdapterCount()
{ return m_pReal->GetAdapterCount(); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9* pIdentifier)
{ return m_pReal->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }

UINT STDMETHODCALLTYPE Direct3D9Proxy::GetAdapterModeCount(UINT Adapter, D3DFORMAT Format)
{ return m_pReal->GetAdapterModeCount(Adapter, Format); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE* pMode)
{ return m_pReal->EnumAdapterModes(Adapter, Format, Mode, pMode); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE* pMode)
{ return m_pReal->GetAdapterDisplayMode(Adapter, pMode); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed)
{ return m_pReal->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat)
{ return m_pReal->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD* pQualityLevels)
{ return m_pReal->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat)
{ return m_pReal->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat)
{ return m_pReal->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }

HRESULT STDMETHODCALLTYPE Direct3D9Proxy::GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9* pCaps)
{ return m_pReal->GetDeviceCaps(Adapter, DeviceType, pCaps); }

HMONITOR STDMETHODCALLTYPE Direct3D9Proxy::GetAdapterMonitor(UINT Adapter)
{ return m_pReal->GetAdapterMonitor(Adapter); }

// CreateDevice - the key interception point
HRESULT STDMETHODCALLTYPE Direct3D9Proxy::CreateDevice(
    UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow,
    DWORD BehaviorFlags, D3DPRESENT_PARAMETERS* pPresentationParameters,
    IDirect3DDevice9** ppReturnedDeviceInterface)
{
    HRESULT hr = m_pReal->CreateDevice(Adapter, DeviceType, hFocusWindow,
                                        BehaviorFlags, pPresentationParameters,
                                        ppReturnedDeviceInterface);
    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        // Wrap the real device with our proxy
        *ppReturnedDeviceInterface = new Direct3DDevice9Proxy(*ppReturnedDeviceInterface, this);
    }
    return hr;
}
