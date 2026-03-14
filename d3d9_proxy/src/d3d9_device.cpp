#include "d3d9_device.h"
#include "post_process.h"
#include "texture_manager.h"
#include "mod_manager.h"
#include "hook.h"
#include "config.h"
#include <cstdio>
#include <cmath>
// ============================================================
// ZOA Engine Hook: CRenderer::BeginDrawTexture
//
// This is the precise 3D→UI transition point. When the ZOA engine
// calls BeginDrawTexture, it switches from 3D perspective rendering
// to 2D orthographic UI rendering. We apply all post-processing
// right before this happens, so 3D gets effects but UI stays sharp.
//
// ZOA engine is statically linked into GOnline.exe (the DLLs on
// disk are never loaded). Function found via RTTI + body matching.
// ============================================================

static PostProcessor*      g_hookPostProcess  = nullptr;
static ModManager*         g_hookModManager   = nullptr;
static IDirect3DDevice9*   g_hookDevice       = nullptr;
static IDirect3DTexture9*  g_hookPreTransTex  = nullptr;
static bool                g_hookInstalled    = false;
static bool                g_inScene          = false;

// void __thiscall CRenderer::BeginDrawTexture(int w, int h, BOOL flag)
// In GOnline.exe @ RVA 0x007F7120
// Prologue: 55 8B EC 83 EC 44 (push ebp; mov ebp,esp; sub esp,44h) — 6 bytes
typedef void (__thiscall *BeginDrawTexture_t)(void* thisPtr, int w, int h, int flag);
static BeginDrawTexture_t g_origBeginDrawTexture = nullptr;

static void __fastcall Hook_BeginDrawTexture(void* thisPtr, void* /*edx*/, int w, int h, int flag)
{
    // Let the engine transition to 2D mode FIRST (disables Z, sets ortho projection).
    // This ensures all 3D rendering including fog/particles is complete.
    g_origBeginDrawTexture(thisPtr, w, h, flag);

    // flag=1 is the 3D→UI boundary (from RE of CRenderer::BeginDrawTexture).
    // flag=0 is a fade overlay call — skip it.
    // Post-processing runs AFTER BDT so we only need to restore simple 2D state,
    // not the complex 3D state (fog, lighting, transforms, etc.).
    if (flag && g_hookPostProcess && g_hookDevice) {
        // Pass pre-transparent scene capture to mod manager for SSAO fog masking
        if (g_hookModManager)
            g_hookModManager->SetPreTransTexture(g_hookPreTransTex);
        g_hookPostProcess->Apply(g_hookDevice, g_hookModManager);
    }
}

static void InstallEngineHooks(IDirect3DDevice9* pDevice, PostProcessor* pp, ModManager* mm)
{
    g_hookDevice      = pDevice;
    g_hookPostProcess = pp;
    g_hookModManager  = mm;

    // ZOA engine is statically linked into GOnline.exe
    HMODULE hExe = GetModuleHandleA(nullptr); // main executable
    if (!hExe) return;

    // CRenderer::BeginDrawTexture @ RVA 0x007F7120
    // Prologue: 55 8B EC 83 EC 44 (6 bytes)
    void* funcAddr = (void*)((uint8_t*)hExe + 0x007F7120);

    if (Hook::HookInline(funcAddr, (void*)&Hook_BeginDrawTexture,
                         (void**)&g_origBeginDrawTexture, 6)) {
        g_hookInstalled = true;
        if (g_Config.enableLog) {
            FILE* f = fopen("d3d9_proxy.log", "a");
            if (f) { fprintf(f, "Engine hook: BeginDrawTexture hooked at %p (GOnline.exe+0x7F7120)\n", funcAddr); fclose(f); }
        }
    } else {
        if (g_Config.enableLog) {
            FILE* f = fopen("d3d9_proxy.log", "a");
            if (f) { fprintf(f, "Engine hook: BeginDrawTexture hook FAILED at %p\n", funcAddr); fclose(f); }
        }
    }
}

static void RemoveEngineHooks()
{
    if (g_origBeginDrawTexture) {
        Hook::UnhookInline((void**)&g_origBeginDrawTexture);
        g_hookInstalled = false;
    }
    g_hookDevice      = nullptr;
    g_hookPostProcess = nullptr;
    g_hookModManager  = nullptr;
}

Direct3DDevice9Proxy::Direct3DDevice9Proxy(IDirect3DDevice9* pReal, IDirect3D9* pD3D)
    : m_pReal(pReal), m_pD3D(pD3D), m_refCount(1), m_inScene(false)
{
    if (g_Config.forceAnisotropic) {
        for (DWORD s = 0; s < 8; s++) {
            pReal->SetSamplerState(s, D3DSAMP_MAXANISOTROPY, g_Config.anisotropicLevel);
        }
    }

    // Check INTZ depth texture support (mods like SSAO need it)
    if (m_pD3D) {
        D3DDEVICE_CREATION_PARAMETERS cp;
        pReal->GetCreationParameters(&cp);
        D3DDISPLAYMODE dm;
        pReal->GetDisplayMode(0, &dm);
        HRESULT hr = m_pD3D->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType,
            dm.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, FOURCC_INTZ);
        m_intzSupported = SUCCEEDED(hr);
        if (g_Config.enableLog) {
            FILE* f = fopen("d3d9_proxy.log", "a");
            if (f) { fprintf(f, "INTZ depth texture: %s\n", m_intzSupported ? "supported" : "NOT supported"); fclose(f); }
        }
    }

    m_postProcess = new PostProcessor(pReal);
    SetupINTZDepth();

    // Initialize mod manager (loads mods from mods/ directory)
    m_modManager = new ModManager();
    m_modManager->Init(pReal, "mods");
    if (m_pDepthTexture)
        m_modManager->SetDepthTexture(m_pDepthTexture);

    // Hook ZOA engine's CRenderer::BeginDrawTexture for precise 3D→UI transition
    InstallEngineHooks(pReal, m_postProcess, m_modManager);
}

Direct3DDevice9Proxy::~Direct3DDevice9Proxy()
{
    RemoveEngineHooks();
    if (m_modManager) {
        m_modManager->Shutdown();
        delete m_modManager;
        m_modManager = nullptr;
    }
    ReleasePreTransCapture();
    ReleaseINTZDepth();
    if (m_texManager) {
        m_texManager->Release();
        delete m_texManager;
        m_texManager = nullptr;
    }
    delete m_postProcess;
    if (m_pReal)
        m_pReal->Release();
}

void Direct3DDevice9Proxy::SetupINTZDepth()
{
    if (!m_intzSupported)
        return;

    // Get backbuffer size to match depth texture
    IDirect3DSurface9* pBB = nullptr;
    m_pReal->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB);
    if (!pBB) return;
    D3DSURFACE_DESC desc;
    pBB->GetDesc(&desc);
    pBB->Release();

    // Create INTZ depth texture (readable as texture + usable as depth stencil)
    HRESULT hr = m_pReal->CreateTexture(desc.Width, desc.Height, 1,
        D3DUSAGE_DEPTHSTENCIL, FOURCC_INTZ, D3DPOOL_DEFAULT,
        &m_pDepthTexture, nullptr);
    if (FAILED(hr)) {
        if (g_Config.enableLog) {
            FILE* f = fopen("d3d9_proxy.log", "a");
            if (f) { fprintf(f, "INTZ texture creation failed: 0x%08lX\n", hr); fclose(f); }
        }
        m_pDepthTexture = nullptr;
        return;
    }

    m_pDepthTexture->GetSurfaceLevel(0, &m_pDepthSurface);

    // Replace the default depth stencil with our INTZ surface
    m_pReal->SetDepthStencilSurface(m_pDepthSurface);

    if (g_Config.enableLog) {
        FILE* f = fopen("d3d9_proxy.log", "a");
        if (f) { fprintf(f, "INTZ depth: %ux%u created and active\n", desc.Width, desc.Height); fclose(f); }
    }

    // Pass depth texture to post processor
    if (m_postProcess)
        m_postProcess->SetDepthTexture(m_pDepthTexture);
}

void Direct3DDevice9Proxy::ReleaseINTZDepth()
{
    if (m_postProcess)
        m_postProcess->SetDepthTexture(nullptr);
    if (m_pDepthSurface) { m_pDepthSurface->Release(); m_pDepthSurface = nullptr; }
    if (m_pDepthTexture) { m_pDepthTexture->Release(); m_pDepthTexture = nullptr; }
}

void Direct3DDevice9Proxy::SetupPreTransCapture()
{
    IDirect3DSurface9* pBB = nullptr;
    m_pReal->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB);
    if (!pBB) return;
    D3DSURFACE_DESC desc;
    pBB->GetDesc(&desc);
    pBB->Release();

    HRESULT hr = m_pReal->CreateTexture(desc.Width, desc.Height, 1,
        D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
        &m_pPreTransTex, nullptr);
    if (SUCCEEDED(hr))
        m_pPreTransTex->GetSurfaceLevel(0, &m_pPreTransSurf);
}

void Direct3DDevice9Proxy::ReleasePreTransCapture()
{
    if (m_pPreTransSurf) { m_pPreTransSurf->Release(); m_pPreTransSurf = nullptr; }
    if (m_pPreTransTex)  { m_pPreTransTex->Release();  m_pPreTransTex = nullptr; }
}

// ============================================================
// IUnknown
// ============================================================

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::QueryInterface(REFIID riid, void** ppvObj)
{
    HRESULT hr = m_pReal->QueryInterface(riid, ppvObj);
    if (SUCCEEDED(hr))
        *ppvObj = this;
    return hr;
}

ULONG STDMETHODCALLTYPE Direct3DDevice9Proxy::AddRef()
{ return ++m_refCount; }

ULONG STDMETHODCALLTYPE Direct3DDevice9Proxy::Release()
{
    ULONG count = --m_refCount;
    if (count == 0) delete this;
    return count;
}

// ============================================================
// Key interception points
// ============================================================

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::BeginScene()
{
    m_inScene = true;
    g_inScene = true;
    m_drawCount = 0;
    m_capturedPreTrans = false;
    m_zWriteWasEnabled = true;
    g_hookPreTransTex = nullptr;

    // Lazy-install engine hook: zoa_graphics.dll may not be loaded
    // at d3d9.dll init time, so we retry each frame until it succeeds.
    if (!g_hookInstalled && m_postProcess)
        InstallEngineHooks(m_pReal, m_postProcess, m_modManager);

    return m_pReal->BeginScene();
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::EndScene()
{
    // Call mod frame hooks
    if (m_inScene && m_modManager)
        m_modManager->OnEndScene(m_pReal);

    // Post-processing is applied by the BeginDrawTexture engine hook,
    // NOT here. This ensures it runs at the precise 3D→UI boundary.

    m_inScene = false;
    g_inScene = false;
    return m_pReal->EndScene();
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::Present(
    CONST RECT* pSourceRect, CONST RECT* pDestRect,
    HWND hDestWindowOverride, CONST RGNDATA* pDirtyRegion)
{
    return m_pReal->Present(pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::Reset(D3DPRESENT_PARAMETERS* pPresentationParameters)
{
    ReleasePreTransCapture();
    ReleaseINTZDepth();
    if (m_modManager)
        m_modManager->OnReset();
    if (m_postProcess)
        m_postProcess->OnLostDevice();
    if (m_texManager)
        m_texManager->OnLostDevice();

    HRESULT hr = m_pReal->Reset(pPresentationParameters);

    if (SUCCEEDED(hr)) {
        SetupINTZDepth();
        if (m_postProcess)
            m_postProcess->OnResetDevice(m_pReal);
        if (m_texManager)
            m_texManager->OnResetDevice(m_pReal);
        if (m_modManager)
            m_modManager->OnResetComplete(m_pReal);
    }

    return hr;
}

// Anisotropic filtering — only upgrade LINEAR->ANISOTROPIC, never touch POINT/NONE
// ZOA engine uses POINT for shadow maps and render targets; changing those breaks rendering.
HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    if (g_Config.forceAnisotropic) {
        if (Type == D3DSAMP_MINFILTER && Value == D3DTEXF_LINEAR) {
            // Only upgrade LINEAR to ANISOTROPIC; leave POINT (shadow/depth) untouched
            Value = D3DTEXF_ANISOTROPIC;
        }
        if (Type == D3DSAMP_MAXANISOTROPY) {
            Value = g_Config.anisotropicLevel;
        }
    }
    if (g_Config.forceTrilinear && Type == D3DSAMP_MIPFILTER && Value == D3DTEXF_LINEAR) {
        // Already linear, keep it (don't force on POINT/NONE)
        Value = D3DTEXF_LINEAR;
    }
    return m_pReal->SetSamplerState(Sampler, Type, Value);
}

// ============================================================
// Passthrough methods
// ============================================================

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::TestCooperativeLevel()
{ return m_pReal->TestCooperativeLevel(); }

UINT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetAvailableTextureMem()
{ return m_pReal->GetAvailableTextureMem(); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::EvictManagedResources()
{ return m_pReal->EvictManagedResources(); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetDirect3D(IDirect3D9** ppD3D9)
{
    if (ppD3D9) {
        *ppD3D9 = m_pD3D;
        m_pD3D->AddRef();
        return D3D_OK;
    }
    return D3DERR_INVALIDCALL;
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetDeviceCaps(D3DCAPS9* pCaps)
{ return m_pReal->GetDeviceCaps(pCaps); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE* pMode)
{ return m_pReal->GetDisplayMode(iSwapChain, pMode); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS* pParameters)
{ return m_pReal->GetCreationParameters(pParameters); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9* pCursorBitmap)
{ return m_pReal->SetCursorProperties(XHotSpot, YHotSpot, pCursorBitmap); }

void STDMETHODCALLTYPE Direct3DDevice9Proxy::SetCursorPosition(int X, int Y, DWORD Flags)
{ m_pReal->SetCursorPosition(X, Y, Flags); }

BOOL STDMETHODCALLTYPE Direct3DDevice9Proxy::ShowCursor(BOOL bShow)
{ return m_pReal->ShowCursor(bShow); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS* pPresentationParameters, IDirect3DSwapChain9** pSwapChain)
{ return m_pReal->CreateAdditionalSwapChain(pPresentationParameters, pSwapChain); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9** pSwapChain)
{ return m_pReal->GetSwapChain(iSwapChain, pSwapChain); }

UINT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetNumberOfSwapChains()
{ return m_pReal->GetNumberOfSwapChains(); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9** ppBackBuffer)
{ return m_pReal->GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS* pRasterStatus)
{ return m_pReal->GetRasterStatus(iSwapChain, pRasterStatus); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetDialogBoxMode(BOOL bEnableDialogs)
{ return m_pReal->SetDialogBoxMode(bEnableDialogs); }

void STDMETHODCALLTYPE Direct3DDevice9Proxy::SetGammaRamp(UINT iSwapChain, DWORD Flags, CONST D3DGAMMARAMP* pRamp)
{ m_pReal->SetGammaRamp(iSwapChain, Flags, pRamp); }

void STDMETHODCALLTYPE Direct3DDevice9Proxy::GetGammaRamp(UINT iSwapChain, D3DGAMMARAMP* pRamp)
{ m_pReal->GetGammaRamp(iSwapChain, pRamp); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9** ppTexture, HANDLE* pSharedHandle)
{ return m_pReal->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9** ppVolumeTexture, HANDLE* pSharedHandle)
{ return m_pReal->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9** ppCubeTexture, HANDLE* pSharedHandle)
{ return m_pReal->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9** ppVertexBuffer, HANDLE* pSharedHandle)
{ return m_pReal->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9** ppIndexBuffer, HANDLE* pSharedHandle)
{ return m_pReal->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
{ return m_pReal->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
{ return m_pReal->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::UpdateSurface(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestinationSurface, CONST POINT* pDestPoint)
{ return m_pReal->UpdateSurface(pSourceSurface, pSourceRect, pDestinationSurface, pDestPoint); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::UpdateTexture(IDirect3DBaseTexture9* pSourceTexture, IDirect3DBaseTexture9* pDestinationTexture)
{ return m_pReal->UpdateTexture(pSourceTexture, pDestinationTexture); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetRenderTargetData(IDirect3DSurface9* pRenderTarget, IDirect3DSurface9* pDestSurface)
{ return m_pReal->GetRenderTargetData(pRenderTarget, pDestSurface); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9* pDestSurface)
{ return m_pReal->GetFrontBufferData(iSwapChain, pDestSurface); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::StretchRect(IDirect3DSurface9* pSourceSurface, CONST RECT* pSourceRect, IDirect3DSurface9* pDestSurface, CONST RECT* pDestRect, D3DTEXTUREFILTERTYPE Filter)
{ return m_pReal->StretchRect(pSourceSurface, pSourceRect, pDestSurface, pDestRect, Filter); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::ColorFill(IDirect3DSurface9* pSurface, CONST RECT* pRect, D3DCOLOR color)
{ return m_pReal->ColorFill(pSurface, pRect, color); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9** ppSurface, HANDLE* pSharedHandle)
{ return m_pReal->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9* pRenderTarget)
{ return m_pReal->SetRenderTarget(RenderTargetIndex, pRenderTarget); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9** ppRenderTarget)
{ return m_pReal->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetDepthStencilSurface(IDirect3DSurface9* pNewZStencil)
{ return m_pReal->SetDepthStencilSurface(pNewZStencil); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetDepthStencilSurface(IDirect3DSurface9** ppZStencilSurface)
{ return m_pReal->GetDepthStencilSurface(ppZStencilSurface); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::Clear(DWORD Count, CONST D3DRECT* pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil)
{ return m_pReal->Clear(Count, pRects, Flags, Color, Z, Stencil); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
{
    // Capture projection matrix to extract near/far planes for SSAO
    if (State == D3DTS_PROJECTION && pMatrix && m_postProcess && !m_inPostProcess) {
        float p34 = pMatrix->m[2][3];
        if (p34 != 0.0f) {
            float p33 = pMatrix->m[2][2];
            float p43 = pMatrix->m[3][2];
            if (p33 != 0.0f) {
                float nearZ = -p43 / p33;
                float farZ = p43 / (1.0f - p33);
                if (nearZ > 0.0f && farZ > nearZ) {
                    m_projParams[0] = nearZ;
                    m_projParams[1] = farZ;
                    m_postProcess->SetProjParams(nearZ, farZ);
                    if (m_modManager)
                        m_modManager->SetProjParams(nearZ, farZ);
                }
            }
        }
    }
    return m_pReal->SetTransform(State, pMatrix);
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX* pMatrix)
{ return m_pReal->GetTransform(State, pMatrix); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX* pMatrix)
{ return m_pReal->MultiplyTransform(State, pMatrix); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetViewport(CONST D3DVIEWPORT9* pViewport)
{ return m_pReal->SetViewport(pViewport); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetViewport(D3DVIEWPORT9* pViewport)
{ return m_pReal->GetViewport(pViewport); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetMaterial(CONST D3DMATERIAL9* pMaterial)
{ return m_pReal->SetMaterial(pMaterial); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetMaterial(D3DMATERIAL9* pMaterial)
{ return m_pReal->GetMaterial(pMaterial); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetLight(DWORD Index, CONST D3DLIGHT9* pLight)
{ return m_pReal->SetLight(Index, pLight); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetLight(DWORD Index, D3DLIGHT9* pLight)
{ return m_pReal->GetLight(Index, pLight); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::LightEnable(DWORD Index, BOOL Enable)
{ return m_pReal->LightEnable(Index, Enable); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetLightEnable(DWORD Index, BOOL* pEnable)
{ return m_pReal->GetLightEnable(Index, pEnable); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetClipPlane(DWORD Index, CONST float* pPlane)
{ return m_pReal->SetClipPlane(Index, pPlane); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetClipPlane(DWORD Index, float* pPlane)
{ return m_pReal->GetClipPlane(Index, pPlane); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetRenderState(D3DRENDERSTATETYPE State, DWORD Value)
{
    // Detect opaque→transparent transition (ZWRITEENABLE TRUE→FALSE).
    // Capture backbuffer at this point as "pre-transparent" scene for SSAO fog masking.
    if (State == D3DRS_ZWRITEENABLE && !m_inPostProcess) {
        if (m_zWriteWasEnabled && Value == FALSE && !m_capturedPreTrans && m_inScene) {
            if (!m_pPreTransTex)
                SetupPreTransCapture();
            if (m_pPreTransSurf) {
                IDirect3DSurface9* pBB = nullptr;
                if (SUCCEEDED(m_pReal->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBB)) && pBB) {
                    m_pReal->StretchRect(pBB, nullptr, m_pPreTransSurf, nullptr, D3DTEXF_NONE);
                    pBB->Release();
                    m_capturedPreTrans = true;
                    g_hookPreTransTex = m_pPreTransTex;
                }
            }
        }
        m_zWriteWasEnabled = (Value != FALSE);
    }
    return m_pReal->SetRenderState(State, Value);
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetRenderState(D3DRENDERSTATETYPE State, DWORD* pValue)
{ return m_pReal->GetRenderState(State, pValue); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9** ppSB)
{ return m_pReal->CreateStateBlock(Type, ppSB); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::BeginStateBlock()
{ return m_pReal->BeginStateBlock(); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::EndStateBlock(IDirect3DStateBlock9** ppSB)
{ return m_pReal->EndStateBlock(ppSB); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetClipStatus(CONST D3DCLIPSTATUS9* pClipStatus)
{ return m_pReal->SetClipStatus(pClipStatus); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetClipStatus(D3DCLIPSTATUS9* pClipStatus)
{ return m_pReal->GetClipStatus(pClipStatus); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetTexture(DWORD Stage, IDirect3DBaseTexture9** ppTexture)
{ return m_pReal->GetTexture(Stage, ppTexture); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetTexture(DWORD Stage, IDirect3DBaseTexture9* pTexture)
{
    // Track stage 0 diffuse texture for normal map lookup
    if (Stage == 0 && m_texManager && !m_inPostProcess) {
        IDirect3DTexture9* pTex2D = nullptr;
        if (pTexture && pTexture->GetType() == D3DRTYPE_TEXTURE)
            pTex2D = static_cast<IDirect3DTexture9*>(pTexture);
        m_texManager->OnSetDiffuseTexture(pTex2D);
    }
    return m_pReal->SetTexture(Stage, pTexture);
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD* pValue)
{ return m_pReal->GetTextureStageState(Stage, Type, pValue); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value)
{ return m_pReal->SetTextureStageState(Stage, Type, Value); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD* pValue)
{ return m_pReal->GetSamplerState(Sampler, Type, pValue); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::ValidateDevice(DWORD* pNumPasses)
{ return m_pReal->ValidateDevice(pNumPasses); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY* pEntries)
{ return m_pReal->SetPaletteEntries(PaletteNumber, pEntries); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY* pEntries)
{ return m_pReal->GetPaletteEntries(PaletteNumber, pEntries); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetCurrentTexturePalette(UINT PaletteNumber)
{ return m_pReal->SetCurrentTexturePalette(PaletteNumber); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetCurrentTexturePalette(UINT* PaletteNumber)
{ return m_pReal->GetCurrentTexturePalette(PaletteNumber); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetScissorRect(CONST RECT* pRect)
{ return m_pReal->SetScissorRect(pRect); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetScissorRect(RECT* pRect)
{ return m_pReal->GetScissorRect(pRect); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetSoftwareVertexProcessing(BOOL bSoftware)
{ return m_pReal->SetSoftwareVertexProcessing(bSoftware); }

BOOL STDMETHODCALLTYPE Direct3DDevice9Proxy::GetSoftwareVertexProcessing()
{ return m_pReal->GetSoftwareVertexProcessing(); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetNPatchMode(float nSegments)
{ return m_pReal->SetNPatchMode(nSegments); }

float STDMETHODCALLTYPE Direct3DDevice9Proxy::GetNPatchMode()
{ return m_pReal->GetNPatchMode(); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount)
{
    ++m_drawCount;

    // Inject normal map shader if available for current texture
    IDirect3DTexture9* pNormal = m_texManager ? m_texManager->GetCurrentNormalMap() : nullptr;
    if (pNormal && !m_inPostProcess) {
        DrawWithNormalMap(pNormal);
        HRESULT hr = m_pReal->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
        RestoreAfterNormalMap();
        return hr;
    }
    return m_pReal->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::DrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount)
{
    ++m_drawCount;

    IDirect3DTexture9* pNormal = m_texManager ? m_texManager->GetCurrentNormalMap() : nullptr;
    if (pNormal && !m_inPostProcess) {
        DrawWithNormalMap(pNormal);
        HRESULT hr = m_pReal->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
        RestoreAfterNormalMap();
        return hr;
    }
    return m_pReal->DrawIndexedPrimitive(PrimitiveType, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{ ++m_drawCount; return m_pReal->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void* pIndexData, D3DFORMAT IndexDataFormat, CONST void* pVertexStreamZeroData, UINT VertexStreamZeroStride)
{ ++m_drawCount; return m_pReal->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9* pDestBuffer, IDirect3DVertexDeclaration9* pVertexDecl, DWORD Flags)
{ return m_pReal->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateVertexDeclaration(CONST D3DVERTEXELEMENT9* pVertexElements, IDirect3DVertexDeclaration9** ppDecl)
{ return m_pReal->CreateVertexDeclaration(pVertexElements, ppDecl); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetVertexDeclaration(IDirect3DVertexDeclaration9* pDecl)
{ return m_pReal->SetVertexDeclaration(pDecl); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetVertexDeclaration(IDirect3DVertexDeclaration9** ppDecl)
{ return m_pReal->GetVertexDeclaration(ppDecl); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetFVF(DWORD FVF)
{ return m_pReal->SetFVF(FVF); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetFVF(DWORD* pFVF)
{ return m_pReal->GetFVF(pFVF); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateVertexShader(CONST DWORD* pFunction, IDirect3DVertexShader9** ppShader)
{ return m_pReal->CreateVertexShader(pFunction, ppShader); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetVertexShader(IDirect3DVertexShader9* pShader)
{ return m_pReal->SetVertexShader(pShader); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetVertexShader(IDirect3DVertexShader9** ppShader)
{ return m_pReal->GetVertexShader(ppShader); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetVertexShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
{ return m_pReal->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetVertexShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{ return m_pReal->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetVertexShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
{ return m_pReal->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetVertexShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{ return m_pReal->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetVertexShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount)
{ return m_pReal->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetVertexShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{ return m_pReal->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9* pStreamData, UINT OffsetInBytes, UINT Stride)
{ return m_pReal->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9** ppStreamData, UINT* pOffsetInBytes, UINT* pStride)
{ return m_pReal->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetStreamSourceFreq(UINT StreamNumber, UINT Setting)
{ return m_pReal->SetStreamSourceFreq(StreamNumber, Setting); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetStreamSourceFreq(UINT StreamNumber, UINT* pSetting)
{ return m_pReal->GetStreamSourceFreq(StreamNumber, pSetting); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetIndices(IDirect3DIndexBuffer9* pIndexData)
{ return m_pReal->SetIndices(pIndexData); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetIndices(IDirect3DIndexBuffer9** ppIndexData)
{ return m_pReal->GetIndices(ppIndexData); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreatePixelShader(CONST DWORD* pFunction, IDirect3DPixelShader9** ppShader)
{ return m_pReal->CreatePixelShader(pFunction, ppShader); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetPixelShader(IDirect3DPixelShader9* pShader)
{ return m_pReal->SetPixelShader(pShader); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetPixelShader(IDirect3DPixelShader9** ppShader)
{ return m_pReal->GetPixelShader(ppShader); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetPixelShaderConstantF(UINT StartRegister, CONST float* pConstantData, UINT Vector4fCount)
{ return m_pReal->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetPixelShaderConstantF(UINT StartRegister, float* pConstantData, UINT Vector4fCount)
{ return m_pReal->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetPixelShaderConstantI(UINT StartRegister, CONST int* pConstantData, UINT Vector4iCount)
{ return m_pReal->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetPixelShaderConstantI(UINT StartRegister, int* pConstantData, UINT Vector4iCount)
{ return m_pReal->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::SetPixelShaderConstantB(UINT StartRegister, CONST BOOL* pConstantData, UINT BoolCount)
{ return m_pReal->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::GetPixelShaderConstantB(UINT StartRegister, BOOL* pConstantData, UINT BoolCount)
{ return m_pReal->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }

// ============================================================
// Normal map injection helpers
// ============================================================

// Saved state for normal map shader injection (one draw call scope)
static IDirect3DPixelShader9*  s_savedPS = nullptr;
static IDirect3DBaseTexture9*  s_savedTex1 = nullptr;
static DWORD s_savedColorOp1 = D3DTOP_DISABLE;
static DWORD s_savedAlphaOp1 = D3DTOP_DISABLE;
static DWORD s_savedMinFilter1 = D3DTEXF_POINT;
static DWORD s_savedMagFilter1 = D3DTEXF_POINT;
static DWORD s_savedAddrU1 = D3DTADDRESS_WRAP;
static DWORD s_savedAddrV1 = D3DTADDRESS_WRAP;

void Direct3DDevice9Proxy::DrawWithNormalMap(IDirect3DTexture9* pNormalMap)
{
    if (!m_texManager) return;

    IDirect3DPixelShader9* pShader = m_texManager->GetNormalMapShader();
    if (!pShader) return;

    // Save state that we'll modify
    m_pReal->GetPixelShader(&s_savedPS);
    m_pReal->GetTexture(1, &s_savedTex1);
    m_pReal->GetTextureStageState(1, D3DTSS_COLOROP, &s_savedColorOp1);
    m_pReal->GetTextureStageState(1, D3DTSS_ALPHAOP, &s_savedAlphaOp1);
    m_pReal->GetSamplerState(1, D3DSAMP_MINFILTER, &s_savedMinFilter1);
    m_pReal->GetSamplerState(1, D3DSAMP_MAGFILTER, &s_savedMagFilter1);
    m_pReal->GetSamplerState(1, D3DSAMP_ADDRESSU, &s_savedAddrU1);
    m_pReal->GetSamplerState(1, D3DSAMP_ADDRESSV, &s_savedAddrV1);

    // Set normal map on stage 1
    m_pReal->SetTexture(1, pNormalMap);
    m_pReal->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    m_pReal->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    m_pReal->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP);
    m_pReal->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP);

    // Set pixel shader and constants
    m_pReal->SetPixelShader(pShader);
    float params[4] = { 0.5f, 0.2f, 0.3f, 0.7f };
    m_pReal->SetPixelShaderConstantF(0, params, 1);
}

void Direct3DDevice9Proxy::RestoreAfterNormalMap()
{
    // Restore saved state
    m_pReal->SetPixelShader(s_savedPS);
    m_pReal->SetTexture(1, s_savedTex1);
    m_pReal->SetTextureStageState(1, D3DTSS_COLOROP, s_savedColorOp1);
    m_pReal->SetTextureStageState(1, D3DTSS_ALPHAOP, s_savedAlphaOp1);
    m_pReal->SetSamplerState(1, D3DSAMP_MINFILTER, s_savedMinFilter1);
    m_pReal->SetSamplerState(1, D3DSAMP_MAGFILTER, s_savedMagFilter1);
    m_pReal->SetSamplerState(1, D3DSAMP_ADDRESSU, s_savedAddrU1);
    m_pReal->SetSamplerState(1, D3DSAMP_ADDRESSV, s_savedAddrV1);

    if (s_savedPS) { s_savedPS->Release(); s_savedPS = nullptr; }
    if (s_savedTex1) { s_savedTex1->Release(); s_savedTex1 = nullptr; }
}

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::DrawRectPatch(UINT Handle, CONST float* pNumSegs, CONST D3DRECTPATCH_INFO* pRectPatchInfo)
{ return m_pReal->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::DrawTriPatch(UINT Handle, CONST float* pNumSegs, CONST D3DTRIPATCH_INFO* pTriPatchInfo)
{ return m_pReal->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::DeletePatch(UINT Handle)
{ return m_pReal->DeletePatch(Handle); }

HRESULT STDMETHODCALLTYPE Direct3DDevice9Proxy::CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9** ppQuery)
{ return m_pReal->CreateQuery(Type, ppQuery); }
