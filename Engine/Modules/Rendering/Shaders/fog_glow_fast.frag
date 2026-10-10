#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uFogged;
layout(set = 0, binding = 1) uniform sampler2D uSceneSource;

layout(push_constant) uniform FogGlowFastPC
{
    float fogGlowIntensity;
    float fogGlowRadius;
    float fogGlowThreshold;
    float fogGlowKnee;
    float fogGlowFadeStart;
    float fogGlowFadeEnd;
    float fogGlowTintR;
    float fogGlowTintG;
    float fogGlowTintB;
    int fogGlowAntiFlicker;
} pc;

// Cross-median stabilization is adapted from KinoBloom v2 by Keijiro
// Takahashi. Copyright (c) 2015-2017, MIT License.
// See ThirdParty/KinoBloom/LICENSE.md and UPSTREAM.md.
vec3 Median3(vec3 a, vec3 b, vec3 c)
{
    return a + b + c - min(min(a, b), c) - max(max(a, b), c);
}

vec3 FilterFogSample(vec2 uv)
{
    vec4 fogged = texture(uFogged, uv);
    vec4 scene = texture(uSceneSource, uv);
    float transmittance = clamp(fogged.a / max(scene.a, 1e-5), 0.0, 1.0);
    float opacity = 1.0 - transmittance;
    float fade = smoothstep(pc.fogGlowFadeStart,
                            max(pc.fogGlowFadeEnd, pc.fogGlowFadeStart + 1e-4),
                            opacity);
    // Fog passes satisfy fogged = scene * transmittance + fog radiance.
    // Reconstructing the additive term prevents surfaces and silhouettes from
    // leaking into the glow mask.
    vec3 fogRadiance = max(fogged.rgb - scene.rgb * transmittance, vec3(0.0));
    vec3 source = min(fogRadiance, vec3(65000.0)) * fade;
    float brightness = max(max(source.r, source.g), source.b);
    float knee = max(pc.fogGlowKnee, 1e-5);
    float soft = clamp((brightness - pc.fogGlowThreshold + knee) / (2.0 * knee), 0.0, 1.0);
    soft = soft * soft * knee;
    float contribution = min(max(brightness - pc.fogGlowThreshold, soft), 100.0);
    return source * (contribution / max(brightness, 1e-6));
}

void main()
{
    vec2 texel = max(pc.fogGlowRadius, 1.0) / vec2(textureSize(uFogged, 0));
    vec3 stabilizedCenter = FilterFogSample(vUV);
    if (pc.fogGlowAntiFlicker != 0)
    {
        vec3 left = FilterFogSample(clamp(vUV - vec2(texel.x, 0.0), vec2(0.0), vec2(1.0)));
        vec3 right = FilterFogSample(clamp(vUV + vec2(texel.x, 0.0), vec2(0.0), vec2(1.0)));
        vec3 down = FilterFogSample(clamp(vUV - vec2(0.0, texel.y), vec2(0.0), vec2(1.0)));
        vec3 up = FilterFogSample(clamp(vUV + vec2(0.0, texel.y), vec2(0.0), vec2(1.0)));
        stabilizedCenter = Median3(Median3(stabilizedCenter, left, right), down, up);
    }

    vec3 glow = vec3(0.0);
    float weight = 0.0;
    vec4 center = texture(uFogged, vUV);
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 sampleUv = clamp(vUV + vec2(x, y) * texel, vec2(0.0), vec2(1.0));
            float w = (x == 0 && y == 0) ? 1.0 : 0.45;
            vec3 sampleValue = (x == 0 && y == 0)
                ? stabilizedCenter
                : FilterFogSample(sampleUv);
            glow += sampleValue * w;
            weight += w;
        }
    }

    vec3 tint = max(vec3(pc.fogGlowTintR, pc.fogGlowTintG, pc.fogGlowTintB), vec3(0.0));
    vec3 result = center.rgb + glow * (tint * max(pc.fogGlowIntensity, 0.0) / max(weight, 1e-5));
    oColor = vec4(result, 1.0);
}
