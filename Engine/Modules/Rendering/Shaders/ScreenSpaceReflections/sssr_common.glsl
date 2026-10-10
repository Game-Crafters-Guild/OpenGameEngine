#ifndef GE_SSSR_COMMON_GLSL
#define GE_SSSR_COMMON_GLSL

#include "Includes/view_params.glsl"
#include "Includes/octahedral_normal.glsl"

layout(push_constant) uniform SssrParams
{
    float sssrIntensity;
    float sssrMaxDistance;
    float sssrThickness;
    float sssrEdgeFade;
    int sssrMaxSteps;
    uint frameIndex;
    uint historyValid;
    uint debugMode;
    int sampleQuality;   // stochastic ray density: 0=Low, 1=Medium, 2=High
    int multiBounce;     // 1 = hits sample last frame's composited color
    // The camera's viewport inside the render target, in target UV: origin in
    // .xy, extent in .zw. A letterboxed view rasterizes the world into a
    // sub-rectangle of a full-size target, so a target UV is NOT an NDC of the
    // camera's projection — every conversion below goes through this rect.
    // (0, 0, 1, 1) whenever the viewport fills the target.
    vec4 viewportRect;
} sssr;

const float GE_PI = 3.14159265358979323846;

bool GE_SssrIsFinite(vec4 value)
{
#if defined(GE_COMPAT_PROFILE)
    return !any(notEqual(value, value)) &&
           !any(greaterThanEqual(abs(value), vec4(3.4e38)));
#else
    return !any(isnan(value)) && !any(isinf(value));
#endif
}

// Target UV -> the camera's NDC. The rect makes the two agree in a letterboxed
// view, where the rasterized image covers only part of the target.
vec2 GE_UvToNdc(vec2 uv)
{
    vec2 v = (uv - sssr.viewportRect.xy) / sssr.viewportRect.zw;
    return vec2(v.x * 2.0 - 1.0, 1.0 - v.y * 2.0);
}

vec2 GE_NdcToUv(vec2 ndc)
{
    vec2 v = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    return sssr.viewportRect.xy + v * sssr.viewportRect.zw;
}

// Reverse-Z raw depth (near=1, far=0) -> linear view-space meters. Same formula
// as Includes/reverse_z.glsl, using the anonymous-block ge_nearFar accessor the
// SSR passes declare. Edge-stopping on linear Z is uniform with distance, unlike
// weighting raw depth (which compresses toward the far plane).
float GE_LinearZ(float rawDepth)
{
    float zNear = max(ge_nearFar.x, 1e-5);
    float zFar = max(ge_nearFar.y, zNear + 1.0);
    return (zNear * zFar) / max(rawDepth * (zFar - zNear) + zNear, 1e-6);
}

vec3 GE_ViewPosition(vec2 uv, float rawDepth)
{
    vec4 p = ge_invProj * vec4(GE_UvToNdc(uv), rawDepth, 1.0);
    return p.xyz / max(abs(p.w), 1e-7);
}

vec2 GE_ProjectView(vec3 positionVS, out float rawDepth)
{
    vec4 clip = ge_proj * vec4(positionVS, 1.0);
    vec3 ndc = clip.xyz / max(abs(clip.w), 1e-7);
    rawDepth = ndc.z;
    return GE_NdcToUv(ndc.xy);
}

// View.NormalRoughness layout (RGBA16F, written by the forward opaque pass):
//   .rg = octahedral view-space normal, .b = perceptual roughness, .a = metallic.
// The composite weights reflections with the forward pass's own specular
// response (View.SSRSpecularWeight), so the metallic channel is not read here.
vec3 GE_DecodeNormalVS(vec4 normalRoughness) { return GE_OctDecode(normalRoughness.rg); }
float GE_Roughness(vec4 normalRoughness) { return normalRoughness.b; }

// Reverse-Z: the far plane is 0, so a raw depth at or below this is sky or an
// unwritten G-buffer pixel — no surface, nothing to reflect.
const float kSssrMinRawDepth = 1e-7;
// The upper edge of the composite's roughness fade. A pixel at or above this
// contributes exactly zero reflection to the frame, so it gets no ray and needs
// no denoising.
const float kSssrMaxRoughness = 0.92;

// Reflection eligibility. Every pass that decides whether a pixel matters must
// agree on this set: classify traces it, the prefilter dispatches only the tiles
// that contain it, and the composite fades to zero outside it. Diverging copies
// of the predicate would make the denoiser spend work on pixels the composite
// discards, or skip pixels it does not.
bool GE_SssrIsReflective(float rawDepth, float roughness)
{
    return rawDepth > kSssrMinRawDepth && roughness < kSssrMaxRoughness;
}

// Spatiotemporal blue noise. A precomputed void-and-cluster tile (RG8, bound as
// uBlueNoise) supplies the per-pixel spatial blue-noise pair; a per-frame
// Cranley-Patterson rotation by the R2 golden-ratio pair decorrelates successive
// frames, so the temporal pass integrates a low-discrepancy sequence. The mask
// tiles across the screen via the wrapped integer fetch.
//
// The frame counter wraps to a power-of-two period BEFORE the float conversion.
// fract() of an unbounded fp32 product starves on ULP as the product's exponent
// grows: the rotation is quantised to 1/ulp(R2*frame) distinct values, which is
// ~1024 by frame 17.7k, 64 by frame 250k and 16 by frame 1M — the noise decays
// with session age, which is a wall-clock-dependent artifact. Wrapping keeps the
// product inside one exponent range, so the rotation set stays the full period
// forever. The period only has to exceed the temporal window the resolve
// integrates over (~7 frames at the mirror feedback weight, ~33 at the rough
// end), so 256 is ample and matches FidelityFX's frame mask.
const uint kBlueNoiseFramePeriod = 256u; // power of two: masked with period - 1

vec2 GE_BlueNoise(sampler2D noiseTex, uvec2 pixel, uint frame)
{
    ivec2 dim = textureSize(noiseTex, 0);
    vec2 base = texelFetch(noiseTex, ivec2(pixel % uvec2(dim)), 0).rg;
    return fract(base + vec2(0.7548776662, 0.5698402910) *
                            float(frame & (kBlueNoiseFramePeriod - 1u)));
}

float GE_Luminance(vec3 c)
{
    return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

// Side length of one average-radiance block. sssr_reproject.comp reduces its
// workgroup to a single texel of the w/B x h/B average target, and
// sssr_prefilter.comp samples that target back at (pixel + 0.5) / (B * blocks).
// Both readings of B must agree with the reproject workgroup's shape, so the
// value lives here rather than being spelled twice.
const int kSssrAvgRadianceBlock = 8;

// Distance to the edge of the RASTERIZED image, not of the target: outside a
// letterboxed viewport there is no scene to have traced, so the fade has to
// start at the viewport border.
float GE_EdgeConfidence(vec2 uv)
{
    vec2 v = (uv - sssr.viewportRect.xy) / sssr.viewportRect.zw;
    float edge = min(min(v.x, v.y), min(1.0 - v.x, 1.0 - v.y));
    return smoothstep(0.0, max(sssr.sssrEdgeFade, 1e-4), edge);
}

#endif
