// ============================================================
// Cinematic Color Grading
//
// Math-based color correction pipeline (no LUT texture needed):
// 1. White balance (temperature / tint)
// 2. Exposure + contrast
// 3. Lift / Gamma / Gain (3-way color correction)
// 4. Saturation + vibrance
// 5. Split toning (shadows/highlights)
// 6. Filmic S-curve (ACES-inspired tone curve)
//
// This single shader does what a full DaVinci Resolve grade does.
// ============================================================
#include "mod_api.h"

static SdgoModAPI* g_api = nullptr;
static IDirect3DPixelShader9* g_pGradeShader = nullptr;

static const char* g_szGradeShader = R"(
    sampler2D sceneTex : register(s0);

    // c0: x=exposure, y=contrast, z=saturation, w=vibrance
    // c1: lift RGB + temperature
    // c2: gamma RGB + tint
    // c3: gain RGB + unused
    // c4: shadow tint RGB + split balance
    // c5: highlight tint RGB + filmic strength
    float4 params0 : register(c0);
    float4 liftTemp : register(c1);
    float4 gammaTint : register(c2);
    float4 gainPad : register(c3);
    float4 shadowTint : register(c4);
    float4 highTint : register(c5);

    // White balance via temperature/tint (Bradford-ish approximation)
    float3 WhiteBalance(float3 c, float temp, float tint)
    {
        // Warm/cool shift
        c.r *= 1.0 + temp * 0.1;
        c.b *= 1.0 - temp * 0.1;
        // Green/magenta tint
        c.g *= 1.0 + tint * 0.05;
        return c;
    }

    // Lift/Gamma/Gain (standard color grading)
    float3 LiftGammaGain(float3 c, float3 lift, float3 gamma, float3 gain)
    {
        // Lift: affects shadows
        c = c * (1.5 - 0.5 * lift) + 0.5 * lift - 0.5;
        c = max(c, 0.0);
        // Gain: affects highlights
        c *= gain;
        // Gamma: affects midtones
        float3 safeGamma = max(gamma, 0.01);
        c = pow(c, 1.0 / safeGamma);
        return c;
    }

    // ACES-inspired filmic curve
    float3 FilmicTonemap(float3 x)
    {
        float a = 2.51;
        float b = 0.03;
        float c = 2.43;
        float d = 0.59;
        float e = 0.14;
        return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
    }

    float4 main(float2 uv : TEXCOORD0) : COLOR0
    {
        float3 c = tex2D(sceneTex, uv).rgb;

        float exposure   = params0.x;
        float contrast   = params0.y;
        float saturation = params0.z;
        float vibrance   = params0.w;

        // 1. Exposure
        c *= pow(2.0, exposure);

        // 2. White balance
        c = WhiteBalance(c, liftTemp.w, gammaTint.w);

        // 3. Contrast (around 0.5 midpoint)
        c = (c - 0.5) * contrast + 0.5;
        c = max(c, 0.0);

        // 4. Lift / Gamma / Gain
        float3 lift  = float3(liftTemp.x,  liftTemp.y,  liftTemp.z);
        float3 gamma = float3(gammaTint.x, gammaTint.y, gammaTint.z);
        float3 gain  = float3(gainPad.x,   gainPad.y,   gainPad.z);
        c = LiftGammaGain(c, lift, gamma, gain);

        // 5. Saturation + Vibrance
        float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
        float3 grey = float3(lum, lum, lum);

        // Vibrance: boost low-saturation colors more than high-saturation
        float currSat = max(max(c.r, c.g), c.b) - min(min(c.r, c.g), c.b);
        float vibranceWeight = 1.0 + vibrance * (1.0 - currSat);
        c = lerp(grey, c, vibranceWeight);

        // Standard saturation
        c = lerp(grey, c, saturation);

        // 6. Split toning
        float balance = shadowTint.w;
        float shadowWeight = 1.0 - smoothstep(0.0, 0.5 + balance * 0.5, lum);
        float highWeight = smoothstep(0.5 - balance * 0.5, 1.0, lum);
        c += shadowTint.xyz * shadowWeight * 0.15;
        c += highTint.xyz * highWeight * 0.15;

        // 7. Filmic S-curve
        float filmicStr = highTint.w;
        float3 filmic = FilmicTonemap(c);
        c = lerp(c, filmic, filmicStr);

        return float4(saturate(c), 1.0);
    }
)";

// Configurable params with cinematic defaults
static float g_exposure    = 0.15f;
static float g_contrast    = 1.15f;
static float g_saturation  = 1.10f;
static float g_vibrance    = 0.30f;
static float g_temperature = 0.10f;   // Slightly warm
static float g_tint        = 0.0f;

// Lift (shadows), Gamma (mids), Gain (highlights) — per channel
static float g_liftR = 1.0f, g_liftG = 1.0f, g_liftB = 1.02f;   // Slight blue in shadows
static float g_gammaR = 1.0f, g_gammaG = 1.0f, g_gammaB = 1.0f;
static float g_gainR = 1.02f, g_gainG = 1.0f, g_gainB = 0.98f;   // Warm highlights

// Split toning: teal shadows + orange highlights (classic blockbuster look)
static float g_shadowR = -0.05f, g_shadowG = 0.08f, g_shadowB = 0.12f;
static float g_highR = 0.10f, g_highG = 0.06f, g_highB = -0.04f;
static float g_splitBalance = 0.0f;  // -1 to 1
static float g_filmicStrength = 0.6f;

static bool ColorGradePass(IDirect3DDevice9* dev,
                           IDirect3DTexture9* pSrc, IDirect3DSurface9* pDst,
                           IDirect3DTexture9* pDepth,
                           float w, float h, float nearZ, float farZ,
                           void* ud)
{
    if (!g_pGradeShader) return false;

    dev->SetRenderTarget(0, pDst);
    dev->SetPixelShader(g_pGradeShader);
    dev->SetTexture(0, pSrc);

    float c0[4] = { g_exposure, g_contrast, g_saturation, g_vibrance };
    float c1[4] = { g_liftR, g_liftG, g_liftB, g_temperature };
    float c2[4] = { g_gammaR, g_gammaG, g_gammaB, g_tint };
    float c3[4] = { g_gainR, g_gainG, g_gainB, 0 };
    float c4[4] = { g_shadowR, g_shadowG, g_shadowB, g_splitBalance };
    float c5[4] = { g_highR, g_highG, g_highB, g_filmicStrength };

    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetPixelShaderConstantF(1, c1, 1);
    dev->SetPixelShaderConstantF(2, c2, 1);
    dev->SetPixelShaderConstantF(3, c3, 1);
    dev->SetPixelShaderConstantF(4, c4, 1);
    dev->SetPixelShaderConstantF(5, c5, 1);

    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    g_api->DrawFullscreenQuad(dev, w, h);

    dev->SetPixelShader(nullptr);
    return true;
}

SDGO_MOD_EXPORT bool ModLoad(SdgoModAPI* api)
{
    g_api = api;

    g_exposure      = api->GetConfigFloat("ColorGrade", "Exposure",      0.15f);
    g_contrast      = api->GetConfigFloat("ColorGrade", "Contrast",      1.15f);
    g_saturation    = api->GetConfigFloat("ColorGrade", "Saturation",    1.10f);
    g_vibrance      = api->GetConfigFloat("ColorGrade", "Vibrance",      0.30f);
    g_temperature   = api->GetConfigFloat("ColorGrade", "Temperature",   0.10f);
    g_tint          = api->GetConfigFloat("ColorGrade", "Tint",          0.0f);

    g_liftR  = api->GetConfigFloat("ColorGrade", "LiftR",  1.0f);
    g_liftG  = api->GetConfigFloat("ColorGrade", "LiftG",  1.0f);
    g_liftB  = api->GetConfigFloat("ColorGrade", "LiftB",  1.02f);
    g_gammaR = api->GetConfigFloat("ColorGrade", "GammaR", 1.0f);
    g_gammaG = api->GetConfigFloat("ColorGrade", "GammaG", 1.0f);
    g_gammaB = api->GetConfigFloat("ColorGrade", "GammaB", 1.0f);
    g_gainR  = api->GetConfigFloat("ColorGrade", "GainR",  1.02f);
    g_gainG  = api->GetConfigFloat("ColorGrade", "GainG",  1.0f);
    g_gainB  = api->GetConfigFloat("ColorGrade", "GainB",  0.98f);

    g_shadowR       = api->GetConfigFloat("ColorGrade", "ShadowTintR",   -0.05f);
    g_shadowG       = api->GetConfigFloat("ColorGrade", "ShadowTintG",    0.08f);
    g_shadowB       = api->GetConfigFloat("ColorGrade", "ShadowTintB",    0.12f);
    g_highR         = api->GetConfigFloat("ColorGrade", "HighTintR",      0.10f);
    g_highG         = api->GetConfigFloat("ColorGrade", "HighTintG",      0.06f);
    g_highB         = api->GetConfigFloat("ColorGrade", "HighTintB",     -0.04f);
    g_splitBalance  = api->GetConfigFloat("ColorGrade", "SplitBalance",   0.0f);
    g_filmicStrength = api->GetConfigFloat("ColorGrade", "FilmicCurve",   0.6f);

    g_pGradeShader = api->CompilePixelShader(g_szGradeShader, "main");
    if (!g_pGradeShader) {
        api->Log("mod_colorgrade: shader compile failed");
        return false;
    }

    api->RegisterPostPass("colorgrade", ColorGradePass, 650, nullptr);
    api->Log("mod_colorgrade: OK (exposure=%.2f contrast=%.2f filmic=%.2f)",
             g_exposure, g_contrast, g_filmicStrength);
    return true;
}

SDGO_MOD_EXPORT void ModUnload()
{
    if (g_api) g_api->UnregisterPostPass("colorgrade");
    if (g_pGradeShader) { g_pGradeShader->Release(); g_pGradeShader = nullptr; }
}

SDGO_MOD_EXPORT const char* ModName()    { return "Cinematic Color Grading"; }
SDGO_MOD_EXPORT const char* ModVersion() { return "1.0"; }
