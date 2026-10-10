#version 450

// Sonic Ether lens-dirt modulation. The engine has already produced the final
// bloom composite; this pass only adds light scattered by the authored dirt
// texture. See ThirdParty/SENaturalBloomDirtyLens/LICENSE.txt and UPSTREAM.md.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uHDR;
layout(set = 0, binding = 1) uniform sampler2D uLensBloom;
layout(set = 0, binding = 2) uniform sampler2D uLensDirt;

layout(push_constant) uniform NaturalBloomLensDirtPC
{
    float bloomLensDirtIntensity;
    int bloomLensDirtVignette;
    float bloomLensDirtVignetteIntensity;
    float bloomLensDirtVignetteRadius;
    float bloomLensDirtVignetteSmoothness;
    int bloomLensDirtVignetteRounded;
    float bloomLensDirtVignetteColorR;
    float bloomLensDirtVignetteColorG;
    float bloomLensDirtVignetteColorB;
    float intensity;
    float bloomTintR;
    float bloomTintG;
    float bloomTintB;
    float bloomDepthVeilTintR;
    float bloomDepthVeilTintG;
    float bloomDepthVeilTintB;
} pc;

void main()
{
    vec4 hdr = texture(uHDR, vUV);
    vec3 lensBloom = max(texture(uLensBloom, vUV).rgb, vec3(0.0));
    vec3 tint = clamp(vec3(pc.bloomTintR, pc.bloomTintG, pc.bloomTintB), 0.0, 1.0);
    vec3 veilTint = clamp(vec3(pc.bloomDepthVeilTintR, pc.bloomDepthVeilTintG, pc.bloomDepthVeilTintB), 0.0, 1.0);
    lensBloom = lensBloom * tint * max(pc.intensity, 0.0) + texture(uLensBloom, vUV).a * veilTint;
    vec3 lens = max(texture(uLensDirt, vUV).rgb, vec3(0.0));
    if (pc.bloomLensDirtVignette != 0)
    {
        float vignetteIntensity = clamp(pc.bloomLensDirtVignetteIntensity, 0.0, 1.0);
        vec2 size = vec2(textureSize(uHDR, 0));
        float aspect = size.x / max(size.y, 1.0);
        vec2 d = abs(vUV - vec2(0.5)) * 2.0;
        d.x *= (pc.bloomLensDirtVignetteRounded != 0) ? aspect : 1.0;
        float radius = clamp(pc.bloomLensDirtVignetteRadius, 0.0, 1.0);
        float feather = max(clamp(pc.bloomLensDirtVignetteSmoothness, 0.0, 1.0) * 3.0, 1e-4);
        float edgeMask = smoothstep(radius, radius + feather, length(d));
        vec3 tint = clamp(vec3(pc.bloomLensDirtVignetteColorR,
                               pc.bloomLensDirtVignetteColorG,
                               pc.bloomLensDirtVignetteColorB),
                          vec3(0.0), vec3(1.0));
        lens *= mix(vec3(1.0), edgeMask * tint, vignetteIntensity);
    }
    float intensity = exp(clamp(pc.bloomLensDirtIntensity, 0.0, 10.0)) - 1.0;
    vec3 color = hdr.rgb + lensBloom * lens * intensity;
    oColor = vec4(max(color, vec3(0.0)), hdr.a);
}
