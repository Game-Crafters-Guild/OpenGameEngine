#version 450

// Post passes sample single-mip render targets: lod-0 taps are exact and
// satisfy WGSL uniformity inside the effect branches.
#include "Includes/compat_profile.glsl"

// Smooth sub-pixel pixel-perfect upscale.
//
// Point-samples a referenceWidth x referenceHeight window out of an offscreen
// render target sized (referenceWidth + 2) x (referenceHeight + 2) and blits it
// at integer zoom into a centered output rect (the remainder is letterboxed to
// black). The sample window is shifted by the sub-pixel camera remainder
// (fracX/fracY, in source texels) so the whole image scrolls smoothly while
// every output pixel remains a crisp zoom x zoom block. The output transfer
// function (sRGB / HDR) is applied in the same pass for a Linear input, so this
// fully replaces the final encode/blit for pixel-perfect cameras; an EncodedSrgb
// input already carries its encoding and passes through raw (or as D(c) for an
// _SRGB destination).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSourceColor; // nearest, clamp

layout(push_constant) uniform UpscalePC
{
    // Offscreen RT dimensions in texels (referenceWidth + 2, referenceHeight + 2).
    vec2 sourceSize;
    // Reference resolution (the sampled window size) in texels.
    vec2 referenceSize;
    // Sample-window origin in source texels: (1 + fracX, 1 - fracY).
    vec2 sampleOrigin;
    // Centered output rect origin in destination pixels (top-left).
    vec2 outputOrigin;
    // Centered output rect size in destination pixels (referenceSize * zoom).
    vec2 outputSize;
    // Integer upscale factor (destination pixels per source texel).
    float zoom;
    // 0=pass-through (linear for a downstream encode, or raw already-encoded
    // bytes — a point-sampled integer upscale never filters, so encoded input
    // is byte-preserving), 1=sRGB, 2=HDR10 PQ, 3=HLG, 4=scRGB/extended linear,
    // 6=D(c) for an already-encoded input into an _SRGB destination (the
    // encode_srgb arm-6 contract: the ROP's re-encode E(D(c)) round-trips).
    int outEncoding;
    // SDR white level in HDR output modes.
    float paperWhiteNits;
    float _pad0;
} pc;

vec3 LinearToSRGB(vec3 c)
{
    vec3 low = 12.92 * c;
    vec3 high = 1.055 * pow(max(c, vec3(1e-6)), vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThanEqual(c, vec3(0.0031308)));
}

// Exact sRGB EOTF, identical to encode_srgb.frag's: the outEncoding 6 arm
// hands the _SRGB destination D(c) and the ROP's fixed-function re-encode
// E(D(c)) must reproduce the input byte for all 256 levels (pinned by
// SrgbEncodeRampRoundTripTests for the shared implementation).
vec3 SRGBToLinear(vec3 c)
{
    vec3 low = c / 12.92;
    vec3 high = pow((max(c, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(c, vec3(0.04045)));
}

vec3 Rec709ToBT2020(vec3 c)
{
    mat3 m = mat3(
        0.6274040, 0.0690970, 0.0163916,
        0.3292820, 0.9195400, 0.0880132,
        0.0433136, 0.0113612, 0.8955950);
    return max(m * c, vec3(0.0));
}

float LinearNitsToPQ(float nits)
{
    nits = clamp(nits / 10000.0, 0.0, 1.0);
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float p = pow(nits, m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

vec3 LinearSceneToHDR10PQ(vec3 sceneLinear)
{
    vec3 bt2020 = Rec709ToBT2020(sceneLinear);
    float paperWhite = max(pc.paperWhiteNits, 80.0);
    return vec3(
        LinearNitsToPQ(bt2020.r * paperWhite),
        LinearNitsToPQ(bt2020.g * paperWhite),
        LinearNitsToPQ(bt2020.b * paperWhite));
}

float LinearToHLG(float x)
{
    x = max(x, 0.0);
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    return x <= (1.0 / 12.0) ? sqrt(3.0 * x) : a * log(12.0 * x - b) + c;
}

vec3 LinearSceneToHLG(vec3 sceneLinear)
{
    const float hlgReferencePeakNits = 1000.0;
    float paperWhiteNits = max(pc.paperWhiteNits, 80.0);
    vec3 bt2020 = Rec709ToBT2020(max(sceneLinear, vec3(0.0)) * (paperWhiteNits / hlgReferencePeakNits));
    return clamp(vec3(LinearToHLG(bt2020.r), LinearToHLG(bt2020.g), LinearToHLG(bt2020.b)), 0.0, 1.0);
}

vec3 EncodeOutput(vec3 linearColor)
{
    vec3 c = max(linearColor, vec3(0.0));
    if (pc.outEncoding == 0)
        return c; // Pass-through: downstream encode owns the OETF, or the bytes are already encoded.
    if (pc.outEncoding == 6)
        return SRGBToLinear(clamp(c, vec3(0.0), vec3(1.0))); // D(c): the _SRGB ROP re-encodes on store.
    if (pc.outEncoding == 2)
        return LinearSceneToHDR10PQ(c);
    if (pc.outEncoding == 3)
        return LinearSceneToHLG(c);
    if (pc.outEncoding == 4)
    {
        // scRGB extended-linear: value 1.0 == 80 nits; lift the SDR anchor to
        // the configured paper-white. Also used to pass linear through unchanged
        // on native-sRGB swapchains (paperWhiteNits == 80 -> scale 1.0).
        float scale = max(pc.paperWhiteNits, 80.0) / 80.0;
        return c * scale;
    }
    return LinearToSRGB(c);
}

void main()
{
    // Destination pixel (top-left origin). vUV is (0,0)..(1,1) top-left to
    // bottom-right; gl_FragCoord.xy already gives the pixel center.
    vec2 dstPixel = gl_FragCoord.xy;
    vec2 local = dstPixel - pc.outputOrigin;

    // Letterbox/pillarbox: anything outside the centered upscaled rect is black.
    if (local.x < 0.0 || local.y < 0.0 ||
        local.x >= pc.outputSize.x || local.y >= pc.outputSize.y)
    {
        oColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    // Map the output pixel into the reference-resolution window, then into the
    // padded source RT. Nearest sampling floors to a single source texel, so a
    // whole zoom x zoom block reads one texel (crisp). The sampleOrigin shift
    // moves the window sub-texel in the camera's direction for smooth scrolling.
    vec2 refPos = local / pc.zoom;            // [0, referenceSize)
    vec2 srcTexel = pc.sampleOrigin + refPos; // continuous source-texel coordinate
    vec2 srcUV = srcTexel / pc.sourceSize;    // nearest sampler snaps to a texel

    vec3 linearColor = GE_TAP_LOD0(uSourceColor, srcUV).rgb;
    oColor = vec4(EncodeOutput(linearColor), 1.0);
}
