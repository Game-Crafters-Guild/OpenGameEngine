#version 450

#include "Includes/compat_profile.glsl"
#include "Includes/screen_position.glsl"
#include "Includes/sky_composite.glsl"
#include "Includes/sun_disc.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform SkyUBO
{
    vec3  cameraPositionWS;   float exposureEV;
    vec3  cameraRightWS;      float sunSize2D;
    vec3  cameraUpWS;         float moonSize2D;
    vec3  cameraForwardWS;    float skyPan2D;
    vec3  sunDirectionWS;     float sunAngularRadius;
    vec3  moonDirectionWS;    float moonIntensity;
    float moonAngularRadius;
    float skyRotateAroundZenithRadians;
    float moonUboPad1;
    float moonUboPad2;
    vec3  sunColor;           float sunIntensity;
    float timeOfDayHours;     float skyPad0;
    float viewportAspect;     float tanHalfFovY;
    vec2  viewportResolution; float nightSkyBlend; float skyTimeSeconds;
    vec4  starSizeShape;
    vec4  starTwinkleParams;
    vec4  starTwinkleAmp;
    vec4  starDensityHorizon;
    vec4  starSizeRange; // z = HDRI rotation radians (or star core size procedural); w = HDRI intensity if >= 0, else procedural sky
    vec4  fallingStarParams; // x = enabled, y = amount, z = frequency, w = speed
    vec4  fallingStarShapeParams; // x = length, y = thickness, z = dot size, w = 2D dot size
    vec4  gradientSkyTop;    // .rgb = scene-linear top color; .w >= 0.5 -> render gradient sky
    vec4  gradientSkyHorizon;
    vec4  gradientSkyBottom;
} uSky;

layout(set = 0, binding = 1) uniform sampler2D uSkyViewLUT;

#include "Includes/sky_atmosphere_ubo.glsl"

layout(set = 0, binding = 3) uniform sampler2D uTransLUT;
layout(set = 0, binding = 4) uniform sampler2D uMoonFullTexture;

const float PI = 3.14159265359;

// Sky-view LUT height (matches Config::skyViewLutHeight); the sampled elevation
// is clamped to a half-texel inset so the warped poles never read past the edge.
const float kSkyViewLutHeight = 256.0;

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

// Stylized 3-color gradient sky: blend Top/Horizon/Bottom by the view direction's world +Y
// component (up -> Top, horizon -> Horizon, down -> Bottom). Branchless; colors are already
// scene-linear (intensity folded in on the CPU). Shared by the perspective and ortho paths.
vec3 GradientSkyColor(vec3 dirWS)
{
    float up   = max(dirWS.y, 0.0);
    float down = max(-dirWS.y, 0.0);
    float mid  = 1.0 - up - down;
    return uSky.gradientSkyTop.rgb * up
         + uSky.gradientSkyHorizon.rgb * mid
         + uSky.gradientSkyBottom.rgb * down;
}

float Hash13(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p += dot(p, p.yzx + 19.19);
    return fract(p.x * p.y * p.z);
}

vec2 EncodeDirToLutUv(vec3 dirWS)
{
    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 camPos = uSky.cameraPositionWS;
    float r = length(camPos - planetCenter);
    vec3 up = (camPos - planetCenter) / r;

    float mu = clamp(dot(dirWS, up), -1.0, 1.0);
    float rNorm = clamp((r - uAtmos.planetRadius) /
                        (uAtmos.atmosphereRadius - uAtmos.planetRadius), 0.0, 1.0);
    float u = 0.5 * (mu + 1.0);
    float v = sqrt(rNorm);
    return vec2(u, v);
}

float MoonPhaseMask(vec2 moonUv, float phase01)
{
    vec2 p = moonUv * 2.0 - 1.0;
    float r2 = dot(p, p);
    if (r2 > 1.0)
        return 0.0;

    float z = sqrt(max(0.0, 1.0 - r2));
    float phase = fract(phase01);
    float angle = (phase - 0.5) * (2.0 * PI);
    vec3 sunDir = normalize(vec3(-sin(angle), 0.0, cos(angle)));
    float lit = dot(vec3(p.x, p.y, z), sunDir);
    return smoothstep(-0.04, 0.04, lit);
}

vec3 FallingStarStartDir(float seed)
{
    float az = (Hash13(vec3(seed, 11.0, 7.0)) + 0.23 * Hash13(vec3(seed, 41.0, 29.0))) * 2.0 * PI;
    float y = mix(0.12, 0.96, pow(Hash13(vec3(seed, 13.0, 3.0)), 0.72));
    float xz = sqrt(max(0.0, 1.0 - y * y));
    return normalize(vec3(cos(az) * xz, y, sin(az) * xz));
}

vec3 FallingStarTangent(vec3 startDir, float seed)
{
    vec3 fallDir = normalize(vec3(mix(-0.9, 0.9, Hash13(vec3(seed, 17.0, 2.0))),
                                  mix(-1.35, -0.45, Hash13(vec3(seed, 23.0, 31.0))),
                                  mix(-0.9, 0.9, Hash13(vec3(seed, 19.0, 5.0)))));
    return normalize(fallDir - startDir * dot(fallDir, startDir));
}

// Sample the same HDR backdrop source as the perspective sky for one virtual
// sky direction. Orthographic rendering still lays those samples out as a
// screen-space gradient below, but the samples must remain on the atmosphere's
// physical scene-linear scale so a single camera EV exposes every projection.
vec3 SampleOrthoBackdropDirection(vec3 dirWS)
{
    float hdriIntensity = uSky.starSizeRange.w;
    if (hdriIntensity >= 0.0)
    {
        return GE_TAP_LOD0(uSkyViewLUT,
                       EncodeDirToLatLongUv(RotateAroundY(dirWS, uSky.starSizeRange.z))).rgb *
               hdriIntensity;
    }

    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 camPos = uSky.cameraPositionWS;
    vec3 up = normalize(camPos - planetCenter);

    vec3 northRef = vec3(0.0, 0.0, 1.0);
    if (abs(dot(northRef, up)) > 0.98)
        northRef = vec3(1.0, 0.0, 0.0);
    vec3 east = normalize(cross(up, northRef));
    vec3 north = normalize(cross(east, up));

    vec3 dLocal = vec3(dot(dirWS, east), dot(dirWS, up), dot(dirWS, north));
    vec2 skyUv = GE_SkyDirToViewLutUv(dLocal);
    skyUv.y = clamp(skyUv.y, 0.5 / kSkyViewLutHeight, 1.0 - 0.5 / kSkyViewLutHeight);
    vec3 skyColor = GE_TAP_LOD0(uSkyViewLUT, skyUv).rgb;

    bool applyStylizedGround = (uAtmos.belowHorizonMode == 2u);
    return GE_ApplySkyGroundAndNight(skyColor, dirWS, dot(dirWS, up),
                                     clamp(uSky.nightSkyBlend, 0.0, 1.0),
                                     applyStylizedGround,
                                     uAtmos.groundAlbedo, uAtmos.groundBrightness,
                                     uAtmos.groundNightColor, uAtmos.belowHorizonDarkness,
                                     uAtmos.belowHorizonDarkColor,
                                     uAtmos.belowHorizonBlendSharpness,
                                     uAtmos.nightSkyHorizonColor);
}

// 2D sky backdrop: simple screen-vertical gradient through HDR zenith / horizon /
// lower-sky samples, plus a sun tag at the sun's elevation. Stars are drawn by
// the billboard pass in their own ortho branch. This deliberately bypasses the
// per-pixel atmospheric ray simulation, which has no meaningful per-pixel
// angular variation in an orthographic projection, while keeping its radiance.
void RenderOrthoBackdrop(vec2 ndc)
{
    float nightAmount = clamp(uSky.nightSkyBlend, 0.0, 1.0);
    vec2 skyNdc = vec2(ndc.x, ndc.y - clamp(uSky.skyPan2D, -1.0, 1.0));

    // t: 0 at screen top, 0.5 at horizon (screen middle), 1 at bottom
    float t = clamp((1.0 - skyNdc.y) * 0.5, 0.0, 1.0);

    // Gradient sky: a stylized vertical gradient with NO celestial bodies. Map the screen-vertical
    // t (top -> Top, horizon -> Horizon, bottom -> Bottom) to the gradient colors and emit before
    // the sun disc / meteor / moon compositing below (matches the perspective path's early-out).
    if (uSky.gradientSkyTop.w >= 0.5)
    {
        vec3 grad = (t < 0.5)
            ? mix(uSky.gradientSkyTop.rgb, uSky.gradientSkyHorizon.rgb, smoothstep(0.0, 0.5, t))
            : mix(uSky.gradientSkyHorizon.rgb, uSky.gradientSkyBottom.rgb, smoothstep(0.5, 1.0, t));
        oColor = vec4(grad * exp2(uSky.exposureEV), 1.0);
        return;
    }

    // Build a stable virtual horizon direction from the camera orientation.
    // Top/bottom views have a forward vector parallel to planet-up, so fall
    // back through the camera's up/right axes before using local north.
    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 planetUp = normalize(uSky.cameraPositionWS - planetCenter);
    vec3 horizonDir = uSky.cameraForwardWS -
                      planetUp * dot(uSky.cameraForwardWS, planetUp);
    if (dot(horizonDir, horizonDir) < 1e-6)
        horizonDir = uSky.cameraUpWS - planetUp * dot(uSky.cameraUpWS, planetUp);
    if (dot(horizonDir, horizonDir) < 1e-6)
        horizonDir = uSky.cameraRightWS - planetUp * dot(uSky.cameraRightWS, planetUp);
    if (dot(horizonDir, horizonDir) < 1e-6)
    {
        vec3 northRef = abs(planetUp.z) < 0.98 ? vec3(0.0, 0.0, 1.0)
                                               : vec3(1.0, 0.0, 0.0);
        horizonDir = northRef - planetUp * dot(northRef, planetUp);
    }
    horizonDir = normalize(horizonDir);

    // These anchors now come from the same HDR atmosphere/HDRI sampled by the
    // perspective path. The old hand-authored ~0..1 palette sat roughly seven
    // stops below a sky driven by a physical-lux sun, forcing incompatible EVs.
    vec3 zenithCol = SampleOrthoBackdropDirection(planetUp);
    vec3 horizonCol = SampleOrthoBackdropDirection(horizonDir);
    vec3 groundCol = SampleOrthoBackdropDirection(-planetUp);

    vec3 col = (t < 0.5)
        ? mix(zenithCol, horizonCol, smoothstep(0.0, 0.5, t))
        : mix(horizonCol, groundCol, smoothstep(0.5, 1.0, t));

    float rimWidth = max(mix(uAtmos.groundHorizonDayCosWidth,
                             uAtmos.groundHorizonNightCosWidth,
                             nightAmount) * 3.0, 0.015);
    float rimMask = 1.0 - smoothstep(0.0, rimWidth, abs(skyNdc.y));
    col = mix(col, horizonCol, clamp(rimMask, 0.0, 1.0));
    vec3 sunDir = normalize(uSky.sunDirectionWS);

    // Sun disk: evaluate against a virtual sky direction, like sampling a
    // cubemap/sky dome through the camera. This keeps the orthographic world
    // projection, but lets camera orbit move the celestial disk left/right.
    if (uSky.sunAngularRadius > 0.0 && sunDir.y > -0.18 && nightAmount < 0.98)
    {
        float aspect = max(uSky.viewportAspect, 0.001);
        const float virtualTanHalfFovY = 1.0;
        vec3 basisRight = normalize(uSky.cameraRightWS);
        vec3 basisUp    = normalize(uSky.cameraUpWS);
        vec3 basisFwd   = normalize(uSky.cameraForwardWS);
        float sunRadius = uSky.sunAngularRadius * max(uSky.sunSize2D, 0.1);
        // The 2D backdrop is a stylised screen-space tag, not the physical disc: it draws the sun
        // in NDC, so this is a screen fraction and not a solid angle. It is scaled off
        // sunAngularRadius only to have one place the sun's size comes from, so the multiplier
        // must move with that constant or the tag silently resizes. sunAngularRadius already
        // carries SkyEnvironment::SunSize, so the two compose and SunSize2D trims the tag on top
        // of the sun's size — as moonSize2D does below.
        // The 0.055 NDC floor applies to that PRODUCT, not to SunSize2D alone: 16 * 0.004651 =
        // 0.0744 NDC at SunSize 1 and SunSize2D 1, so the tag stops shrinking once
        // SunSize * SunSize2D drops under ~0.74 and both dials are inert below that combined
        // value.
        float effectiveRadius = max(sunRadius * 16.0, 0.055);
        float disc = 0.0;
        float ramp = 0.0;
        if (uSky.skyRotateAroundZenithRadians > 0.5)
        {
            // Editor 2D mode keeps the sun visible as a circular screen orbit.
            vec2 sunScreen = vec2(dot(sunDir, basisRight),
                                  dot(sunDir, basisUp));
            const float maxSunScreenRadius = 0.82;
            float sunScreenLen = length(sunScreen);
            if (sunScreenLen > maxSunScreenRadius)
                sunScreen *= maxSunScreenRadius / sunScreenLen;
            vec2 sunNDC = vec2(sunScreen.x / aspect, sunScreen.y);
            vec2 d = vec2((skyNdc.x - sunNDC.x) * aspect, skyNdc.y - sunNDC.y);
            float r2 = dot(d, d);
            disc = exp(-r2 / max(effectiveRadius * effectiveRadius, 1e-6));
            ramp = exp(-r2 / max(effectiveRadius * effectiveRadius * 26.0, 1e-6));
        }
        else
        {
            // Generic orthographic mode moves the sun with the camera-facing
            // sky basis, but measures the disk in screen space so side views
            // do not squash the sun into an ellipse.
            vec2 sunScreen = vec2(dot(sunDir, basisRight),
                                  dot(sunDir, basisUp));
            vec2 sunNDC = vec2(sunScreen.x / aspect, sunScreen.y);
            vec2 d = vec2((skyNdc.x - sunNDC.x) * aspect, skyNdc.y - sunNDC.y);
            float sunForward = dot(sunDir, basisFwd);
            float frontFade = smoothstep(-0.05, 0.05, sunForward);
            float r2 = dot(d, d);
            disc = exp(-r2 / max(effectiveRadius * effectiveRadius, 1e-6)) * frontFade;
            ramp = exp(-r2 / max(effectiveRadius * effectiveRadius * 26.0, 1e-6)) * frontFade;
        }
        float horizonFade = smoothstep(-0.05, 0.05, sunDir.y);
        float horizonMask = smoothstep(-0.006, 0.006, skyNdc.y);
        float dayFade = 1.0 - nightAmount;
        float sunsetT = 1.0 - smoothstep(0.04, 0.55, sunDir.y);
        vec3 warmRampColor = mix(uSky.sunColor, vec3(1.0, 0.48, 0.18), sunsetT);
        float rampStrength = clamp(uSky.sunIntensity * 0.018, 0.0, 1.2);
        col += warmRampColor * (rampStrength * horizonFade * dayFade) * ramp * horizonMask;
        col += uSky.sunColor * (uSky.sunIntensity * 0.4 * horizonFade * dayFade) * disc * horizonMask;
    }

    if (uSky.fallingStarParams.x > 0.5 && nightAmount > 0.01)
    {
        float aspect = max(uSky.viewportAspect, 0.001);
        vec3 basisRight = normalize(uSky.cameraRightWS);
        vec3 basisUp    = normalize(uSky.cameraUpWS);
        vec3 basisFwd   = normalize(uSky.cameraForwardWS);
        float amount = clamp(uSky.fallingStarParams.y, 0.0, 1.0);
        float frequency = clamp(uSky.fallingStarParams.z, 0.0, 4.0);
        float speed = clamp(uSky.fallingStarParams.w, 0.2, 50.0);
        float trailLength = clamp(uSky.fallingStarShapeParams.x, 0.2, 4.0);
        float trailThickness = clamp(uSky.fallingStarShapeParams.y, 0.1, 4.0);
        float dotSize = clamp(uSky.fallingStarShapeParams.w, 0.1, 4.0);
        const int kMaxMeteors = 8;
        for (int i = 0; i < kMaxMeteors; ++i)
        {
            float enabled = step(float(i) + 0.5, amount * float(kMaxMeteors));
            float seed = float(i) * 31.73 + floor(uSky.skyTimeSeconds * 0.017 + float(i) * 7.13) * 19.19 + 3.11;
            float periodJitter = mix(0.58, 1.72, Hash13(vec3(seed, 37.0, 9.0)));
            float period = mix(8.5, 1.0, frequency / 4.0) * periodJitter;
            float t = fract(uSky.skyTimeSeconds / max(period, 0.1) + Hash13(vec3(seed, 2.0, 5.0)));
            float speedJitter = mix(0.55, 1.65, Hash13(vec3(seed, 43.0, 17.0)));
            float activeEnd = mix(0.42, 0.13, clamp((speed * speedJitter) / 4.0, 0.0, 1.0));
            float meteorActive = smoothstep(0.0, 0.035, t) * (1.0 - smoothstep(activeEnd * 0.58, activeEnd, t));
            vec3 startDir = FallingStarStartDir(seed);
            vec2 p = vec2(skyNdc.x * aspect, skyNdc.y);
            vec2 startScreen = vec2(dot(startDir, basisRight) / aspect,
                                    dot(startDir, basisUp));
            if (uSky.skyRotateAroundZenithRadians > 0.5)
            {
                const float maxMeteorScreenRadius = 1.18;
                float startLen = length(startScreen);
                if (startLen > maxMeteorScreenRadius)
                    startScreen *= maxMeteorScreenRadius / startLen;
            }
            float angle = mix(-2.55, -0.58, Hash13(vec3(seed, 71.0, 5.0)));
            vec2 v = normalize(vec2(cos(angle) / aspect, sin(angle)));
            float travel = t * 0.58 * speed * speedJitter;
            vec2 h = vec2(startScreen.x * aspect, startScreen.y) + v * travel;
            float lengthJitter = mix(0.55, 1.75, Hash13(vec3(seed, 53.0, 11.0)));
            float brightnessJitter = mix(0.55, 1.85, Hash13(vec3(seed, 61.0, 13.0)));
            float alongMax = 0.30 * trailLength * lengthJitter;
            float visibleAlongMax = max(alongMax * smoothstep(0.0, activeEnd * 0.55, t), 0.0001);
            float rawAlong = dot(p - h, -v);
            float along = clamp(rawAlong, 0.0, visibleAlongMax);
            float dist = length(p - (h - v * along));
            float frontFade = smoothstep(-0.03, 0.03, dot(startDir, basisFwd));
            float widthFalloff = 105000.0 / max(trailThickness * trailThickness, 0.01);
            float lengthFalloff = 20.0 / max(trailLength * lengthJitter, 0.2);
            float behindTip = smoothstep(0.0, 0.006, rawAlong) *
                              (1.0 - smoothstep(visibleAlongMax * 0.88, visibleAlongMax, rawAlong));
            float tail = exp(-dist * dist * widthFalloff) * exp(-along * lengthFalloff) * behindTip;
            float tip = exp(-dot(p - h, p - h) * widthFalloff * 0.42 / max(dotSize * dotSize, 0.01));
            vec3 tailColor = vec3(0.62, 0.78, 1.0);
            vec3 tipColor = vec3(1.0, 0.96, 0.82);
            col += (tailColor * tail * 0.85 + tipColor * tip * 2.4) *
                   meteorActive * enabled * frontFade * nightAmount * brightnessJitter;
        }
    }

    vec3 moonDir = normalize(uSky.moonDirectionWS);
    float moonAboveHorizon = smoothstep(-0.18, 0.02, moonDir.y);
    if (uSky.moonIntensity > 0.0 && moonAboveHorizon > 0.0)
    {
        float aspect = max(uSky.viewportAspect, 0.001);
        vec3 basisRight = normalize(uSky.cameraRightWS);
        vec3 basisUp    = normalize(uSky.cameraUpWS);
        vec2 moonScreen = vec2(dot(moonDir, basisRight),
                               dot(moonDir, basisUp));
        if (uSky.skyRotateAroundZenithRadians > 0.5)
        {
            const float maxMoonScreenRadius = 0.82;
            float moonScreenLen = length(moonScreen);
            if (moonScreenLen > maxMoonScreenRadius)
                moonScreen *= maxMoonScreenRadius / moonScreenLen;
        }

        vec2 moonNDC = vec2(moonScreen.x / aspect, moonScreen.y);
        float moonRadius = max(uSky.moonAngularRadius * 6.0 * max(uSky.moonSize2D, 0.1), 0.05);
        vec2 d = vec2((skyNdc.x - moonNDC.x) * aspect, skyNdc.y - moonNDC.y);
        float diskDist = length(d) / moonRadius;
        float moonHorizonMask = smoothstep(-0.006, 0.006, skyNdc.y);
        float moonDisk = smoothstep(1.0, 0.92, diskDist) * moonHorizonMask;
        if (moonDisk > 0.0)
        {
            vec2 moonUv = d / moonRadius * 0.5 + 0.5;
            moonUv.y = 1.0 - moonUv.y;
            vec4 moonAlbedo = GE_TAP_LOD0(uMoonFullTexture, moonUv);
            float phaseMask = MoonPhaseMask(moonUv, uSky.moonUboPad1);
            float phaseShade = mix(0.08, 1.0, phaseMask);
            vec3 moonColor = moonAlbedo.rgb;
            float moonAlpha = moonAlbedo.a;
            float visibility = mix(0.2, 1.0, nightAmount);
            col += moonColor * vec3(0.85, 0.9, 1.05) *
                   (uSky.moonIntensity * exp2(uSky.moonUboPad2) * visibility) *
                   moonDisk * moonAlpha * phaseShade * moonAboveHorizon;
        }
    }

    oColor = vec4(col * exp2(uSky.exposureEV), 1.0);
}

void main()
{
    vec2 ndc = GE_ViewportUVToYUpNdc(vUV);

    float thfov = uSky.tanHalfFovY;
    float aspect = max(uSky.viewportAspect, 0.001);

    // tanHalfFovY <= 0 is the host-side sentinel for orthographic projection.
    // Switch to a 2D backdrop instead of the per-pixel atmospheric simulation,
    // which produces a uniform color in ortho (all rays are parallel).
    if (thfov <= 0.0)
    {
        RenderOrthoBackdrop(ndc);
        return;
    }

    vec3 basisRight = normalize(uSky.cameraRightWS);
    vec3 basisUp    = normalize(uSky.cameraUpWS);
    vec3 basisFwd   = normalize(uSky.cameraForwardWS);

    vec3 dirView = normalize(vec3(ndc.x * aspect * thfov,
                                  ndc.y * thfov,
                                  -1.0));
    vec3 dirWS = normalize(dirView.x * basisRight +
                           dirView.y * basisUp +
                          (-dirView.z) * basisFwd);

    // Gradient sky: stylized 3-color vertical gradient, no atmosphere / sun disc / stars.
    if (uSky.gradientSkyTop.w >= 0.5)
    {
        oColor = vec4(GradientSkyColor(dirWS) * exp2(uSky.exposureEV), 1.0);
        return;
    }

    float hdriIntensity = uSky.starSizeRange.w;
    if (hdriIntensity >= 0.0)
    {
        vec3 envColor = GE_TAP_LOD0(uSkyViewLUT, EncodeDirToLatLongUv(RotateAroundY(dirWS, uSky.starSizeRange.z))).rgb;
        oColor = vec4(envColor * hdriIntensity * exp2(uSky.exposureEV), 1.0);
        return;
    }

    // Planet-relative up for stable zenith across camera movement
    vec3 planetCenter = vec3(0.0, -uAtmos.planetRadius, 0.0);
    vec3 camPos       = uSky.cameraPositionWS;
    float camR        = length(camPos - planetCenter);
    vec3 up           = (camPos - planetCenter) / max(camR, 1e-5);

    float viewZenithCos = dot(dirWS, up);

    vec3 sunDir = normalize(uSky.sunDirectionWS);

    float nightAmount = clamp(uSky.nightSkyBlend, 0.0, 1.0);

    vec3 northRef = vec3(0.0, 0.0, 1.0);
    if (abs(dot(northRef, up)) > 0.98)
        northRef = vec3(1.0, 0.0, 0.0);
    vec3 east = normalize(cross(up, northRef));
    vec3 north = normalize(cross(east, up));

    vec3 dLocal = vec3(dot(dirWS, east), dot(dirWS, up), dot(dirWS, north));
    vec2 skyUv = GE_SkyDirToViewLutUv(dLocal);
    skyUv.y = clamp(skyUv.y, 0.5 / kSkyViewLutHeight, 1.0 - 0.5 / kSkyViewLutHeight);
    vec3 skyColor = GE_TAP_LOD0(uSkyViewLUT, skyUv).rgb;

    // ---- Sun disk ----
    // Drawn at the sun's apparent angular radius and given the radiance that makes the drawn
    // profile carry the sun's irradiance (E / omega_drawn), attenuated along the view ray by
    // the same transmittance the sky itself is built from. The disc's size and its brightness
    // are therefore one decision, not two: nothing here scales the sun by elevation, because
    // the atmosphere's own optical depth is what reddens and dims it at sunset.
    float muSun = dot(dirWS, sunDir);
    float sunRadius = uSky.sunAngularRadius;

    float sunDisk = 0.0;
    if (sunRadius > 0.0 && muSun > 0.0 && viewZenithCos > 0.0)
    {
        float angle = acos(clamp(muSun, -1.0, 1.0));
        sunDisk = clamp(GE_SunDiscProfile(angle, sunRadius), 0.0, 1.0);

        const float kHorizonFadeEnd = 0.02;
        float horizonFade = smoothstep(0.0, kHorizonFadeEnd, viewZenithCos);
        sunDisk *= horizonFade;
        sunDisk *= (1.0 - nightAmount);
    }

    vec2 transUv = EncodeDirToLutUv(dirWS);
    vec3 transmittance = GE_TAP_LOD0(uTransLUT, transUv).rgb;

    // The sky's single exposure factor; the disc's peak is bounded against the value it puts
    // in the fp16 target, so it has to know the multiply that lands it there.
    float exposure = exp2(uSky.exposureEV);
    float sunPeakRadiance = GE_SunDiscPeakRadiance(uSky.sunIntensity, sunRadius, exposure);
    vec3 sunRadiance = uSky.sunColor * sunPeakRadiance * sunDisk * transmittance;

    // ---- Moon disk (phase: terminator + earthshine on dark limb) ----
    vec3 moonRadiance = vec3(0.0);
    if (uSky.moonIntensity > 0.0)
    {
        vec3 moonDir = normalize(uSky.moonDirectionWS);
        float muMoon = dot(dirWS, moonDir);
        float moonAboveHorizon = smoothstep(0.0, 0.02, dot(moonDir, up));
        float moonR = max(1e-6, uSky.moonAngularRadius > 0.0 ? uSky.moonAngularRadius : 0.026);
        if (muMoon > cos(moonR * 1.02) && viewZenithCos > 0.0 && moonAboveHorizon > 0.0)
        {
            float tDisk = sqrt(max(0.0, 2.0 * (1.0 - muMoon))) / moonR;
            float moonDisk = smoothstep(1.0, 0.92, tDisk);

            vec3 mUpFallback = abs(moonDir.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            vec3 mU = normalize(cross(mUpFallback, moonDir));
            vec3 mV = cross(moonDir, mU);
            vec3 perp = dirWS - muMoon * moonDir;
            float du = dot(perp, mU);
            float dv = dot(perp, mV);
            const float kEarthshine = 0.08;
            vec2 moonUv = vec2(du / moonR * 0.5 + 0.5,
                               0.5 - dv / moonR * 0.5);
            vec4 moonAlbedo = GE_TAP_LOD0(uMoonFullTexture, moonUv);
            float phaseMask = MoonPhaseMask(moonUv, uSky.moonUboPad1);
            float phaseShade = mix(kEarthshine, 1.0, phaseMask);

            float horizonFade = smoothstep(0.0, 0.02, viewZenithCos);
            moonDisk *= horizonFade * moonAboveHorizon * moonAlbedo.a * phaseShade;

            float visibility = mix(0.15, 1.0, nightAmount);
            const vec3 kMoonTint = vec3(0.85, 0.9, 1.05);
            moonRadiance = moonAlbedo.rgb * kMoonTint * (uSky.moonIntensity * exp2(uSky.moonUboPad2) * visibility) * moonDisk;
        }
    }

    vec3 fallingStarRadiance = vec3(0.0);
    if (uSky.fallingStarParams.x > 0.5 && nightAmount > 0.01)
    {
        float amount = clamp(uSky.fallingStarParams.y, 0.0, 1.0);
        float frequency = clamp(uSky.fallingStarParams.z, 0.0, 4.0);
        float speed = clamp(uSky.fallingStarParams.w, 0.2, 50.0);
        float trailLength = clamp(uSky.fallingStarShapeParams.x, 0.2, 4.0);
        float trailThickness = clamp(uSky.fallingStarShapeParams.y, 0.1, 4.0);
        float dotSize = clamp(uSky.fallingStarShapeParams.z, 0.1, 4.0);
        const int kMaxMeteors = 8;
        for (int i = 0; i < kMaxMeteors; ++i)
        {
            float enabled = step(float(i) + 0.5, amount * float(kMaxMeteors));
            float seed = float(i) * 31.73 + floor(uSky.skyTimeSeconds * 0.017 + float(i) * 7.13) * 19.19 + 3.11;
            float periodJitter = mix(0.58, 1.72, Hash13(vec3(seed, 37.0, 9.0)));
            float period = mix(8.5, 1.0, frequency / 4.0) * periodJitter;
            float t = fract(uSky.skyTimeSeconds / max(period, 0.1) + Hash13(vec3(seed, 2.0, 5.0)));
            float speedJitter = mix(0.55, 1.65, Hash13(vec3(seed, 43.0, 17.0)));
            float activeEnd = mix(0.42, 0.13, clamp((speed * speedJitter) / 4.0, 0.0, 1.0));
            float meteorActive = smoothstep(0.0, 0.035, t) * (1.0 - smoothstep(activeEnd * 0.58, activeEnd, t));
            vec3 startDir = FallingStarStartDir(seed);
            vec3 tangentWS = FallingStarTangent(startDir, seed);
            vec3 headDir = normalize(startDir + tangentWS * (t * 0.8 * speed * speedJitter));
            vec3 sideWS = normalize(cross(headDir, tangentWS));
            vec3 delta = dirWS - headDir;
            float lengthJitter = mix(0.55, 1.75, Hash13(vec3(seed, 53.0, 11.0)));
            float brightnessJitter = mix(0.55, 1.85, Hash13(vec3(seed, 61.0, 13.0)));
            float alongMax = 0.10 * trailLength * lengthJitter;
            float visibleAlongMax = max(alongMax * smoothstep(0.0, activeEnd * 0.55, t), 0.0001);
            float rawAlong = dot(delta, -tangentWS);
            float along = clamp(rawAlong, 0.0, visibleAlongMax);
            float dist = abs(dot(delta + tangentWS * along, sideWS));
            float facing = smoothstep(cos(0.7), cos(0.35), dot(dirWS, headDir));
            float widthFalloff = 340000.0 / max(trailThickness * trailThickness, 0.01);
            float lengthFalloff = 44.0 / max(trailLength * lengthJitter, 0.2);
            float behindTip = smoothstep(0.0, 0.002, rawAlong) *
                              (1.0 - smoothstep(visibleAlongMax * 0.88, visibleAlongMax, rawAlong));
            float tail = exp(-dist * dist * widthFalloff) * exp(-along * lengthFalloff) * facing * behindTip;
            float tip = exp(-max(0.0, 1.0 - dot(dirWS, headDir)) * 9000.0 / max(dotSize * dotSize, 0.01)) * facing;
            vec3 tailColor = vec3(0.62, 0.78, 1.0);
            vec3 tipColor = vec3(1.0, 0.96, 0.82);
            fallingStarRadiance += (tailColor * tail * 0.85 + tipColor * tip * 2.4) *
                                   meteorActive * enabled * nightAmount * brightnessJitter;
        }
    }

    // ---- Ground / below-horizon blend + night-sky gradient ----
    // Extracted into sky_composite.glsl so the IBL capture (sky_capture_cube.frag)
    // composites the sky identically. The bright discs (sun/moon/stars) stay below,
    // inline, so the capture can stop after this call.
    //
    // Below-horizon mode selects whether the stylized ground composite runs on read:
    //   ContinueHorizon (0) / PlanetGround (1): the LUT already holds the right
    //     below-horizon color (folded horizon or lit planet floor), so apply NO
    //     stylized ground — sample the LUT straight (applyGround = false).
    //   StylizedGround (2): the raw LUT trends black below, so paint the stylized
    //     ground/dark blend over it (applyGround = true) — the current look.
    // The night gradient above the horizon applies in every mode.
    bool applyStylizedGround = (uAtmos.belowHorizonMode == 2u);
    skyColor = GE_ApplySkyGroundAndNight(skyColor, dirWS, viewZenithCos, nightAmount,
                                         applyStylizedGround,
                                         uAtmos.groundAlbedo, uAtmos.groundBrightness,
                                         uAtmos.groundNightColor, uAtmos.belowHorizonDarkness,
                                         uAtmos.belowHorizonDarkColor, uAtmos.belowHorizonBlendSharpness,
                                         uAtmos.nightSkyHorizonColor);

    // PlanetGround composites a sharp lit floor per-pixel (the LUT stores only the
    // folded horizon-haze backdrop for this mode). The per-fragment ray-sphere hit
    // resolves the grazing planet edge at full resolution -- no LUT magnification.
    // GE_CompositeGroundFloor is shared with the IBL capture so the ground a camera
    // sees and the ground bounce a surface receives cannot drift apart.
    //
    // GE_CompositeGroundFloor is the shared visible+IBL floor. Mean sky
    // radiance is a per-pixel cost here; hoist it to the UBO when a scene
    // actually pins PlanetGround.
    if (uAtmos.belowHorizonMode == 1u && viewZenithCos < 0.005)
    {
        skyColor = GE_CompositeGroundFloor(skyColor, uTransLUT,
                                           GE_SkyDiffuseMeanRadiance(uSkyViewLUT, kSkyViewLutHeight),
                                           camPos, dirWS, planetCenter, uAtmos.planetRadius, camR,
                                           viewZenithCos,
                                           uAtmos.groundAlbedo, uAtmos.groundBrightness,
                                           sunDir, uSky.sunColor, uSky.sunIntensity,
                                           uAtmos.betaRayleigh, uAtmos.rayleighScaleHeight,
                                           uAtmos.groundHazeStrength,
                                           uAtmos.groundNightColor, nightAmount);
    }

    // Horizon rim is a stylized overlay: the rim ramp painted as a band straddling
    // the horizon. Only StylizedGround wants it. The physical modes (ContinueHorizon
    // / PlanetGround) keep the atmospheric horizon clean, so no rim band there.
    if (uAtmos.belowHorizonMode == 2u)
    {
        float nightRimT = clamp(uSky.nightSkyBlend, 0.0, 1.0);
        float rimW = max(mix(uAtmos.groundHorizonDayCosWidth, uAtmos.groundHorizonNightCosWidth, nightRimT), 1e-5);
        float rimMask = 1.0 - smoothstep(0.0, rimW, abs(viewZenithCos));
        vec3 rimCol = mix(uAtmos.groundHorizonColor, uAtmos.groundHorizonNightColor, nightRimT);
        skyColor = mix(skyColor, rimCol, clamp(rimMask, 0.0, 1.0));
    }

    vec3 finalColor = skyColor + fallingStarRadiance + sunRadiance + moonRadiance;

    oColor = vec4(finalColor * exposure, 1.0);
}
