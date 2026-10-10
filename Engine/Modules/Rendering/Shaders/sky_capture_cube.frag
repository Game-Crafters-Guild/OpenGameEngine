#version 450

#include "Includes/screen_position.glsl"
#include "Includes/sky_composite.glsl"

// IBL environment capture: bakes the SAME atmospheric sky the user sees on screen
// (sky_render.frag) into a cube face, MINUS the bright discs (sun / moon / stars).
// The directional light owns the sun's specular highlight; a baked disk fireflies
// the low-res prefilter. So this samples the atmospheric sky-view LUT (with
// multiscatter) exactly like sky_render.frag, layers the shared ground /
// below-horizon / night composite, and stops.
//
// Per-face camera basis (cameraRight/Up/Forward) is supplied by the CPU; the
// fragment reconstructs a 90-degree-FOV ray per face (matches the old analytic
// cube capture's projection so the six faces tile the sphere seamlessly). The
// capture still excludes the bright discs, but it does sample transmittance: the
// ground bounce below the horizon needs the sun's attenuation down to the surface.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

// SkyUBO (set 0, binding 0). std140: every vec3 is followed by a float so each
// row is 16 bytes. Mirrors the prefix sky_view_lut.comp / sky_render.frag read;
// only the fields this capture consumes are named, the rest reserve their slots.
layout(set = 0, binding = 0) uniform SkyUBO
{
    vec3  cameraPositionWS;   float exposureEV;
    vec3  cameraRightWS;      float pad0;
    vec3  cameraUpWS;         float pad1;
    vec3  cameraForwardWS;    float pad2;
    vec3  sunDirectionWS;     float sunAngularRadius;
    vec3  moonDirectionWS;    float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    vec3  sunColor;           float sunIntensity;
    float timeOfDayHours;     float iblLowerHemisphereDarkness;
    float viewportAspect;     float nightSkyBlend;
    vec4  gradientSkyTop;     // .rgb = scene-linear top color; .w >= 0.5 -> bake the gradient
    vec4  gradientSkyHorizon;
    vec4  gradientSkyBottom;
} uSky;

// Sky-view LUT (azimuth/elevation mapping, planet-local east/up/north frame)
// produced by sky_view_lut.comp — the atmospheric scattering + multiscatter
// result. Sampled with the same GE_SkyDirToViewLutUv encode as the on-screen sky.
// When moonUboPad2 >= 0 this binding is the active HDRI skybox texture instead,
// sampled with the same lat-long path as sky_render.frag.
layout(set = 0, binding = 1) uniform sampler2D uSkyViewLUT;

// AtmosphereUBO (set 0, binding 2) -- shared canonical std140 block.
#include "Includes/sky_atmosphere_ubo.glsl"

// Transmittance LUT (binding 3, matching sky_render.frag's slot): sun attenuation
// through the atmosphere, needed by the baked ground bounce.
layout(set = 0, binding = 3) uniform sampler2D uTransLUT;

const float PI = 3.14159265359;

vec2 EncodeDirToLatLongUv(vec3 dirWS)
{
    float u = atan(dirWS.z, dirWS.x) / (2.0 * PI) + 0.5;
    float v = acos(clamp(dirWS.y, -1.0, 1.0)) / PI;
    return vec2(u, v);
}

vec3 RotateAroundY(vec3 dirWS, float radians)
{
    float s = sin(radians);
    float c = cos(radians);
    return vec3(c * dirWS.x - s * dirWS.z,
                dirWS.y,
                s * dirWS.x + c * dirWS.z);
}

// Stylized gradient sky blended by view-dir.y (up=Top, horizon=Horizon, down=Bottom); colors are
// already scene-linear. Identical to sky_render.frag so the baked IBL matches the on-screen dome.
vec3 GradientSkyColor(vec3 dirWS)
{
    float up   = max(dirWS.y, 0.0);
    float down = max(-dirWS.y, 0.0);
    float mid  = 1.0 - up - down;
    return uSky.gradientSkyTop.rgb * up
         + uSky.gradientSkyHorizon.rgb * mid
         + uSky.gradientSkyBottom.rgb * down;
}

void main()
{
    // Per-face 90-degree-FOV ray (same convention as the old analytic cube
    // capture): ray through (ndc.x, ndc.y, -1) in the face's local basis.
    vec2 ndc = GE_ViewportUVToYUpNdc(vUV);
    vec3 dirView = normalize(vec3(ndc.x, ndc.y, -1.0));
    vec3 dirWS = normalize(
        dirView.x * uSky.cameraRightWS +
        dirView.y * uSky.cameraUpWS +
        (-dirView.z) * uSky.cameraForwardWS);

    // Planet-relative up + east/north frame, identical to sky_render.frag's
    // procedural path, so the oct UV indexes the LUT the same way.
    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 camPos       = uSky.cameraPositionWS;
    float camR        = length(camPos - planetCenter);
    vec3 up           = (camPos - planetCenter) / max(camR, 1e-5);

    float viewZenithCos = dot(dirWS, up);

    // Gradient sky: bake the SAME stylized gradient the on-screen dome shows, so the diffuse
    // irradiance + specular prefilter derive from it. No sun disc / atmosphere composite.
    if (uSky.gradientSkyTop.w >= 0.5)
    {
        oColor = vec4(GradientSkyColor(dirWS) * exp2(uSky.exposureEV), 1.0);
        return;
    }

    float hdriIntensity = uSky.moonUboPad2;
    if (hdriIntensity >= 0.0)
    {
        vec3 skyColor = texture(uSkyViewLUT, EncodeDirToLatLongUv(RotateAroundY(dirWS, uSky.moonUboPad1))).rgb;

        oColor = vec4(skyColor * hdriIntensity * exp2(uSky.exposureEV), 1.0);
        return;
    }

    vec3 northRef = vec3(0.0, 0.0, 1.0);
    if (abs(dot(northRef, up)) > 0.98)
        northRef = vec3(1.0, 0.0, 0.0);
    vec3 east = normalize(cross(up, northRef));
    vec3 north = normalize(cross(east, up));

    // Sky-view LUT height (matches Config::skyViewLutHeight); clamp the warped
    // elevation to a half-texel inset so the poles never read past the edge row.
    const float kSkyViewLutHeight = 256.0;

    vec3 dLocal = vec3(dot(dirWS, east), dot(dirWS, up), dot(dirWS, north));
    vec2 skyUv = GE_SkyDirToViewLutUv(dLocal);
    skyUv.y = clamp(skyUv.y, 0.5 / kSkyViewLutHeight, 1.0 - 0.5 / kSkyViewLutHeight);
    vec3 skyColor = texture(uSkyViewLUT, skyUv).rgb;

    // Layer the SAME shared ground / below-horizon / night composite the on-screen sky
    // applies (sky_render.frag), so the baked environment lights surfaces with the sky
    // the user actually sees: the diffuse irradiance integral picks up the darker
    // below-horizon hemisphere and the night gradient, so a night scene's ambient goes
    // dark instead of staying daytime-blue. nightAmount matches sky_render.frag exactly
    // (clamp of nightSkyBlend). The bright discs (sun / moon / stars) remain excluded so
    // the low-res specular prefilter never fireflies off a baked highlight.
    float nightAmount = clamp(uSky.nightSkyBlend, 0.0, 1.0);
    bool applyStylizedGround = (uAtmos.belowHorizonMode == 2u);
    skyColor = GE_ApplySkyGroundAndNight(skyColor, dirWS, viewZenithCos, nightAmount,
                                         applyStylizedGround,
                                         uAtmos.groundAlbedo, uAtmos.groundBrightness,
                                         uAtmos.groundNightColor, uAtmos.belowHorizonDarkness,
                                         uAtmos.belowHorizonDarkColor, uAtmos.belowHorizonBlendSharpness,
                                         uAtmos.nightSkyHorizonColor);

    // Ground bounce. The physical below-horizon modes describe how the VISIBLE backdrop
    // is drawn -- ContinueHorizon folds the horizon haze up, PlanetGround paints a lit
    // floor -- but the ground exists under the camera either way, so the baked
    // environment carries its bounce in both. Without this the lower hemisphere is
    // folded SKY, and a surface in shadow receives clear-sky irradiance with no light
    // from the ground: a shadowed neutral reads blue instead of dark and desaturated.
    // StylizedGround owns its authored ground colour, composited above, and is skipped.
    //
    // This is the term that reaches VERTICAL surfaces: half of a vertical Lambertian
    // surface's cosine-weighted hemisphere lies below the horizon, which neither the
    // AmbientTintGround slot (weight max(-N.y,0) == 0 there) nor IblLowerHemisphereDarkness
    // (smoothstep(0,0.25,-N.y) == 0 there) can reach.
    if (uAtmos.belowHorizonMode != 2u && viewZenithCos < 0.005)
    {
        skyColor = GE_CompositeGroundFloor(skyColor, uTransLUT,
                                           GE_SkyDiffuseMeanRadiance(uSkyViewLUT, kSkyViewLutHeight),
                                           camPos, dirWS, planetCenter, uAtmos.planetRadius, camR,
                                           viewZenithCos,
                                           uAtmos.groundAlbedo, uAtmos.groundBrightness,
                                           normalize(uSky.sunDirectionWS),
                                           uSky.sunColor, uSky.sunIntensity,
                                           uAtmos.betaRayleigh, uAtmos.rayleighScaleHeight,
                                           uAtmos.groundHazeStrength,
                                           uAtmos.groundNightColor, nightAmount);
    }

    oColor = vec4(skyColor * exp2(uSky.exposureEV), 1.0);
}
