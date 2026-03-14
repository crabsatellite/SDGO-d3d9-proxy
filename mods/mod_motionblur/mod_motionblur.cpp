// ============================================================
// Camera Motion Blur
//
// Per-pixel motion blur using depth-based velocity estimation:
// 1. Radial blur from screen center (camera zoom / dolly effect)
// 2. Depth-aware: distant objects blur more (parallax simulation)
// 3. Configurable intensity, sample count, center offset
//
// Creates a cinematic sense of speed during combat.
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pBlurShader = nullptr;

static const char* g_szBlurShader = R"(
    sampler2D sceneTex : register(s0);
    sampler2D depthTex : register(s1);

    // c0: x=centerX, y=centerY, z=intensity, w=depthInfluence
    // c1: x=nearZ, y=farZ, z=maxBlur, w=sampleWeight
    float4 centerInt : register(c0);
    float4 projMax   : register(c1);

    float LinearDepth(float2 uv)
    {
        float z = tex2D(depthTex, uv).r;
        float n = projMax.x;
        float f = projMax.y;
        return (n * f) / (f - z * (f - n));
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float2 center = float2(centerInt.x, centerInt.y);
        float intensity = centerInt.z;
        float depthInfl = centerInt.w;

        float depth = LinearDepth(uv);

        // Radial velocity: direction from center, scaled by distance from center
        float2 velocity = (uv - center) * intensity;

        // Depth influence: far objects blur more
        float depthFactor = saturate(depth * depthInfl);
        velocity *= (0.3 + depthFactor * 0.7);

        // Clamp max blur
        float speed = length(velocity);
        if (speed > projMax.z)
            velocity = velocity / speed * projMax.z;

        if (speed < 0.0005) return tex2D(sceneTex, uv);

        // 12-sample directional blur along velocity
        float4 color = 0;
        float totalWeight = 0;

        for (int i = -6; i <= 5; i++)
        {
            float t = float(i) / 6.0;
            float2 sampleUV = uv + velocity * t;

            // Gaussian-like weight: center samples stronger
            float w = 1.0 - abs(t) * 0.5;

            color += tex2D(sceneTex, sampleUV) * w;
            totalWeight += w;
        }

        return color / totalWeight;
    }
)";

static float g_centerX      = 0.5f;
static float g_centerY      = 0.5f;
static float g_intensity    = 0.015f;  // Subtle by default
static float g_depthInfl    = 0.008f;
static float g_maxBlur      = 0.03f;

static bool MotionBlurPass(IDirect3DDevice9* dev,
                           IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                           IDirect3DTexture9* pDepth,
                           float w, float h, float nearZ, float farZ,
                           void* ud)
{
    if (!g_pBlurShader || !pDepth) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pBlurShader);
    dev->SetTexture(0, pSrc);
    dev->SetTexture(1, pDepth);

    float c0[4] = { g_centerX, g_centerY, g_intensity, g_depthInfl };
    float c1[4] = { nearZ, farZ, g_maxBlur, 0 };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);

    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);

    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetTexture(1, nullptr);
    dev->SetPixelShader(nullptr);
    return true;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;
    g_centerX   = api->GetConfigFloat("MotionBlur", "CenterX",       0.5f);
    g_centerY   = api->GetConfigFloat("MotionBlur", "CenterY",       0.5f);
    g_intensity = api->GetConfigFloat("MotionBlur", "Intensity",      0.015f);
    g_depthInfl = api->GetConfigFloat("MotionBlur", "DepthInfluence", 0.008f);
    g_maxBlur   = api->GetConfigFloat("MotionBlur", "MaxBlur",        0.03f);

    g_pBlurShader = api->CompilePixelShader(g_szBlurShader, "main");
    if (!g_pBlurShader) {
        api->Log("mod_motionblur: shader compile failed");
        return false;
    }

    api->RegisterPostPass("motionblur", MotionBlurPass, 300, nullptr);
    api->Log("mod_motionblur: OK (intensity=%.3f)", g_intensity);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("motionblur");
    if (g_pBlurShader) { g_pBlurShader->Release(); g_pBlurShader = nullptr; }
}

SDGO_MOD_EXPORT const char* ModName()    { return "Motion Blur"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
