// ============================================================
// Depth of Field - Auto-focus
//
// Fixes the "everything blurry in battle" problem by dynamically
// determining focus distance from the scene depth buffer instead
// of using a fixed value.
//
// Architecture:
//   Pass 0 (priority 249): Auto-focus — sample center depth,
//          blend with previous frame's focus in a 1x1 RT for
//          smooth temporal transitions.
//   Pass 1 (priority 250): CoC-weighted horizontal Gaussian blur
//   Pass 2 (priority 251): CoC-weighted vertical Gaussian blur
//
// The auto-focus samples a 3x3 grid around screen center and
// takes the median depth, which is robust against crosshairs,
// particles, and HUD elements.
//
// Priority 250: runs after SSAO (200), before motion blur (300).
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;

// Shaders
static IDirect3DPixelShader9* g_pFocusShader = nullptr;
static IDirect3DPixelShader9* g_pBlurHShader = nullptr;
static IDirect3DPixelShader9* g_pBlurVShader = nullptr;

// 1x1 render target for temporal focus smoothing
static IDirect3DTexture9*  g_pFocusTex  = nullptr;
static IDirect3DSurface9*  g_pFocusSurf = nullptr;
static bool g_focusInitialized = false;

// Config
static float g_aperture     = 3.0f;    // blur strength multiplier
static float g_maxBlur      = 3.0f;    // max blur radius in pixels
static float g_focusBand    = 40.0f;   // distance range around focus that stays sharp
static float g_smoothSpeed  = 0.05f;   // temporal smoothing (0=frozen, 1=instant)
static float g_nearBlur     = 0.5f;    // near-field blur strength (0=disabled)
static float g_fixedFocus   = 0.0f;    // 0=auto-focus, >0=fixed distance
static float g_skyThreshold = 0.998f;  // depth value above which = sky/UI

// ============================================================
// HLSL: Auto-focus pass
//
// Samples depth at screen center (3x3 grid), finds median,
// converts to linear depth, blends with previous frame's focus
// stored in the 1x1 focus texture.
// ============================================================
static const char* g_szAutoFocus = R"(
    sampler2D prevFocusTex : register(s0);
    sampler2D depthTex     : register(s1);

    // c0: x=1/width, y=1/height, z=smoothSpeed, w=fixedFocus
    // c1: x=nearZ, y=farZ, z=skyThreshold, w=initialized
    float4 params0 : register(c0);
    float4 params1 : register(c1);

    float LinearDepth(float z)
    {
        float n = params1.x;
        float f = params1.y;
        return (n * f) / (f - z * (f - n));
    }

    // Simple sort2 for median finding
    void sort2(inout float a, inout float b)
    {
        float lo = min(a, b);
        float hi = max(a, b);
        a = lo;
        b = hi;
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        // Fixed focus mode: just output constant
        if (params0.w > 0.001)
        {
            float fd = params0.w;
            return float4(fd / 10000.0, 0, 0, 1);
        }

        // Sample 3x3 grid around screen center
        float2 center = float2(0.5, 0.5);
        float spread = 0.03;  // 3% of screen

        float samples[9];
        int validCount = 0;
        float validSum = 0;

        float sky = params1.z;

        for (int dy = -1; dy <= 1; dy++)
        {
            for (int dx = -1; dx <= 1; dx++)
            {
                float2 sampleUV = center + float2(dx, dy) * spread;
                float z = tex2D(depthTex, sampleUV).r;
                if (z < sky)
                {
                    float d = LinearDepth(z);
                    samples[validCount] = d;
                    validSum += d;
                    validCount++;
                }
            }
        }

        float focusDepth;
        if (validCount == 0)
        {
            // All sky — use far distance
            focusDepth = params1.y * 0.5;
        }
        else if (validCount <= 2)
        {
            // Few valid samples — use average
            focusDepth = validSum / (float)validCount;
        }
        else
        {
            // Use trimmed mean: discard min and max, average rest
            // Simple approximation of median that works in ps_3_0
            float minVal = samples[0];
            float maxVal = samples[0];
            for (int i = 1; i < validCount; i++)
            {
                minVal = min(minVal, samples[i]);
                maxVal = max(maxVal, samples[i]);
            }
            focusDepth = (validSum - minVal - maxVal) / (float)(validCount - 2);
        }

        // Temporal smoothing: blend with previous frame
        float prevFocus = tex2D(prevFocusTex, float2(0.5, 0.5)).r * 10000.0;
        float speed = params0.z;

        // If not yet initialized, snap to current
        if (params1.w < 0.5)
            prevFocus = focusDepth;

        float smoothed = lerp(prevFocus, focusDepth, speed);

        // Encode: store as focusDepth / 10000.0 to fit in [0,1]
        return float4(smoothed / 10000.0, 0, 0, 1);
    }
)";

// ============================================================
// HLSL: CoC-weighted horizontal blur
//
// Reads focus distance from 1x1 focus texture.
// Computes CoC based on distance from focal plane.
// Supports both near and far blur with independent control.
// Depth gradient check skips particles/effects.
// ============================================================
static const char* g_szCocBlurH = R"(
    sampler2D sceneTex : register(s0);
    sampler2D depthTex : register(s1);
    sampler2D focusTex : register(s2);

    // c0: x=1/width, y=1/height, z=aperture, w=maxBlur
    // c1: x=nearZ, y=farZ, z=focusBand, w=nearBlur
    // c2: x=skyThreshold, y=0, z=0, w=0
    float4 params0 : register(c0);
    float4 params1 : register(c1);
    float4 params2 : register(c2);

    float LinearDepth(float z)
    {
        float n = params1.x;
        float f = params1.y;
        return (n * f) / (f - z * (f - n));
    }

    float ComputeCoC(float depth, float focusDist)
    {
        float diff = depth - focusDist;
        float band = params1.z;

        float coc;
        if (diff > 0)
        {
            // Far field: standard falloff
            coc = max(diff - band, 0.0) / (band + 0.001);
        }
        else
        {
            // Near field: controlled separately
            coc = max(-diff - band * 0.5, 0.0) / (band + 0.001);
            coc *= params1.w;  // nearBlur multiplier
        }

        coc = saturate(coc) * params0.z;
        coc = min(coc, params0.w);
        return coc;
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float z = tex2D(depthTex, uv).r;

        // Skip sky / UI / no depth
        if (z > params2.x)
            return tex2D(sceneTex, uv);

        float depth = LinearDepth(z);

        // Read auto-focus distance
        float focusDist = tex2D(focusTex, float2(0.5, 0.5)).r * 10000.0;

        float coc = ComputeCoC(depth, focusDist);

        // Skip pixels with negligible blur
        if (coc < 0.3)
            return tex2D(sceneTex, uv);

        // Depth gradient check: skip particles / alpha-blended effects
        float zR = tex2D(depthTex, uv + float2(params0.x * 2.0, 0)).r;
        float zL = tex2D(depthTex, uv - float2(params0.x * 2.0, 0)).r;
        float dR = (zR < params2.x) ? LinearDepth(zR) : depth;
        float dL = (zL < params2.x) ? LinearDepth(zL) : depth;
        float gradient = abs(dR - dL) / (depth + 0.001);
        if (gradient > 0.5)
            return tex2D(sceneTex, uv);

        // 9-tap horizontal Gaussian, depth-aware
        float weights[5] = { 0.227027, 0.194596, 0.121622, 0.054054, 0.016216 };
        float dx = params0.x * coc;

        float4 color = tex2D(sceneTex, uv) * weights[0];
        float totalW = weights[0];

        for (int i = 1; i < 5; i++)
        {
            float fi = (float)i;
            float2 uvP = uv + float2(dx * fi, 0);
            float2 uvN = uv - float2(dx * fi, 0);

            // Depth-aware: reduce weight for samples at very different depths
            float zP = tex2D(depthTex, uvP).r;
            float zN = tex2D(depthTex, uvN).r;
            float depthP = (zP < params2.x) ? LinearDepth(zP) : depth;
            float depthN = (zN < params2.x) ? LinearDepth(zN) : depth;

            float wP = weights[i] * (1.0 - saturate(abs(depthP - depth) / (depth * 0.3 + 1.0)));
            float wN = weights[i] * (1.0 - saturate(abs(depthN - depth) / (depth * 0.3 + 1.0)));

            // Don't blur sharp-focus pixels into blurry areas
            float cocP = ComputeCoC(depthP, focusDist);
            float cocN = ComputeCoC(depthN, focusDist);
            wP *= saturate(cocP / (coc + 0.001));
            wN *= saturate(cocN / (coc + 0.001));

            color += tex2D(sceneTex, uvP) * wP;
            color += tex2D(sceneTex, uvN) * wN;
            totalW += wP + wN;
        }

        return color / totalW;
    }
)";

// ============================================================
// HLSL: CoC-weighted vertical blur (same logic, Y axis)
// ============================================================
static const char* g_szCocBlurV = R"(
    sampler2D sceneTex : register(s0);
    sampler2D depthTex : register(s1);
    sampler2D focusTex : register(s2);

    float4 params0 : register(c0);
    float4 params1 : register(c1);
    float4 params2 : register(c2);

    float LinearDepth(float z)
    {
        float n = params1.x;
        float f = params1.y;
        return (n * f) / (f - z * (f - n));
    }

    float ComputeCoC(float depth, float focusDist)
    {
        float diff = depth - focusDist;
        float band = params1.z;

        float coc;
        if (diff > 0)
        {
            coc = max(diff - band, 0.0) / (band + 0.001);
        }
        else
        {
            coc = max(-diff - band * 0.5, 0.0) / (band + 0.001);
            coc *= params1.w;
        }

        coc = saturate(coc) * params0.z;
        coc = min(coc, params0.w);
        return coc;
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float z = tex2D(depthTex, uv).r;

        if (z > params2.x)
            return tex2D(sceneTex, uv);

        float depth = LinearDepth(z);
        float focusDist = tex2D(focusTex, float2(0.5, 0.5)).r * 10000.0;
        float coc = ComputeCoC(depth, focusDist);

        if (coc < 0.3)
            return tex2D(sceneTex, uv);

        // Depth gradient check
        float zU = tex2D(depthTex, uv + float2(0, params0.y * 2.0)).r;
        float zD = tex2D(depthTex, uv - float2(0, params0.y * 2.0)).r;
        float dU = (zU < params2.x) ? LinearDepth(zU) : depth;
        float dD = (zD < params2.x) ? LinearDepth(zD) : depth;
        float gradient = abs(dU - dD) / (depth + 0.001);
        if (gradient > 0.5)
            return tex2D(sceneTex, uv);

        float weights[5] = { 0.227027, 0.194596, 0.121622, 0.054054, 0.016216 };
        float dy = params0.y * coc;

        float4 color = tex2D(sceneTex, uv) * weights[0];
        float totalW = weights[0];

        for (int i = 1; i < 5; i++)
        {
            float fi = (float)i;
            float2 uvP = uv + float2(0, dy * fi);
            float2 uvN = uv - float2(0, dy * fi);

            float zP = tex2D(depthTex, uvP).r;
            float zN = tex2D(depthTex, uvN).r;
            float depthP = (zP < params2.x) ? LinearDepth(zP) : depth;
            float depthN = (zN < params2.x) ? LinearDepth(zN) : depth;

            float wP = weights[i] * (1.0 - saturate(abs(depthP - depth) / (depth * 0.3 + 1.0)));
            float wN = weights[i] * (1.0 - saturate(abs(depthN - depth) / (depth * 0.3 + 1.0)));

            float cocP = ComputeCoC(depthP, focusDist);
            float cocN = ComputeCoC(depthN, focusDist);
            wP *= saturate(cocP / (coc + 0.001));
            wN *= saturate(cocN / (coc + 0.001));

            color += tex2D(sceneTex, uvP) * wP;
            color += tex2D(sceneTex, uvN) * wN;
            totalW += wP + wN;
        }

        return color / totalW;
    }
)";

// ============================================================
// Focus RT management
// ============================================================
static void EnsureFocusRT(IDirect3DDevice9* dev)
{
    if (g_pFocusTex) return;

    HRESULT hr = dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET,
                                     D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT,
                                     &g_pFocusTex, nullptr);
    if (FAILED(hr))
    {
        // Fallback to R32F
        hr = dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET,
                                D3DFMT_R32F, D3DPOOL_DEFAULT,
                                &g_pFocusTex, nullptr);
    }
    if (FAILED(hr))
    {
        // Last resort: standard ARGB (less precision but works everywhere)
        hr = dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET,
                                D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                &g_pFocusTex, nullptr);
    }
    if (SUCCEEDED(hr) && g_pFocusTex)
        g_pFocusTex->GetSurfaceLevel(0, &g_pFocusSurf);
}

static void ReleaseFocusRT()
{
    if (g_pFocusSurf) { g_pFocusSurf->Release(); g_pFocusSurf = nullptr; }
    if (g_pFocusTex)  { g_pFocusTex->Release();  g_pFocusTex = nullptr; }
    g_focusInitialized = false;
}

// ============================================================
// Sampler setup helpers
// ============================================================
static void SetupDepthSampler(IDirect3DDevice9* dev, IDirect3DTexture9* pDepth, int stage)
{
    dev->SetTexture(stage, pDepth);
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

static void SetupFocusSampler(IDirect3DDevice9* dev, int stage)
{
    dev->SetTexture(stage, g_pFocusTex);
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

// ============================================================
// Pass 0: Auto-focus — compute and smooth focus distance
// ============================================================
static bool DofFocusPass(IDirect3DDevice9* dev,
                         IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                         IDirect3DTexture9* pDepth,
                         float w, float h, float nearZ, float farZ,
                         void* ud)
{
    if (!g_pFocusShader || !pDepth) return false;

    EnsureFocusRT(dev);
    if (!g_pFocusSurf) return false;

    // Save the original render target
    IDirect3DSurface9* pOldRT = nullptr;
    dev->GetRenderTarget(0, &pOldRT);

    // Render to 1x1 focus RT
    dev->SetRenderTarget(0, g_pFocusSurf);
    dev->SetPixelShader(g_pFocusShader);

    // s0 = previous focus texture (self-reference is fine for 1x1)
    dev->SetTexture(0, g_pFocusTex);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    // s1 = depth texture
    SetupDepthSampler(dev, pDepth, 1);

    float c0[4] = { 1.0f/w, 1.0f/h, g_smoothSpeed, g_fixedFocus };
    float c1[4] = { nearZ, farZ, g_skyThreshold,
                    g_focusInitialized ? 1.0f : 0.0f };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);

    g_api->DrawFullscreenQuad(dev, 1.0f, 1.0f);
    g_focusInitialized = true;

    // Restore original RT
    dev->SetRenderTarget(0, pOldRT);
    if (pOldRT) pOldRT->Release();

    dev->SetTexture(0, nullptr);
    dev->SetTexture(1, nullptr);
    dev->SetPixelShader(nullptr);

    // This pass doesn't write to pDst (scene texture is unchanged)
    return false;
}

// ============================================================
// Pass 1: Horizontal blur weighted by CoC
// ============================================================
static bool DofPassH(IDirect3DDevice9* dev,
                     IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                     IDirect3DTexture9* pDepth,
                     float w, float h, float nearZ, float farZ,
                     void* ud)
{
    if (!g_pBlurHShader || !pDepth || !g_pFocusTex) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pBlurHShader);

    // s0 = scene
    dev->SetTexture(0, pSrc);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    // s1 = depth
    SetupDepthSampler(dev, pDepth, 1);

    // s2 = focus distance (1x1 RT)
    SetupFocusSampler(dev, 2);

    float c0[4] = { 1.0f/w, 1.0f/h, g_aperture, g_maxBlur };
    float c1[4] = { nearZ, farZ, g_focusBand, g_nearBlur };
    float c2[4] = { g_skyThreshold, 0, 0, 0 };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetPixelShaderConstantF(2, c2, 1);

    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetTexture(1, nullptr);
    dev->SetTexture(2, nullptr);
    dev->SetPixelShader(nullptr);
    return true;
}

// ============================================================
// Pass 2: Vertical blur weighted by CoC
// ============================================================
static bool DofPassV(IDirect3DDevice9* dev,
                     IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                     IDirect3DTexture9* pDepth,
                     float w, float h, float nearZ, float farZ,
                     void* ud)
{
    if (!g_pBlurVShader || !pDepth || !g_pFocusTex) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pBlurVShader);

    dev->SetTexture(0, pSrc);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

    SetupDepthSampler(dev, pDepth, 1);
    SetupFocusSampler(dev, 2);

    float c0[4] = { 1.0f/w, 1.0f/h, g_aperture, g_maxBlur };
    float c1[4] = { nearZ, farZ, g_focusBand, g_nearBlur };
    float c2[4] = { g_skyThreshold, 0, 0, 0 };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetPixelShaderConstantF(2, c2, 1);

    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetTexture(1, nullptr);
    dev->SetTexture(2, nullptr);
    dev->SetPixelShader(nullptr);
    return true;
}

// ============================================================
// Mod lifecycle
// ============================================================
SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;

    g_aperture     = api->GetConfigFloat("DoF", "Aperture",     3.0f);
    g_maxBlur      = api->GetConfigFloat("DoF", "MaxBlur",      3.0f);
    g_focusBand    = api->GetConfigFloat("DoF", "FocusBand",    40.0f);
    g_smoothSpeed  = api->GetConfigFloat("DoF", "SmoothSpeed",  0.05f);
    g_nearBlur     = api->GetConfigFloat("DoF", "NearBlur",     0.5f);
    g_fixedFocus   = api->GetConfigFloat("DoF", "FixedFocus",   0.0f);
    g_skyThreshold = api->GetConfigFloat("DoF", "SkyThreshold", 0.998f);

    // Clamp smooth speed to sane range
    if (g_smoothSpeed < 0.01f) g_smoothSpeed = 0.01f;
    if (g_smoothSpeed > 1.0f)  g_smoothSpeed = 1.0f;

    g_pFocusShader = api->CompilePixelShader(g_szAutoFocus, "main");
    g_pBlurHShader = api->CompilePixelShader(g_szCocBlurH, "main");
    g_pBlurVShader = api->CompilePixelShader(g_szCocBlurV, "main");

    if (!g_pFocusShader || !g_pBlurHShader || !g_pBlurVShader) {
        if (g_pFocusShader) { g_pFocusShader->Release(); g_pFocusShader = nullptr; }
        if (g_pBlurHShader) { g_pBlurHShader->Release(); g_pBlurHShader = nullptr; }
        if (g_pBlurVShader) { g_pBlurVShader->Release(); g_pBlurVShader = nullptr; }
        api->Log("mod_dof: shader compile failed");
        return false;
    }

    api->RegisterPostPass("dof_focus", DofFocusPass, 249, nullptr);
    api->RegisterPostPass("dof_h",     DofPassH,     250, nullptr);
    api->RegisterPostPass("dof_v",     DofPassV,     251, nullptr);

    const char* mode = (g_fixedFocus > 0.001f) ? "fixed" : "auto";
    api->Log("mod_dof: OK (mode=%s aperture=%.1f maxBlur=%.1f band=%.0f smooth=%.2f)",
             mode, g_aperture, g_maxBlur, g_focusBand, g_smoothSpeed);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) {
        g_api->UnregisterPostPass("dof_focus");
        g_api->UnregisterPostPass("dof_h");
        g_api->UnregisterPostPass("dof_v");
    }
    ReleaseFocusRT();
    if (g_pFocusShader) { g_pFocusShader->Release(); g_pFocusShader = nullptr; }
    if (g_pBlurHShader) { g_pBlurHShader->Release(); g_pBlurHShader = nullptr; }
    if (g_pBlurVShader) { g_pBlurVShader->Release(); g_pBlurVShader = nullptr; }
}

SDGO_MOD_EXPORT void ModDeviceLost()
{
    ReleaseFocusRT();
}

SDGO_MOD_EXPORT void ModDeviceReset(IDirect3DDevice9* /*pDevice*/)
{
    // Focus RT is lazily recreated in DofFocusPass
}

SDGO_MOD_EXPORT const char* ModName()    { return "Depth of Field"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "2.0"; }
