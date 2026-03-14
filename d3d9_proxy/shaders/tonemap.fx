// Tonemap + Color Correction for D3D9
// Reinhard tonemap, contrast, saturation, gamma adjustment

texture SceneTexture;
float Exposure = 1.0;
float Gamma = 1.0;
float Saturation = 1.1;
float Contrast = 1.05;

sampler SceneSampler = sampler_state {
    Texture = <SceneTexture>;
    MinFilter = Linear;
    MagFilter = Linear;
    MipFilter = None;
    AddressU = Clamp;
    AddressV = Clamp;
};

float4 PS_Tonemap(float2 uv : TEXCOORD0) : COLOR0
{
    float3 color = tex2D(SceneSampler, uv).rgb;

    // Exposure adjustment
    color *= Exposure;

    // Reinhard tonemapping
    color = color / (1.0 + color);

    // Contrast adjustment (pivot around mid-gray 0.5)
    color = ((color - 0.5) * Contrast) + 0.5;

    // Saturation adjustment
    float grey = dot(color, float3(0.299, 0.587, 0.114));
    color = lerp(float3(grey, grey, grey), color, Saturation);

    // Gamma correction
    color = pow(saturate(color), 1.0 / Gamma);

    return float4(color, 1.0);
}

technique Tonemap {
    pass P0 {
        PixelShader = compile ps_2_0 PS_Tonemap();
    }
}
