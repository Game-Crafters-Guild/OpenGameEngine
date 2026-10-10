#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor;
layout(set=0,binding=0) uniform sampler2D uHDR;
layout(set=0,binding=1) uniform sampler2D uBloom;
layout(set=0,binding=2) uniform sampler2D uScattering;
layout(push_constant) uniform PC {
    float bloomScatteringAmount;
    float intensity;
    float bloomTintR;
    float bloomTintG;
    float bloomTintB;
    float bloomDepthVeilTintR;
    float bloomDepthVeilTintG;
    float bloomDepthVeilTintB;
    int bloomDepthVeilEnabled;
    float bloomDepthVeilIntensity;
    float bloomDepthVeilStart;
    float bloomDepthVeilEnd;
} pc;

void main()
{
    vec3 base = texture(uHDR, vUV).rgb;
    if (pc.bloomScatteringAmount > 0.0)
        base = mix(base, texture(uScattering, vUV).rgb, clamp(pc.bloomScatteringAmount, 0.0, 1.0));
    const float epsilon = 1.0 / 1024.0;
    bool veilActive = pc.bloomDepthVeilEnabled != 0 && pc.bloomDepthVeilIntensity > epsilon
                   && pc.bloomDepthVeilEnd > pc.bloomDepthVeilStart + epsilon;
    // With highlights and depth veil off the pipeline binds the engine's black
    // texture to uBloom and this branch skips it: it adds neither light nor a veil.
    vec4 bloom = vec4(0.0);
    if (pc.intensity > epsilon || veilActive) bloom = texture(uBloom, vUV);
    if (!veilActive) bloom.a = 0.0;
    vec3 tint = clamp(vec3(pc.bloomTintR, pc.bloomTintG, pc.bloomTintB), 0.0, 1.0);
    vec3 veilTint = clamp(vec3(pc.bloomDepthVeilTintR, pc.bloomDepthVeilTintG, pc.bloomDepthVeilTintB), 0.0, 1.0);
    oColor = vec4(base + bloom.rgb * tint * max(pc.intensity, 0.0) + bloom.a * veilTint, 1.0);
}
