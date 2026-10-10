#pragma once

#include "Types/Types.h"

#include "ECS/Entity.h" // ECS::EntityHandle
#include "Rendering/Sky/SkySettings.h" // Rendering::SkyBelowHorizonMode
#include "Rendering/Sky/SolarPath.h" // Rendering::kMarchEquinoxDay
#include "Rendering/Sky/SkySystemConfig.h" // Rendering::SkySystemConfig
#include "Components/Rendering/LightPhotometry.h" // kClearNoonSunIlluminanceLux
#include "Components/Rendering/SkySunDrive.h" // SkySunDrive::DefaultSunIlluminanceCurve
#include "Mathematics/Curve.h" // Math::Curve, Math::CurveKey
#include "Types/ColorUtils.h" // ColorUtils::LinearRec709Luminance

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace Components {

/// Three linear RGB values at midnight (0h), dawn (6h), midday (12h), and sunset (18h).
// @ge-no-add  data helper, not user-addable in the editor
struct SkyVec3DayKeys {
    float32 Midnight[3];
    float32 Dawn[3];
    float32 Midday[3];
    float32 Sunset[3];
};

// @ge-no-add  data helper, not user-addable in the editor
struct SkyScalarDayKeys {
    float32 Midnight;
    float32 Dawn;
    float32 Midday;
    float32 Sunset;
};

enum class SkyScalarCurveShapeMode : uint8
{
    KeyCurve = 0,
    CubicBezier = 1,
};

// Dedicated scalar Bezier for sky day curves. X handles are normalized day time [0,1],
// Y handles are authored in the scalar's actual value units.
struct SkyScalarCubicBezier
{
    float32 Control1X = 0.25f;
    float32 Control1Y = 0.10f;
    float32 Control2X = 0.25f;
    float32 Control2Y = 1.00f;
    float32 AnchorStartY = 0.0f;
    float32 AnchorEndY = 1.0f;
};

inline void SkyVec3DayKeysSetUniform(SkyVec3DayKeys& k, float32 r, float32 g, float32 b)
{
    float32* slots[4] = {k.Midnight, k.Dawn, k.Midday, k.Sunset};
    for (float32* slot : slots)
    {
        slot[0] = r;
        slot[1] = g;
        slot[2] = b;
    }
}

/// Build a sky scalar day-curve from four day-phase values (keys placed in hours).
inline Math::Curve MakeSkyScalarCurve(float32 midnight, float32 dawn, float32 midday, float32 sunset)
{
    Math::Curve c;
    c.TryInsert(Math::CurveKey{0.0f, midnight});
    c.TryInsert(Math::CurveKey{6.0f, dawn});
    c.TryInsert(Math::CurveKey{12.0f, midday});
    c.TryInsert(Math::CurveKey{18.0f, sunset});
    return c;
}

/// Evaluate a sky day-curve (keys in hours) at the given time of day, wrapping the last
/// key back to the first across the 24 h boundary so the day cycle stays continuous.
inline float32 EvaluateSkyDayCurve(const Math::Curve& curve, float32 todHours)
{
    constexpr float32 kDayHours = 24.0f;
    return Math::EvaluateCurveKeysWrapped(curve.Keys, curve.KeyCount, todHours, kDayHours);
}

inline float32 EvaluateSkyScalarCubicBezier(const SkyScalarCubicBezier& bezier, float32 todHours)
{
    constexpr float32 kDayHours = 24.0f;
    float32 x = std::fmod(todHours, kDayHours);
    if (x < 0.0f)
        x += kDayHours;
    x /= kDayHours;

    const float32 c1x = std::clamp(bezier.Control1X, 0.0f, 1.0f);
    const float32 c2x = std::clamp(bezier.Control2X, 0.0f, 1.0f);
    auto sampleX = [c1x, c2x](float32 u) {
        const float32 inv = 1.0f - u;
        return 3.0f * inv * inv * u * c1x +
               3.0f * inv * u * u * c2x +
               u * u * u;
    };
    auto sampleY = [&bezier](float32 u) {
        const float32 inv = 1.0f - u;
        const float32 inv3 = inv * inv * inv;
        const float32 u3 = u * u * u;
        return inv3 * bezier.AnchorStartY +
               3.0f * inv * inv * u * bezier.Control1Y +
               3.0f * inv * u * u * bezier.Control2Y +
               u3 * bezier.AnchorEndY;
    };

    float32 lo = 0.0f;
    float32 hi = 1.0f;
    float32 u = x;
    for (int i = 0; i < 16; ++i)
    {
        u = (lo + hi) * 0.5f;
        if (sampleX(u) < x)
            lo = u;
        else
            hi = u;
    }
    return sampleY(u);
}

inline float32 EvaluateSkyScalarDayCurve(const Math::Curve& keys,
                                         SkyScalarCurveShapeMode mode,
                                         const SkyScalarCubicBezier& bezier,
                                         float32 todHours)
{
    if (mode == SkyScalarCurveShapeMode::CubicBezier)
        return EvaluateSkyScalarCubicBezier(bezier, todHours);
    return EvaluateSkyDayCurve(keys, todHours);
}

/// Authoring/runtime clamp for SkyEnvironment::SkyExposureTrim, in EV (stops). Shared by the system
/// (clamps before the GPU) and the inspector (bounds the drag widget) so the limit lives once.
inline constexpr float32 kSkyExposureTrimLimitEv = 2.0f;

/// How the sky background (and its derived IBL) is produced.
///   Physical - the Hillaire atmosphere model (default; unchanged behavior).
///   Gradient - a stylized 3-color vertical gradient (top/horizon/bottom), blended by
///              view-direction.y. The gradient IS the sky AND the ambient source: the IBL
///              capture bakes the same gradient, so the irradiance + reflections follow it.
enum class SkyMode : uint8
{
    Physical = 0,
    Gradient = 1,
};

/// Where the sun's path comes from. Earth: a place on Earth on a day of the year (Latitude,
/// DayOfYear, NorthHeading). Custom: an authored axis and noon height, for a world that is not
/// Earth. Each keeps its own fields while the other is in use.
enum class SkySunPathKind : uint8
{
    Earth = 0,
    Custom = 1,
};

/// Where the illuminance of the sun light the sky drives comes from. Light: the light's own Intensity,
/// the author's clear-noon value, which the sky dims through the light's colour. Curve: the sky's
/// SunIlluminanceCurve in lux over the day, which the sky writes into the light's Intensity.
enum class SkySunIlluminanceSource : uint8
{
    Light = 0,
    Curve = 1,
};

/// The illuminance of a full moon high in the sky on a surface facing it, in lux, as the sky ships it:
/// the stylized moonlight the linked light has always delivered at full night from a 100 000 lx sun
/// (the light's day value times SkySystemConfig::moonLightIlluminanceScale times the luminance of
/// SkySystemConfig::moonColor), about 5.158 lx. A real full moon gives about 0.25 lx.
inline constexpr float32 kDefaultMoonlightIlluminanceLux =
    kClearNoonSunIlluminanceLux * Rendering::SkySystemConfig{}.moonLightIlluminanceScale *
    ColorUtils::LinearRec709Luminance(Rendering::SkySystemConfig{}.moonColor);
/// The brightest moonlight an author may set, in lux: a near-fixed exposure across day and night.
inline constexpr float32 kMoonlightIlluminanceMaxLux = 10000.0f;

/// Default gradient-sky luminance in nits. Shared by the component default and the inspector's
/// drag-reset so the two never drift. Colors x this / the 203-nit reference white = scene-linear.
inline constexpr float32 kDefaultGradientSkyIntensityNits = 4000.0f;

/// Authoring/runtime clamp for SkyEnvironment::SunSize. Shared by the system (clamps before the
/// GPU), the scene parser and the inspector so the limit lives once. The floor is not lower
/// because the sun already subtends only 4.35 px of radius at 1080p and a 60 degree vertical
/// FOV; much under half that is a sub-pixel disc, which crawls under sub-pixel camera motion
/// however soft sun_disc.glsl draws its edge.
inline constexpr float32 kSunSizeMin = 0.5f;
inline constexpr float32 kSunSizeMax = 8.0f;

struct SkyEnvironment {
    /// Selects the sky type. Physical keeps the full atmosphere model; Gradient renders the
    /// stylized 3-color gradient below and derives ambient/reflections from it.
    SkyMode Mode           = SkyMode::Physical;
    /// Gradient-sky colors (authored linear RGB, 0..1). Blended by view-direction.y:
    /// up -> Top, horizon -> Horizon, down -> Bottom. Only used when Mode == Gradient.
    float32 GradientSkyTopColor[3]    = {0.20f, 0.42f, 0.78f};
    float32 GradientSkyHorizonColor[3] = {0.60f, 0.72f, 0.86f};
    float32 GradientSkyBottomColor[3]  = {0.32f, 0.30f, 0.28f};
    /// Gradient-sky luminance in nits (cd/m^2). Colors x this / the 203-nit reference white gives
    /// scene-linear radiance on the SAME anchor as the physical sky + auto-exposure. Only used when
    /// Mode == Gradient.
    float32 GradientSkyIntensity = kDefaultGradientSkyIntensityNits;
    /// Local solar time in hours: 12 is solar noon wherever the sky is.
    float32 TimeOfDayHours = 12.0f;
    /// Degrees north (positive) or south of the equator, in [-90, 90]. With DayOfYear it sets the
    /// sun's height at noon and the length of the day. The default, the equator on the March
    /// equinox, puts the noon sun overhead and gives a 12-hour day.
    float32 Latitude = 0.0f;
    /// Day of a non-leap year: 1 = 1 January, 172 = 21 June, 355 = 21 December.
    int32 DayOfYear = Rendering::kMarchEquinoxDay;
    /// Which way north points in the scene: a heading about +Y in degrees, in [0, 360), clockwise
    /// seen from above, where 0 is +Z and 90 is +X. It turns the whole sun path; the sun rises to
    /// the east of north. The sky entity's own rotation plays no part.
    float32 NorthHeading = 0.0f;
    /// Which fields place the sun: Latitude, DayOfYear and NorthHeading, or the Custom* fields.
    SkySunPathKind SunPath = SkySunPathKind::Earth;
    /// The Custom path's axis, the pole the sun circles once a day: its heading about +Y in degrees,
    /// in [0, 360), with the same convention as NorthHeading.
    float32 CustomAxisHeading = 0.0f;
    /// The Custom path's axis: its altitude above the horizon in degrees, in [-90, 90]. At 90 the sun
    /// circles the horizon at one height and never sets.
    float32 CustomAxisAltitude = 0.0f;
    /// The Custom path's noon sun: its height in degrees, measured from the horizon on the side away
    /// from the axis heading and continuing over the top of the sky, so 120 is 60 degrees up on the
    /// axis side; below 0 or above 180 the noon sun is under the horizon (Rendering::CustomPathAngles,
    /// Rendering::ReachableNoonHeights for what an axis can reach). With a level axis at heading 0
    /// the default 90 is the default Earth path.
    float32 CustomNoonHeight = Rendering::kOverheadNoonHeightDegrees;
    /// When true, `TimeOfDayHours` advances every frame (`TimeOfDayCycleSeconds` wall time per full 24 h).
    bool    AnimateTimeOfDay     = false;
    float32 TimeOfDayCycleSeconds = 120.0f;
    /// Flat sky-vs-object/ambient exposure trim, in EV (stops), applied to the visible sky dome and
    /// the baked IBL. This is NOT a day/night brightness driver: once the sky is physically anchored
    /// to the directional sun, the atmosphere model + camera auto-exposure own the day/night arc
    /// (sunset dims/reddens from the optical-depth integral, not from a baked multiplier). Leave at 0
    /// for the physical look; nudge +/- a couple of stops as a deliberate per-scene art override.
    float32 SkyExposureTrim = 0.0f;
    /// Linear multiplier on the image-based-lighting (sky) ambient + reflections.
    /// 1 = physical; lower it to tame an over-bright/over-blue sky fill.
    float32 IblIntensity   = 1.0f;
    /// Dims DOWN-FACING sampling of the baked IBL, without affecting the visible sky.
    /// 0 = off, 1 = fully dark. It attenuates by the sampling DIRECTION -- the receiving
    /// surface's own normal for diffuse (`smoothstep(0, 0.25, -N.y)`), and the reflection
    /// vector for specular prefilter, refraction and clearcoat -- so it never touches a
    /// vertical surface's diffuse, but it does reach a vertical MIRROR that reflects
    /// downward. It is an attenuation at sampling time, not an occlusion inside the
    /// irradiance integral, and it is applied per sample rather than baked into the cube.
    ///
    /// This 0.0 default is specific to the SKY environment source, where the capture now
    /// bakes a real ground bounce below the horizon: the lower hemisphere carries light
    /// reflected off the earth rather than open sky, so darkening it would subtract
    /// exactly the energy that keeps a shadowed surface from reading blue. Raise it for
    /// scenes that want the old sky-fill-suppression behaviour. The same knob name ships
    /// three different defaults across the three environment sources -- SkyEnvironment
    /// 0.0 (here), ReflectionProbe 0.3, Skybox 1.0 -- because each bakes a different
    /// lower hemisphere; they are not copies of one setting.
    float32 IblLowerHemisphereDarkness = 0.0f;

    /// Three-color ambient gradient TINT (scene-linear RGB) multiplied into the diffuse
    /// IBL irradiance ONLY (specular reflections stay physical). Blended by world-normal.y
    /// as a three-way ramp: up -> Sky, horizontal -> Equator, down -> Ground. This
    /// is the faithful bridge for Synty ER-style authored washes (purple/magenta/cyan) — a
    /// non-destructive art tint over the physically baked irradiance, applied at IBL sampling
    /// time (not baked, so editing it needs no re-bake). Default white = identity (no change).
    float32 AmbientTintSky[3]     = {1.0f, 1.0f, 1.0f};
    float32 AmbientTintEquator[3] = {1.0f, 1.0f, 1.0f};
    float32 AmbientTintGround[3]  = {1.0f, 1.0f, 1.0f};
    /// Shared key times (hours in [0, 24)) for the vec3 COLOR day-keys only (GroundAlbedoKeys,
    /// GroundNightColorKeys, ...). The scalar day curves below are arbitrary Math::Curves that carry
    /// their OWN per-key times, so a scalar key's hour is independent of these color-stop times.
    /// Default keyframe hours are 0 / 6 / 12 / 18.
    SkyScalarDayKeys DayKeyTimesHours{0.0f, 6.0f, 12.0f, 18.0f};

    /// STYLISTIC. Scene-linear tint on the celestial source ABOVE the atmosphere, keyed over the
    /// day on `DayKeyTimesHours`. Default white at every key = identity, so the shipped look is the
    /// physical one and this costs nothing until it is authored.
    ///
    /// It changes the light going IN, not the picture coming out: the tinted source is what the
    /// sky-view LUT scatters, what the sun disc emits, what the IBL capture bakes, and what the
    /// ground-level extinction attenuates into the linked light and the fog sun. So an orange
    /// evening key gives an orange sky, an orange key light, orange aerial perspective and orange
    /// ambient — each from one multiply at the source, with the transport left physical. It is not
    /// a grade over the finished sky.
    ///
    /// Every consumer scales proportionally except one, and it is deliberate: the volumetric fog's
    /// AMBIENT wash normalizes the source by luminance and mixes the result in at 35 %
    /// (VolumetricFogRenderer.cpp, ResolveFogAmbient), so it takes the tint's HUE once but not its
    /// magnitude. That wash is a hue blend by design, not a light being multiplied.
    ///
    /// It tints the SUN only. The moon keeps SkySystemConfig::moonColor on every path — its light,
    /// its in-scatter and its disc — because the disc is drawn untinted, and a tinted moonlight
    /// under an untinted moon is an orange dome over a grey disc. So a night key changes nothing
    /// once the moon owns the sky; author the night through the moon's own colour instead.
    SkyVec3DayKeys SunTintKeys{
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
    };

    /// Reflectance of the planet floor (midnight / dawn / midday / sunset). Drives the
    /// ground bounce baked into the IBL (the lower hemisphere a surface receives) and the
    /// visible below-horizon ground in the PlanetGround and StylizedGround modes.
    ///
    /// Every key is the same neutral grey, chosen by measurement: the sky model stands in
    /// for the ground the scene actually has, and in the default scene (a light floor,
    /// noon) 0.59 is the largest neutral albedo whose baked -Y irradiance and -Y
    /// prefilter faces stay at or below those of a whole-scene probe capture on every
    /// channel; blue reaches the probe first, because the floor is lit by the blue sky.
    /// The ground's colour does not change with the hour; its brightness follows the sun
    /// (N.L and transmittance) and the sky's mean radiance.
    SkyVec3DayKeys GroundAlbedoKeys{
        {0.59f, 0.59f, 0.59f},
        {0.59f, 0.59f, 0.59f},
        {0.59f, 0.59f, 0.59f},
        {0.59f, 0.59f, 0.59f},
    };
    SkyVec3DayKeys GroundNightColorKeys{
        {0.03f, 0.032f, 0.045f},
        {0.022f, 0.022f, 0.03f},
        {0.008f, 0.008f, 0.012f},
        {0.02f, 0.018f, 0.028f},
    };
    /// A flat 0.9 at every hour: the bounce already dims with the sun's N.L and transmittance
    /// and with the sky's mean radiance, so a day curve here would dim it twice.
    Math::Curve GroundBrightnessKeys = MakeSkyScalarCurve(0.9f, 0.9f, 0.9f, 0.9f);
    SkyScalarCurveShapeMode GroundBrightnessShapeMode = SkyScalarCurveShapeMode::KeyCurve;
    SkyScalarCubicBezier GroundBrightnessBezier{0.25f, 0.9f, 0.75f, 0.9f, 0.9f, 0.9f};

    /// How the sky is handled below the horizon: continue the atmospheric horizon
    /// (default), a lit planet ground that tracks the sun, or the stylized ground.
    Rendering::SkyBelowHorizonMode BelowHorizonMode = Rendering::SkyBelowHorizonMode::ContinueHorizon;

    Math::Curve BelowHorizonBlendSharpnessKeys = MakeSkyScalarCurve(2.3f, 1.4f, 1.8f, 2.1f);
    SkyScalarCurveShapeMode BelowHorizonBlendSharpnessShapeMode = SkyScalarCurveShapeMode::KeyCurve;
    SkyScalarCubicBezier BelowHorizonBlendSharpnessBezier{0.25f, 1.4f, 0.75f, 1.8f, 2.3f, 2.3f};
    Math::Curve BelowHorizonDarknessKeys = MakeSkyScalarCurve(1.0f, 0.8f, 0.35f, 0.75f);
    SkyScalarCurveShapeMode BelowHorizonDarknessShapeMode = SkyScalarCurveShapeMode::KeyCurve;
    SkyScalarCubicBezier BelowHorizonDarknessBezier{0.25f, 0.8f, 0.75f, 0.35f, 1.0f, 1.0f};
    SkyVec3DayKeys BelowHorizonDarkColorKeys{
        {0.012f, 0.014f, 0.022f},
        {0.018f, 0.017f, 0.02f},
        {0.03f, 0.031f, 0.034f},
        {0.024f, 0.018f, 0.02f},
    };
    /// PlanetGround only: how strongly the lit floor hazes into the horizon with
    /// distance. 1 = physical Rayleigh extinction (ground stays crisp to the horizon);
    /// higher softens the ground/horizon transition. No effect in the other modes.
    float32 GroundHazeStrength = 1.0f;

    SkyVec3DayKeys GroundHorizonColorKeys{
        {0.01f, 0.015f, 0.03f},
        {0.08f, 0.065f, 0.055f},
        {0.07843137f, 0.12156863f, 0.21960784f},
        {0.13f, 0.08f, 0.09f},
    };
    /// Night rim tint keys (scene-linear); default matches sRGB `#020305`.
    SkyVec3DayKeys GroundHorizonNightColorKeys{
        {0.000607054f, 0.000910581f, 0.001517635f},
        {0.003f, 0.0025f, 0.0035f},
        {0.000607054f, 0.000910581f, 0.001517635f},
        {0.002f, 0.0016f, 0.0026f},
    };
    /// Night-sky horizon glow, scene-linear on the 203-nit anchor. Values sit
    /// around 0.05-0.4 nits (a real light-polluted night sky): bright enough to
    /// silhouette the horizon once auto-exposure adapts, dim enough that
    /// midnight meters BELOW the AutoExposureMinEv floor and reads as night
    /// instead of a pastel sky.
    SkyVec3DayKeys NightSkyHorizonColorKeys{
        {0.0012f, 0.00035f, 0.0019f},
        {0.0009f, 0.0005f, 0.0011f},
        {0.0002f, 0.0002f, 0.0003f},
        {0.0018f, 0.0007f, 0.0016f},
    };
    Math::Curve GroundHorizonCosWidthKeys = MakeSkyScalarCurve(0.02f, 0.03f, 0.03f, 0.055f);
    SkyScalarCurveShapeMode GroundHorizonCosWidthShapeMode = SkyScalarCurveShapeMode::KeyCurve;
    SkyScalarCubicBezier GroundHorizonCosWidthBezier{0.25f, 0.03f, 0.75f, 0.03f, 0.02f, 0.02f};
    Math::Curve GroundHorizonNightCosWidthKeys = MakeSkyScalarCurve(0.06f, 0.03f, 0.02f, 0.05f);
    SkyScalarCurveShapeMode GroundHorizonNightCosWidthShapeMode = SkyScalarCurveShapeMode::KeyCurve;
    SkyScalarCubicBezier GroundHorizonNightCosWidthBezier{0.25f, 0.03f, 0.75f, 0.02f, 0.06f, 0.06f};

    /// Toward-sun direction (surface → sun), not shine. Identity light shines
    /// +Z, so a zenith sun orients the linked light to shine (0, -1, 0).
    float32 SunDirOverride[3] = {0.0f, 1.0f, 0.0f};
    bool    AutoSunMoon    = true;
    /// Optional unparented directional-light entity kept in two-way sync with the
    /// sky sun: rotating the light drives the sun as a manual override (off-arc
    /// rotations allowed; the Time of Day slider then shows the nearest on-arc
    /// time), and changing Time of Day / the override rotates the light to match.
    /// Persisted across scene reload by the EntityHandle scene serializer.
    ECS::EntityHandle SunLight{};
    /// When true, the time-of-day primary light source writes back to the linked
    /// directional light in real time: sun by day, moon by night. The sky always owns
    /// the light's direction; DriveSunColor and SunIlluminanceSource say what else it
    /// writes. Every field the sky stops writing, for any reason, is handed back to the
    /// light (SkyEnvironmentSystem).
    bool    TimeOfDayDrivesSunLight = true;
    /// While the drive is on: the sky writes the light's colour, the ground-level one, so a lit
    /// surface warms with the sky it stands under. Under SkySunIlluminanceSource::Light the colour
    /// carries the sky's extinction and the handover to the moon, so it is written whatever this
    /// holds; under a curve it is the hue alone and turning it off leaves the author's colour.
    /// Turning it off hands the colour back white.
    bool    DriveSunColor = true;
    /// Where the driven light's illuminance comes from (SkySunIlluminanceSource).
    SkySunIlluminanceSource SunIlluminanceSource = SkySunIlluminanceSource::Light;
    /// The sun's illuminance on a surface facing it, in lux, over the day: keys in hours [0, 24),
    /// interpolated in the log domain (SkySunDrive::CurveIlluminanceLux). Read while
    /// SunIlluminanceSource is Curve and kept while it is not.
    Math::Curve SunIlluminanceCurve = SkySunDrive::DefaultSunIlluminanceCurve();
    /// The moon's illuminance on a surface facing it, in lux, with a full moon high in the sky, in
    /// [0, kMoonlightIlluminanceMaxLux]. Absolute: dimming the sun leaves the night where it is. It
    /// also sets the night sky's and the moon disc's brightness.
    float32 MoonlightIlluminance = kDefaultMoonlightIlluminanceLux;
    bool    ShowSun        = true;
    bool    ShowMoon       = true;
    /// Stylistic multiplier on the angular radius the sun disc is DRAWN at; 1 = the sun's
    /// physical ~0.53 degree apparent diameter. Look only in the sense that matters for
    /// lighting: the key light comes from the linked directional light and the IBL capture
    /// excludes the disc, so neither moves with this. Bloom does — at the shipped sun illuminance
    /// the disc's peak radiance sits at the RGBA16F storage bound at every size in range, so the
    /// energy the disc puts into SceneColor grows with the drawn area. That bound releases only
    /// at SunSize ≈ 36, or ≈ 18 at the -2 EV exposure-trim limit, so inside the range only a sun
    /// authored under about 5% of the shipped illuminance ever comes off it. Also scales the 2D
    /// backdrop's sun, which SunSize2D then trims, exactly as MoonSize and MoonSize2D compose.
    float32 SunSize        = 1.0f;
    float32 SunSize2D      = 1.0f;
    float32 SkyPan2D       = 0.0f;

    /// The lit shape of the moon's disc: 0=new, 0.25=first quarter, 0.5=full, 0.75=last quarter.
    float32 MoonPhase01 = 0.5f;
    /// Where the moon sits on the sun's daily circle (the sun's declination), as a fraction of a turn
    /// of hour angle from the sun: 0=new moon position, 0.25=first-quarter position, 0.5=full-moon
    /// position, 0.75=last-quarter position.
    float32 MoonArcPosition = 0.5f;
    /// When true and `AnimateTimeOfDay`, advances MoonArcPosition over MoonCycleDays.
    bool AutoMoonArc = true;
    /// Length of one New Moon-to-New Moon lunar cycle, in simulated days (24 h each).
    float32 MoonCycleDays = 29.5f;
    /// Multiplier on the default apparent moon disk radius (1.0 ≈ ~1.5°).
    float32 MoonSize = 2.39f;
    float32 MoonSize2D = 0.334f;
    /// Moon-vs-sky brightness contrast trim, in EV (stops). Boosts the small moon disk relative to
    /// the night sky so it reads against the stars; independent of SkyExposureTrim and of the camera
    /// auto-exposure (the disk is too small to move the meter).
    float32 MoonExposureEV = 4.0f;
    bool FallingStarsEnabled = true;
    float32 FallingStarAmount = 0.35f;
    float32 FallingStarFrequency = 0.35f;
    float32 FallingStarSpeed = 4.0f;
    float32 FallingStarLength = 1.0f;
    float32 FallingStarThickness = 0.485f;
    float32 FallingStarDotSize = 0.1f;
    float32 FallingStarDotSize2D = 0.979f;

    float32 StarDensity       = 0.5f;
    float32 StarBrightness    = 0.6f;
    float32 StarSize          = 1.0f;
    float32 StarDiamondShape  = 0.5f;
    float32 StarCoreSize      = 0.15f;
    float32 StarGlowFalloff   = 10.0f;
    float32 TwinkleSpeed      = 0.5f;
    float32 TwinkleIntensity  = 0.3f;
};

} // namespace Components
} // namespace GameEngine
