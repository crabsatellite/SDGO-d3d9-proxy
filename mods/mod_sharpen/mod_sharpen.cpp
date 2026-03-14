// ============================================================
// CAS - Contrast Adaptive Sharpening
//
// Sharpens image while avoiding halos on high-contrast edges.
// Priority 500: runs after FXAA to restore detail lost by AA.
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pShader = nullptr;
static float g_strength = 0.6f;

static const char* g_szShader = R"(
    sampler2D SceneSampler : register(s0);
    float4 Params : register(c0);  // x=strength, y=1/width, z=1/height

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float3 c = tex2D(SceneSampler, uv).rgb;
        float3 n = tex2D(SceneSampler, uv + float2(0, -Params.z)).rgb;
        float3 s = tex2D(SceneSampler, uv + float2(0,  Params.z)).rgb;
        float3 e = tex2D(SceneSampler, uv + float2( Params.y, 0)).rgb;
        float3 w = tex2D(SceneSampler, uv + float2(-Params.y, 0)).rgb;

        float3 blur = (n + s + e + w) * 0.25;
        float3 diff = c - blur;

        float localContrast = length(diff);
        float adaptiveStrength = Params.x / (1.0 + localContrast * 5.0);

        float3 sharp = c + diff * adaptiveStrength;
        return float4(saturate(sharp), 1.0);
    }
)";

static bool SharpenPass(IDirect3DDevice9* dev,
                        IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                        IDirect3DTexture9* pDepth,
                        float w, float h, float nearZ, float farZ,
                        void* ud)
{
    if (!g_pShader) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pShader);
    dev->SetTexture(0, pSrc);

    float c0[4] = { g_strength, 1.0f / w, 1.0f / h, 0 };
    dev->SetPixelShaderConstantF(0, c0, 1);

    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetPixelShader(nullptr);
    return true;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;
    g_strength = api->GetConfigFloat("Sharpen", "Strength", 0.6f);

    g_pShader = api->CompilePixelShader(g_szShader, "main");
    if (!g_pShader) {
        api->Log("mod_sharpen: shader compile failed");
        return false;
    }
    api->RegisterPostPass("sharpen", SharpenPass, 500, nullptr);
    api->Log("mod_sharpen: OK (strength=%.2f)", g_strength);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("sharpen");
    if (g_pShader) { g_pShader->Release(); g_pShader = nullptr; }
}

SDGO_MOD_EXPORT const char* ModName()    { return "Sharpen"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
