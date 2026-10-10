#version 450

// Energy-normalized bloom reconstruction from independently filtered pyramid
// levels. Keeping this shader in the renderer core makes octave count and
// scatter available as part of core bloom.
// Fractional radius scaling is adapted from KinoBloom v2:
// Copyright (c) 2015-2017 Keijiro Takahashi, MIT License.
// See ThirdParty/KinoBloom/LICENSE.md and UPSTREAM.md.
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uBloom0;
layout(set = 0, binding = 1) uniform sampler2D uBloom1;
layout(set = 0, binding = 2) uniform sampler2D uBloom2;
layout(set = 0, binding = 3) uniform sampler2D uBloom3;
layout(set = 0, binding = 4) uniform sampler2D uBloom4;
layout(set = 0, binding = 5) uniform sampler2D uBloom5;
layout(set = 0, binding = 6) uniform sampler2D uBloom6;
layout(set = 0, binding = 7) uniform sampler2D uBloom7;

layout(push_constant) uniform BloomOctaveGatherPC
{
    int bloomOctaves;
    float bloomSampleScale;
    float bloomScatter;
    float bloomOctaveBlend;
} pc;

vec4 CubicWeights(float f)
{
    float t = 1.0 - f;
    return vec4(t*t*t, 3.0*f*f*f - 6.0*f*f + 4.0,
                -3.0*f*f*f + 3.0*f*f + 3.0*f + 1.0, f*f*f) / 6.0;
}

vec4 SmoothOctave(sampler2D octave)
{
    // Positive cubic B-spline reconstruction. Pair adjacent weights into
    // four bilinear fetches, giving a continuously differentiable halo
    // without negative lobes or rings around high-intensity sources.
    vec2 size = vec2(textureSize(octave, 0));
    vec2 p = vUV * size - 0.5;
    vec2 base = floor(p);
    vec4 wx = CubicWeights(fract(p.x));
    vec4 wy = CubicWeights(fract(p.y));
    vec2 gx = vec2(wx.x + wx.y, wx.z + wx.w);
    vec2 gy = vec2(wy.x + wy.y, wy.z + wy.w);
    vec2 x = (base.x + vec2(-0.5 + wx.y / gx.x, 1.5 + wx.w / gx.y)) / size.x;
    vec2 y = (base.y + vec2(-0.5 + wy.y / gy.x, 1.5 + wy.w / gy.y)) / size.y;
    vec4 filtered = texture(octave, vec2(x.x, y.x)) * gx.x * gy.x
                + texture(octave, vec2(x.y, y.x)) * gx.y * gy.x
                + texture(octave, vec2(x.x, y.y)) * gx.x * gy.y
                + texture(octave, vec2(x.y, y.y)) * gx.y * gy.y;
    if (pc.bloomSampleScale >= 1.0) return filtered;
    // Below the minimum pyramid depth, retain a continuous radius control.
    return mix(texture(octave, vUV), filtered,
               clamp(pc.bloomSampleScale * 2.0 - 1.0, 0.0, 1.0));
}

void AccumulateOctave(sampler2D octave, float octaveNumber,
                      float exponent, inout vec4 sum, inout float weight)
{
    float w = pow(octaveNumber, exponent);
    if (octaveNumber > 3.0 && int(octaveNumber) == clamp(pc.bloomOctaves, 3, 8))
        w *= clamp(pc.bloomOctaveBlend, 0.0, 1.0);
    sum += SmoothOctave(octave) * w;
    weight += w;
}

void main()
{
    int octaveCount = clamp(pc.bloomOctaves, 3, 8);
    float exponent = clamp(pc.bloomScatter, 0.0, 1.0) * 5.0 - 2.5;
    vec4 sum = vec4(0.0);
    float weight = 0.0;

    AccumulateOctave(uBloom0, 1.0, exponent, sum, weight);
    AccumulateOctave(uBloom1, 2.0, exponent, sum, weight);
    AccumulateOctave(uBloom2, 3.0, exponent, sum, weight);
    if (octaveCount >= 4) AccumulateOctave(uBloom3, 4.0, exponent, sum, weight);
    if (octaveCount >= 5) AccumulateOctave(uBloom4, 5.0, exponent, sum, weight);
    if (octaveCount >= 6) AccumulateOctave(uBloom5, 6.0, exponent, sum, weight);
    if (octaveCount >= 7) AccumulateOctave(uBloom6, 7.0, exponent, sum, weight);
    if (octaveCount >= 8) AccumulateOctave(uBloom7, 8.0, exponent, sum, weight);

    oColor = max(sum / max(weight, 1e-5), vec4(0.0));
}
