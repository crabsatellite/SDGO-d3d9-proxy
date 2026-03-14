// ============================================================
// Cinematic Lens Effects
//
// Simulates real camera lens imperfections:
// 1. Chromatic Aberration — RGB channel separation toward edges
// 2. Barrel Distortion — subtle lens curvature
// 3. Anamorphic Lens Flare — horizontal streaks on bright spots
// 4. Film Grain — subtle analog noise overlay
//
// These small details add the "expensive camera" feeling
// that separates indie from AAA visual quality.
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pLensShader = nullptr;

static const char* g_szLensShader = R"(
    sampler2D sceneTex : register(s0);

    // c0: x=chromatic aberration, y=barrel distortion, z=grain intensity, w=grain size
    // c1: x=flare intensity, y=flare threshold, z=time(frame counter), w=aspect ratio
    float4 params0 : register(c0);
    float4 params1 : register(c1);

    // Simple hash for film grain
    float Hash(float2 p)
    {
        float3 p3 = frac(float3(p.xyx) * 0.1031);
        p3 += dot(p3, p3.yzx + 33.33);
        return frac((p3.x + p3.y) * p3.z);
    }

    float2 BarrelDistort(float2 uv, float strength)
    {
        float2 centered = uv - 0.5;
        float r2 = dot(centered, centered);
        float distort = 1.0 + r2 * strength;
        return saturate(centered * distort + 0.5);
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float chromatic = params0.x;
        float barrel    = params0.y;
        float grainStr  = params0.z;
        float grainSize = params0.w;
        float flareInt  = params1.x;
        float flareThresh = params1.y;
        float time      = params1.z;

        // --- Barrel Distortion ---
        float2 distUV = BarrelDistort(uv, barrel);

        // --- Chromatic Aberration ---
        // Offset increases toward edges
        float2 center = distUV - 0.5;
        float edgeDist = length(center);
        float2 dir = center / max(edgeDist, 0.001);
        float caAmount = chromatic * edgeDist * edgeDist;

        float r = tex2D(sceneTex, saturate(distUV + dir * caAmount)).r;
        float g = tex2D(sceneTex, distUV).g;
        float b = tex2D(sceneTex, saturate(distUV - dir * caAmount)).b;
        float4 color = float4(r, g, b, 1.0);

        // --- Anamorphic Lens Flare (horizontal streak) ---
        if (flareInt > 0.001)
        {
            float3 flare = 0;
            float flareSamples = 17;
            for (int i = -8; i <= 8; i++)
            {
                float t = float(i) / 8.0;
                float2 sampleUV = distUV + float2(t * 0.15, 0.0);  // Horizontal only
                float3 s = tex2D(sceneTex, sampleUV).rgb;
                float lum = dot(s, float3(0.2126, 0.7152, 0.0722));
                float weight = smoothstep(flareThresh, 1.0, lum);
                weight *= (1.0 - abs(t));  // Falloff from center
                flare += s * weight;
            }
            flare /= flareSamples;

            // Blue-ish tint for anamorphic look
            flare *= float3(0.6, 0.8, 1.2);
            color.rgb += flare * flareInt;
        }

        // --- Film Grain ---
        if (grainStr > 0.001)
        {
            float2 grainUV = uv * grainSize + time;
            float grain = Hash(grainUV) - 0.5;
            // Luminance-aware grain: less grain in bright areas
            float lum = dot(color.rgb, float3(0.2126, 0.7152, 0.0722));
            float grainMask = 1.0 - smoothstep(0.3, 0.8, lum);
            color.rgb += grain * grainStr * grainMask;
        }

        return saturate(color);
    }
)";

static float g_chromatic    = 0.004f;
static float g_barrel       = -0.05f;
static float g_grainStr     = 0.04f;
static float g_grainSize    = 512.0f;
static float g_flareInt     = 0.3f;
static float g_flareThresh  = 0.75f;
static float g_frameCounter = 0;

static bool LensPass(IDirect3DDevice9* dev,
                     IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                     IDirect3DTexture9* pDepth,
                     float w, float h, float nearZ, float farZ,
                     void* ud)
{
    if (!g_pLensShader) return false;

    g_frameCounter += 0.01f;
    if (g_frameCounter > 100.0f) g_frameCounter -= 100.0f;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pLensShader);
    dev->SetTexture(0, pSrc);

    float c0[4] = { g_chromatic, g_barrel, g_grainStr, g_grainSize };
    float c1[4] = { g_flareInt, g_flareThresh, g_frameCounter, w / h };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);

    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetPixelShader(nullptr);
    return true;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;
    g_chromatic   = api->GetConfigFloat("LensEffects", "ChromaticAberration", 0.004f);
    g_barrel      = api->GetConfigFloat("LensEffects", "BarrelDistortion",   -0.05f);
    g_grainStr    = api->GetConfigFloat("LensEffects", "FilmGrain",           0.04f);
    g_grainSize   = api->GetConfigFloat("LensEffects", "GrainSize",           512.0f);
    g_flareInt    = api->GetConfigFloat("LensEffects", "FlareIntensity",      0.3f);
    g_flareThresh = api->GetConfigFloat("LensEffects", "FlareThreshold",      0.75f);

    g_pLensShader = api->CompilePixelShader(g_szLensShader, "main");
    if (!g_pLensShader) {
        api->Log("mod_lenseffects: shader compile failed");
        return false;
    }

    api->RegisterPostPass("lenseffects", LensPass, 600, nullptr);
    api->Log("mod_lenseffects: OK (CA=%.4f grain=%.3f flare=%.2f)",
             g_chromatic, g_grainStr, g_flareInt);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("lenseffects");
    if (g_pLensShader) { g_pLensShader->Release(); g_pLensShader = nullptr; }
}

SDGO_MOD_EXPORT const char* ModName()    { return "Lens Effects"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
