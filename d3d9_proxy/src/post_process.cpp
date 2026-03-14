#include "post_process.h"
#include "mod_manager.h"
#include "config.h"
#include <cstdio>

// ============================================================
// PostProcessor - Resource manager + state save/restore
//
// All actual effects (FXAA, bloom, SSAO, etc.) are mod DLLs.
// This class only manages render targets and ensures the
// engine's render state is preserved across post-processing.
// ============================================================

PostProcessor::PostProcessor(IDirect3DDevice9* pDevice)
{
    CreateResources(pDevice);
}

PostProcessor::~PostProcessor()
{
    ReleaseResources();
}

bool PostProcessor::CreateResources(IDirect3DDevice9* pDevice)
{
    if (m_initialized)
        return true;

    IDirect3DSurface9* pBackBuffer = nullptr;
    pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer);
    if (!pBackBuffer) return false;

    D3DSURFACE_DESC desc;
    pBackBuffer->GetDesc(&desc);
    pBackBuffer->Release();

    m_width = desc.Width;
    m_height = desc.Height;

    // Full-res render targets for ping-pong
    HRESULT hr = pDevice->CreateTexture(m_width, m_height, 1,
        D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
        &m_pSceneTexture, nullptr);
    if (FAILED(hr)) return false;
    m_pSceneTexture->GetSurfaceLevel(0, &m_pSceneSurface);

    hr = pDevice->CreateTexture(m_width, m_height, 1,
        D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
        &m_pTempTexture, nullptr);
    if (FAILED(hr)) return false;
    m_pTempTexture->GetSurfaceLevel(0, &m_pTempSurface);

    m_initialized = true;

    if (g_Config.enableLog) {
        FILE* f = fopen("d3d9_proxy.log", "a");
        if (f) {
            fprintf(f, "PostProcessor: %ux%u (resource manager only, effects are mods)\n",
                m_width, m_height);
            fclose(f);
        }
    }
    return true;
}

void PostProcessor::ReleaseResources()
{
    if (m_pSceneSurface) { m_pSceneSurface->Release(); m_pSceneSurface = nullptr; }
    if (m_pSceneTexture) { m_pSceneTexture->Release(); m_pSceneTexture = nullptr; }
    if (m_pTempSurface)  { m_pTempSurface->Release();  m_pTempSurface = nullptr; }
    if (m_pTempTexture)  { m_pTempTexture->Release();  m_pTempTexture = nullptr; }
    m_initialized = false;
}

void PostProcessor::OnLostDevice()
{
    ReleaseResources();
}

void PostProcessor::OnResetDevice(IDirect3DDevice9* pDevice)
{
    CreateResources(pDevice);
}

void PostProcessor::Apply(IDirect3DDevice9* pDevice, ModManager* modManager)
{
    if (!modManager || !modManager->HasPostPasses())
        return;

    if (!m_initialized) {
        if (!CreateResources(pDevice))
            return;
    }

    // Get backbuffer
    IDirect3DSurface9* pBackBuffer = nullptr;
    if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) || !pBackBuffer)
        return;

    // Check if backbuffer size changed
    D3DSURFACE_DESC bbDesc;
    pBackBuffer->GetDesc(&bbDesc);
    if (bbDesc.Width != m_width || bbDesc.Height != m_height) {
        ReleaseResources();
        pBackBuffer->Release();
        if (!CreateResources(pDevice))
            return;
        if (FAILED(pDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)) || !pBackBuffer)
            return;
    }

    // Copy backbuffer to scene texture
    if (FAILED(pDevice->StretchRect(pBackBuffer, nullptr, m_pSceneSurface, nullptr, D3DTEXF_NONE))) {
        pBackBuffer->Release();
        return;
    }

    // ---- Save render state ----
    IDirect3DSurface9*        oldRT = nullptr;
    IDirect3DSurface9*        oldDS = nullptr;
    IDirect3DPixelShader9*    oldPS = nullptr;
    IDirect3DVertexShader9*   oldVS = nullptr;
    IDirect3DBaseTexture9*    oldTex0 = nullptr;
    IDirect3DBaseTexture9*    oldTex1 = nullptr;
    IDirect3DVertexDeclaration9* oldDecl = nullptr;
    DWORD oldFVF, oldZEnable, oldZWrite, oldAlphaBlend, oldAlphaTest;
    DWORD oldCull, oldStencil, oldClip, oldLight, oldFog, oldSrgbW, oldColorWrite;
    DWORD oldSrcBlend, oldDstBlend;
    DWORD oldMinFilter, oldMagFilter, oldMipFilter, oldAddrU, oldAddrV, oldSrgbTex;
    DWORD oldColorOp0, oldColorArg1_0, oldAlphaOp0, oldAlphaArg1_0;
    DWORD oldColorOp1, oldAlphaOp1;
    DWORD oldMinFilter1, oldMagFilter1, oldMipFilter1, oldAddrU1, oldAddrV1, oldSrgbTex1;

    pDevice->GetRenderTarget(0, &oldRT);
    pDevice->GetDepthStencilSurface(&oldDS);
    pDevice->GetPixelShader(&oldPS);
    pDevice->GetVertexShader(&oldVS);
    pDevice->GetTexture(0, &oldTex0);
    pDevice->GetTexture(1, &oldTex1);
    pDevice->GetVertexDeclaration(&oldDecl);
    pDevice->GetFVF(&oldFVF);
    pDevice->GetRenderState(D3DRS_ZENABLE, &oldZEnable);
    pDevice->GetRenderState(D3DRS_ZWRITEENABLE, &oldZWrite);
    pDevice->GetRenderState(D3DRS_ALPHABLENDENABLE, &oldAlphaBlend);
    pDevice->GetRenderState(D3DRS_ALPHATESTENABLE, &oldAlphaTest);
    pDevice->GetRenderState(D3DRS_CULLMODE, &oldCull);
    pDevice->GetRenderState(D3DRS_STENCILENABLE, &oldStencil);
    pDevice->GetRenderState(D3DRS_CLIPPING, &oldClip);
    pDevice->GetRenderState(D3DRS_LIGHTING, &oldLight);
    pDevice->GetRenderState(D3DRS_FOGENABLE, &oldFog);
    pDevice->GetRenderState(D3DRS_SRGBWRITEENABLE, &oldSrgbW);
    pDevice->GetRenderState(D3DRS_COLORWRITEENABLE, &oldColorWrite);
    pDevice->GetRenderState(D3DRS_SRCBLEND, &oldSrcBlend);
    pDevice->GetRenderState(D3DRS_DESTBLEND, &oldDstBlend);
    pDevice->GetSamplerState(0, D3DSAMP_MINFILTER, &oldMinFilter);
    pDevice->GetSamplerState(0, D3DSAMP_MAGFILTER, &oldMagFilter);
    pDevice->GetSamplerState(0, D3DSAMP_MIPFILTER, &oldMipFilter);
    pDevice->GetSamplerState(0, D3DSAMP_ADDRESSU, &oldAddrU);
    pDevice->GetSamplerState(0, D3DSAMP_ADDRESSV, &oldAddrV);
    pDevice->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &oldSrgbTex);
    pDevice->GetSamplerState(1, D3DSAMP_MINFILTER, &oldMinFilter1);
    pDevice->GetSamplerState(1, D3DSAMP_MAGFILTER, &oldMagFilter1);
    pDevice->GetSamplerState(1, D3DSAMP_MIPFILTER, &oldMipFilter1);
    pDevice->GetSamplerState(1, D3DSAMP_ADDRESSU, &oldAddrU1);
    pDevice->GetSamplerState(1, D3DSAMP_ADDRESSV, &oldAddrV1);
    pDevice->GetSamplerState(1, D3DSAMP_SRGBTEXTURE, &oldSrgbTex1);
    pDevice->GetTextureStageState(0, D3DTSS_COLOROP, &oldColorOp0);
    pDevice->GetTextureStageState(0, D3DTSS_COLORARG1, &oldColorArg1_0);
    pDevice->GetTextureStageState(0, D3DTSS_ALPHAOP, &oldAlphaOp0);
    pDevice->GetTextureStageState(0, D3DTSS_ALPHAARG1, &oldAlphaArg1_0);
    pDevice->GetTextureStageState(1, D3DTSS_COLOROP, &oldColorOp1);
    pDevice->GetTextureStageState(1, D3DTSS_ALPHAOP, &oldAlphaOp1);

    // ---- Setup post-processing state ----
    pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    pDevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_CLIPPING, FALSE);
    pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    pDevice->SetRenderState(D3DRS_FOGENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    pDevice->SetDepthStencilSurface(nullptr);
    pDevice->SetVertexShader(nullptr);
    pDevice->SetVertexDeclaration(nullptr);
    pDevice->SetPixelShader(nullptr);

    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);

    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    float fullW = (float)m_width;
    float fullH = (float)m_height;

    // ---- Run mod passes (sorted by priority) ----
    modManager->RunPostPasses(pDevice,
        m_pSceneTexture, m_pSceneSurface,
        m_pTempTexture, m_pTempSurface,
        m_pDepthTexture, fullW, fullH, m_nearZ, m_farZ);

    // ---- Copy result back to backbuffer ----
    pDevice->StretchRect(m_pSceneSurface, nullptr, pBackBuffer, nullptr, D3DTEXF_NONE);

    // ---- Restore render state ----
    pDevice->SetRenderTarget(0, oldRT);
    pDevice->SetDepthStencilSurface(oldDS);
    pDevice->SetPixelShader(oldPS);
    pDevice->SetVertexShader(oldVS);
    pDevice->SetTexture(0, oldTex0);
    pDevice->SetTexture(1, oldTex1);
    pDevice->SetVertexDeclaration(oldDecl);
    pDevice->SetFVF(oldFVF);
    pDevice->SetRenderState(D3DRS_ZENABLE, oldZEnable);
    pDevice->SetRenderState(D3DRS_ZWRITEENABLE, oldZWrite);
    pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, oldAlphaBlend);
    pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, oldAlphaTest);
    pDevice->SetRenderState(D3DRS_CULLMODE, oldCull);
    pDevice->SetRenderState(D3DRS_STENCILENABLE, oldStencil);
    pDevice->SetRenderState(D3DRS_CLIPPING, oldClip);
    pDevice->SetRenderState(D3DRS_LIGHTING, oldLight);
    pDevice->SetRenderState(D3DRS_FOGENABLE, oldFog);
    pDevice->SetRenderState(D3DRS_SRGBWRITEENABLE, oldSrgbW);
    pDevice->SetRenderState(D3DRS_COLORWRITEENABLE, oldColorWrite);
    pDevice->SetRenderState(D3DRS_SRCBLEND, oldSrcBlend);
    pDevice->SetRenderState(D3DRS_DESTBLEND, oldDstBlend);
    pDevice->SetSamplerState(0, D3DSAMP_MINFILTER, oldMinFilter);
    pDevice->SetSamplerState(0, D3DSAMP_MAGFILTER, oldMagFilter);
    pDevice->SetSamplerState(0, D3DSAMP_MIPFILTER, oldMipFilter);
    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSU, oldAddrU);
    pDevice->SetSamplerState(0, D3DSAMP_ADDRESSV, oldAddrV);
    pDevice->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, oldSrgbTex);
    pDevice->SetSamplerState(1, D3DSAMP_MINFILTER, oldMinFilter1);
    pDevice->SetSamplerState(1, D3DSAMP_MAGFILTER, oldMagFilter1);
    pDevice->SetSamplerState(1, D3DSAMP_MIPFILTER, oldMipFilter1);
    pDevice->SetSamplerState(1, D3DSAMP_ADDRESSU, oldAddrU1);
    pDevice->SetSamplerState(1, D3DSAMP_ADDRESSV, oldAddrV1);
    pDevice->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, oldSrgbTex1);
    pDevice->SetTextureStageState(0, D3DTSS_COLOROP, oldColorOp0);
    pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, oldColorArg1_0);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, oldAlphaOp0);
    pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, oldAlphaArg1_0);
    pDevice->SetTextureStageState(1, D3DTSS_COLOROP, oldColorOp1);
    pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, oldAlphaOp1);

    if (oldRT)   oldRT->Release();
    if (oldDS)   oldDS->Release();
    if (oldPS)   oldPS->Release();
    if (oldVS)   oldVS->Release();
    if (oldTex0) oldTex0->Release();
    if (oldTex1) oldTex1->Release();
    if (oldDecl) oldDecl->Release();
    pBackBuffer->Release();
}
