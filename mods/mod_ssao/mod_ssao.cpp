// ============================================================
// SSAO - Screen Space Ambient Occlusion
//
// High-quality Alchemy-style AO with depth-aware bilateral blur.
// Pipeline: AO generate (half-res) → blur H → blur V → composite.
// Fog masking: AO fades out at distance to avoid conflict with
// the game's built-in fog.
//
// Priority 200: runs after bloom (100), before DoF (250).
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;

// Shaders
static IDirect3DPixelShader9* g_pAoGenShader   = nullptr;
static IDirect3DPixelShader9* g_pBlurHShader   = nullptr;
static IDirect3DPixelShader9* g_pBlurVShader   = nullptr;
static IDirect3DPixelShader9* g_pCompositeShader = nullptr;

// Half-res AO render targets (ping-pong)
static IDirect3DTexture9*  g_pAoTexA  = nullptr;
static IDirect3DSurface9*  g_pAoSurfA = nullptr;
static IDirect3DTexture9*  g_pAoTexB  = nullptr;
static IDirect3DSurface9*  g_pAoSurfB = nullptr;
static UINT g_halfW = 0, g_halfH = 0;

// Config
static float g_radius      = 1.2f;   // AO sampling radius (world-ish scale)
static float g_strength     = 0.6f;   // AO intensity multiplier
static float g_bias         = 0.02f;  // depth bias to prevent self-occlusion
static float g_fogStart     = 200.0f; // distance where AO begins fading
static float g_fogEnd       = 500.0f; // distance where AO fully fades out
static float g_edgeThresh   = 0.3f;   // depth gradient threshold for particle/fog rejection
static int   g_sampleCount  = 12;     // 8 or 12 samples (clamped)

// ============================================================
// HLSL: AO generation (half-res)
//
// Reconstructs view-space normal from depth gradients (ddx/ddy)
// to detect solid geometry vs particles/fog. Particles and fog
// have erratic depth gradients and are excluded. Only hemisphere
// samples consistent with the surface normal contribute.
// ============================================================
static const char* g_szAoGen = R"(
    sampler2D depthTex : register(s0);

    // c0: x=1/width, y=1/height, z=radius, w=bias
    // c1: x=nearZ, y=farZ, z=strength, w=fogStart
    // c2: x=fogEnd, y=sampleCount, z=aspect, w=tanHalfFov
    // c3: x=depthEdgeThreshold, y=0, z=0, w=0
    float4 params0 : register(c0);
    float4 params1 : register(c1);
    float4 params2 : register(c2);
    float4 params3 : register(c3);

    float LinearDepth(float z)
    {
        float n = params1.x;
        float f = params1.y;
        return (n * f) / (f - z * (f - n));
    }

    float3 ReconstructPos(float2 uv, float linDepth)
    {
        float2 ndc = uv * 2.0 - 1.0;
        ndc.y = -ndc.y;
        float3 pos;
        pos.x = ndc.x * params2.z * params2.w * linDepth;
        pos.y = ndc.y * params2.w * linDepth;
        pos.z = linDepth;
        return pos;
    }

    float2 GetNoise(float2 uv)
    {
        float2 screen = uv / float2(params0.x, params0.y);
        float n = frac(sin(dot(screen * 0.01, float2(12.9898, 78.233))) * 43758.5453);
        float angle = n * 6.2831853;
        return float2(cos(angle), sin(angle));
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float z = tex2D(depthTex, uv).r;

        // Sky / no depth → no occlusion
        if (z > 0.999)
            return float4(1, 1, 1, 1);

        float depth = LinearDepth(z);

        // Fog masking: fade AO in distant areas
        float fogMask = 1.0 - saturate((depth - params1.w) / (params2.x - params1.w));
        if (fogMask < 0.01)
            return float4(1, 1, 1, 1);

        // ---- Particle / fog rejection via depth gradient ----
        // Use ddx/ddy to compute screen-space depth gradient.
        // Solid geometry has smooth, predictable gradients.
        // Particles and fog quads create huge discontinuities.
        float dzdx = ddx(depth);
        float dzdy = ddy(depth);
        float gradMag = abs(dzdx) + abs(dzdy);

        // If gradient is too large relative to depth, this pixel is
        // likely a particle edge, fog boundary, or alpha-blended quad.
        // Skip AO entirely for these pixels.
        float depthEdgeThreshold = params3.x;
        float gradRatio = gradMag / (depth + 0.001);
        if (gradRatio > depthEdgeThreshold)
            return float4(1, 1, 1, 1);

        // ---- Reconstruct view-space normal from depth ----
        float3 posC = ReconstructPos(uv, depth);
        float3 posDx = ReconstructPos(uv + float2(params0.x, 0), LinearDepth(
            tex2D(depthTex, uv + float2(params0.x, 0)).r));
        float3 posDy = ReconstructPos(uv + float2(0, params0.y), LinearDepth(
            tex2D(depthTex, uv + float2(0, params0.y)).r));

        float3 normal = normalize(cross(posDx - posC, posDy - posC));
        // Ensure normal points toward camera (negative Z in view space)
        if (normal.z > 0) normal = -normal;

        // If the normal is nearly degenerate (particle), skip
        float normalLen = length(cross(posDx - posC, posDy - posC));
        if (normalLen < 0.0001)
            return float4(1, 1, 1, 1);

        // Scale radius by depth
        float radiusScreen = params0.z / depth;

        // Per-pixel rotation to break banding
        float2 noise = GetNoise(uv);

        // 12-sample Poisson disc
        float2 poisson[12] = {
            float2(-0.326, -0.406), float2(-0.840, -0.074),
            float2(-0.696,  0.457), float2(-0.203,  0.621),
            float2( 0.962, -0.195), float2( 0.473, -0.480),
            float2( 0.519,  0.767), float2( 0.185, -0.893),
            float2( 0.507,  0.064), float2(-0.321,  0.054),
            float2(-0.791, -0.573), float2( 0.691,  0.369)
        };

        int numSamples = (int)params2.y;
        if (numSamples > 12) numSamples = 12;
        if (numSamples < 4)  numSamples = 4;

        float occlusion = 0.0;
        float validSamples = 0.0;

        for (int i = 0; i < numSamples; i++)
        {
            float2 s = poisson[i];
            float2 rotated;
            rotated.x = s.x * noise.x - s.y * noise.y;
            rotated.y = s.x * noise.y + s.y * noise.x;

            float2 sampleUV = uv + rotated * radiusScreen;
            sampleUV = saturate(sampleUV);

            float sz = tex2D(depthTex, sampleUV).r;

            // Skip sky pixels
            if (sz > 0.999) continue;

            float sampleDepth = LinearDepth(sz);

            // ---- Strict range check ----
            // Reject samples whose depth differs too much from center.
            // This prevents particles/fog at random depths from
            // contributing false occlusion to nearby solid geometry.
            float depthDiff = abs(sampleDepth - depth);
            float maxRange = params0.z * 2.0;
            if (depthDiff > maxRange) continue;

            float3 samplePos = ReconstructPos(sampleUV, sampleDepth);
            float3 diff = samplePos - posC;
            float dist = length(diff);
            float3 dir = diff / (dist + 0.0001);

            // ---- Hemisphere check ----
            // Only count occlusion from samples that are in the
            // hemisphere opposite the surface normal (i.e., "behind"
            // the surface). This naturally rejects flat fog/particle
            // quads since their reconstructed normal is nearly
            // perpendicular to the viewing direction.
            float nDotV = dot(normal, dir);
            float ao = max(0.0, nDotV - params0.w);

            // Smooth range falloff
            float rangeCheck = 1.0 - saturate(depthDiff / maxRange);
            rangeCheck *= rangeCheck;  // Quadratic falloff

            occlusion += ao * rangeCheck;
            validSamples += 1.0;
        }

        // Need at least a few valid samples, otherwise skip
        if (validSamples < 2.0)
            return float4(1, 1, 1, 1);

        occlusion = occlusion / validSamples;
        occlusion = saturate(occlusion * params1.z);
        occlusion *= fogMask;

        float ao = 1.0 - occlusion;
        return float4(ao, ao, ao, 1.0);
    }
)";

// ============================================================
// HLSL: Depth-aware bilateral blur (horizontal)
// ============================================================
static const char* g_szBlurH = R"(
    sampler2D aoTex    : register(s0);
    sampler2D depthTex : register(s1);

    // c0: x=1/width, y=1/height, z=0, w=0
    // c1: x=nearZ, y=farZ, z=0, w=0
    float4 params0 : register(c0);
    float4 params1 : register(c1);

    float LinearDepth(float z)
    {
        float n = params1.x;
        float f = params1.y;
        return (n * f) / (f - z * (f - n));
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float centerAo = tex2D(aoTex, uv).r;
        float centerZ  = tex2D(depthTex, uv).r;

        if (centerZ > 0.999)
            return float4(1, 1, 1, 1);

        float centerDepth = LinearDepth(centerZ);

        // 9-tap bilateral Gaussian blur
        float weights[5] = { 0.227027, 0.194596, 0.121622, 0.054054, 0.016216 };
        float dx = params0.x * 2.0;  // 2-texel stride on half-res = 1 texel full-res

        float totalAo = centerAo * weights[0];
        float totalW  = weights[0];

        for (int i = 1; i < 5; i++)
        {
            float fi = (float)i;
            float2 offset = float2(dx * fi, 0);

            // Positive direction
            float2 uvP = uv + offset;
            float zP = tex2D(depthTex, uvP).r;
            float dP = LinearDepth(zP);
            float wP = weights[i] * (1.0 - saturate(abs(dP - centerDepth) * 0.5));
            totalAo += tex2D(aoTex, uvP).r * wP;
            totalW  += wP;

            // Negative direction
            float2 uvN = uv - offset;
            float zN = tex2D(depthTex, uvN).r;
            float dN = LinearDepth(zN);
            float wN = weights[i] * (1.0 - saturate(abs(dN - centerDepth) * 0.5));
            totalAo += tex2D(aoTex, uvN).r * wN;
            totalW  += wN;
        }

        float ao = totalAo / totalW;
        return float4(ao, ao, ao, 1.0);
    }
)";

// ============================================================
// HLSL: Depth-aware bilateral blur (vertical)
// ============================================================
static const char* g_szBlurV = R"(
    sampler2D aoTex    : register(s0);
    sampler2D depthTex : register(s1);

    float4 params0 : register(c0);
    float4 params1 : register(c1);

    float LinearDepth(float z)
    {
        float n = params1.x;
        float f = params1.y;
        return (n * f) / (f - z * (f - n));
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float centerAo = tex2D(aoTex, uv).r;
        float centerZ  = tex2D(depthTex, uv).r;

        if (centerZ > 0.999)
            return float4(1, 1, 1, 1);

        float centerDepth = LinearDepth(centerZ);

        float weights[5] = { 0.227027, 0.194596, 0.121622, 0.054054, 0.016216 };
        float dy = params0.y * 2.0;

        float totalAo = centerAo * weights[0];
        float totalW  = weights[0];

        for (int i = 1; i < 5; i++)
        {
            float fi = (float)i;
            float2 offset = float2(0, dy * fi);

            float2 uvP = uv + offset;
            float zP = tex2D(depthTex, uvP).r;
            float dP = LinearDepth(zP);
            float wP = weights[i] * (1.0 - saturate(abs(dP - centerDepth) * 0.5));
            totalAo += tex2D(aoTex, uvP).r * wP;
            totalW  += wP;

            float2 uvN = uv - offset;
            float zN = tex2D(depthTex, uvN).r;
            float dN = LinearDepth(zN);
            float wN = weights[i] * (1.0 - saturate(abs(dN - centerDepth) * 0.5));
            totalAo += tex2D(aoTex, uvN).r * wN;
            totalW  += wN;
        }

        float ao = totalAo / totalW;
        return float4(ao, ao, ao, 1.0);
    }
)";

// ============================================================
// HLSL: Composite - multiply AO onto scene
// ============================================================
static const char* g_szComposite = R"(
    sampler2D sceneTex : register(s0);
    sampler2D aoTex    : register(s1);

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float3 scene = tex2D(sceneTex, uv).rgb;
        float ao = tex2D(aoTex, uv).r;
        return float4(scene * ao, 1.0);
    }
)";

// ============================================================
// Render target management
// ============================================================
static void EnsureAoTargets(IDirect3DDevice9* dev, float fullW, float fullH)
{
    UINT hw = (UINT)(fullW * 0.5f);
    UINT hh = (UINT)(fullH * 0.5f);
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;
    if (g_pAoTexA && g_halfW == hw && g_halfH == hh) return;

    if (g_pAoSurfA) { g_pAoSurfA->Release(); g_pAoSurfA = nullptr; }
    if (g_pAoTexA)  { g_pAoTexA->Release();  g_pAoTexA = nullptr; }
    if (g_pAoSurfB) { g_pAoSurfB->Release(); g_pAoSurfB = nullptr; }
    if (g_pAoTexB)  { g_pAoTexB->Release();  g_pAoTexB = nullptr; }

    dev->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                       D3DPOOL_DEFAULT, &g_pAoTexA, nullptr);
    if (g_pAoTexA) g_pAoTexA->GetSurfaceLevel(0, &g_pAoSurfA);

    dev->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                       D3DPOOL_DEFAULT, &g_pAoTexB, nullptr);
    if (g_pAoTexB) g_pAoTexB->GetSurfaceLevel(0, &g_pAoSurfB);

    g_halfW = hw;
    g_halfH = hh;
}

static void ReleaseAoTargets()
{
    if (g_pAoSurfA) { g_pAoSurfA->Release(); g_pAoSurfA = nullptr; }
    if (g_pAoTexA)  { g_pAoTexA->Release();  g_pAoTexA = nullptr; }
    if (g_pAoSurfB) { g_pAoSurfB->Release(); g_pAoSurfB = nullptr; }
    if (g_pAoTexB)  { g_pAoTexB->Release();  g_pAoTexB = nullptr; }
    g_halfW = g_halfH = 0;
}

static void SetupDepthSampler(IDirect3DDevice9* dev, IDirect3DTexture9* pDepth, int stage)
{
    dev->SetTexture(stage, pDepth);
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
}

// ============================================================
// Post-pass: single callback that runs all internal passes
// ============================================================
static bool SsaoPass(IDirect3DDevice9* dev,
                     IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                     IDirect3DTexture9* pDepth,
                     float w, float h, float nearZ, float farZ,
                     void* ud)
{
    if (!g_pAoGenShader || !g_pBlurHShader || !g_pBlurVShader || !g_pCompositeShader)
        return false;
    if (!pDepth)
        return false;

    EnsureAoTargets(dev, w, h);
    if (!g_pAoTexA || !g_pAoTexB) return false;

    float halfW = (float)g_halfW;
    float halfH = (float)g_halfH;

    // Compute approximate aspect ratio and tanHalfFov from projection
    // Default assumptions: 45deg vertical FOV (tan(22.5°) ≈ 0.4142)
    float aspect = w / h;
    float tanHalfFov = 0.4142f;

    // ---- Pass 1: Generate AO (half-res) ----
    dev->SetRenderTarget(0, g_pAoSurfA);
    dev->SetPixelShader(g_pAoGenShader);
    SetupDepthSampler(dev, pDepth, 0);

    float c0[4] = { 1.0f / halfW, 1.0f / halfH, g_radius, g_bias };
    float c1[4] = { nearZ, farZ, g_strength, g_fogStart };
    float c2[4] = { g_fogEnd, (float)g_sampleCount, aspect, tanHalfFov };
    float c3[4] = { g_edgeThresh, 0, 0, 0 };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetPixelShaderConstantF(2, c2, 1);
    dev->SetPixelShaderConstantF(3, c3, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // ---- Pass 2: Bilateral blur H → AO tex B ----
    dev->SetRenderTarget(0, g_pAoSurfB);
    dev->SetPixelShader(g_pBlurHShader);
    dev->SetTexture(0, g_pAoTexA);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    SetupDepthSampler(dev, pDepth, 1);

    float bc0[4] = { 1.0f / halfW, 1.0f / halfH, 0, 0 };
    float bc1[4] = { nearZ, farZ, 0, 0 };
    dev->SetPixelShaderConstantF(0, bc0, 1);
    dev->SetPixelShaderConstantF(1, bc1, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // ---- Pass 3: Bilateral blur V → AO tex A ----
    dev->SetRenderTarget(0, g_pAoSurfA);
    dev->SetPixelShader(g_pBlurVShader);
    dev->SetTexture(0, g_pAoTexB);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    SetupDepthSampler(dev, pDepth, 1);
    dev->SetPixelShaderConstantF(0, bc0, 1);
    dev->SetPixelShaderConstantF(1, bc1, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // ---- Pass 4: Composite AO onto scene → pDst ----
    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pCompositeShader);
    dev->SetTexture(0, pSrc);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetTexture(1, g_pAoTexA);
    dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_api->DrawFullscreenQuad(dev, w, h);

    // Cleanup
    dev->SetTexture(1, nullptr);
    dev->SetPixelShader(nullptr);
    return true;
}

// ============================================================
// Mod lifecycle
// ============================================================
SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;

    g_radius      = api->GetConfigFloat("SSAO", "Radius",      1.2f);
    g_strength    = api->GetConfigFloat("SSAO", "Strength",     0.6f);
    g_bias        = api->GetConfigFloat("SSAO", "Bias",         0.02f);
    g_fogStart    = api->GetConfigFloat("SSAO", "FogStart",     200.0f);
    g_fogEnd      = api->GetConfigFloat("SSAO", "FogEnd",       500.0f);
    g_edgeThresh  = api->GetConfigFloat("SSAO", "EdgeThreshold", 0.3f);
    g_sampleCount = api->GetConfigInt("SSAO",   "Samples",      12);

    if (g_sampleCount < 4)  g_sampleCount = 4;
    if (g_sampleCount > 12) g_sampleCount = 12;

    g_pAoGenShader     = api->CompilePixelShader(g_szAoGen,     "main");
    g_pBlurHShader     = api->CompilePixelShader(g_szBlurH,     "main");
    g_pBlurVShader     = api->CompilePixelShader(g_szBlurV,     "main");
    g_pCompositeShader = api->CompilePixelShader(g_szComposite, "main");

    if (!g_pAoGenShader || !g_pBlurHShader || !g_pBlurVShader || !g_pCompositeShader) {
        if (g_pAoGenShader)     { g_pAoGenShader->Release();     g_pAoGenShader = nullptr; }
        if (g_pBlurHShader)     { g_pBlurHShader->Release();     g_pBlurHShader = nullptr; }
        if (g_pBlurVShader)     { g_pBlurVShader->Release();     g_pBlurVShader = nullptr; }
        if (g_pCompositeShader) { g_pCompositeShader->Release(); g_pCompositeShader = nullptr; }
        api->Log("mod_ssao: shader compile failed");
        return false;
    }

    api->RegisterPostPass("ssao", SsaoPass, 200, nullptr);
    api->Log("mod_ssao: OK (radius=%.2f strength=%.2f samples=%d fog=%.0f~%.0f)",
             g_radius, g_strength, g_sampleCount, g_fogStart, g_fogEnd);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("ssao");
    ReleaseAoTargets();
    if (g_pAoGenShader)     { g_pAoGenShader->Release();     g_pAoGenShader = nullptr; }
    if (g_pBlurHShader)     { g_pBlurHShader->Release();     g_pBlurHShader = nullptr; }
    if (g_pBlurVShader)     { g_pBlurVShader->Release();     g_pBlurVShader = nullptr; }
    if (g_pCompositeShader) { g_pCompositeShader->Release(); g_pCompositeShader = nullptr; }
}

SDGO_MOD_EXPORT void ModDeviceLost()
{
    ReleaseAoTargets();
}

SDGO_MOD_EXPORT void ModDeviceReset(IDirect3DDevice9* /*pDevice*/)
{
    // AO targets are lazily recreated in SsaoPass via EnsureAoTargets
}

SDGO_MOD_EXPORT const char* ModName()    { return "SSAO"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
