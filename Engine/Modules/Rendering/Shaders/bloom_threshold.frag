#version 450
#include "Includes/exposed_brightness.glsl"
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor;
layout(set=0,binding=0) uniform sampler2D uHDR;
// Auto-exposure scale (last frame's adapted exposure; the "ExposureHistory" blueprint resource,
// zero-initialized on creation). Read only when useAutoExposure != 0 — matches the Tonemap node's
// exposure resolution so the bright-pass threshold operates in the same exposed space the
// tonemap will display, keeping the bloom knee stable as auto-exposure adapts.
layout(set=0,binding=1,std430) readonly buffer ExposureHistory { float exposureScale; uint valid; } uExposure;
layout(set=0,binding=2) uniform sampler2D uDepth;
layout(set=0,binding=3,std140) uniform ViewParamsBlock {
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;
// The depth veil follows an MIT-licensed OCASM technique while using the
// engine's multiscale bloom reconstruction in place of a temporal ghost buffer.
// See ThirdParty/OCASMDepthVeil/LICENSE and UPSTREAM.md.
layout(push_constant) uniform PC {
    float threshold;
    float knee;
    float exposure;          // static exposure scale (Fixed/Manual/Physical), resolved per-view
    int   useAutoExposure;   // != 0 -> read uExposure.exposureScale instead of `exposure`
    int   bloomAntiFlicker;
    int   bloomDepthVeilEnabled;
    float bloomDepthVeilIntensity;
    float bloomDepthVeilStart;
    float bloomDepthVeilEnd;
    int thresholdFree;
} pc;

vec3 SafeHDR(vec3 c)
{
    return clamp(c, vec3(0.0), vec3(65000.0));
}

// Cap on the exposed max-channel a single bright-pass texel may contribute.
// Without it one ultra-hot texel (sun glint, specular firefly) blooms
// unbounded and flickers as it crosses pixel centers. 100x display white
// keeps physical-unit emitters (2000-nit panels, the sun disk's tail)
// differentiated while still bounding true fireflies; promote to a
// BloomEffect field if content needs to tune it.
const float kBloomClampMax = 100.0;

float LinearizeReverseZ(float depth)
{
    float zNear = max(ViewParams.ge_nearFar.x, 1e-5);
    float zFar = max(ViewParams.ge_nearFar.y, zNear + 1.0);
    return (zNear * zFar) / max(depth * (zFar - zNear) + zNear, 1e-6);
}

vec3 BrightPass(vec3 c, float expScale)
{
    float t = pc.threshold;
    // Match the inspector response curve and keep a nominally hard knee
    // finite; 1e-5 is visually indistinguishable from zero.
    float k = max(pc.knee, 1e-5);
    float le = GE_ExposedMaxChannel(c, expScale);  // exposed max channel drives the bright-pass decision
    float soft = clamp((le - t + k) / (2.0*k), 0.0, 1.0);
    soft = soft * soft * k;                          // quadratic knee
    // Firefly clamp folded into the pass-through weight: a texel contributes
    // at most kBloomClampMax exposed units to the blur chain.
    float w = min(max(le - t, soft), kBloomClampMax);
    return c * (w / max(le, 1e-6)); // scene-linear result from an exposed fraction
}

vec3 ModernSource(vec2 uv, float expScale)
{
    vec3 c = SafeHDR(texture(uHDR, uv).rgb);
    // Threshold each tap before the wide average. Thresholding the diluted
    // result would erase small emitters even when they exceed the threshold.
    if (pc.thresholdFree != 0) return c;
    if (pc.bloomAntiFlicker != 0)
    {
        // Clamp isolated outliers relative to their four neighbours, in exposed
        // units. One shared RGB scale preserves hue; real multi-pixel emitters
        // keep their brightness. The shoulder preserves small colored lights
        // below sixteen display whites, then softly bounds isolated outliers.
        vec2 d = 1.0 / vec2(textureSize(uHDR, 0));
        float neighbour = max(min(GE_ExposedMaxChannel(SafeHDR(texture(uHDR, uv + vec2(d.x, 0)).rgb), expScale),
                                  GE_ExposedMaxChannel(SafeHDR(texture(uHDR, uv - vec2(d.x, 0)).rgb), expScale)),
                              min(GE_ExposedMaxChannel(SafeHDR(texture(uHDR, uv + vec2(0, d.y)).rgb), expScale),
                                  GE_ExposedMaxChannel(SafeHDR(texture(uHDR, uv - vec2(0, d.y)).rgb), expScale)));
        float brightness = GE_ExposedMaxChannel(c, expScale);
        float shoulder = max(neighbour * 2.0, pc.threshold + 16.0);
        float excess = max(brightness - shoulder, 0.0);
        float stable = min(brightness, shoulder) + excess / (1.0 + excess / 2.0);
        c *= stable / max(brightness, 1e-6);
    }
    return BrightPass(c, expScale);
}

// Normalized 13-tap footprint: four outer 2x2 groups share half the
// weight; the four inner taps share the other half. All channels use the
// same linear weights, preserving tiny colored lights and constant fields.
vec3 WidePrefilter(vec2 d, float expScale)
{
    vec3 a = ModernSource(vUV + d * vec2(-2, -2), expScale);
    vec3 b = ModernSource(vUV + d * vec2( 0, -2), expScale);
    vec3 c = ModernSource(vUV + d * vec2( 2, -2), expScale);
    vec3 e = ModernSource(vUV + d * vec2(-2,  0), expScale);
    vec3 f = ModernSource(vUV, expScale);
    vec3 g = ModernSource(vUV + d * vec2( 2,  0), expScale);
    vec3 i = ModernSource(vUV + d * vec2(-2,  2), expScale);
    vec3 j = ModernSource(vUV + d * vec2( 0,  2), expScale);
    vec3 k = ModernSource(vUV + d * vec2( 2,  2), expScale);
    vec3 inner = ModernSource(vUV + d * vec2(-1, -1), expScale)
               + ModernSource(vUV + d * vec2( 1, -1), expScale)
               + ModernSource(vUV + d * vec2(-1,  1), expScale)
               + ModernSource(vUV + d * vec2( 1,  1), expScale);
    return (a + c + i + k) * 0.03125 + (b + e + g + j) * 0.0625
         + f * 0.125 + inner * 0.125;
}

void main(){
    float expScale = GE_ResolveExposureScale(pc.exposure, pc.useAutoExposure, uExposure.exposureScale);

    vec2 texel = 1.0 / vec2(textureSize(uHDR, 0));
    vec3 bloomSource = pc.bloomAntiFlicker != 0 ? WidePrefilter(texel, expScale)
                                                            : ModernSource(vUV, expScale);
    float veilSource = 0.0;
    if (pc.thresholdFree == 0 && pc.bloomDepthVeilEnabled != 0 && pc.bloomDepthVeilIntensity > 0.0
        && pc.bloomDepthVeilEnd > pc.bloomDepthVeilStart)
    {
        float rawDepth = texture(uDepth, vUV).r;
        float linearDepth = rawDepth <= 1e-6 ? ViewParams.ge_nearFar.y : LinearizeReverseZ(rawDepth);
        float startDistance = max(pc.bloomDepthVeilStart, 0.0);
        float endDistance = max(pc.bloomDepthVeilEnd, startDistance + 1e-4);
        float depthMask = smoothstep(startDistance, endDistance, linearDepth);
        veilSource = depthMask * pc.bloomDepthVeilIntensity / expScale;
    }
    oColor = vec4(bloomSource, veilSource);
}
