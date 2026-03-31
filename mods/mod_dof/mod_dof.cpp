// ============================================================
// Depth of Field - Cinematic focus blur
//
// Multi-pass: CoC map → downsample+blur H → blur V → composite.
// Supports both near-field (foreground) and far-field (background)
// blur with depth-aware edge preservation.
//
// Priority 250: runs after SSAO (200), before motion blur (300).
// ============================================================
#include "mod_api.h"
#include <cmath>

static SdgoModAPI* g_api = nullptr;

// Shaders
static IDirect3DPixelShader9* g_pAutoFocusShader  = nullptr;  // Screen center depth sampling
static IDirect3DPixelShader9* g_pCocShader        = nullptr;  // CoC map generation
static IDirect3DPixelShader9* g_pDownBlurHShader  = nullptr;  // Downsample + horizontal blur
static IDirect3DPixelShader9* g_pBlurVShader      = nullptr;  // Vertical blur
static IDirect3DPixelShader9* g_pCompositeShader  = nullptr;  // Final composite

// Render targets
static IDirect3DTexture9*  g_pFocusTex   = nullptr;   // 1x1 auto-focus depth readback
static IDirect3DSurface9*  g_pFocusSurf  = nullptr;
static IDirect3DSurface9*  g_pFocusReadback = nullptr; // System-memory surface for CPU readback
static IDirect3DTexture9*  g_pCocTex     = nullptr;   // Full-res CoC map (R=far, G=near, B=0)
static IDirect3DSurface9*  g_pCocSurf    = nullptr;
static IDirect3DTexture9*  g_pBlurTexA   = nullptr;   // Half-res ping-pong A
static IDirect3DSurface9*  g_pBlurSurfA  = nullptr;
static IDirect3DTexture9*  g_pBlurTexB   = nullptr;   // Half-res ping-pong B
static IDirect3DSurface9*  g_pBlurSurfB  = nullptr;
static UINT g_fullW = 0, g_fullH = 0;
static UINT g_halfW = 0, g_halfH = 0;

// Auto-focus state (CPU-side temporal smoothing)
static float g_currentFocusDist = 15.0f; // Smoothed focus distance, updated each frame

// Config
static float g_focusDist   = 15.0f;    // Fallback focus distance (world units)
static float g_focusRange  = 10.0f;    // Range around focus that stays sharp
static float g_nearBlur    = 1.0f;     // Near-field blur intensity (0=off)
static float g_farBlur     = 1.0f;     // Far-field blur intensity
static float g_maxBlur     = 1.0f;     // Maximum blur radius multiplier
static int   g_quality     = 1;        // 0=low(5-tap), 1=medium(9-tap), 2=high(13-tap)
static bool  g_autoFocus   = true;     // Auto-focus on screen center depth
static float g_focusSmoothSpeed = 0.1f; // Auto-focus temporal smoothing (0=instant, 1=very slow)

// ============================================================
// HLSL: Circle of Confusion (CoC) map
//
// Generates per-pixel blur amount from depth.
// R channel = far-field CoC (background blur)
// G channel = near-field CoC (foreground blur)
//
// Auto-focus: samples depth at screen center (0.5, 0.5) and
// uses that as the focus plane. The CPU passes a smoothed
// focus distance each frame for temporal stability.
// ============================================================
static const char* g_szCoc = R"(
    sampler2D depthTex : register(s0);

    // c0: x=nearZ, y=farZ, z=focusDist (smoothed), w=focusRange
    // c1: x=nearBlur, y=farBlur, z=maxBlur, w=0
    float4 params0 : register(c0);
    float4 params1 : register(c1);

    float LinearDepth(float z)
    {
        float n = params0.x;
        float f = params0.y;
        return (n * f) / (f - z * (f - n));
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float z = tex2D(depthTex, uv).r;

        // Sky → full far blur
        if (z > 0.999)
            return float4(params1.z, 0.0, 0.0, 1.0);

        float depth = LinearDepth(z);
        float focusDist  = params0.z;
        float focusRange = params0.w;
        float maxBlur    = params1.z;

        // Signed distance from focus plane
        float delta = depth - focusDist;

        // Far-field CoC: positive delta → background blur
        float farCoC = 0.0;
        if (delta > 0.0)
        {
            farCoC = saturate(delta / max(focusRange, 0.01)) * params1.y;
        }

        // Near-field CoC: negative delta → foreground blur
        float nearCoC = 0.0;
        if (delta < 0.0)
        {
            nearCoC = saturate(-delta / max(focusRange * 0.5, 0.01)) * params1.x;
        }

        farCoC  = min(farCoC,  maxBlur);
        nearCoC = min(nearCoC, maxBlur);

        return float4(farCoC, nearCoC, depth / params0.y, 1.0);
    }
)";

// ============================================================
// HLSL: Auto-focus - sample center depth and output to 1x1 RT
// ============================================================
static const char* g_szAutoFocus = R"(
    sampler2D depthTex : register(s0);

    // c0: x=nearZ, y=farZ, z=0, w=0
    float4 params0 : register(c0);

    float LinearDepth(float z)
    {
        float n = params0.x;
        float f = params0.y;
        return (n * f) / (f - z * (f - n));
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        // Sample a small area around screen center for stability
        float2 center = float2(0.5, 0.5);
        float d = 0.005;  // ~1% of screen

        float z0 = tex2D(depthTex, center).r;
        float z1 = tex2D(depthTex, center + float2( d, 0)).r;
        float z2 = tex2D(depthTex, center + float2(-d, 0)).r;
        float z3 = tex2D(depthTex, center + float2(0,  d)).r;
        float z4 = tex2D(depthTex, center + float2(0, -d)).r;

        // Use minimum (closest object) to avoid focusing on background
        // through gaps in foreground geometry
        float zMin = min(min(min(z0, z1), min(z2, z3)), z4);

        // Sky check: if center is sky, use a moderate default
        if (zMin > 0.999)
            return float4(0.5, 0, 0, 1);  // Encode as normalized depth

        float depth = LinearDepth(zMin);
        // Encode linear depth normalized by farZ
        float encoded = saturate(depth / params0.y);
        return float4(encoded, encoded, encoded, 1.0);
    }
)";

// ============================================================
// HLSL: Downsample scene to half-res + horizontal blur
//
// Reads scene color weighted by CoC. Uses variable-width
// kernel based on per-pixel blur amount.
// ============================================================
static const char* g_szDownBlurH = R"(
    sampler2D sceneTex : register(s0);
    sampler2D cocTex   : register(s1);

    // c0: x=1/width, y=1/height, z=quality, w=0
    float4 params0 : register(c0);

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float4 cocVal = tex2D(cocTex, uv);
        float coc = max(cocVal.r, cocVal.g);  // Use whichever is stronger

        if (coc < 0.01)
        {
            return tex2D(sceneTex, uv);
        }

        float dx = params0.x * coc * 3.0;  // Scale kernel by CoC
        int quality = (int)params0.z;

        float3 color = float3(0, 0, 0);
        float totalWeight = 0.0;

        if (quality >= 2)
        {
            // High: 13-tap
            float offsets[7] = { -6.0, -4.0, -2.0, 0.0, 2.0, 4.0, 6.0 };
            float weights[7] = { 0.015, 0.060, 0.175, 0.500, 0.175, 0.060, 0.015 };
            for (int i = 0; i < 7; i++)
            {
                float2 sampleUV = uv + float2(dx * offsets[i], 0);
                float4 sc = tex2D(cocTex, sampleUV);
                float sampleCoc = max(sc.r, sc.g);

                // Depth-aware: don't let sharp foreground bleed into background
                float w = weights[i] * (sampleCoc > 0.01 ? 1.0 : 0.3);
                color += tex2D(sceneTex, sampleUV).rgb * w;
                totalWeight += w;
            }
        }
        else if (quality >= 1)
        {
            // Medium: 9-tap
            float offsets[5] = { -4.0, -2.0, 0.0, 2.0, 4.0 };
            float weights[5] = { 0.054, 0.194, 0.504, 0.194, 0.054 };
            for (int i = 0; i < 5; i++)
            {
                float2 sampleUV = uv + float2(dx * offsets[i], 0);
                float4 sc = tex2D(cocTex, sampleUV);
                float sampleCoc = max(sc.r, sc.g);
                float w = weights[i] * (sampleCoc > 0.01 ? 1.0 : 0.3);
                color += tex2D(sceneTex, sampleUV).rgb * w;
                totalWeight += w;
            }
        }
        else
        {
            // Low: 5-tap
            float offsets[3] = { -2.0, 0.0, 2.0 };
            float weights[3] = { 0.194, 0.612, 0.194 };
            for (int i = 0; i < 3; i++)
            {
                float2 sampleUV = uv + float2(dx * offsets[i], 0);
                float4 sc = tex2D(cocTex, sampleUV);
                float sampleCoc = max(sc.r, sc.g);
                float w = weights[i] * (sampleCoc > 0.01 ? 1.0 : 0.3);
                color += tex2D(sceneTex, sampleUV).rgb * w;
                totalWeight += w;
            }
        }

        color /= max(totalWeight, 0.001);
        return float4(color, coc);
    }
)";

// ============================================================
// HLSL: Vertical blur pass (half-res)
//
// Same variable-width kernel, vertical direction.
// Alpha channel carries CoC for the composite pass.
// ============================================================
static const char* g_szBlurV = R"(
    sampler2D blurTex : register(s0);
    sampler2D cocTex  : register(s1);

    // c0: x=1/width, y=1/height, z=quality, w=0
    float4 params0 : register(c0);

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float4 center = tex2D(blurTex, uv);
        float coc = center.a;

        if (coc < 0.01)
        {
            return center;
        }

        float dy = params0.y * coc * 3.0;
        int quality = (int)params0.z;

        float3 color = float3(0, 0, 0);
        float totalWeight = 0.0;

        if (quality >= 2)
        {
            float offsets[7] = { -6.0, -4.0, -2.0, 0.0, 2.0, 4.0, 6.0 };
            float weights[7] = { 0.015, 0.060, 0.175, 0.500, 0.175, 0.060, 0.015 };
            for (int i = 0; i < 7; i++)
            {
                float2 sampleUV = uv + float2(0, dy * offsets[i]);
                float4 s = tex2D(blurTex, sampleUV);
                float w = weights[i] * (s.a > 0.01 ? 1.0 : 0.3);
                color += s.rgb * w;
                totalWeight += w;
            }
        }
        else if (quality >= 1)
        {
            float offsets[5] = { -4.0, -2.0, 0.0, 2.0, 4.0 };
            float weights[5] = { 0.054, 0.194, 0.504, 0.194, 0.054 };
            for (int i = 0; i < 5; i++)
            {
                float2 sampleUV = uv + float2(0, dy * offsets[i]);
                float4 s = tex2D(blurTex, sampleUV);
                float w = weights[i] * (s.a > 0.01 ? 1.0 : 0.3);
                color += s.rgb * w;
                totalWeight += w;
            }
        }
        else
        {
            float offsets[3] = { -2.0, 0.0, 2.0 };
            float weights[3] = { 0.194, 0.612, 0.194 };
            for (int i = 0; i < 3; i++)
            {
                float2 sampleUV = uv + float2(0, dy * offsets[i]);
                float4 s = tex2D(blurTex, sampleUV);
                float w = weights[i] * (s.a > 0.01 ? 1.0 : 0.3);
                color += s.rgb * w;
                totalWeight += w;
            }
        }

        color /= max(totalWeight, 0.001);
        return float4(color, coc);
    }
)";

// ============================================================
// HLSL: Composite - blend blurred with sharp scene
//
// Uses CoC to interpolate between sharp (in-focus) and
// blurred (out-of-focus) pixels. Near-field gets priority
// to handle foreground bokeh correctly.
// ============================================================
static const char* g_szComposite = R"(
    sampler2D sceneTex : register(s0);   // Original sharp scene
    sampler2D blurTex  : register(s1);   // Blurred result (half-res, upsampled)
    sampler2D cocTex   : register(s2);   // CoC map

    // c0: x=nearBlur, y=farBlur, z=0, w=0
    float4 params0 : register(c0);

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float3 sharp  = tex2D(sceneTex, uv).rgb;
        float3 blurred = tex2D(blurTex, uv).rgb;
        float4 cocVal  = tex2D(cocTex, uv);

        float farCoC  = cocVal.r;
        float nearCoC = cocVal.g;

        // Combined CoC: near-field takes priority (rendered on top)
        float coc = max(farCoC, nearCoC);

        // Smooth interpolation with slight bias toward blur
        // to avoid harsh transitions
        float blendFactor = smoothstep(0.0, 0.8, coc);

        float3 result = lerp(sharp, blurred, blendFactor);
        return float4(result, 1.0);
    }
)";

// ============================================================
// Render target management
// ============================================================
static void EnsureTargets(IDirect3DDevice9* dev, float w, float h)
{
    UINT fw = (UINT)w;
    UINT fh = (UINT)h;
    UINT hw = fw / 2;
    UINT hh = fh / 2;
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;

    if (g_pCocTex && g_fullW == fw && g_fullH == fh) return;

    // Release old
    if (g_pFocusSurf)    { g_pFocusSurf->Release();    g_pFocusSurf = nullptr; }
    if (g_pFocusTex)     { g_pFocusTex->Release();      g_pFocusTex = nullptr; }
    if (g_pFocusReadback){ g_pFocusReadback->Release();  g_pFocusReadback = nullptr; }
    if (g_pCocSurf)      { g_pCocSurf->Release();       g_pCocSurf = nullptr; }
    if (g_pCocTex)       { g_pCocTex->Release();         g_pCocTex = nullptr; }
    if (g_pBlurSurfA)    { g_pBlurSurfA->Release();      g_pBlurSurfA = nullptr; }
    if (g_pBlurTexA)     { g_pBlurTexA->Release();       g_pBlurTexA = nullptr; }
    if (g_pBlurSurfB)    { g_pBlurSurfB->Release();      g_pBlurSurfB = nullptr; }
    if (g_pBlurTexB)     { g_pBlurTexB->Release();       g_pBlurTexB = nullptr; }

    // 1x1 auto-focus render target + system memory readback surface
    dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                       D3DPOOL_DEFAULT, &g_pFocusTex, nullptr);
    if (g_pFocusTex) g_pFocusTex->GetSurfaceLevel(0, &g_pFocusSurf);
    dev->CreateOffscreenPlainSurface(1, 1, D3DFMT_A8R8G8B8,
                                     D3DPOOL_SYSTEMMEM, &g_pFocusReadback, nullptr);

    // Full-res CoC map
    dev->CreateTexture(fw, fh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                       D3DPOOL_DEFAULT, &g_pCocTex, nullptr);
    if (g_pCocTex) g_pCocTex->GetSurfaceLevel(0, &g_pCocSurf);

    // Half-res blur targets
    dev->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                       D3DPOOL_DEFAULT, &g_pBlurTexA, nullptr);
    if (g_pBlurTexA) g_pBlurTexA->GetSurfaceLevel(0, &g_pBlurSurfA);

    dev->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                       D3DPOOL_DEFAULT, &g_pBlurTexB, nullptr);
    if (g_pBlurTexB) g_pBlurTexB->GetSurfaceLevel(0, &g_pBlurSurfB);

    g_fullW = fw;
    g_fullH = fh;
    g_halfW = hw;
    g_halfH = hh;
}

static void ReleaseTargets()
{
    if (g_pFocusSurf)    { g_pFocusSurf->Release();    g_pFocusSurf = nullptr; }
    if (g_pFocusTex)     { g_pFocusTex->Release();      g_pFocusTex = nullptr; }
    if (g_pFocusReadback){ g_pFocusReadback->Release();  g_pFocusReadback = nullptr; }
    if (g_pCocSurf)      { g_pCocSurf->Release();       g_pCocSurf = nullptr; }
    if (g_pCocTex)       { g_pCocTex->Release();         g_pCocTex = nullptr; }
    if (g_pBlurSurfA)    { g_pBlurSurfA->Release();      g_pBlurSurfA = nullptr; }
    if (g_pBlurTexA)     { g_pBlurTexA->Release();       g_pBlurTexA = nullptr; }
    if (g_pBlurSurfB)    { g_pBlurSurfB->Release();      g_pBlurSurfB = nullptr; }
    if (g_pBlurTexB)     { g_pBlurTexB->Release();       g_pBlurTexB = nullptr; }
    g_fullW = g_fullH = 0;
    g_halfW = g_halfH = 0;
}

static void SetupPointSampler(IDirect3DDevice9* dev, int stage)
{
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

static void SetupLinearSampler(IDirect3DDevice9* dev, int stage)
{
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

// ============================================================
// Post-pass callback
// ============================================================
static bool DofPass(IDirect3DDevice9* dev,
                    IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                    IDirect3DTexture9* pDepth,
                    float w, float h, float nearZ, float farZ,
                    void* ud)
{
    if (!g_pCocShader || !g_pDownBlurHShader || !g_pBlurVShader || !g_pCompositeShader)
        return false;
    if (!pDepth)
        return false;

    EnsureTargets(dev, w, h);
    if (!g_pCocTex || !g_pBlurTexA || !g_pBlurTexB) return false;

    float halfW = (float)g_halfW;
    float halfH = (float)g_halfH;

    float focusDist = g_focusDist;

    // ---- Auto-focus: render center depth to 1x1 RT, readback to CPU ----
    if (g_autoFocus && g_pAutoFocusShader && g_pFocusSurf && g_pFocusReadback)
    {
        dev->SetRenderTarget(0, g_pFocusSurf);
        dev->SetPixelShader(g_pAutoFocusShader);
        dev->SetTexture(0, pDepth);
        SetupPointSampler(dev, 0);

        float af0[4] = { nearZ, farZ, 0, 0 };
        dev->SetPixelShaderConstantF(0, af0, 1);
        g_api->DrawFullscreenQuad(dev, 1.0f, 1.0f);

        // Readback: GetRenderTargetData copies GPU → system memory
        HRESULT hr = dev->GetRenderTargetData(g_pFocusSurf, g_pFocusReadback);
        if (SUCCEEDED(hr))
        {
            D3DLOCKED_RECT lr;
            hr = g_pFocusReadback->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            if (SUCCEEDED(hr))
            {
                // A8R8G8B8: byte order is B, G, R, A
                BYTE* pixel = (BYTE*)lr.pBits;
                float encoded = pixel[2] / 255.0f;  // R channel = normalized depth
                float sampledDist = encoded * farZ;

                // Clamp to reasonable range
                if (sampledDist < nearZ * 2.0f) sampledDist = nearZ * 2.0f;
                if (sampledDist > farZ * 0.95f) sampledDist = g_focusDist; // Sky fallback

                // Temporal smoothing: lerp toward sampled distance
                float speed = g_focusSmoothSpeed;
                g_currentFocusDist += (sampledDist - g_currentFocusDist) * speed;

                g_pFocusReadback->UnlockRect();
            }
        }

        focusDist = g_currentFocusDist;
    }

    // ---- Pass 1: Generate CoC map (full-res) ----
    dev->SetRenderTarget(0, g_pCocSurf);
    dev->SetPixelShader(g_pCocShader);
    dev->SetTexture(0, pDepth);
    SetupPointSampler(dev, 0);

    float c0[4] = { nearZ, farZ, focusDist, g_focusRange };
    float c1[4] = { g_nearBlur, g_farBlur, g_maxBlur, 0 };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    g_api->DrawFullscreenQuad(dev, w, h);

    // ---- Pass 2: Downsample + horizontal blur → blurA (half-res) ----
    dev->SetRenderTarget(0, g_pBlurSurfA);
    dev->SetPixelShader(g_pDownBlurHShader);
    dev->SetTexture(0, pSrc);
    SetupLinearSampler(dev, 0);
    dev->SetTexture(1, g_pCocTex);
    SetupLinearSampler(dev, 1);

    float b0[4] = { 1.0f / w, 1.0f / h, (float)g_quality, 0 };
    dev->SetPixelShaderConstantF(0, b0, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // ---- Pass 3: Vertical blur → blurB (half-res) ----
    dev->SetRenderTarget(0, g_pBlurSurfB);
    dev->SetPixelShader(g_pBlurVShader);
    dev->SetTexture(0, g_pBlurTexA);
    SetupLinearSampler(dev, 0);
    dev->SetTexture(1, g_pCocTex);
    SetupLinearSampler(dev, 1);

    float v0[4] = { 1.0f / halfW, 1.0f / halfH, (float)g_quality, 0 };
    dev->SetPixelShaderConstantF(0, v0, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // ---- Pass 4: Second blur iteration for smoother bokeh ----
    // H blur → blurA
    dev->SetRenderTarget(0, g_pBlurSurfA);
    dev->SetPixelShader(g_pDownBlurHShader);
    dev->SetTexture(0, g_pBlurTexB);
    SetupLinearSampler(dev, 0);
    dev->SetTexture(1, g_pCocTex);
    SetupLinearSampler(dev, 1);

    float b1[4] = { 1.0f / halfW, 1.0f / halfH, (float)g_quality, 0 };
    dev->SetPixelShaderConstantF(0, b1, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // V blur → blurB
    dev->SetRenderTarget(0, g_pBlurSurfB);
    dev->SetPixelShader(g_pBlurVShader);
    dev->SetTexture(0, g_pBlurTexA);
    SetupLinearSampler(dev, 0);
    dev->SetTexture(1, g_pCocTex);
    SetupLinearSampler(dev, 1);
    dev->SetPixelShaderConstantF(0, v0, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // ---- Pass 5: Composite → pDst (full-res) ----
    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pCompositeShader);
    dev->SetTexture(0, pSrc);           // Sharp scene
    SetupLinearSampler(dev, 0);
    dev->SetTexture(1, g_pBlurTexB);    // Blurred result
    SetupLinearSampler(dev, 1);
    dev->SetTexture(2, g_pCocTex);      // CoC map
    SetupLinearSampler(dev, 2);

    float cp[4] = { g_nearBlur, g_farBlur, 0, 0 };
    dev->SetPixelShaderConstantF(0, cp, 1);
    g_api->DrawFullscreenQuad(dev, w, h);

    // Cleanup
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

    g_focusDist        = api->GetConfigFloat("DoF", "FocusDistance",  15.0f);
    g_focusRange       = api->GetConfigFloat("DoF", "FocusRange",    10.0f);
    g_nearBlur         = api->GetConfigFloat("DoF", "NearBlur",       1.0f);
    g_farBlur          = api->GetConfigFloat("DoF", "FarBlur",        1.0f);
    g_maxBlur          = api->GetConfigFloat("DoF", "MaxBlur",        1.0f);
    g_quality          = api->GetConfigInt("DoF",   "Quality",        1);
    g_autoFocus        = api->GetConfigBool("DoF",  "AutoFocus",      true);
    g_focusSmoothSpeed = api->GetConfigFloat("DoF", "FocusSmoothSpeed", 0.1f);

    if (g_quality < 0) g_quality = 0;
    if (g_quality > 2) g_quality = 2;
    if (g_focusSmoothSpeed < 0.01f) g_focusSmoothSpeed = 0.01f;
    if (g_focusSmoothSpeed > 1.0f)  g_focusSmoothSpeed = 1.0f;

    g_currentFocusDist = g_focusDist;

    g_pAutoFocusShader = api->CompilePixelShader(g_szAutoFocus,  "main");
    g_pCocShader       = api->CompilePixelShader(g_szCoc,        "main");
    g_pDownBlurHShader = api->CompilePixelShader(g_szDownBlurH,  "main");
    g_pBlurVShader     = api->CompilePixelShader(g_szBlurV,      "main");
    g_pCompositeShader = api->CompilePixelShader(g_szComposite,  "main");

    if (!g_pCocShader || !g_pDownBlurHShader || !g_pBlurVShader || !g_pCompositeShader) {
        if (g_pAutoFocusShader) { g_pAutoFocusShader->Release(); g_pAutoFocusShader = nullptr; }
        if (g_pCocShader)       { g_pCocShader->Release();       g_pCocShader = nullptr; }
        if (g_pDownBlurHShader) { g_pDownBlurHShader->Release();  g_pDownBlurHShader = nullptr; }
        if (g_pBlurVShader)     { g_pBlurVShader->Release();      g_pBlurVShader = nullptr; }
        if (g_pCompositeShader) { g_pCompositeShader->Release();  g_pCompositeShader = nullptr; }
        api->Log("mod_dof: shader compile failed");
        return false;
    }

    if (!g_pAutoFocusShader) {
        api->Log("mod_dof: auto-focus shader failed, using fixed focus");
        g_autoFocus = false;
    }

    api->RegisterPostPass("dof", DofPass, 250, nullptr);
    api->Log("mod_dof: OK (focus=%.1f range=%.1f autoFocus=%d smooth=%.2f quality=%d)",
             g_focusDist, g_focusRange, g_autoFocus ? 1 : 0, g_focusSmoothSpeed, g_quality);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("dof");
    ReleaseTargets();
    if (g_pAutoFocusShader) { g_pAutoFocusShader->Release(); g_pAutoFocusShader = nullptr; }
    if (g_pCocShader)       { g_pCocShader->Release();       g_pCocShader = nullptr; }
    if (g_pDownBlurHShader) { g_pDownBlurHShader->Release();  g_pDownBlurHShader = nullptr; }
    if (g_pBlurVShader)     { g_pBlurVShader->Release();      g_pBlurVShader = nullptr; }
    if (g_pCompositeShader) { g_pCompositeShader->Release();  g_pCompositeShader = nullptr; }
}

SDGO_MOD_EXPORT void ModDeviceLost()
{
    ReleaseTargets();
}

SDGO_MOD_EXPORT void ModDeviceReset(IDirect3DDevice9* /*pDevice*/)
{
    // Targets are lazily recreated in DofPass via EnsureTargets
}

SDGO_MOD_EXPORT const char* ModName()    { return "Depth of Field"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
