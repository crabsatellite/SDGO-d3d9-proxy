// ============================================================
// Bloom - Bright area glow
//
// Multi-pass: extract bright → blur H → blur V → composite.
// Creates its own half-res render targets.
// Priority 100: runs first to extract from raw scene.
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pExtractShader = nullptr;
static IDirect3DPixelShader9* g_pBlurShader = nullptr;
static IDirect3DPixelShader9* g_pCompositeShader = nullptr;

static IDirect3DTexture9*  g_pBloomTexA = nullptr;
static IDirect3DSurface9*  g_pBloomSurfA = nullptr;
static IDirect3DTexture9*  g_pBloomTexB = nullptr;
static IDirect3DSurface9*  g_pBloomSurfB = nullptr;
static UINT g_halfW = 0, g_halfH = 0;

static float g_threshold = 0.7f;
static float g_intensity = 0.4f;

static const char* g_szExtract = R"(
    sampler2D SceneSampler : register(s0);
    float4 Params : register(c0);  // x=threshold, y=1/width, z=1/height

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float2 ht = float2(Params.y, Params.z) * 0.5;
        float3 c  = tex2D(SceneSampler, uv + float2(-ht.x, -ht.y)).rgb;
        c += tex2D(SceneSampler, uv + float2( ht.x, -ht.y)).rgb;
        c += tex2D(SceneSampler, uv + float2(-ht.x,  ht.y)).rgb;
        c += tex2D(SceneSampler, uv + float2( ht.x,  ht.y)).rgb;
        c *= 0.25;

        float brightness = dot(c, float3(0.299, 0.587, 0.114));
        float knee = 0.3;
        float soft = clamp((brightness - Params.x + knee) / (2.0 * knee), 0.0, 1.0);
        soft = soft * soft;
        float contribution = max(brightness - Params.x, soft * knee) / max(brightness, 0.001);
        contribution = saturate(contribution);

        // Tone-compress bloom to avoid harsh white blocks on explosions
        float3 bloom = c * contribution;
        bloom = bloom / (1.0 + bloom);

        return float4(bloom, 1.0);
    }
)";

static const char* g_szBlur = R"(
    sampler2D BloomSampler : register(s0);
    float4 Params : register(c0);  // x=1/texWidth, y=1/texHeight, z=dirX, w=dirY

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float2 dir = float2(Params.x * Params.z, Params.y * Params.w);

        float3 c  = tex2D(BloomSampler, uv - dir * 4.0).rgb * 0.0162;
        c += tex2D(BloomSampler, uv - dir * 3.0).rgb * 0.0540;
        c += tex2D(BloomSampler, uv - dir * 2.0).rgb * 0.1216;
        c += tex2D(BloomSampler, uv - dir * 1.0).rgb * 0.1945;
        c += tex2D(BloomSampler, uv            ).rgb * 0.2270;
        c += tex2D(BloomSampler, uv + dir * 1.0).rgb * 0.1945;
        c += tex2D(BloomSampler, uv + dir * 2.0).rgb * 0.1216;
        c += tex2D(BloomSampler, uv + dir * 3.0).rgb * 0.0540;
        c += tex2D(BloomSampler, uv + dir * 4.0).rgb * 0.0162;

        return float4(c, 1.0);
    }
)";

static const char* g_szComposite = R"(
    sampler2D BloomSampler : register(s0);
    float4 Params : register(c0);  // x=intensity

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float3 bloom = tex2D(BloomSampler, uv).rgb;
        return float4(bloom * Params.x, 1.0);
    }
)";

static void EnsureBloomTargets(IDirect3DDevice9* dev, float fullW, float fullH)
{
    UINT hw = (UINT)(fullW * 0.5f);
    UINT hh = (UINT)(fullH * 0.5f);
    if (hw < 1) hw = 1;
    if (hh < 1) hh = 1;
    if (g_pBloomTexA && g_halfW == hw && g_halfH == hh) return;

    // Release old
    if (g_pBloomSurfA) { g_pBloomSurfA->Release(); g_pBloomSurfA = nullptr; }
    if (g_pBloomTexA)  { g_pBloomTexA->Release();  g_pBloomTexA = nullptr; }
    if (g_pBloomSurfB) { g_pBloomSurfB->Release(); g_pBloomSurfB = nullptr; }
    if (g_pBloomTexB)  { g_pBloomTexB->Release();  g_pBloomTexB = nullptr; }

    dev->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                       D3DPOOL_DEFAULT, &g_pBloomTexA, nullptr);
    if (g_pBloomTexA) g_pBloomTexA->GetSurfaceLevel(0, &g_pBloomSurfA);

    dev->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                       D3DPOOL_DEFAULT, &g_pBloomTexB, nullptr);
    if (g_pBloomTexB) g_pBloomTexB->GetSurfaceLevel(0, &g_pBloomSurfB);

    g_halfW = hw;
    g_halfH = hh;
}

static bool BloomPass(IDirect3DDevice9* dev,
                      IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                      IDirect3DTexture9* pDepth,
                      float w, float h, float nearZ, float farZ,
                      void* ud)
{
    if (!g_pExtractShader || !g_pBlurShader || !g_pCompositeShader) return false;

    EnsureBloomTargets(dev, w, h);
    if (!g_pBloomTexA || !g_pBloomTexB) return false;

    float halfW = (float)g_halfW;
    float halfH = (float)g_halfH;

    // 1) Bright extract + downsample → bloomA
    dev->SetRenderTarget(0, g_pBloomSurfA);
    dev->SetTexture(0, pSrc);
    dev->SetPixelShader(g_pExtractShader);
    float ep[4] = { g_threshold, 1.0f / w, 1.0f / h, 0 };
    dev->SetPixelShaderConstantF(0, ep, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // Two-pass Gaussian blur, repeated twice for wider spread.
    // This eliminates hard edges on fog/particle quads.
    float bh[4] = { 1.0f / halfW, 1.0f / halfH, 1.0f, 0.0f };
    float bv[4] = { 1.0f / halfW, 1.0f / halfH, 0.0f, 1.0f };
    dev->SetPixelShader(g_pBlurShader);

    // 2) H blur → bloomB
    dev->SetRenderTarget(0, g_pBloomSurfB);
    dev->SetTexture(0, g_pBloomTexA);
    dev->SetPixelShaderConstantF(0, bh, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // 3) V blur → bloomA
    dev->SetRenderTarget(0, g_pBloomSurfA);
    dev->SetTexture(0, g_pBloomTexB);
    dev->SetPixelShaderConstantF(0, bv, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // 4) H blur → bloomB (second iteration)
    dev->SetRenderTarget(0, g_pBloomSurfB);
    dev->SetTexture(0, g_pBloomTexA);
    dev->SetPixelShaderConstantF(0, bh, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // 5) V blur → bloomA (second iteration)
    dev->SetRenderTarget(0, g_pBloomSurfA);
    dev->SetTexture(0, g_pBloomTexB);
    dev->SetPixelShaderConstantF(0, bv, 1);
    g_api->DrawFullscreenQuad(dev, halfW, halfH);

    // 4) Composite: additive blend bloom onto dst
    // First copy src → dst (so we have the scene as base)
    dev->SetRenderTarget(0, pDst);
    dev->SetTexture(0, pSrc);
    dev->SetPixelShader(nullptr);
    g_api->DrawFullscreenQuad(dev, w, h);

    // Then add bloom on top
    dev->SetTexture(0, g_pBloomTexA);
    dev->SetPixelShader(g_pCompositeShader);
    float cp[4] = { g_intensity, 0, 0, 0 };
    dev->SetPixelShaderConstantF(0, cp, 1);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_api->DrawFullscreenQuad(dev, w, h);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    dev->SetPixelShader(nullptr);
    return true;
}

static void ReleaseBloomTargets()
{
    if (g_pBloomSurfA) { g_pBloomSurfA->Release(); g_pBloomSurfA = nullptr; }
    if (g_pBloomTexA)  { g_pBloomTexA->Release();  g_pBloomTexA = nullptr; }
    if (g_pBloomSurfB) { g_pBloomSurfB->Release(); g_pBloomSurfB = nullptr; }
    if (g_pBloomTexB)  { g_pBloomTexB->Release();  g_pBloomTexB = nullptr; }
    g_halfW = g_halfH = 0;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;
    g_threshold = api->GetConfigFloat("Bloom", "Threshold", 0.7f);
    g_intensity = api->GetConfigFloat("Bloom", "Intensity", 0.4f);

    g_pExtractShader   = api->CompilePixelShader(g_szExtract, "main");
    g_pBlurShader      = api->CompilePixelShader(g_szBlur, "main");
    g_pCompositeShader = api->CompilePixelShader(g_szComposite, "main");

    if (!g_pExtractShader || !g_pBlurShader || !g_pCompositeShader) {
        if (g_pExtractShader)   { g_pExtractShader->Release();   g_pExtractShader = nullptr; }
        if (g_pBlurShader)      { g_pBlurShader->Release();      g_pBlurShader = nullptr; }
        if (g_pCompositeShader) { g_pCompositeShader->Release(); g_pCompositeShader = nullptr; }
        api->Log("mod_bloom: shader compile failed");
        return false;
    }
    api->RegisterPostPass("bloom", BloomPass, 100, nullptr);
    api->Log("mod_bloom: OK (threshold=%.2f intensity=%.2f)", g_threshold, g_intensity);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("bloom");
    ReleaseBloomTargets();
    if (g_pExtractShader)   { g_pExtractShader->Release();   g_pExtractShader = nullptr; }
    if (g_pBlurShader)      { g_pBlurShader->Release();      g_pBlurShader = nullptr; }
    if (g_pCompositeShader) { g_pCompositeShader->Release(); g_pCompositeShader = nullptr; }
}

SDGO_MOD_EXPORT void ModDeviceLost()
{
    ReleaseBloomTargets();
}

SDGO_MOD_EXPORT void ModDeviceReset(IDirect3DDevice9* /*pDevice*/)
{
    // Bloom targets are lazily recreated in BloomPass via EnsureBloomTargets
}

SDGO_MOD_EXPORT const char* ModName()    { return "Bloom"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
