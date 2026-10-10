#ifndef GE_SKY_COMPOSITE_GLSL
#define GE_SKY_COMPOSITE_GLSL

#include "compat_profile.glsl"

#include "sky_ray_sphere.glsl"

// Shared sky composite: the ground / below-horizon blend and the night-sky
// gradient that sit on TOP of the atmospheric sky-view LUT sample. Factored out
// of sky_render.frag so the IBL capture (sky_capture_cube.frag) reproduces the
// EXACT same end-result sky the user sees on screen — minus the bright discs
// (sun / moon / stars), which the capture omits so the prefilter never fireflies.
//
// These functions are a verbatim extract of the inlined blocks that used to live
// in sky_render.frag's perspective path; the on-screen sky output is unchanged.
// All inputs are passed explicitly (no UBO dependency) so this include compiles
// against any UBO layout that supplies the fields.

#ifndef GE_SKY_PI
#define GE_SKY_PI 3.14159265359
#endif

// Sky-view LUT mapping: planet-local direction (east/up/north frame) <-> LUT UV.
// u is the full-circle azimuth (atan over east/north), so it wraps continuously
// across u=0/1 with a Repeat-u sampler — no seam anywhere on the visible sky.
// v is the elevation, warped by a signed-sqrt so texels concentrate at the
// horizon (v=0.5) where the atmospheric gradient changes fastest, and thin out
// toward the poles. This replaces the octahedral encode, whose diagonal fold
// produced a visible seam along the horizon on the directly-viewed sky.
//
// GE_SkyDirToViewLutUv and GE_SkyViewLutUvToDir are exact inverses; the compute
// pass decodes UV->dir to march, the render/capture passes encode dir->UV to read.

// Encode a planet-local direction (x=east, y=up, z=north) to the sky-view LUT UV.
vec2 GE_SkyDirToViewLutUv(vec3 dLocal)
{
    float u = atan(dLocal.z, dLocal.x) * (0.5 / GE_SKY_PI) + 0.5; // azimuth, wraps at u=0/1
    float elev = asin(clamp(dLocal.y, -1.0, 1.0));                // [-PI/2, PI/2]
    float e = elev / GE_SKY_PI + 0.5;                             // [0,1], 0.5 == horizon
    float s = e * 2.0 - 1.0;
    float v = 0.5 + 0.5 * sign(s) * sqrt(abs(s));                 // concentrate texels at horizon
    return vec2(u, v);
}

// Decode a sky-view LUT UV back to a planet-local direction (exact inverse of
// GE_SkyDirToViewLutUv). Returns x=east, y=up, z=north components.
vec3 GE_SkyViewLutUvToDir(vec2 uv)
{
    float az = (uv.x - 0.5) * (2.0 * GE_SKY_PI);
    float s = uv.y * 2.0 - 1.0;
    float e = 0.5 + 0.5 * sign(s) * (s * s);                      // invert the sqrt warp
    float elev = (e - 0.5) * GE_SKY_PI;
    float cosEl = cos(elev), sinEl = sin(elev);
    return vec3(cosEl * cos(az), sinEl, cosEl * sin(az));
}

float GE_SkyHash13(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.1, 0.2, 0.3));
    p += dot(p, p.yzx + 19.19);
    return fract(p.x * p.y * p.z);
}

// Layers the ground / below-horizon blend and the night-sky gradient over the
// atmospheric sky color. Mirrors sky_render.frag's "Ground color blend" +
// "Night sky gradient" blocks exactly (same branches, same constants, same
// noise term). Returns the composited sky color (still pre-exposure).
//
//   skyColor       - the sky-view LUT sample (scene-linear, pre-composite)
//   dirWS          - world-space view direction (for the night noise term)
//   viewZenithCos  - dot(dirWS, planet-local up)
//   nightAmount    - clamp(nightSkyBlend, 0, 1)
//   applyGround    - when false, skip the below-horizon ground/dark block so the
//                    raw atmospheric sky shows below the horizon (open sky). The
//                    night gradient (above the horizon) is unaffected either way.
//   groundAlbedo / groundBrightness / groundNightColor / belowHorizon* /
//   nightSkyHorizonColor - the AtmosphereUBO fields the blocks consume.
vec3 GE_ApplySkyGroundAndNight(vec3 skyColor,
                               vec3 dirWS,
                               float viewZenithCos,
                               float nightAmount,
                               bool applyGround,
                               vec3 groundAlbedo,
                               float groundBrightness,
                               vec3 groundNightColor,
                               float belowHorizonDarkness,
                               vec3 belowHorizonDarkColor,
                               float belowHorizonBlendSharpness,
                               vec3 nightSkyHorizonColor)
{
    // ---- Ground color blend ----
    vec3 dayGroundColor = groundAlbedo * max(groundBrightness, 0.0);
    vec3 groundColor = mix(dayGroundColor, groundNightColor, clamp(nightAmount, 0.0, 1.0));
    if (applyGround && viewZenithCos < 0.0)
    {
        float darkness = clamp(belowHorizonDarkness, 0.0, 1.0);
        skyColor = mix(groundColor, belowHorizonDarkColor, darkness);
        float k = max(belowHorizonBlendSharpness, 0.001);
        float t = clamp(-viewZenithCos * k, 0.0, 1.0);
        skyColor = mix(skyColor, groundColor, t);
    }

    // ---- Night sky gradient ----
    // Anchor the night sky to the actual horizon twilight (the sky-view LUT
    // sample) and darken smoothly toward the zenith, then add the authored glow
    // as a horizon-weighted term that is strongest AT the horizon and fades up.
    // This fuses the LUT twilight and the authored glow into ONE horizon glow
    // instead of injecting a second band above the horizon: the old gradient
    // ramped nightSkyHorizonColor in via smoothstep(0, 0.20, viewZenithCos),
    // which held the night color off the horizon and left the LUT twilight
    // showing through underneath it as a separate band.
    // When the stylized ground is painted (applyGround), keep the night gradient
    // strictly above the horizon so it never tints that ground; the rim +
    // nightSkyHorizonColor overlay in the render pass owns the StylizedGround horizon.
    // The physical modes (ContinueHorizon / PlanetGround) instead MIRROR the night sky
    // below the horizon -- |viewZenithCos| drives the gradient, so the horizon glow
    // continues down and darkens to the nadir as a seamless night sphere (PlanetGround
    // then composites its lit floor on top).
    float nightFloorCos = applyGround ? 0.0 : -1.0;
    if (nightAmount > 0.0 && viewZenithCos > nightFloorCos)
    {
        // |viewZenithCos| for the physical modes mirrors the gradient below the horizon;
        // StylizedGround clamps to the upper half so its painted ground stays untouched.
        float elevCos = applyGround ? max(viewZenithCos, 0.0) : abs(viewZenithCos);

        // Darken the twilight toward a dark night zenith. zenithDarken is 0 at the
        // horizon (keep the LUT twilight) and approaches 1 at the zenith/nadir.
        // Scene-linear on the 203-nit anchor, ~0.02-0.12 nits: a real night
        // zenith, so midnight meters below the auto-exposure floor and stays dark
        // (the dimmer default NightSkyHorizonColorKeys carry the same rationale).
        const vec3 kNightZenithCol = vec3(0.00018, 0.00009, 0.0006);
        float zenithDarken = smoothstep(0.0, 0.85, elevCos);
        vec3 baseNight = mix(skyColor, kNightZenithCol, zenithDarken);

        // The authored glow owns the night horizon: blend toward it so the bright LUT
        // twilight grazing glow is subdued under it (it would otherwise read as a
        // separate grey band beside the purple), fading to the darkened twilight away.
        float horizonWeight = 1.0 - smoothstep(0.0, 0.55, elevCos);
        vec3 nightSky = mix(baseNight, nightSkyHorizonColor, horizonWeight);

        float noise = GE_SkyHash13(normalize(dirWS) * 37.0);
        float noiseFactor = 1.0 + (noise - 0.5) * 0.05;
        nightSky *= noiseFactor;

        float mixStrength = clamp(nightAmount, 0.0, 1.0);
        skyColor = mix(skyColor, nightSky, mixStrength);
    }

    return skyColor;
}

// Aerial perspective: the floor sits behind the in-scattered air light (the horizon
// haze the LUT stored for this direction). A single grey view-ray transmittance keyed
// off distance fades the floor into that haze with no per-pixel march.
vec3 GE_AerialPerspectiveComposite(vec3 hazeBackdrop, vec3 floorColor, float distToFloor, float extinction)
{
    float viewTrans = exp(-distToFloor * extinction);
    return mix(hazeBackdrop, floorColor, viewTrans);
}

// Number of sky-view LUT taps used for the skylight the ground receives. The taps are
// drawn from the cosine-weighted density itself (below), so every tap carries weight
// 1/N and the estimator is exact for a uniform sky at any N; N buys accuracy on a sky
// that varies, which is the low-sun case.
//
// Cost, stated without flattery: glslang leaves this loop ROLLED (one OpLoopMerge in the
// emitted SPIR-V), so it is N sequential iterations of a CDF inverse, a sincos and a
// texture fetch -- not N unrolled ALU ops. The tap UVs are identical for every
// invocation, which is the one thing in its favour (the fetches hit the same texels
// frame-wide), but this is a real per-invocation cost wherever it runs per pixel.
#ifndef GE_SKY_AMBIENT_SAMPLE_COUNT
#define GE_SKY_AMBIENT_SAMPLE_COUNT 16
#endif

// Cosine-weighted mean sky radiance over the upper hemisphere, as seen from the ground:
//
//     E    = integral of L(w) (n.w) dw over the upper hemisphere
//     Lbar = E / PI            (the integral of (n.w) dw over that hemisphere is PI)
//
// so a Lambertian floor of albedo p leaves radiance p*E/PI == p*Lbar. Lbar is what this
// returns, and GE_GroundBounceRadiance multiplies it by the albedo and nothing else.
//
// The taps are importance-sampled from the cosine density: its CDF in the zenith angle
// is sin^2(theta), so theta_i = asin(sqrt(u_i)) over stratified u_i puts the taps where
// the weight is and the estimate is the plain MEAN of the taps.
//
// The shape of that density is worth stating, because BOTH ways of picking one
// representative direction are wrong. The weight per unit zenith angle is
// cos(theta)sin(theta): ZERO at the zenith (vanishing solid angle), ZERO at the horizon
// (vanishing cosine), peaking at 45 degrees, with mean exactly 45 degrees. A zenith tap
// understates a sky that brightens toward the horizon; a horizon tap overstates it by
// weighting the directions the cosine is busy suppressing.
//
// Azimuth turns by the golden angle per tap rather than sitting on a ring, so a low-sun
// sky -- bright and warm toward the sun, dim and blue away from it -- is sampled all the
// way round instead of being read off one side.
vec3 GE_SkyDiffuseMeanRadiance(sampler2D skyViewLut, float lutHeight)
{
    const float kGoldenAngleRadians = 2.39996323;
    float vMin = 0.5 / lutHeight;
    float vMax = 1.0 - vMin;

    vec3 sum = vec3(0.0);
    for (int i = 0; i < GE_SKY_AMBIENT_SAMPLE_COUNT; ++i)
    {
        float u    = (float(i) + 0.5) / float(GE_SKY_AMBIENT_SAMPLE_COUNT);
        float sinT = sqrt(u);                   // sin(theta) = sqrt(u) inverts the CDF
        float cosT = sqrt(max(1.0 - u, 0.0));
        float phi  = float(i) * kGoldenAngleRadians;

        // Planet-local: x east, y up, z north -- the frame GE_SkyDirToViewLutUv encodes.
        vec3 dir = vec3(sinT * cos(phi), cosT, sinT * sin(phi));
        vec2 uv  = GE_SkyDirToViewLutUv(dir);
        uv.y     = clamp(uv.y, vMin, vMax);
        // Compat needs an explicit lod: this mean is sampled from a per-fragment
        // PlanetGround branch, and WGSL forbids implicit lod in non-uniform flow.
        sum += GE_TAP_LOD0(skyViewLut, uv).rgb;
    }
    return sum / float(GE_SKY_AMBIENT_SAMPLE_COUNT);
}

// Radiance leaving the Lambertian planet floor: sunlight attenuated by the atmosphere on
// the way down, plus the skylight the floor actually receives, both reflected by the
// ground albedo.
//
// Both terms are irradiance through the same p/PI Lambertian BRDF:
//   direct    E_sun = (n.l) * transmittance * sunColor * sunIntensity  ->  p/PI * E_sun
//   skylight  E_sky = PI * Lbar (definition above)                     ->  p * Lbar
// which is why the skylight term carries neither a 1/PI nor a tuning fraction: for a
// uniform sky of radiance L a Lambertian ground of albedo p leaves exactly p*L, so any
// constant in front of Lbar is an energy error rather than a dial.
//
// Energy-bounded by construction -- the albedo multiplies an irradiance the surface
// actually receives, so the result cannot exceed albedo x incident.
//
// At night both terms go to zero -- the sun is below the horizon so the N.L clamp kills
// the direct term, and the sky-view LUT the skylight integrates is near-black -- which
// would leave the floor black. That is wrong here for a specific reason, not a taste one:
// the LUT is not the sky this engine renders at night. GE_ApplySkyGroundAndNight
// composites an authored night gradient ON TOP of the LUT, so a floor derived from the
// LUT alone reflects a sky nobody sees, which is exactly the capture/visible divergence
// this whole path exists to remove. So the floor crosses to the authored night ground
// colour on the same nightAmount the sky gradient uses, mirroring the blend the stylized
// path already does at the top of this file. groundNightColor is a radiance in its own
// right (the stylized path writes it straight into skyColor), so like there it is NOT
// scaled by groundBrightness.
vec3 GE_GroundBounceRadiance(vec3 groundAlbedo,
                             float groundBrightness,
                             vec3 surfaceUp,
                             vec3 sunDir,
                             vec3 transToSun,
                             vec3 sunColor,
                             float sunIntensity,
                             vec3 skyDiffuseMeanRadiance,
                             vec3 groundNightColor,
                             float nightAmount)
{
    float NdotL   = max(dot(surfaceUp, sunDir), 0.0);
    vec3  direct  = groundAlbedo * (1.0 / GE_SKY_PI) * NdotL * transToSun * sunColor * sunIntensity;
    vec3  ambient = groundAlbedo * skyDiffuseMeanRadiance;
    vec3  lit     = (direct + ambient) * max(groundBrightness, 0.0);
    return mix(lit, groundNightColor, clamp(nightAmount, 0.0, 1.0));
}

// Composites the lit planet floor below the horizon over `skyColor`, hazed into the
// horizon backdrop by aerial perspective.
//
// ONE implementation of the ground-bounce MATH, shared by the VISIBLE sky
// (sky_render.frag) and the IBL capture (sky_capture_cube.frag), so the two cannot drift
// apart in what a lit floor is worth. WHETHER a call site composites a floor is the call
// site's decision and the two deliberately differ: the capture bakes the bounce for every
// physical below-horizon mode, while the visible sky paints a floor only in PlanetGround
// and otherwise folds the horizon haze down. No scene in the repo pins PlanetGround, so
// in practice the shipped configuration is exactly that split -- baked bounce, folded
// visible backdrop -- and it is intended, not a parity bug.
//
// Returns `skyColor` untouched when the ray misses the planet.
vec3 GE_CompositeGroundFloor(vec3 skyColor,
                             sampler2D transLUT,
                             vec3 skyDiffuseMeanRadiance,
                             vec3 camPos,
                             vec3 dirWS,
                             vec3 planetCenter,
                             float planetRadius,
                             float camR,
                             float viewZenithCos,
                             vec3 groundAlbedo,
                             float groundBrightness,
                             vec3 sunDir,
                             vec3 sunColor,
                             float sunIntensity,
                             vec3 betaRayleigh,
                             float rayleighScaleHeight,
                             float groundHazeStrength,
                             vec3 groundNightColor,
                             float nightAmount)
{
    vec3  upAtHit;
    float distToFloor;

    // On the sphere the ray-sphere solve is DEGENERATE and must not be used. For a camera
    // at camR == planetRadius, every below-horizon direction gives
    //   tca = R*sin(theta),  thc = sqrt(R^2 - R^2*cos^2(theta)) = R*sin(theta)
    //   t0  = tca - thc = 0  exactly
    // so a `t0 > 0` guard rejects the whole lower hemisphere and float32 ULP noise (~0.5
    // at 6.4e6) is left deciding which directions composite a floor at all. The IBL
    // capture renders from the world origin, which is exactly on the sphere, so this is
    // its normal case and not an edge case. Handle it analytically: on the surface every
    // below-horizon ray meets the ground at distance ~0, the surface normal is the
    // camera's own up, and there is no haze column to cross.
    // The tolerance is RELATIVE to the planet radius, because what it has to survive is
    // float32 resolution at that radius, not a distance in metres. camR reaches this
    // function through a length(), and SPIR-V permits 3 ULP in sqrt: at R = 6.36e6 one
    // ULP is ~0.5 m, so a fixed 1 m band is ~2 ULP and a conformant driver may land camR
    // outside it while the camera is exactly on the surface -- silently restoring the
    // degenerate intersection this branch exists to avoid. 8e-7 * R is ~5.1 m at Earth
    // scale, a bit over 10 ULP.
    //
    // Cost of the wider band, stated so it is not discovered later: inside it the floor
    // composites with distToFloor = 0, i.e. no aerial perspective. A ray straight down
    // from 5 m travels 5 m and loses nothing; the worst case is a grazing ray, which at
    // that altitude reaches the ground ~8 km away, where the measured extinction
    // (~1.35e-5 /m) would have mixed in ~10% haze. The 1 m band already made the same
    // trade at ~5%, and horizonBlend fades exactly that grazing band in.
    const float kGroundFloorSurfaceToleranceRelative = 8e-7;
    float surfaceTolerance = planetRadius * kGroundFloorSurfaceToleranceRelative;

    // At or BELOW the surface takes the analytic branch deliberately: underground there
    // is no sky below the horizon to show, and the capture -- which is the reason this
    // path exists -- renders from a camera the fill code places exactly on the sphere.
    // The one degenerate input left is a camera at the planet centre, where the up
    // direction is undefined; pick world up rather than emit normalize(0).
    if (camR - planetRadius <= surfaceTolerance)
    {
        vec3  toCam    = camPos - planetCenter;
        float toCamLen = length(toCam);
        upAtHit     = toCamLen > 0.0 ? toCam / toCamLen : vec3(0.0, 1.0, 0.0);
        distToFloor = 0.0;
    }
    else
    {
        float tg0, tg1;
        if (!GE_RaySphereIntersect(camPos, dirWS, planetCenter, planetRadius, tg0, tg1) || tg0 <= 0.0)
            return skyColor;
        upAtHit     = normalize((camPos + dirWS * tg0) - planetCenter);
        distToFloor = tg0;
    }

    // Sun shadowing from the transmittance LUT at the surface row (the hit point is on
    // the sphere, so the normalized height is 0).
    float uMu        = 0.5 + 0.5 * dot(upAtHit, sunDir);
    // Compat needs an explicit lod here: the hit branch is per-fragment, and
    // WGSL forbids implicit lod in non-uniform control flow.
    vec3  transToSun = GE_TAP_LOD0(transLUT, vec2(uMu, 0.0)).rgb;

    vec3 floorColor = GE_GroundBounceRadiance(groundAlbedo, groundBrightness, upAtHit, sunDir,
                                              transToSun, sunColor, sunIntensity, skyDiffuseMeanRadiance,
                                              groundNightColor, nightAmount);

    // Grey view-ray extinction derived from the configured atmosphere at the camera
    // altitude, so distance haze scales with the planet rather than a fixed constant.
    // At distToFloor == 0 this is exp(0) == 1 -- the floor outright -- so the analytic
    // branch needs no separate compositing path.
    float camDensity       = exp(-(camR - planetRadius) / rayleighScaleHeight);
    float aerialExtinction = dot(betaRayleigh, vec3(0.299, 0.587, 0.114)) *
                             camDensity * max(groundHazeStrength, 0.0);
    vec3  lit = GE_AerialPerspectiveComposite(skyColor, floorColor, distToFloor, aerialExtinction);

    // Fade the floor in across the geometric horizon so it never pops as a hard line,
    // independent of camera altitude.
    float horizonBlend = smoothstep(-0.004, 0.0, -viewZenithCos);
    return mix(skyColor, lit, horizonBlend);
}

#endif
