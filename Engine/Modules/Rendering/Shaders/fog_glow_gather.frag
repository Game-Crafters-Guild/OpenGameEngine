#version 450

// Fractional radius scaling is adapted from KinoBloom v2 by Keijiro
// Takahashi. Copyright (c) 2015-2017, MIT License.
// See ThirdParty/KinoBloom/LICENSE.md and UPSTREAM.md.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uFogGlow0;
layout(set = 0, binding = 1) uniform sampler2D uFogGlow1;
layout(set = 0, binding = 2) uniform sampler2D uFogGlow2;
layout(set = 0, binding = 3) uniform sampler2D uFogGlow3;
layout(set = 0, binding = 4) uniform sampler2D uFogGlow4;
layout(set = 0, binding = 5) uniform sampler2D uFogGlow5;
layout(set = 0, binding = 6) uniform sampler2D uFogGlow6;
layout(set = 0, binding = 7) uniform sampler2D uFogGlow7;

layout(push_constant) uniform FogGlowGatherPC
{
    int fogGlowOctaves;
    float fogGlowSampleScale;
    float fogGlowScatter;
} pc;

vec3 SmoothOctave(sampler2D octave)
{
    vec2 hp = 0.5 * clamp(pc.fogGlowSampleScale, 0.5, 1.5) / vec2(textureSize(octave, 0));
    vec3 sum = texture(octave, vUV + vec2(-hp.x * 2.0, 0.0)).rgb;
    sum += texture(octave, vUV + vec2(-hp.x, hp.y)).rgb * 2.0;
    sum += texture(octave, vUV + vec2(0.0, hp.y * 2.0)).rgb;
    sum += texture(octave, vUV + vec2(hp.x, hp.y)).rgb * 2.0;
    sum += texture(octave, vUV + vec2(hp.x * 2.0, 0.0)).rgb;
    sum += texture(octave, vUV + vec2(hp.x, -hp.y)).rgb * 2.0;
    sum += texture(octave, vUV + vec2(0.0, -hp.y * 2.0)).rgb;
    sum += texture(octave, vUV + vec2(-hp.x, -hp.y)).rgb * 2.0;
    return sum * (1.0 / 12.0);
}

void AddOctave(sampler2D octave, float number, float exponent, inout vec3 sum, inout float weight)
{
    float w = pow(number, exponent);
    sum += SmoothOctave(octave) * w;
    weight += w;
}

void main()
{
    int count = clamp(pc.fogGlowOctaves, 3, 8);
    float exponent = clamp(pc.fogGlowScatter, 0.0, 1.0) * 5.0 - 2.5;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    AddOctave(uFogGlow0, 1.0, exponent, sum, weight);
    AddOctave(uFogGlow1, 2.0, exponent, sum, weight);
    AddOctave(uFogGlow2, 3.0, exponent, sum, weight);
    if (count >= 4) AddOctave(uFogGlow3, 4.0, exponent, sum, weight);
    if (count >= 5) AddOctave(uFogGlow4, 5.0, exponent, sum, weight);
    if (count >= 6) AddOctave(uFogGlow5, 6.0, exponent, sum, weight);
    if (count >= 7) AddOctave(uFogGlow6, 7.0, exponent, sum, weight);
    if (count >= 8) AddOctave(uFogGlow7, 8.0, exponent, sum, weight);
    oColor = vec4(max(sum / max(weight, 1e-5), vec3(0.0)), 1.0);
}
