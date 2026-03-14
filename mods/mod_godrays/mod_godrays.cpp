// ============================================================
// God Rays — Volumetric Light Scattering
//
// 2-pass screen-space radial blur from a configurable light position.
// Pass 1: Extract bright pixels to half-res RT
// Pass 2: Radial-blur bright tex toward light pos, add to scene
//
// Based on GPU Gems 3 Ch.13 "Volumetric Light Scattering".
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pExtractShader = nullptr;
static IDirect3DPixelShader9* g_pScatterCompositeShader = nullptr;

// Pass 1: Soft threshold to extract bright areas
static const char* g_szExtract = R"(
    sampler2D sceneTex : register(s0);
    float4 params : register(c0);  // x=threshold, y=softKnee

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float3 c = tex2D(sceneTex, uv).rgb;
        float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
        float knee = params.x * params.y;
        float soft = lum - params.x + knee;
        soft = clamp(soft, 0.0, 2.0 * knee);
        soft = soft * soft / (4.0 * knee + 0.0001);
        float w = max(soft, lum - params.x) / max(lum, 0.0001);
        return float4(c * saturate(w), 1.0);
    }
)";

// Pass 2: Radial blur on bright tex + additive composite with scene
static const char* g_szScatterComposite = R"(
    sampler2D sceneTex  : register(s0);
    sampler2D brightTex : register(s1);

    float4 lightPos      : register(c0);  // xy = screen-space light (0..1)
    float4 scatterParams : register(c1);  // x=density, y=decay, z=weight, w=exposure
    float4 tintIntensity : register(c2);  // xyz=tint RGB, w=intensity

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float4 scene = tex2D(sceneTex, uv);

        // Radial blur toward light source
        float2 ray = lightPos.xy - uv;
        float dist = length(ray);
        float2 delta = (ray / max(dist, 0.001)) * min(dist, 0.5)
                       * scatterParams.x / 48.0;

        float2 sampleUV = uv;
        float3 accum = 0;
        float illumDecay = 1.0;

        for (int i = 0; i < 48; i++)
        {
            sampleUV += delta;
            float3 s = tex2D(brightTex, sampleUV).rgb;
            accum += s * illumDecay * scatterParams.z;
            illumDecay *= scatterParams.y;
        }

        accum *= scatterParams.w;  // exposure
        scene.rgb += accum * tintIntensity.xyz * tintIntensity.w;
        return scene;
    }
)";

// Config
static float g_lightX = 0.5f, g_lightY = 0.15f;
static float g_density   = 0.85f;
static float g_decay     = 0.96f;
static float g_weight    = 0.35f;
static float g_exposure  = 0.40f;
static float g_threshold = 0.65f;
static float g_softKnee  = 0.5f;
static float g_intensity = 1.2f;
static float g_tintR = 1.0f, g_tintG = 0.93f, g_tintB = 0.76f;

// Half-res RT for bright extraction
static IDirect3DTexture9*  g_pBrightTex  = nullptr;
static IDirect3DSurface9*  g_pBrightSurf = nullptr;
static UINT g_rtW = 0, g_rtH = 0;

static void EnsureRT(IDirect3DDevice9* dev, UINT w, UINT h)
{
    UINT hw = w / 2, hh = h / 2;
    if (g_pBrightTex && g_rtW == hw && g_rtH == hh) return;
    if (g_pBrightSurf) { g_pBrightSurf->Release(); g_pBrightSurf = nullptr; }
    if (g_pBrightTex)  { g_pBrightTex->Release();  g_pBrightTex  = nullptr; }
    if (SUCCEEDED(dev->CreateTexture(hw, hh, 1, D3DUSAGE_RENDERTARGET,
                                     D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                                     &g_pBrightTex, nullptr))) {
        g_pBrightTex->GetSurfaceLevel(0, &g_pBrightSurf);
        g_rtW = hw; g_rtH = hh;
    }
}

static bool GodraysPass(IDirect3DDevice9* dev,
                        IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                        IDirect3DTexture9* pDepth,
                        float w, float h, float nearZ, float farZ,
                        void* ud)
{
    if (!g_pExtractShader || !g_pScatterCompositeShader) return false;

    EnsureRT(dev, (UINT)w, (UINT)h);
    if (!g_pBrightTex) return false;

    // --- Pass 1: Extract bright → half-res RT ---
    IDirect3DSurface9* pOldRT = nullptr;
    dev->GetRenderTarget(0, &pOldRT);

    dev->SetRenderTarget(0, g_pBrightSurf);
    dev->SetPixelShader(g_pExtractShader);
    dev->SetTexture(0, pSrc);
    float ep[4] = { g_threshold, g_softKnee, 0, 0 };
    dev->SetPixelShaderConstantF(0, ep, 1);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_api->DrawFullscreenQuad(dev, (float)g_rtW, (float)g_rtH);

    // --- Pass 2: Scatter + Composite → pDst ---
    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pScatterCompositeShader);
    dev->SetTexture(0, pSrc);         // Original scene
    dev->SetTexture(1, g_pBrightTex); // Bright extraction
    float lp[4] = { g_lightX, g_lightY, 0, 0 };
    float sp[4] = { g_density, g_decay, g_weight, g_exposure };
    float ti[4] = { g_tintR, g_tintG, g_tintB, g_intensity };
    dev->SetPixelShaderConstantF(0, lp, 1);
    dev->SetPixelShaderConstantF(1, sp, 1);
    dev->SetPixelShaderConstantF(2, ti, 1);
    dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_api->DrawFullscreenQuad(dev, w, h);

    // Cleanup
    dev->SetTexture(1, nullptr);
    dev->SetPixelShader(nullptr);
    if (pOldRT) pOldRT->Release();
    return true;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;
    g_lightX    = api->GetConfigFloat("GodRays", "LightX",    0.5f);
    g_lightY    = api->GetConfigFloat("GodRays", "LightY",    0.15f);
    g_density   = api->GetConfigFloat("GodRays", "Density",   0.85f);
    g_decay     = api->GetConfigFloat("GodRays", "Decay",     0.96f);
    g_weight    = api->GetConfigFloat("GodRays", "Weight",    0.35f);
    g_exposure  = api->GetConfigFloat("GodRays", "Exposure",  0.40f);
    g_threshold = api->GetConfigFloat("GodRays", "Threshold", 0.65f);
    g_intensity = api->GetConfigFloat("GodRays", "Intensity", 1.2f);
    g_tintR     = api->GetConfigFloat("GodRays", "TintR",     1.0f);
    g_tintG     = api->GetConfigFloat("GodRays", "TintG",     0.93f);
    g_tintB     = api->GetConfigFloat("GodRays", "TintB",     0.76f);

    g_pExtractShader          = api->CompilePixelShader(g_szExtract,           "main");
    g_pScatterCompositeShader = api->CompilePixelShader(g_szScatterComposite,  "main");
    if (!g_pExtractShader || !g_pScatterCompositeShader) {
        api->Log("mod_godrays: shader compile failed");
        return false;
    }

    api->RegisterPostPass("godrays", GodraysPass, 400, nullptr);
    api->Log("mod_godrays: OK (light=%.2f,%.2f)", g_lightX, g_lightY);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("godrays");
    if (g_pBrightSurf) { g_pBrightSurf->Release(); g_pBrightSurf = nullptr; }
    if (g_pBrightTex)  { g_pBrightTex->Release();  g_pBrightTex  = nullptr; }
    if (g_pExtractShader)          { g_pExtractShader->Release();          g_pExtractShader = nullptr; }
    if (g_pScatterCompositeShader) { g_pScatterCompositeShader->Release(); g_pScatterCompositeShader = nullptr; }
}

SDGO_MOD_EXPORT void ModDeviceLost()
{
    if (g_pBrightSurf) { g_pBrightSurf->Release(); g_pBrightSurf = nullptr; }
    if (g_pBrightTex)  { g_pBrightTex->Release();  g_pBrightTex  = nullptr; }
    g_rtW = 0; g_rtH = 0;
}

SDGO_MOD_EXPORT void ModDeviceReset(IDirect3DDevice9* /*pDevice*/)
{
    // Lazily recreated in GodraysPass via EnsureRT
}

SDGO_MOD_EXPORT const char* ModName()    { return "God Rays"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
