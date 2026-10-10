#version 450

// Spatial resample for the internal-resolution split when the view does NOT
// run temporal AA (with TAA the temporal resolve is the crossing and this pass
// is elided). One resample from the internal extent to the display extent —
// the single crossing the whole chain is allowed. Both directions cross here:
// render scale < 1 magnifies, > 1 (SSAA) minifies.
//
// Explicit LOD keeps sampling independent of control-flow derivatives.
// The CPU supplies the exact destination extent for fractional SSAA scales.
//
// Magnification: Catmull-Rom (bicubic, B=0, C=0.5) over a 4x4 footprint,
// gathered in 9 bilinear fetches instead of 16 point fetches: sharper than the
// hardware bilinear this replaces, for 9 samples. The kernel has negative
// lobes, so it can undershoot below zero on high contrast; the result is
// clamped non-negative because the HDR chain downstream (bloom threshold,
// tonemap) treats negatives as fireflies.
//
// Minification: a separable tent with a one-output-pixel radius suppresses
// frequencies above the display sampling limit more strongly than a box.
// Positive weights avoid ringing around bright HDR edges. Four bilinear
// fetches cover the full kernel at all supported SSAA scales (1..2).

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(push_constant) uniform SpatialResampleParams { vec4 uOutputExtent; } Resample;

layout(set = 0, binding = 0) uniform sampler2D uSource; // internal-extent, linear clamp

vec4 SampleCatmullRom(vec2 uv, vec2 sourceSize)
{
    // Sample position in source texel space, and the centre texel it lands in.
    vec2 samplePos = uv * sourceSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;
    vec2 f = samplePos - texPos1;

    // Catmull-Rom weights for the four taps along each axis.
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    // Fold taps 1 and 2 into one bilinear fetch at their weighted midpoint —
    // the standard 16-tap-to-9-fetch reduction (both weights share a sign, so
    // the hardware lerp reproduces the pair exactly).
    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / max(w12, vec2(1e-6));

    vec2 texPos0 = (texPos1 - 1.0) / sourceSize;
    vec2 texPos3 = (texPos1 + 2.0) / sourceSize;
    vec2 texPos12 = (texPos1 + offset12) / sourceSize;

    vec4 result = vec4(0.0);
    result += textureLod(uSource, vec2(texPos0.x, texPos0.y), 0.0) * w0.x * w0.y;
    result += textureLod(uSource, vec2(texPos12.x, texPos0.y), 0.0) * w12.x * w0.y;
    result += textureLod(uSource, vec2(texPos3.x, texPos0.y), 0.0) * w3.x * w0.y;

    result += textureLod(uSource, vec2(texPos0.x, texPos12.y), 0.0) * w0.x * w12.y;
    result += textureLod(uSource, vec2(texPos12.x, texPos12.y), 0.0) * w12.x * w12.y;
    result += textureLod(uSource, vec2(texPos3.x, texPos12.y), 0.0) * w3.x * w12.y;

    result += textureLod(uSource, vec2(texPos0.x, texPos3.y), 0.0) * w0.x * w3.y;
    result += textureLod(uSource, vec2(texPos12.x, texPos3.y), 0.0) * w12.x * w3.y;
    result += textureLod(uSource, vec2(texPos3.x, texPos3.y), 0.0) * w3.x * w3.y;
    return result;
}

// Pair the four source sample weights into two hardware-linear fetches.
// Normalize each axis so fractional scales and pixel phases preserve DC.
void TentAxis(float center, float radius, out vec2 positions, out vec2 weights)
{
    float first = floor(center - radius - 0.5) + 1.0;
    vec4 centers = first + vec4(0.5, 1.5, 2.5, 3.5);
    vec4 taps = max(vec4(1.0) - abs(centers - center) / radius, vec4(0.0));
    weights = vec2(taps.x + taps.y, taps.z + taps.w);
    positions = first + vec2(0.5, 2.5) + vec2(taps.y, taps.w) / max(weights, vec2(1e-6));
    weights /= max(weights.x + weights.y, 1e-6);
}

vec4 SampleTentDown(vec2 uv, vec2 sourceSize, vec2 ratio)
{
    vec2 center = uv * sourceSize;
    vec2 x, y, wx, wy;
    TentAxis(center.x, max(ratio.x, 1.0), x, wx);
    TentAxis(center.y, max(ratio.y, 1.0), y, wy);
    x /= sourceSize.x;
    y /= sourceSize.y;
    return textureLod(uSource, vec2(x.x, y.x), 0.0) * wx.x * wy.x +
           textureLod(uSource, vec2(x.y, y.x), 0.0) * wx.y * wy.x +
           textureLod(uSource, vec2(x.x, y.y), 0.0) * wx.x * wy.y +
           textureLod(uSource, vec2(x.y, y.y), 0.0) * wx.y * wy.y;
}

void main()
{
    vec2 sourceSize = vec2(textureSize(uSource, 0));
    vec2 ratio = sourceSize / max(Resample.uOutputExtent.xy, vec2(1.0));
    vec4 c = (max(ratio.x, ratio.y) > 1.001)
                 ? SampleTentDown(vUV, sourceSize, ratio)
                 : SampleCatmullRom(vUV, sourceSize);
    oColor = vec4(max(c.rgb, vec3(0.0)), c.a);
}
