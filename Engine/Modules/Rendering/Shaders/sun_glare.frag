#version 450

// Sun glare: the veiling light the optics scatter out of the sun's beam.
//
// Composited additively into the HDR colour target in the same scene-linear units the sun
// disc writes, before tonemapping and before the bright-pass that feeds bloom. The radiance
// is analytic — a fraction of the sun's UNCLAMPED irradiance times a unit-integral kernel —
// because the disc stored in SceneColor is fp16-clamped to a fraction of a percent of the
// sun's energy, so no amount of blurring the frame can recover a halo from it.

#include "Includes/screen_position.glsl"
#include "Includes/sun_glare.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 1) flat in vec3 vSunTransmittance;
layout(location = 2) flat in float vSunGlareScale;

layout(location = 0) out vec4 oColor;

// Bindings 0 (uTransLUT) and 1 (uSceneDepth) are read by the vertex stage only — the sun's
// transmittance and visibility are one value per frame, hoisted there.
layout(set = 0, binding = 2, std140) uniform ViewParamsBlock {
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#include "Includes/view_params_fields.glsl"
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

// A 32-byte PREFIX of the vertex stage's GlarePC. Vulkan lets each stage declare the range it
// uses, and the packager checks the SUM of the per-stage ranges against a 128-byte limit, so
// the fragment stage declares only the two vec4s it reads. Everything else the glare needs is
// per-frame and arrives through vSunGlareScale.
layout(push_constant) uniform GlarePC
{
    // xyz = world-space direction TOWARD the sun (unit), w = sun angular radius (rad).
    vec4 sunDirRadius;
    // xyz = sun colour (scene-linear), w = above-atmosphere irradiance E.
    vec4 sunColorIrradiance;
} pc;

void main()
{
    // World-space ray for this pixel, through the view's own matrices so any projection,
    // jitter or aspect is already accounted for. Reverse-Z puts the near plane at z = 1.
    vec2 ndc = GE_ViewportUVToYUpNdc(vUV);
    vec4 viewPos = ViewParams.ge_invProj * vec4(ndc, 1.0, 1.0);
    vec3 dirView = normalize(viewPos.xyz / viewPos.w);
    vec3 dirWS = normalize(mat3(ViewParams.ge_invView) * dirView);

    vec3 sunDir = pc.sunDirRadius.xyz;
    float chordSq = GE_SunGlareChordSq(dirWS, sunDir);

    float radiance = GE_SunGlareRadiance(pc.sunColorIrradiance.w, chordSq, pc.sunDirRadius.w,
                                         GE_SUN_GLARE_FRACTION);
    radiance *= vSunGlareScale;

    vec3 glare = pc.sunColorIrradiance.rgb * vSunTransmittance * radiance;
    // Bound what lands in the fp16 target. At the shipped glare fraction and a 100 klx sun it
    // does not bind at the default sky exposure; where it does bind — a brighter authored sun,
    // or the maximum +2 EV trim — the disc is drawn over that region anyway. It is what keeps
    // either case from writing +Inf, which would reach NaN through bloom's Karis weight and
    // TAA's neighbourhood clamp.
    glare = min(glare, vec3(GE_SUN_GLARE_MAX_STORED_RADIANCE));

    // Additive blend (One/One); alpha 0 leaves the target's coverage alone.
    oColor = vec4(glare, 0.0);
}
