// FXAA 3.11 - Fast Approximate Anti-Aliasing
// Adapted for D3D9 / SM 2.0

texture SceneTexture;
float2 TexelSize;  // 1.0 / screen dimensions

sampler SceneSampler = sampler_state {
    Texture = <SceneTexture>;
    MinFilter = Linear;
    MagFilter = Linear;
    MipFilter = None;
    AddressU = Clamp;
    AddressV = Clamp;
};

static const float FXAA_EDGE_THRESHOLD     = 0.125;  // 1/8
static const float FXAA_EDGE_THRESHOLD_MIN = 0.0625; // 1/16
static const float FXAA_SUBPIX_QUALITY     = 0.75;

float4 PS_FXAA(float2 uv : TEXCOORD0) : COLOR0
{
    float3 rgbM  = tex2D(SceneSampler, uv).rgb;

    // Sample neighbors
    float3 rgbN  = tex2D(SceneSampler, uv + float2( 0, -1) * TexelSize).rgb;
    float3 rgbS  = tex2D(SceneSampler, uv + float2( 0,  1) * TexelSize).rgb;
    float3 rgbW  = tex2D(SceneSampler, uv + float2(-1,  0) * TexelSize).rgb;
    float3 rgbE  = tex2D(SceneSampler, uv + float2( 1,  0) * TexelSize).rgb;
    float3 rgbNW = tex2D(SceneSampler, uv + float2(-1, -1) * TexelSize).rgb;
    float3 rgbNE = tex2D(SceneSampler, uv + float2( 1, -1) * TexelSize).rgb;
    float3 rgbSW = tex2D(SceneSampler, uv + float2(-1,  1) * TexelSize).rgb;
    float3 rgbSE = tex2D(SceneSampler, uv + float2( 1,  1) * TexelSize).rgb;

    // Luminance
    float3 luma = float3(0.299, 0.587, 0.114);
    float lumaN  = dot(rgbN,  luma);
    float lumaS  = dot(rgbS,  luma);
    float lumaW  = dot(rgbW,  luma);
    float lumaE  = dot(rgbE,  luma);
    float lumaM  = dot(rgbM,  luma);
    float lumaNW = dot(rgbNW, luma);
    float lumaNE = dot(rgbNE, luma);
    float lumaSW = dot(rgbSW, luma);
    float lumaSE = dot(rgbSE, luma);

    float lumaMin = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float lumaMax = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    float lumaRange = lumaMax - lumaMin;

    // Early exit for low-contrast areas
    if (lumaRange < max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD))
        return float4(rgbM, 1.0);

    // Edge direction
    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * TexelSize;

    // Two-tap filter
    float3 rgbA = 0.5 * (
        tex2D(SceneSampler, uv + dir * (1.0/3.0 - 0.5)).rgb +
        tex2D(SceneSampler, uv + dir * (2.0/3.0 - 0.5)).rgb);

    // Four-tap filter
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        tex2D(SceneSampler, uv + dir * -0.5).rgb +
        tex2D(SceneSampler, uv + dir *  0.5).rgb);

    float lumaB = dot(rgbB, luma);

    // Subpixel anti-aliasing
    float lumaAvg = (1.0/12.0) * (2.0 * (lumaN + lumaS + lumaW + lumaE) +
                                    (lumaNW + lumaNE + lumaSW + lumaSE));
    float subpixA = saturate(abs(lumaAvg - lumaM) / lumaRange);
    float subpixB = (-2.0 * subpixA + 3.0) * subpixA * subpixA;
    float subpixC = subpixB * subpixB * FXAA_SUBPIX_QUALITY;

    float3 result = (lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB;

    // Blend with subpixel AA
    result = lerp(result, rgbM, max(0, 0.5 - subpixC));

    return float4(result, 1.0);
}

technique FXAA {
    pass P0 {
        PixelShader = compile ps_2_0 PS_FXAA();
    }
}
