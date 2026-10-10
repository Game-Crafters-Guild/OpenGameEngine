#ifndef GE_SUN_GLARE_VS_GLSL
#define GE_SUN_GLARE_VS_GLSL

// Shared vertex stage for the sun glare pass. Both depth-source variants -- sun_glare.vert
// (single-sample scene depth) and sun_glare_ms.vert (multisampled) -- include this after
// declaring their own depth uniform and defining GE_SUN_GLARE_PROBE(sunUV, radiusUV) to call
// the matching visibility function. Nothing else differs between them, so nothing else is
// duplicated: the shaderpkg packager has no keyword mechanism, so a variant is a second file,
// and a second file is only safe when it is this thin.
//
// Everything the glare needs that does NOT vary per pixel is evaluated here and handed on as
// flat varyings: the transmittance along the ray to the sun, and whether the direct beam
// reaches the camera at all. All of it is a function of push constants and ONE world position,
// so evaluating it per fragment would repeat the same fetches for every pixel of a fullscreen
// draw. Three vertices, one result.

#include "Includes/sun_glare.glsl"

layout(location = 0) out vec2 vUV;
layout(location = 1) flat out vec3 vSunTransmittance;
layout(location = 2) flat out float vSunGlareScale;

layout(set = 0, binding = 0) uniform sampler2D uTransLUT;
layout(set = 0, binding = 3) uniform sampler2DArrayShadow uShadowMapArray;

// Leading prefix of the ShadowData block the shadow cascades upload
// (shadow_sampling.glsl, set 0 binding 8). Only the first three fields are declared: std140
// makes a prefix binding-compatible, and a source contract test pins these three against that
// header so the two layouts cannot drift apart silently.
layout(set = 0, binding = 4, std140) uniform SunGlareShadowData
{
    mat4 ge_shadowVP[4];   // Light view-projection per cascade
    vec4 ge_shadowSplits;  // View-space split distances
    vec4 ge_shadowParams;  // x=depthBias, y=normalBias, z=numCascades, w=maxShadowDistance
} ShadowData;

layout(push_constant) uniform GlarePC
{
    // xyz = world-space direction TOWARD the sun (unit), w = sun angular radius (rad).
    vec4 sunDirRadius;
    // xyz = sun colour (scene-linear), w = above-atmosphere irradiance E.
    vec4 sunColorIrradiance;
    // xy = sun's projected position in viewport UV, zw = probe ring radius in UV, given PER
    // AXIS so the ring is a circle in pixels rather than stretched by the viewport aspect.
    vec4 probe;
    // x = the sky's own scaling of the drawn sun: its exposure trim times the day-to-night
    // fade, so the halo tracks the disc. y = cosine of the sun's zenith angle at the camera,
    // z = normalised camera altitude in the atmosphere (both are the transmittance LUT's
    // coordinates). w = the scene depth texture's SAMPLE COUNT when the screen probe is
    // meaningful (depth bound, sun on screen), and 0 when it is not -- one field carrying both
    // because the block's 80-byte size is asserted against the shaders on both sides.
    vec4 params;
    // xyz = camera world position, w = 1 when the cascade bindings are real this frame
    // (the cascade COUNT comes from the shadow block, which is its owner).
    vec4 cameraShadow;
    // x = terrain-skyline visibility in [0, 1], marched on the CPU against the heightfield so
    // it holds whether or not the sun is on screen. yzw are std140 padding.
    vec4 horizon;
} pc;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = vec2(p.x, 1.0 - p.y);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);

    // Transmittance LUT parameterisation (sky_transmittance_lut.comp): u encodes the ray's
    // zenith cosine, v the square root of the normalised altitude. Sampled at the SUN's
    // direction, which is the ray the sun's light actually travels down — the disc's own
    // lookup keyed on the view direction reduces to this one at the disc's centre.
    vec2 transUv = vec2(0.5 * (clamp(pc.params.y, -1.0, 1.0) + 1.0),
                        sqrt(clamp(pc.params.z, 0.0, 1.0)));
    vSunTransmittance = textureLod(uTransLUT, transUv, 0.0).rgb;

    // Shadow cascades: anything that CASTS, on screen or off, including a sun behind the
    // camera. The MINIMUM over cascades, not the one a view depth would select — each
    // cascade's map contains only the casters inside its own light frustum, and the beam has
    // to survive the whole way; a cascade that does not cover the eye returns lit and drops
    // out of the minimum harmlessly.
    float sunlit = 1.0;
    int cascades = pc.cameraShadow.w > 0.5 ? int(ShadowData.ge_shadowParams.z + 0.5) : 0;
    for (int i = 0; i < 4; ++i)
    {
        if (i >= cascades)
            break;
        sunlit = min(sunlit, GE_SunGlareCascadeSunlit(uShadowMapArray, ShadowData.ge_shadowVP[i],
                                                      i, pc.cameraShadow.xyz,
                                                      ShadowData.ge_shadowParams.x));
    }

    // Scene depth: anything in the depth attachment the world pass wrote, CBT terrain
    // included. Only meaningful while the sun is on screen, so it is gated rather than
    // guessed — params.w is the sample count then and 0 otherwise.
    float onScreen = pc.params.w >= 1.0 ? GE_SUN_GLARE_PROBE(pc.probe.xy, pc.probe.zw) : 1.0;

    // One flat scalar rather than four: the sky's own scaling of the drawn sun and all three
    // visibility terms are per-frame constants, so the fragment stage has no reason to
    // see them separately — and keeping them out of its push-constant block is what lets that
    // block stay a 32-byte prefix of this one (the packager sums the per-stage ranges against
    // a 128-byte limit).
    // Three terms, multiplied, because each sees occluders the others cannot: the cascades
    // see casting meshes anywhere, the screen probe sees anything in the depth buffer while the
    // sun is on screen, and the skyline sees terrain whether the sun is on screen or not.
    vSunGlareScale = pc.params.x * sunlit * onScreen * pc.horizon.x;
}

#endif // GE_SUN_GLARE_VS_GLSL
