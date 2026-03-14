// ============================================================
// SSAO - Screen Space Ambient Occlusion
//
// Darkens crevices and occluded areas for depth perception.
// Reads INTZ depth texture from the framework.
// Priority 200: runs early, after bloom, before color effects.
//
// KNOWN ISSUE: Fog/smoke/particle pixels cause block artifacts
// because INTZ depth only contains opaque geometry depth.
// Needs proper fog detection (not threshold-based) to fix.
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pShader = nullptr;
static float g_radius = 1.0f;
static float g_intensity = 1.5f;

static const char* g_szShader = R"(
    sampler2D SceneSampler : register(s0);
    sampler2D DepthSampler : register(s1);
    float4 Params : register(c0);   // x=1/width, y=1/height, z=radius, w=intensity
    float4 ProjInfo : register(c1); // x=nearZ, y=farZ

    float linearizeDepth(float z)
    {
        float n = ProjInfo.x;
        float f = ProjInfo.y;
        return n * f / (f - z * (f - n));
    }

    float3 getViewPos(float2 uv)
    {
        float z = tex2D(DepthSampler, uv).r;
        float linearZ = linearizeDepth(z);
        float2 ndc = uv * 2.0 - 1.0;
        ndc.y = -ndc.y;
        return float3(ndc * linearZ, linearZ);
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float3 color = tex2D(SceneSampler, uv).rgb;
        float depth = tex2D(DepthSampler, uv).r;

        if (depth > 0.999)
            return float4(color, 1.0);

        float3 pos = getViewPos(uv);

        float3 posR = getViewPos(uv + float2(Params.x, 0));
        float3 posL = getViewPos(uv - float2(Params.x, 0));
        float3 posD = getViewPos(uv + float2(0, Params.y));
        float3 posU = getViewPos(uv - float2(0, Params.y));

        float3 dx = (abs(posR.z - pos.z) < abs(pos.z - posL.z)) ? (posR - pos) : (pos - posL);
        float3 dy = (abs(posD.z - pos.z) < abs(pos.z - posU.z)) ? (posD - pos) : (pos - posU);
        float3 normal = normalize(cross(dx, dy));

        float radius = Params.z / pos.z;
        float occlusion = 0.0;
        int samples = 16;

        float2 pixelPos = uv / float2(Params.x, Params.y);
        float angle = frac(sin(dot(pixelPos, float2(12.9898, 78.233))) * 43758.5453) * 6.2832;

        // 16-sample Poisson disc (better coverage, less noise)
        float2 offsets[16] = {
            float2(-0.094, 0.929), float2( 0.440, 0.768),
            float2(-0.595, 0.627), float2( 0.806, 0.404),
            float2(-0.876, 0.179), float2( 0.938,-0.105),
            float2(-0.713,-0.383), float2( 0.553,-0.586),
            float2(-0.254,-0.789), float2( 0.168,-0.941),
            float2( 0.310, 0.271), float2(-0.369, 0.134),
            float2( 0.098,-0.327), float2(-0.186,-0.441),
            float2( 0.655, 0.048), float2(-0.509,-0.074)
        };

        float cs = cos(angle);
        float sn = sin(angle);

        for (int i = 0; i < samples; i++)
        {
            float2 off = float2(
                offsets[i].x * cs - offsets[i].y * sn,
                offsets[i].x * sn + offsets[i].y * cs
            ) * radius;

            float2 sampleUV = uv + off;
            float3 samplePos = getViewPos(sampleUV);
            float3 diff = samplePos - pos;

            float dist = length(diff);
            float3 dir = diff / (dist + 0.0001);

            float nDotDir = max(dot(normal, dir), 0.0);
            float rangeCheck = smoothstep(0.0, 1.0, Params.z / (abs(dist) + 0.001));

            occlusion += nDotDir * rangeCheck;
        }

        occlusion = 1.0 - (occlusion / samples) * Params.w;
        occlusion = saturate(occlusion);

        return float4(color * occlusion, 1.0);
    }
)";

static bool SSAOPass(IDirect3DDevice9* dev,
                     IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                     IDirect3DTexture9* pDepth,
                     float w, float h, float nearZ, float farZ,
                     void* ud)
{
    if (!g_pShader || !pDepth) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pShader);
    dev->SetTexture(0, pSrc);
    dev->SetTexture(1, pDepth);

    dev->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(1, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE);

    float c0[4] = { 1.0f / w, 1.0f / h, g_radius, g_intensity };
    dev->SetPixelShaderConstantF(0, c0, 1);
    float c1[4] = { nearZ, farZ, 0, 0 };
    dev->SetPixelShaderConstantF(1, c1, 1);

    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetTexture(1, nullptr);
    dev->SetPixelShader(nullptr);
    return true;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;
    g_radius    = api->GetConfigFloat("SSAO", "Radius",    1.0f);
    g_intensity = api->GetConfigFloat("SSAO", "Intensity", 1.5f);

    g_pShader = api->CompilePixelShader(g_szShader, "main");
    if (!g_pShader) {
        api->Log("mod_ssao: shader compile failed");
        return false;
    }
    api->RegisterPostPass("ssao", SSAOPass, 200, nullptr);
    api->Log("mod_ssao: OK (radius=%.2f intensity=%.2f)", g_radius, g_intensity);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("ssao");
    if (g_pShader) { g_pShader->Release(); g_pShader = nullptr; }
}

SDGO_MOD_EXPORT const char* ModName()    { return "SSAO"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
