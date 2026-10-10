#version 450

// Builds a wide, low-resolution illumination field for Sonic Ether lens dirt.
// See ThirdParty/SENaturalBloomDirtyLens/LICENSE.txt and UPSTREAM.md. Keeping the
// eight-octave gather out of the full-resolution composite makes dirt coverage
// independent of the normal bloom preset without paying for 32 extra samples
// at every output pixel.
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

layout(push_constant) uniform LensDirtGatherPC
{
    float bloomLensDirtScatter;
} pc;

vec4 SmoothOctave(sampler2D octave)
{
    vec2 hp = 0.75 / vec2(textureSize(octave, 0));
    vec4 sum = texture(octave, vUV + vec2(-hp.x, -hp.y));
    sum += texture(octave, vUV + vec2( hp.x, -hp.y));
    sum += texture(octave, vUV + vec2(-hp.x,  hp.y));
    sum += texture(octave, vUV + vec2( hp.x,  hp.y));
    return sum * 0.25;
}

void main()
{
    float exponent = clamp(pc.bloomLensDirtScatter, 0.0, 1.0) * 5.0 - 2.5;
    vec4 sum = vec4(0.0);
    float weight = 0.0;

#define ACCUMULATE(octave, number)                    \
    {                                                  \
        float w = pow(number, exponent);               \
        sum += SmoothOctave(octave) * w;               \
        weight += w;                                   \
    }

    ACCUMULATE(uBloom0, 1.0);
    ACCUMULATE(uBloom1, 2.0);
    ACCUMULATE(uBloom2, 3.0);
    ACCUMULATE(uBloom3, 4.0);
    ACCUMULATE(uBloom4, 5.0);
    ACCUMULATE(uBloom5, 6.0);
    ACCUMULATE(uBloom6, 7.0);
    ACCUMULATE(uBloom7, 8.0);

#undef ACCUMULATE

    oColor = max(sum / max(weight, 1e-5), vec4(0.0));
}
