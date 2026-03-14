// ============================================================
// FXAA - Fast Approximate Anti-Aliasing
//
// Smooths jagged edges on 3D geometry without blurring textures.
// Priority 400: runs after color/lighting effects, before sharpen.
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pShader = nullptr;

static const char* g_szShader = R"(
    sampler2D SceneSampler : register(s0);
    float4 TexelSize : register(c0);

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float3 rgbM  = tex2D(SceneSampler, uv).rgb;
        float3 rgbNW = tex2D(SceneSampler, uv + float2(-1, -1) * TexelSize.xy).rgb;
        float3 rgbNE = tex2D(SceneSampler, uv + float2( 1, -1) * TexelSize.xy).rgb;
        float3 rgbSW = tex2D(SceneSampler, uv + float2(-1,  1) * TexelSize.xy).rgb;
        float3 rgbSE = tex2D(SceneSampler, uv + float2( 1,  1) * TexelSize.xy).rgb;

        float3 luma = float3(0.299, 0.587, 0.114);
        float lumaNW = dot(rgbNW, luma);
        float lumaNE = dot(rgbNE, luma);
        float lumaSW = dot(rgbSW, luma);
        float lumaSE = dot(rgbSE, luma);
        float lumaM  = dot(rgbM,  luma);

        float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
        float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

        float2 dir;
        dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
        dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

        float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);
        float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
        dir = clamp(dir * rcpDirMin, -8.0, 8.0) * TexelSize.xy;

        float3 rgbA = 0.5 * (
            tex2D(SceneSampler, uv + dir * (1.0/3.0 - 0.5)).rgb +
            tex2D(SceneSampler, uv + dir * (2.0/3.0 - 0.5)).rgb);

        float3 rgbB = rgbA * 0.5 + 0.25 * (
            tex2D(SceneSampler, uv + dir * -0.5).rgb +
            tex2D(SceneSampler, uv + dir *  0.5).rgb);

        float lumaB = dot(rgbB, luma);
        float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;
        return float4(result, 1.0);
    }
)";

static bool FXAAPass(IDirect3DDevice9* dev,
                     IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                     IDirect3DTexture9* pDepth,
                     float w, float h, float nearZ, float farZ,
                     void* ud)
{
    if (!g_pShader) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pShader);
    dev->SetTexture(0, pSrc);

    float c0[4] = { 1.0f / w, 1.0f / h, 0, 0 };
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
    g_pShader = api->CompilePixelShader(g_szShader, "main");
    if (!g_pShader) {
        api->Log("mod_fxaa: shader compile failed");
        return false;
    }
    api->RegisterPostPass("fxaa", FXAAPass, 400, nullptr);
    api->Log("mod_fxaa: OK");
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("fxaa");
    if (g_pShader) { g_pShader->Release(); g_pShader = nullptr; }
}

SDGO_MOD_EXPORT const char* ModName()    { return "FXAA"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
