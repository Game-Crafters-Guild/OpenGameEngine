#ifndef GE_SUN_DISC_GLSL
#define GE_SUN_DISC_GLSL

// The visible sun disc: the drawn profile and the radiance that makes it carry the sun's
// energy. Profile and normalisation live together because they are one quantity — the peak
// radiance is defined as "whatever makes THIS profile integrate to the sun's irradiance", so
// a change to one that misses the other is an energy error. Shared with the probe that
// integrates the profile numerically (sun_disc_probe.comp), so the disc a camera sees and
// the disc the test measures cannot drift apart.
//
// All radiances are on the engine's unitless scene-linear scale (nits / 203, see
// LightPhotometry.h). Irradiance in means radiance out; the caller supplies the sun's
// above-atmosphere irradiance and multiplies the result by the view ray's transmittance.

#ifndef GE_SKY_PI
#define GE_SKY_PI 3.14159265359
#endif

// Half-width of the limb, as a fraction of the drawn angular radius: the profile is flat at
// 1.0 inside R (1 - f), passes through 0.5 at exactly R, and is 0 outside R (1 + f). The disc
// sits one to two orders over the tonemapper's white point, so only the outer part of the
// smoothstep is visible on screen: the tonemapper compresses this limb to a visible edge of
// 0.35 px at noon and 0.76 px at a 17:48 sun, at 900 px vertical over a 60 degree FOV. Drawn
// alone that edge crawls under sub-pixel camera motion — up to ~216 codes per quarter-pixel
// step with the glare pass off — and the SunGlare render-graph node is what damps the crawl,
// to 8 codes at worst and 3 at the 95th percentile. A limb far wider than a pixel would
// shrink the disc's apparent size below the angle it subtends.
//
// 0.25 is one pixel of limb where the disc is smallest. At 2 tan(FOV/2) / H radians per pixel
// at the screen centre, the sun's R = 0.004651 rad is 3.6 px of radius at 900 px vertical over
// a 60 degree vertical FOV, so the limb half-width there is 0.91 px.
//
// A fraction of the radius rather than a pixel footprint, so the disc reads the same at every
// resolution and FOV instead of hardening as the window grows, and so the profile stays a
// function of (theta, R) alone — which is what lets the solid angle below be a closed form and
// lets the compute probe measure the shipped function. The limb in pixels is always a quarter
// of the disc's own pixel radius: 0.91 px at 900p/60, 1.1 at 1080p/60, 2.2 at 2160p/60, 2.3 at
// 1080p/30, 0.63 at 1080p/90. It falls under half a pixel only once the whole disc is under
// 2 px of radius (540p at 90 degrees leaves 1.3 px), where nothing about the sun is resolved.
#define GE_SUN_DISC_LIMB_FRACTION 0.25

// Drawn profile, peak 1.0 at the sun's centre. theta is the angle from that centre, radians.
//
// The radius is floored for the same reason GE_SunDiscDrawnSolidAngle floors its own, and with
// the extra edge that a zero radius collapses the smoothstep's two edges onto each other,
// which is a division by zero inside it rather than a step.
float GE_SunDiscProfile(float theta, float drawnAngularRadius)
{
    float r = max(drawnAngularRadius, 1e-6);
    float limb = r * GE_SUN_DISC_LIMB_FRACTION;
    return 1.0 - smoothstep(r - limb, r + limb, theta);
}

// Solid angle the drawn profile covers, steradians: the integral of GE_SunDiscProfile over
// the sphere. Exact in the small-angle limit the sun lives in, where dOmega = 2 pi theta
// dtheta. With a = R * GE_SUN_DISC_LIMB_FRACTION, splitting at the flat top's edge:
//
//   Omega = 2 pi [ integral(theta dtheta, 0..R-a) + integral((1 - smoothstep) theta dtheta) ]
//         = 2 pi [ (R-a)^2 / 2 + a R - 0.4 a^2 ]
//         = pi (R^2 + 0.2 a^2)
//
// the limb term following from integral(g du, 0..1) = 1/2 and integral(u g du, 0..1) = 3/20
// for g(u) = 1 - u^2 (3 - 2u), the smoothstep's complement across the limb.
//
// The sphere's true measure is 2 pi sin(theta) dtheta, and using theta instead costs a
// relative R^2 / 12 = 1.8e-6 at the sun's 0.27 degree angular radius — some 500x under the
// 1e-3 the energy tests assert to, so the small-angle form is the exact one here.
//
// The radius is floored for the same reason GE_SunDiscProfile floors its own: a hidden sun
// uploads sunAngularRadius = 0 (SkyRenderNode: showSunDisk ? radius : 0), and an unfloored
// zero here divides the sun's irradiance by zero. That is +Inf, or NaN when the irradiance is
// also zero, and either one multiplied by the disc profile's zero is a NaN in SceneColor.
float GE_SunDiscDrawnSolidAngle(float drawnAngularRadius)
{
    float r = max(drawnAngularRadius, 1e-6);
    float limb = r * GE_SUN_DISC_LIMB_FRACTION;
    return GE_SKY_PI * (r * r + 0.2 * limb * limb);
}

// SceneColor is RGBA16F. Anything above 65504 stores as +Inf, and an Inf reaches NaN
// downstream: bloom's Karis prefilter weights a tap by 1/(1 + brightness), so Inf * 0 = NaN
// (bloom_threshold.frag), and TAA's neighbourhood clamp does the same. The disc's physical
// radiance is ~9.5e6 on this scale, over two orders above the format, so what is stored is
// clamped across the whole flat top.
//
// This is the STORAGE limit, not a look dial. Everything from here up is already
// indistinguishable on screen: bloom bounds what a single texel contributes to the blur
// chain (kBloomClampMax, bloom_threshold.frag) and the tonemapper maps this and the
// unclamped value to the same white. The clamp applies to the scalar peak, so the sun's
// colour and the view ray's transmittance still set the channel ratios and sunset reddening
// survives it.
//
// Half the format maximum: the remaining stop is headroom for the fog in-scatter and the TAA
// neighbourhood blend that composite over the disc later in the frame.
#define GE_SUN_DISC_MAX_STORED_RADIANCE 32752.0

// Peak radiance of the drawn disc: the sun's irradiance divided by the solid angle it is
// drawn over. The profile then integrates back to exactly that irradiance, so the disc
// carries the sun's energy at whatever radius it is drawn — draw it larger and it gets
// correspondingly dimmer.
//
// exposureScale is the multiplier the caller applies to the whole sky before writing
// SceneColor (exp2 of the sky exposure trim). Folding it in here bounds the value that is
// actually stored, rather than a value some later multiply can push back over the format.
float GE_SunDiscPeakRadiance(float irradiance, float drawnAngularRadius, float exposureScale)
{
    float physical = irradiance / GE_SunDiscDrawnSolidAngle(drawnAngularRadius);
    return min(physical, GE_SUN_DISC_MAX_STORED_RADIANCE / max(exposureScale, 1e-6));
}

#endif // GE_SUN_DISC_GLSL
