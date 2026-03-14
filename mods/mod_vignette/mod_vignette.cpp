// ============================================================
// Cinematic Vignette
//
// Screen edge darkening with optional color tint:
// 1. Smooth radial falloff from center
// 2. Configurable shape (oval via aspect correction)
// 3. Inner color tint option (warm/cool edge tinting)
// 4. Subtle enough to enhance without distracting
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pVignetteShader = nullptr;

static const char* g_szVignetteShader = R"(
    sampler2D sceneTex : register(s0);

    // c0: x=intensity, y=radius, z=softness, w=roundness
    // c1: xyz=tint color, w=tint strength
    float4 params : register(c0);
    float4 tint   : register(c1);

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float4 color = tex2D(sceneTex, uv);

        float2 center = uv - 0.5;

        // Roundness: 1.0 = circular, <1 = wider oval
        center.x *= params.w;

        float dist = length(center);
        float vig = smoothstep(params.y, params.y - params.z, dist);
        vig = lerp(1.0, vig, params.x);

        // Optional color tint in darkened areas
        float tintMask = (1.0 - vig) * tint.w;
        color.rgb = lerp(color.rgb * vig, tint.xyz * color.rgb * vig, tintMask);

        return color;
    }
)";

static float g_intensity = 0.55f;
static float g_radius    = 0.80f;
static float g_softness  = 0.50f;
static float g_roundness = 0.85f;
static float g_tintR = 0.9f, g_tintG = 0.85f, g_tintB = 0.8f;
static float g_tintStr = 0.2f;

static bool VignettePass(IDirect3DDevice9* dev,
                         IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                         IDirect3DTexture9* pDepth,
                         float w, float h, float nearZ, float farZ,
                         void* ud)
{
    if (!g_pVignetteShader) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pVignetteShader);
    dev->SetTexture(0, pSrc);

    float c0[4] = { g_intensity, g_radius, g_softness, g_roundness };
    float c1[4] = { g_tintR, g_tintG, g_tintB, g_tintStr };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);

    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetPixelShader(nullptr);
    return true;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;
    g_intensity = api->GetConfigFloat("Vignette", "Intensity", 0.55f);
    g_radius    = api->GetConfigFloat("Vignette", "Radius",    0.80f);
    g_softness  = api->GetConfigFloat("Vignette", "Softness",  0.50f);
    g_roundness = api->GetConfigFloat("Vignette", "Roundness", 0.85f);
    g_tintR     = api->GetConfigFloat("Vignette", "TintR",     0.9f);
    g_tintG     = api->GetConfigFloat("Vignette", "TintG",     0.85f);
    g_tintB     = api->GetConfigFloat("Vignette", "TintB",     0.8f);
    g_tintStr   = api->GetConfigFloat("Vignette", "TintStrength", 0.2f);

    g_pVignetteShader = api->CompilePixelShader(g_szVignetteShader, "main");
    if (!g_pVignetteShader) {
        api->Log("mod_vignette: shader compile failed");
        return false;
    }

    api->RegisterPostPass("vignette", VignettePass, 700, nullptr);
    api->Log("mod_vignette: OK (intensity=%.2f radius=%.2f)", g_intensity, g_radius);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("vignette");
    if (g_pVignetteShader) { g_pVignetteShader->Release(); g_pVignetteShader = nullptr; }
}

SDGO_MOD_EXPORT const char* ModName()    { return "Vignette"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
