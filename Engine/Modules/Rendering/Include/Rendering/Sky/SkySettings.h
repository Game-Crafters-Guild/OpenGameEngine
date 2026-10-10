#pragma once

#include <cstdint>
#include "Rendering/Core/Handle.h"

namespace GameEngine {
namespace Rendering {

// How the sky-view LUT (and therefore the on-screen sky + IBL bake) handles the
// region below the horizon. The mode is baked INTO the LUT — flipping it re-bakes.
//   ContinueHorizon - fold below-horizon texels up to the grazing horizon so the
//                     atmospheric color continues smoothly downward (no ground).
//   PlanetGround    - march the ray to the planet and shade a lit Lambertian floor
//                     that tracks the sun.
//   StylizedGround  - leave the raw march (~black) below; the on-screen composite
//                     paints the stylized ground over it.
enum class SkyBelowHorizonMode : uint32_t
{
    ContinueHorizon = 0,
    PlanetGround    = 1,
    StylizedGround  = 2,
};

// The sun's apparent angular RADIUS from Earth: half its ~0.533 degree apparent diameter, radians.
inline constexpr float kSunAngularRadiusRad = 0.004651f;

// High-level sky configuration for a frame.
// For now we keep this tightly focused on a primary sun and exposure; we
// will extend it with more detailed atmospheric parameters as we move to
// a full Hillaire-style model.
struct SkySettings
{
    // Time-of-day in hours [0, 24); optional convenience for day/night cycles.
    float timeOfDayHours = 12.0f;
    // Exposure in photographic "stops"; scene multiplier is 2^exposureEV.
    float exposureEV = 0.0f;
    float padding0 = 0.0f;

    // Continuous sun direction used for atmosphere scattering LUT generation.
    float scatteringSunDir[3] = { 0.0f, 1.0f, 0.0f };
    float paddingScatteringSunDir = 0.0f;

    // Primary blended light direction used by disk/lighting presentation.
    float primarySunDir[3] = { 0.0f, 1.0f, 0.0f };
    float padding1 = 0.0f;

    /// Colour of the primary celestial source ABOVE the atmosphere: the sun's white scaled by the
    /// authored stylistic tint (SkyEnvironment::SunTintKeys, white by default) crossfading to the
    /// moon's own untinted colour at night. The sky passes apply the atmosphere's own extinction
    /// themselves — the sky-view LUT multiplies the transmittance into its in-scatter source term
    /// per march step, the sun disc multiplies it along the view ray — so this value stays
    /// un-attenuated or sunset reddening is applied twice.
    float primarySunColor[3] = { 1.0f, 1.0f, 1.0f };
    /// The same source AFTER that extinction: the colour a GROUND-level surface is lit by, so warm
    /// and dim at low elevation, unchanged overhead (the light's authored illuminance is already a
    /// ground value, so the extinction is taken relative to the zenith path), near-black below the
    /// horizon. Drives the linked directional light and the fog sun; never a sky-shader input.
    float primarySunGroundColor[3] = { 1.0f, 1.0f, 1.0f };
    float primarySunIntensity = 10.0f;
    bool showSunDisk = true;
    /// Stylistic multiplier on the DRAWN sun disk radius (CPU clamps from
    /// SkyEnvironment::SunSize; 1 = kSunAngularRadiusRad, the physical radius). Scales the disk
    /// only — the sun's irradiance, the linked directional light and the glare source width
    /// are all independent of it.
    float sunSize = 1.0f;

    // Moon disk presentation. Direction is the world-space vector from the
    // viewer toward the moon; the disk is rendered when it sits above the
    // horizon. Intensity is a multiplier in the same scale as primarySunIntensity.
    float moonDirWS[3] = { 0.0f, -1.0f, 0.0f };
    float moonIntensity = 0.5f;

    /// Apparent moon disk radius in radians (CPU sets from SkyEnvironment.MoonSize).
    float moonAngularRadius = 0.026f;
    bool showMoonDisk = true;
    float sunSize2D = 1.0f;
    float moonSize2D = 0.334f;
    float skyPan2D = 0.0f;
    float moonExposureEV = 1.29f;

    float moonArcPosition = 0.5f;
    float moonPhase01 = 0.5f;
    bool fallingStarsEnabled = true;
    float fallingStarAmount = 0.35f;
    float fallingStarFrequency = 0.35f;
    float fallingStarSpeed = 1.0f;
    float fallingStarLength = 1.0f;
    float fallingStarThickness = 1.0f;
    float fallingStarDotSize = 1.0f;
    float fallingStarDotSize2D = 1.0f;

    float nightSkyBlend = 0.0f;
    float skyTimeSeconds = 0.0f;
    float starDensity = 0.5f;
    float starBrightness = 0.6f;

    float starSize = 1.0f;
    float starDiamondShape = 0.5f;
    float starCoreSize = 0.15f;
    float starGlowFalloff = 10.0f;

    float twinkleSpeed = 0.5f;
    float twinkleIntensity = 0.3f;
    float hdriIntensity = 1.0f;
    float skyboxRotationRadians = 0.0f;
    /// Linear multiplier on the image-based-lighting ambient + reflections
    /// (SkyEnvironment.IblIntensity); pushed to ImageBasedLightingFeature.
    float iblIntensity = 1.0f;
    /// IBL-only lower-hemisphere darkening (SkyEnvironment.IblLowerHemisphereDarkness):
    /// baked into the env capture, leaves the visible sky unchanged. 0 = off, 1 = black.
    float iblLowerHemisphereDarkness = 1.0f;

    /// Ambient gradient tint (scene-linear RGB) multiplied into the diffuse IBL irradiance
    /// only. Blended by world-normal.y (up=Sky, horizon=Equator, down=Ground). Mirrors
    /// SkyEnvironment.AmbientTint*. White = identity. Consumed at IBL sampling time via the
    /// EnvData UBO (NOT baked into the cubes), so editing it never triggers a re-bake.
    float ambientTintSky[3]     = {1.0f, 1.0f, 1.0f};
    float ambientTintEquator[3] = {1.0f, 1.0f, 1.0f};
    float ambientTintGround[3]  = {1.0f, 1.0f, 1.0f};

    /// Gradient sky (SkyEnvironment.Mode == Gradient). 0 = Physical atmosphere (default),
    /// 1 = stylized 3-color gradient. The gradient colors below are scene-linear radiance
    /// (the system has already folded in GradientSkyIntensity / the 203-nit reference white),
    /// blended by view-direction.y (up=Top, horizon=Horizon, down=Bottom) for BOTH the visible
    /// dome and the IBL capture, so the ambient/reflections derive from the same gradient.
    uint32_t skyMode = 0;
    float gradientSkyTopColor[3]    = {0.0f, 0.0f, 0.0f};
    float gradientSkyHorizonColor[3] = {0.0f, 0.0f, 0.0f};
    float gradientSkyBottomColor[3]  = {0.0f, 0.0f, 0.0f};

    /// Matches `SkyEnvironment.GroundAlbedo`; used for the procedural sky below-horizon blend (`sky_render.frag`).
    float groundAlbedo[3] = {0.3f, 0.3f, 0.3f};
    /// Scene-linear below-horizon ground tint used at night.
    float groundNightColor[3] = {0.025f, 0.025f, 0.035f};
    /// Multiplier on `groundAlbedo` for the below-horizon tint (legacy default 0.8).
    float groundBrightness = 0.8f;
    /// Selects the below-horizon LUT bake mode (continue horizon / lit planet ground / stylized ground).
    /// Mirrors `SkyEnvironment.BelowHorizonMode`; folded into the sky-view LUT + IBL digests.
    uint32_t belowHorizonMode = 0;
    /// How quickly the sky lerps to ground when looking below the horizon; higher = less dark LUT near the rim.
    float belowHorizonBlendSharpness = 2.0f;
    /// Strength of the dark sky-view LUT band below the horizon. 0 = remove it, 1 = current physically-derived darkness.
    float belowHorizonDarkness = 1.0f;
    /// Color used for the dark sky-view LUT band below the horizon.
    float belowHorizonDarkColor[3] = {0.02f, 0.02f, 0.025f};
    /// PlanetGround floor haze: multiplier on the physical aerial-perspective extinction.
    /// 1 = crisp ground to the horizon; higher softens the ground/horizon transition.
    float groundHazeStrength = 1.0f;
    /// Tint mixed over the sky LUT in a band just under the horizon (replaces the default dark fringe).
    float groundHorizonColor[3] = {0.04f, 0.05f, 0.07f};
    /// Rim tint at night; shaders mix with `groundHorizonColor` using `nightSkyBlend`. Default matches sRGB `#020305` (scene-linear).
    float groundHorizonNightColor[3] = {0.000607054f, 0.000910581f, 0.001517635f};
    /// Procedural night sky gradient at the horizon (`sky_render` / Tier 2).
    float nightSkyHorizonColor[3] = {0.12f, 0.035f, 0.19f};
    /// Day rim half-width in cosine space (larger = wider rim; typical 0.02–0.06).
    float groundHorizonCosWidth = 0.035f;
    /// Night rim half-width; shaders mix with `groundHorizonCosWidth` using `nightSkyBlend`.
    float groundHorizonNightCosWidth = 0.035f;

    TextureHandle environmentTexture{};
};

// Derived per-frame environment state used by the renderer.
// This is separated from SkySettings so we can cache computed values
// (sun direction from time/latitude, exposure multipliers, etc.).
struct SkyEnvironmentState
{
    float primarySunDir[3] = { 0.0f, 1.0f, 0.0f };
    float padding0 = 0.0f;

    float primarySunColor[3] = { 1.0f, 1.0f, 1.0f };
    float primarySunIntensity = 10.0f;

    float exposureMultiplier = 1.0f;
    float padding1[3] = { 0.0f, 0.0f, 0.0f };
};

} // namespace Rendering
} // namespace GameEngine
