// ============================================================
// Cinematic Depth of Field
//
// High-quality bokeh-style DoF using depth buffer:
// 1. Compute circle of confusion (CoC) from linear depth
// 2. 16-sample Poisson disc blur weighted by CoC
// 3. Smooth near/far plane transitions
// 4. Preserves sharp focus at focal distance
//
// Simulates real camera lens behavior with configurable
// focal distance, aperture width, and bokeh shape.
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;

// Simple 2-pass separable Gaussian blur weighted by CoC.
// Pass 1: compute CoC + horizontal blur.  Pass 2: vertical blur.
// Each pass only needs 9 taps — well within ps_2_0 limits.

static const char* g_szCocBlurH = R"(
    sampler2D sceneTex : register(s0);
    sampler2D depthTex : register(s1);

    // c0: x=1/width, y=1/height, z=focusDist, w=aperture
    // c1: x=nearZ, y=farZ, z=maxBlur, w=focusFalloff
    float4 params0 : register(c0);
    float4 params1 : register(c1);

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float z = tex2D(depthTex, uv).r;

        // Skip sky/UI/loading screens (no depth written)
        if (z > 0.999)
            return tex2D(sceneTex, uv);

        float n = params1.x;
        float f = params1.y;
        float depth = (n * f) / (f - z * (f - n));

        // Only blur far objects (beyond focus distance), keep near sharp
        float coc = max(depth - params0.z, 0.0) / params1.w;
        coc = saturate(coc) * params0.w;
        coc = min(coc, params1.z);

        if (coc < 0.5)
            return tex2D(sceneTex, uv);

        // 9-tap horizontal Gaussian
        float weights[5] = { 0.227027, 0.194596, 0.121622, 0.054054, 0.016216 };
        float dx = params0.x * coc;

        float4 color = tex2D(sceneTex, uv) * weights[0];
        color += tex2D(sceneTex, uv + float2(dx,   0)) * weights[1];
        color += tex2D(sceneTex, uv - float2(dx,   0)) * weights[1];
        color += tex2D(sceneTex, uv + float2(dx*2, 0)) * weights[2];
        color += tex2D(sceneTex, uv - float2(dx*2, 0)) * weights[2];
        color += tex2D(sceneTex, uv + float2(dx*3, 0)) * weights[3];
        color += tex2D(sceneTex, uv - float2(dx*3, 0)) * weights[3];
        color += tex2D(sceneTex, uv + float2(dx*4, 0)) * weights[4];
        color += tex2D(sceneTex, uv - float2(dx*4, 0)) * weights[4];
        return color;
    }
)";

static const char* g_szCocBlurV = R"(
    sampler2D sceneTex : register(s0);
    sampler2D depthTex : register(s1);

    float4 params0 : register(c0);
    float4 params1 : register(c1);

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float z = tex2D(depthTex, uv).r;

        // Skip sky/UI/loading screens (no depth written)
        if (z > 0.999)
            return tex2D(sceneTex, uv);

        float n = params1.x;
        float f = params1.y;
        float depth = (n * f) / (f - z * (f - n));

        // Only blur far objects (beyond focus distance), keep near sharp
        float coc = max(depth - params0.z, 0.0) / params1.w;
        coc = saturate(coc) * params0.w;
        coc = min(coc, params1.z);

        if (coc < 0.5)
            return tex2D(sceneTex, uv);

        float weights[5] = { 0.227027, 0.194596, 0.121622, 0.054054, 0.016216 };
        float dy = params0.y * coc;

        float4 color = tex2D(sceneTex, uv) * weights[0];
        color += tex2D(sceneTex, uv + float2(0, dy  )) * weights[1];
        color += tex2D(sceneTex, uv - float2(0, dy  )) * weights[1];
        color += tex2D(sceneTex, uv + float2(0, dy*2)) * weights[2];
        color += tex2D(sceneTex, uv - float2(0, dy*2)) * weights[2];
        color += tex2D(sceneTex, uv + float2(0, dy*3)) * weights[3];
        color += tex2D(sceneTex, uv - float2(0, dy*3)) * weights[3];
        color += tex2D(sceneTex, uv + float2(0, dy*4)) * weights[4];
        color += tex2D(sceneTex, uv - float2(0, dy*4)) * weights[4];
        return color;
    }
)";

static float g_focusDist   = 80.0f;
static float g_aperture    = 4.0f;
static float g_maxBlur     = 4.0f;
static float g_focusFalloff = 120.0f;

static IDirect3DPixelShader9* g_pBlurHShader = nullptr;
static IDirect3DPixelShader9* g_pBlurVShader = nullptr;

static void SetupDofSamplers(IDirect3DDevice9* dev, IDirect3DTexture9* pDepth)
{
    dev->SetTexture(1, pDepth);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE);
}

// Pass 1: Horizontal blur weighted by CoC
static bool DofPassH(IDirect3DDevice9* dev,
                     IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                     IDirect3DTexture9* pDepth,
                     float w, float h, float nearZ, float farZ,
                     void* ud)
{
    if (!g_pBlurHShader || !pDepth) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pBlurHShader);
    dev->SetTexture(0, pSrc);
    SetupDofSamplers(dev, pDepth);

    float c0[4] = { 1.0f/w, 1.0f/h, g_focusDist, g_aperture };
    float c1[4] = { nearZ, farZ, g_maxBlur, g_focusFalloff };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);

    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetTexture(1, nullptr);
    dev->SetPixelShader(nullptr);
    return true;
}

// Pass 2: Vertical blur weighted by CoC
static bool DofPassV(IDirect3DDevice9* dev,
                     IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                     IDirect3DTexture9* pDepth,
                     float w, float h, float nearZ, float farZ,
                     void* ud)
{
    if (!g_pBlurVShader || !pDepth) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pBlurVShader);
    dev->SetTexture(0, pSrc);
    SetupDofSamplers(dev, pDepth);

    float c0[4] = { 1.0f/w, 1.0f/h, g_focusDist, g_aperture };
    float c1[4] = { nearZ, farZ, g_maxBlur, g_focusFalloff };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);

    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetTexture(1, nullptr);
    dev->SetPixelShader(nullptr);
    return true;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;
    g_focusDist    = api->GetConfigFloat("DoF", "FocusDistance", 80.0f);
    g_aperture     = api->GetConfigFloat("DoF", "Aperture",     4.0f);
    g_maxBlur      = api->GetConfigFloat("DoF", "MaxBlur",      4.0f);
    g_focusFalloff = api->GetConfigFloat("DoF", "FocusFalloff", 120.0f);

    g_pBlurHShader = api->CompilePixelShader(g_szCocBlurH, "main");
    g_pBlurVShader = api->CompilePixelShader(g_szCocBlurV, "main");
    if (!g_pBlurHShader || !g_pBlurVShader) {
        if (g_pBlurHShader) { g_pBlurHShader->Release(); g_pBlurHShader = nullptr; }
        if (g_pBlurVShader) { g_pBlurVShader->Release(); g_pBlurVShader = nullptr; }
        api->Log("mod_dof: shader compile failed");
        return false;
    }

    api->RegisterPostPass("dof_h", DofPassH, 250, nullptr);
    api->RegisterPostPass("dof_v", DofPassV, 251, nullptr);
    api->Log("mod_dof: OK (focus=%.0f aperture=%.1f falloff=%.0f)",
             g_focusDist, g_aperture, g_focusFalloff);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) {
        g_api->UnregisterPostPass("dof_h");
        g_api->UnregisterPostPass("dof_v");
    }
    if (g_pBlurHShader) { g_pBlurHShader->Release(); g_pBlurHShader = nullptr; }
    if (g_pBlurVShader) { g_pBlurVShader->Release(); g_pBlurVShader = nullptr; }
}

SDGO_MOD_EXPORT const char* ModName()    { return "Depth of Field"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
