#include "ECSModules/Rendering/Systems/SkyEnvironmentSystem.h"

#include "Components/Hierarchy.h"
#include "Components/Rendering/Light.h"
#include "Components/Rendering/LightPhotometry.h"
#include "Components/Rendering/SkyEnvironment.h"
#include "Components/Rendering/SkySunDrive.h"
#include "Components/Rendering/SkySunIlluminance.h"
#include "Components/Rendering/SkySunPath.h"
#include "Components/Rendering/Skybox.h"
#include "Components/Transform.h"
#include "AssetCore/GUID.h"
#include "ECS/Components.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "ECS/ECSTemplates.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/SkyRenderFeature.h"
#include "Logger/Logger.h"
#include "Mathematics/Quaternion.h"
#include "Mathematics/Vector3.h"
#include "Rendering/Sky/AtmosphereTransmittance.h"
#include "Rendering/Sky/SkySettings.h"
#include "Rendering/Sky/SkySystem.h"
#include "Rendering/Sky/SolarPath.h"
#include "Types/ColorUtils.h"

namespace GameEngine { namespace Engine::Renderer {
using namespace ::GameEngine::Rendering;

namespace
{
constexpr float kPi = 3.14159265f;
constexpr float kMinSkyTimeOfDayCycleSeconds = 0.25f;
constexpr float kDefaultMoonAngularRadiusRad = 0.026f;

// Physically anchor the sky's solar irradiance to the scene's directional sun. A light's
// unitless intensity already equals its illuminance / kReferenceWhiteNits (the 203-nit anchor), so
// the sky's top-of-atmosphere solar irradiance is that same number scaled by kSkyIrradianceScale
// (the scattering source term is attenuated by the transmittance LUT before it reaches the ground).
// The sky then TRACKS the sun on the SAME scene-linear scale as sun-lit surfaces: brighten the
// directional light and the dome + baked IBL brighten coherently with it. The directional light's
// illuminance is the GROUND value; the atmosphere source term wants the TOA irradiance, which is the
// ground value lifted by 1/transmittance, so kSkyIrradianceScale ~= 1.33. That 1.33 is 1/0.75, a
// REAL-EARTH clear-sky zenith transmittance with ozone and aerosol — it is not this engine's
// atmosphere, which has no ozone (FillDefaultAtmosphere) and transmits ~0.90 at the zenith for a
// lift of 1.11, so the dome sits about a quarter stop above the surfaces it lights.
// Grey-card-verified: a sun-lit 18% card and a sky-lit card agree on the 203 scale, and the lit
// sky sits at a plausible sub-sun level.
constexpr float kSkyIrradianceScale = 1.33f;
// Returned when nothing in the scene is a sun: no usable link and no emitting directional light.
constexpr float kNoSceneSunSentinel = -1.0f;
// A colour whose luminance is at or below this carries no hue (a black night key, a sun far below
// the horizon): the hue comes from the other body instead.
constexpr float kMinimumHueLuminance = 1e-12f;

// The illuminance the sky's atmosphere is driven by, on the 203 scale (unitless).
//
// The sky's sun is the SCENE's sun. `SkyEnvironment::SunLight` is an optional two-way-sync link,
// not the source of this number: a scene that never sets it still has a sun, and a sky that reads
// only the link puts the dome, the sun disc and the baked IBL on a different scale from the surfaces
// underneath — six stops apart under a 100000 lx sun, which reads as a black sky.
//
// Reading the scene's sun is safe; WRITING it is not, so the drive path (WriteLinkedSunDirection) still
// touches nothing but an explicitly linked light. An unlinked scene gets a sky that matches its sun,
// never a light rotated out from under its author.
//
// Read only while the illuminance comes from the light: the sky then never writes the light's
// Intensity (a driven light's night lives in its colour), so the value read here is the author's
// noon illuminance at every hour. Under a curve the sky reads the curve at the hour instead
// (SkySunDrive::SkySourceLux).
//
// In order:
//   1. the explicitly linked light, which stays authoritative even when another directional is
//      brighter. Intensity is transform-independent, so a parented sun anchors the sky identically
//      to an unparented one.
//   2. the scene's brightest emitting directional light.
//   3. the sentinel: nothing in the scene is a sun.
// Steps 1 and 2 are SkySunIlluminance::ResolvedSunLight, which the sky inspector reads too.
float ResolveSkySunUnitlessIntensity(ECS::World& world, const Components::SkyEnvironment& comp)
{
    const ECS::EntityHandle sun = Components::SkySunIlluminance::ResolvedSunLight(world, comp);
    const Components::Light* light =
        sun.IsValid() ? world.GetComponent<Components::Light>(sun) : nullptr;
    if (!light)
        return kNoSceneSunSentinel;
    return Components::LightIntensityToUnitless(light->Intensity, light->IntensityUnit);
}

void TickAnimatedTimeOfDay(Components::SkyEnvironment& env, float32 deltaSeconds)
{
    float cycle = std::max(kMinSkyTimeOfDayCycleSeconds, env.TimeOfDayCycleSeconds);
    env.TimeOfDayHours += (24.0f / cycle) * deltaSeconds;
    env.TimeOfDayHours = std::fmod(env.TimeOfDayHours, 24.0f);
    if (env.TimeOfDayHours < 0.0f)
        env.TimeOfDayHours += 24.0f;

    if (env.AutoMoonArc && env.AutoSunMoon)
    {
        const float cycleHours = std::max(0.001f, env.MoonCycleDays) * 24.0f;
        env.MoonArcPosition += deltaSeconds / cycleHours;
        env.MoonArcPosition = std::fmod(env.MoonArcPosition, 1.0f);
        if (env.MoonArcPosition < 0.0f)
            env.MoonArcPosition += 1.0f;
    }
}

// Smallest-arc rotation taking unit `from` to unit `to`.
Mathematics::Quaternion RotationFromTo(const Mathematics::Vector3& from,
                                       const Mathematics::Vector3& to)
{
    const float d = std::clamp(Mathematics::Vector3::Dot(from, to), -1.0f, 1.0f);
    if (d >= 0.999999f)
        return Mathematics::Quaternion(1.0f, 0.0f, 0.0f, 0.0f);
    if (d <= -0.999999f)
    {
        Mathematics::Vector3 axis =
            Mathematics::Vector3::Cross(Mathematics::Vector3(1.0f, 0.0f, 0.0f), from);
        if (axis.Length() < 1e-4f)
            axis = Mathematics::Vector3::Cross(Mathematics::Vector3(0.0f, 1.0f, 0.0f), from);
        return Mathematics::Quaternion::FromAxisAngle(axis.Normalize(), kPi);
    }
    Mathematics::Vector3 axis = Mathematics::Vector3::Cross(from, to).Normalize();
    return Mathematics::Quaternion::FromAxisAngle(axis, std::acos(d));
}

// A directional light's `directionWS` (the way it SHINES) = +Z column = +col2.
// The sun sits the opposite way, so the sky's toward-sun direction =
// -directionWS = -col2 of the WORLD matrix.
void ReadLightSunDir(const Components::WorldTransform& xf, float out[3])
{
    out[0] = -xf.matrix[8];
    out[1] = -xf.matrix[9];
    out[2] = -xf.matrix[10];
    ::GameEngine::Rendering::Normalize3(out);
}

// Orient the light so it shines from the sun toward the scene: directionWS =
// -sunDir. Extraction reads directionWS = +col2 = q.Rotate(+Z). Writes LOCAL
// Transform; SyncSunLight only drives unparented lights, so local == world
// after the hierarchy propagates.
void WriteLightSunDir(Components::Transform& xf, const float sunDir[3])
{
    Mathematics::Vector3 shine(-sunDir[0], -sunDir[1], -sunDir[2]);
    const Mathematics::Quaternion q =
        RotationFromTo(Mathematics::Vector3(0.0f, 0.0f, 1.0f), shine.Normalize());
    xf = Components::Transform::FromTRS(xf.GetPosition(), q, xf.GetScale());
}

// Writes the drive's color to the light and reports the color the light now holds.
void WriteLinkedSunColor(ECS::World& world, ECS::EntityHandle lightEntity, const float targetColor[3],
                         float writtenColor[3])
{
    const auto* light = world.GetComponent<Components::Light>(lightEntity);

    float tint[3] = {1.0f, 1.0f, 1.0f};
    if (light->UseColorTemperature)
        Components::KelvinToLinearRGB(light->ColorTemperature, tint);

    float newColor[3];
    for (int i = 0; i < 3; ++i)
        newColor[i] = targetColor[i] / std::max(tint[i], 1e-4f);
    for (int i = 0; i < 3; ++i)
        writtenColor[i] = newColor[i];

    // Value-gated write: this runs every frame while the sky drives the sun,
    // so only take the write grant when the color actually changes (idle
    // frames must not stamp — change-signaling IdleEditorStampCount gate).
    if (newColor[0] == light->Color[0] && newColor[1] == light->Color[1] &&
        newColor[2] == light->Color[2])
        return;

    if (auto* lightW = world.GetComponentForWrite<Components::Light>(lightEntity))
    {
        for (int i = 0; i < 3; ++i)
            lightW->Color[i] = newColor[i];
    }
}

// Writes the drive's Intensity to the light and reports the value the light now holds.
void WriteLinkedSunIntensity(ECS::World& world, ECS::EntityHandle lightEntity, float intensity, float& writtenIntensity)
{
    writtenIntensity = intensity;
    // Value-gated, as the colour: idle frames must not stamp (IdleEditorStampCount gate).
    if (world.GetComponent<Components::Light>(lightEntity)->Intensity == intensity)
        return;
    if (auto* lightW = world.GetComponentForWrite<Components::Light>(lightEntity))
        lightW->Intensity = intensity;
}

// `color` at unit luminance. False, leaving `outHue` alone, when it is too dark to carry a hue.
bool HueOf(const float color[3], float outHue[3])
{
    const float luminance = ColorUtils::LinearRec709Luminance(color);
    if (!(luminance > kMinimumHueLuminance))
        return false;
    for (int c = 0; c < 3; ++c)
        outHue[c] = color[c] / luminance;
    return true;
}

// The hue a curve-driven light takes: the hue of the energy blend of the two bodies,
// (1 - w) x curveLux x sunHue + w x moonLux x moonHue, which is the sun's hue exactly by day. Where
// the blend is dark it is the hue of the body the blend favours, then the other body's, then white.
void CurveDrivenHue(const float sunHue[3], bool sunHasHue, const float moonHue[3], bool moonHasHue, float moonBlend,
                    float curveLux, float moonLux, float outHue[3])
{
    const float white[3] = {1.0f, 1.0f, 1.0f};
    const float* favoured = (moonBlend < 0.5f) ? (sunHasHue ? sunHue : (moonHasHue ? moonHue : white))
                                               : (moonHasHue ? moonHue : (sunHasHue ? sunHue : white));
    if (moonBlend == 0.0f || !moonHasHue || !sunHasHue)
    {
        for (int c = 0; c < 3; ++c)
            outHue[c] = favoured[c];
        return;
    }
    float blend[3];
    for (int c = 0; c < 3; ++c)
        blend[c] = (1.0f - moonBlend) * curveLux * sunHue[c] + moonBlend * moonLux * moonHue[c];
    if (HueOf(blend, outHue))
        return;
    for (int c = 0; c < 3; ++c)
        outHue[c] = favoured[c];
}

// Turns `lightEntity`, which the drive has already checked is drivable, to shine from `targetDir`.
void WriteLinkedSunDirection(ECS::World& world, ECS::EntityHandle lightEntity, const float targetDir[3])
{
    const auto* localXf = world.GetComponent<Components::Transform>(lightEntity);

    // Value-gated writes below: this runs every frame while Time of Day
    // drives the light, so write grants are only taken when a value actually
    // changes (idle frames must not stamp — IdleEditorStampCount gate).
    //
    // The gate compares the SHINE DIRECTION, not the raw matrix bytes:
    // WriteLightSunDir recomposes via FromTRS(GetScale()), and GetScale's
    // column-length decompose never round-trips a scaled matrix bitwise — the
    // old full-matrix memcmp therefore never converged, rewriting the sun
    // EVERY frame at a static time of day (a 1-ulp quaternion wobble plus a
    // slow ratchet of the authored scale) and stamping Transform churn into
    // every idle-detection consumer downstream (light-list versions, feed
    // fast-path movers, the idle recompute elision gates).
    //
    // The hold band QUANTIZES sun motion into ~0.028-degree steps (1e-7
    // requested on the cosine; float32 rounds the effective band to
    // ~1.19e-7). Crossing it every frame at 60fps needs a day shorter than
    // ~4 minutes: a 20-minute day steps every ~5-6 frames, a realtime day
    // every ~6-7 s. Each step displaces cascade-0 shadows by roughly one
    // shadow texel (~10 mm at 20 m), so the stepping is not visible.
    //
    // The scale ratchet is only RATE-LIMITED by this, not fixed: at rest the
    // writes stop, but every write that does fire still round-trips
    // GetScale()'s column-length decompose, so under a fast-animating time
    // of day the decay proceeds at the old rate. The root fix — recomposing
    // from the authored scale instead of re-deriving it from matrix
    // columns — is tracked.
    {
        // Toward-sun is -col2 (shine is +col2). Compare that to primarySunDir
        // so a settled write does not restamp every idle frame.
        float curSun[3] = {-localXf->matrix[8], -localXf->matrix[9], -localXf->matrix[10]};
        ::GameEngine::Rendering::Normalize3(curSun);
        float tgtSun[3] = {targetDir[0], targetDir[1], targetDir[2]};
        ::GameEngine::Rendering::Normalize3(tgtSun);
        const float align =
            curSun[0] * tgtSun[0] + curSun[1] * tgtSun[1] + curSun[2] * tgtSun[2];
        constexpr float kSunDirHoldBand = 1e-7f;
        // Written as !(>=) so a NaN align (degenerate/zero-scale authored
        // matrix) fails OPEN into the write path — `align < band` would hold
        // forever and leave the sun permanently un-drivable.
        if (!(align >= 1.0f - kSunDirHoldBand))
        {
            Components::Transform updatedXf = *localXf;
            WriteLightSunDir(updatedXf, targetDir);
            if (auto* xfW = world.GetComponentForWrite<Components::Transform>(lightEntity))
                *xfW = updatedXf;
        }
    }
}
}

void SkyEnvironmentSystem::Update(ECS::World& world, float32 deltaTime)
{
    // The light this frame's drive writes and its fields, by the definitions every editor reader
    // shares (SkySunIlluminance::DrivenSunLight, FieldsSkyDrives). Taken before anything below returns
    // early, so a drive that ends is handed back on every path.
    const ECS::EntityHandle renderedSky = Components::SkySunIlluminance::RenderedSky(world);
    const auto* renderedComp =
        renderedSky.IsValid() ? world.GetComponent<Components::SkyEnvironment>(renderedSky) : nullptr;
    const ECS::EntityHandle drivenLight =
        renderedComp ? Components::SkySunIlluminance::LightSkyWouldDrive(world, *renderedComp) : ECS::EntityHandle{};
    const uint8 drivenFields =
        drivenLight.IsValid() ? Components::SkySunIlluminance::FieldsSkyDrives(*renderedComp) : uint8{0};
    const ECS::EntityHandle candidate =
        (renderedComp && Components::SkySunIlluminance::IsDrivableSunLight(world, renderedComp->SunLight))
            ? renderedComp->SunLight
            : ECS::EntityHandle{};
    TrackDrive(world, candidate, drivenLight, drivenFields);

    RenderServices* rs = m_RenderServices;
    if (!rs)
        return;

    auto& sky = rs->EnsureFeature<SkyRenderFeature>();
    // Snapshot last frame's settings so a failed HDRI upload does not leave the sky pass
    // inactive for a frame (that toggles world color load/clear and reads as flicker).
    const Rendering::SkySettings* fallbackSkySettings =
        sky.HasActiveSettings() ? &sky.GetSettings() : nullptr;

    Components::Skybox skyboxComp{};
    bool foundSkybox = false;
    int skyboxCount = 0;

    {
        world.Query<ECS::Read<Components::Skybox>>()
            .Each([&](ECS::EntityHandle /*e*/, const Components::Skybox& box) {
                if (box.HDRIAssetGuid[0] == '\0')
                    return;
                ++skyboxCount;
                if (foundSkybox)
                    return;
                skyboxComp = box;
                foundSkybox = true;
            });
    }

    if (skyboxCount > 1)
    {
        static bool warnedMultipleSkyboxes = false;
        if (!warnedMultipleSkyboxes)
        {
            LOG_WARNING("Multiple Skybox components found ({}). Only the first will be used.", skyboxCount);
            warnedMultipleSkyboxes = true;
        }
    }

    if (!foundSkybox)
        m_LastSkyboxHdriEvictionGuid = GUID{};

    if (foundSkybox)
    {
        Rendering::SkySettings settings{};
        settings.scatteringSunDir[0] = settings.primarySunDir[0];
        settings.scatteringSunDir[1] = settings.primarySunDir[1];
        settings.scatteringSunDir[2] = settings.primarySunDir[2];
        settings.exposureEV = 0.0f;
        settings.hdriIntensity = std::max(0.0f, skyboxComp.HDRIIntensity);
        settings.skyboxRotationRadians = skyboxComp.RotationDegrees * 0.01745329251994329577f;
        settings.iblIntensity = std::max(0.0f, skyboxComp.IblIntensity);
        settings.iblLowerHemisphereDarkness =
            std::clamp(skyboxComp.IblLowerHemisphereDarkness, 0.0f, 1.0f);

        GUID guid(std::string(skyboxComp.GetHDRIAssetGuid()));

        // Force a fresh GPU upload when the skybox switches HDRIs so cached textures cannot
        // stick across Polyhaven resolution changes (GUID swap or stale metadata/bind path).
        if (!guid.IsNull() && guid != m_LastSkyboxHdriEvictionGuid)
        {
            if (!m_LastSkyboxHdriEvictionGuid.IsNull())
                rs->Textures().Evict(m_LastSkyboxHdriEvictionGuid);
            rs->Textures().Evict(guid);
            m_LastSkyboxHdriEvictionGuid = guid;
        }

        Rendering::TextureHandle envTex{};
        if (!guid.IsNull())
            envTex = rs->Textures().GetOrUpload(guid);

        if (!envTex.IsValid() && fallbackSkySettings &&
            fallbackSkySettings->environmentTexture.IsValid())
            envTex = fallbackSkySettings->environmentTexture;

        if (!envTex.IsValid())
        {
            if (fallbackSkySettings)
            {
                Rendering::SkySystemState state{};
                sky.SetSettings(*fallbackSkySettings, state);
            }
            return;
        }

        settings.environmentTexture = envTex;

        Rendering::SkySystemState state{};
        sky.SetSettings(settings, state);
        return;
    }

    sky.ClearActiveSettings();

    Components::SkyEnvironment comp{};
    bool foundEnvironment = false;
    int environmentCount = 0;

    {
        world.Query<ECS::Write<Components::SkyEnvironment>>()
            .Each([&](Components::SkyEnvironment& env) {
                ++environmentCount;
                if (foundEnvironment)
                    return;
                if (env.AnimateTimeOfDay)
                    TickAnimatedTimeOfDay(env, deltaTime);
                comp = env;
                foundEnvironment = true;
            });
    }

    if (environmentCount > 1)
    {
        static bool warnedMultiple = false;
        if (!warnedMultiple)
        {
            LOG_WARNING("Multiple SkyEnvironment components found ({}). Only the first will be used.", environmentCount);
            warnedMultiple = true;
        }
    }

    if (!foundEnvironment)
    {
        if (sky.HasActiveSettings())
            sky.ClearActiveSettings();
        return;
    }

    const Rendering::SolarFrame solarFrame =
        Rendering::MakeSolarFrame(Components::SkySunPath::PathAngles(comp));

    // When the sky follows (not drives) a linked directional light, adopt the
    // light's world direction as the sun before building the settings (flips the
    // local comp.AutoSunMoon / SunDirOverride / TimeOfDayHours).
    SyncSunLight(world, comp, solarFrame);

    Rendering::SkySettings settings{};
    settings.scatteringSunDir[0] = settings.primarySunDir[0];
    settings.scatteringSunDir[1] = settings.primarySunDir[1];
    settings.scatteringSunDir[2] = settings.primarySunDir[2];
    settings.timeOfDayHours = comp.TimeOfDayHours;
    const float dayKeyTimesHours[4] = {
        comp.DayKeyTimesHours.Midnight,
        comp.DayKeyTimesHours.Dawn,
        comp.DayKeyTimesHours.Midday,
        comp.DayKeyTimesHours.Sunset,
    };
    // Flat sky-vs-object/ambient trim (the day/night arc is owned by the atmosphere model + camera
    // auto-exposure now that the sky is physically anchored to the directional sun).
    settings.exposureEV = std::clamp(comp.SkyExposureTrim,
                                     -Components::kSkyExposureTrimLimitEv,
                                     Components::kSkyExposureTrimLimitEv);

    const float tod = comp.TimeOfDayHours;
    SampleSkyVec3KeyframeCurve(tod,
                               dayKeyTimesHours,
                               comp.GroundAlbedoKeys.Midnight,
                               comp.GroundAlbedoKeys.Dawn,
                               comp.GroundAlbedoKeys.Midday,
                               comp.GroundAlbedoKeys.Sunset,
                               settings.groundAlbedo);
    SampleSkyVec3KeyframeCurve(tod,
                               dayKeyTimesHours,
                               comp.GroundNightColorKeys.Midnight,
                               comp.GroundNightColorKeys.Dawn,
                               comp.GroundNightColorKeys.Midday,
                               comp.GroundNightColorKeys.Sunset,
                               settings.groundNightColor);
    settings.groundBrightness = std::max(
        0.0f,
        Components::EvaluateSkyScalarDayCurve(
            comp.GroundBrightnessKeys, comp.GroundBrightnessShapeMode, comp.GroundBrightnessBezier, tod));
    settings.belowHorizonBlendSharpness = std::max(
        0.001f,
        Components::EvaluateSkyScalarDayCurve(
            comp.BelowHorizonBlendSharpnessKeys,
            comp.BelowHorizonBlendSharpnessShapeMode,
            comp.BelowHorizonBlendSharpnessBezier,
            tod));
    settings.belowHorizonDarkness = std::clamp(
        Components::EvaluateSkyScalarDayCurve(
            comp.BelowHorizonDarknessKeys,
            comp.BelowHorizonDarknessShapeMode,
            comp.BelowHorizonDarknessBezier,
            tod),
        0.0f,
        1.0f);
    settings.belowHorizonMode = static_cast<uint32_t>(comp.BelowHorizonMode);
    settings.groundHazeStrength = comp.GroundHazeStrength;
    SampleSkyVec3KeyframeCurve(tod,
                               dayKeyTimesHours,
                               comp.BelowHorizonDarkColorKeys.Midnight,
                               comp.BelowHorizonDarkColorKeys.Dawn,
                               comp.BelowHorizonDarkColorKeys.Midday,
                               comp.BelowHorizonDarkColorKeys.Sunset,
                               settings.belowHorizonDarkColor);
    SampleSkyVec3KeyframeCurve(tod,
                               dayKeyTimesHours,
                               comp.GroundHorizonColorKeys.Midnight,
                               comp.GroundHorizonColorKeys.Dawn,
                               comp.GroundHorizonColorKeys.Midday,
                               comp.GroundHorizonColorKeys.Sunset,
                               settings.groundHorizonColor);
    SampleSkyVec3KeyframeCurve(tod,
                               dayKeyTimesHours,
                               comp.GroundHorizonNightColorKeys.Midnight,
                               comp.GroundHorizonNightColorKeys.Dawn,
                               comp.GroundHorizonNightColorKeys.Midday,
                               comp.GroundHorizonNightColorKeys.Sunset,
                               settings.groundHorizonNightColor);
    SampleSkyVec3KeyframeCurve(tod,
                               dayKeyTimesHours,
                               comp.NightSkyHorizonColorKeys.Midnight,
                               comp.NightSkyHorizonColorKeys.Dawn,
                               comp.NightSkyHorizonColorKeys.Midday,
                               comp.NightSkyHorizonColorKeys.Sunset,
                               settings.nightSkyHorizonColor);
    // The night horizon's keys are authored against the default moonlight: the dome above them takes its
    // night source from Moonlight (the moon branch below), so they scale with it too, or a bright moon
    // leaves the horizon band at the default's darkness under a bright sky. Exactly 1 at the default.
    {
        const float moonlightRatio = Components::SkySunDrive::MoonlightRatio(comp);
        for (int c = 0; c < 3; ++c)
        {
            settings.groundHorizonNightColor[c] *= moonlightRatio;
            settings.nightSkyHorizonColor[c] *= moonlightRatio;
        }
    }
    settings.groundHorizonCosWidth = std::clamp(
        Components::EvaluateSkyScalarDayCurve(
            comp.GroundHorizonCosWidthKeys,
            comp.GroundHorizonCosWidthShapeMode,
            comp.GroundHorizonCosWidthBezier,
            tod),
        0.001f,
        0.2f);
    settings.groundHorizonNightCosWidth = std::clamp(
        Components::EvaluateSkyScalarDayCurve(
            comp.GroundHorizonNightCosWidthKeys,
            comp.GroundHorizonNightCosWidthShapeMode,
            comp.GroundHorizonNightCosWidthBezier,
            tod),
        0.001f,
        0.2f);
    settings.starDensity = comp.StarDensity;
    settings.starBrightness = comp.StarBrightness;
    settings.starSize = comp.StarSize;
    settings.starDiamondShape = comp.StarDiamondShape;
    settings.starCoreSize = comp.StarCoreSize;
    settings.starGlowFalloff = comp.StarGlowFalloff;
    settings.twinkleSpeed = comp.TwinkleSpeed;
    settings.twinkleIntensity = comp.TwinkleIntensity;
    settings.showSunDisk = comp.ShowSun;
    settings.showMoonDisk = comp.ShowMoon;
    // This one multiplies the radius the sun disc is DRAWN at, which sun_disc.glsl divides the
    // sun's irradiance by. std::clamp passes a NaN straight through, and a NaN radius there
    // reaches SceneColor as a NaN the whole frame carries — so a non-finite authored size falls
    // back to the physical sun rather than propagating.
    settings.sunSize = std::isfinite(comp.SunSize)
                           ? std::clamp(comp.SunSize, Components::kSunSizeMin, Components::kSunSizeMax)
                           : 1.0f;
    settings.sunSize2D = std::clamp(comp.SunSize2D, 0.1f, 6.0f);
    settings.moonSize2D = std::clamp(comp.MoonSize2D, 0.1f, 6.0f);
    settings.skyPan2D = std::clamp(comp.SkyPan2D, -1.0f, 1.0f);
    settings.moonExposureEV = std::clamp(comp.MoonExposureEV, -8.0f, 8.0f);
    settings.fallingStarsEnabled = comp.FallingStarsEnabled;
    settings.fallingStarAmount = std::clamp(comp.FallingStarAmount, 0.0f, 1.0f);
    settings.fallingStarFrequency = std::clamp(comp.FallingStarFrequency, 0.0f, 4.0f);
    settings.fallingStarSpeed = std::clamp(comp.FallingStarSpeed, 0.2f, 50.0f);
    settings.fallingStarLength = std::clamp(comp.FallingStarLength, 0.2f, 4.0f);
    settings.fallingStarThickness = std::clamp(comp.FallingStarThickness, 0.1f, 4.0f);
    settings.fallingStarDotSize = std::clamp(comp.FallingStarDotSize, 0.1f, 4.0f);
    settings.fallingStarDotSize2D = std::clamp(comp.FallingStarDotSize2D, 0.1f, 4.0f);
    Rendering::SkySystemConfig config{};
    Rendering::SkySystemState state{};

    // Physically anchor the sky's solar irradiance to the scene's directional sun. Set BEFORE the
    // sun/moon blend, which reads primarySunIntensity as its daytime input
    // (ApplySimpleLightingToSkySettings). The manual-override branch keeps this value, so an
    // override sky also renders on the 203 scale.
    //
    // When the sky's sun has no intensity — an explicitly linked light authored at zero, or no
    // emitting directional in the scene at all — there is nothing to anchor to, so the dome keeps
    // the SkySettings default, which sits far below any physically authored sun. Say what is wrong
    // and how to fix it rather than quietly rendering a dark sky. Only the physical sky reads this
    // number, so a stylized gradient sky with no sun in the scene is a valid setup and says nothing.
    // Cleared once a sun exists, so a scene that breaks again is reported again.
    //
    // While the curve writes the light's Intensity the sky's own sun follows the curve at this hour
    // (SkySunDrive::SkySourceLux), never the light: the light then holds the dusk the sky wrote, and
    // reading it back would feed the dusk into the dome a second time. The noon reference is kept for
    // the hand-back when the drive ends.
    const bool curveDrives = (drivenFields & Components::SkySunIlluminance::kDrivesIntensity) != 0;
    const float sunUnitless =
        curveDrives ? Components::LightIntensityToUnitless(
                          Components::SkySunDrive::SkySourceLux(comp, comp.TimeOfDayHours), Components::LightUnit::Lux)
                    : ResolveSkySunUnitlessIntensity(world, comp);
    if (curveDrives)
        m_DrivenNoonReferenceLux = Components::SkySunDrive::NoonReferenceLux(comp);
    if (sunUnitless > 0.0f)
    {
        settings.primarySunIntensity = sunUnitless * kSkyIrradianceScale;
        m_WarnedNoSceneSun = false;
    }
    else if (comp.Mode == Components::SkyMode::Physical && !m_WarnedNoSceneSun)
    {
        m_WarnedNoSceneSun = true;
        LOG_WARNING(
            "SkyEnvironment: the sky's sun — the light linked in Sun Light if there is one, "
            "otherwise the scene's brightest directional — has no intensity, so the physical sky "
            "has nothing to take its brightness from and renders far darker than the surfaces it "
            "lights. Add or enable a directional light, give the linked one an intensity, or set "
            "the sky's Mode to Gradient to author its brightness directly.");
    }

    // The one stylistic lever on the sun's colour, sampled before anything consumes the source.
    // White at every key by default, so the shipped look is the physical one and this is the
    // identity until someone authors it.
    //
    // It scales the SUN only. The moon keeps config.moonColor, because the moon DISC is drawn from
    // that colour with no tint applied (sky_render.frag) — tinting the moon's light but not its
    // disc would hang an orange dome over a grey moon. So an evening key colours the evening and
    // lets go of the night, which is what "sun tint" should mean.
    float sunTint[3] = {1.0f, 1.0f, 1.0f};
    SampleSkyVec3KeyframeCurve(tod,
                               dayKeyTimesHours,
                               comp.SunTintKeys.Midnight,
                               comp.SunTintKeys.Dawn,
                               comp.SunTintKeys.Midday,
                               comp.SunTintKeys.Sunset,
                               sunTint);

    if (comp.AutoSunMoon)
    {
        settings.moonArcPosition = std::clamp(comp.MoonArcPosition, 0.0f, 1.0f);
        settings.moonPhase01 = std::clamp(comp.MoonPhase01, 0.0f, 1.0f);
        settings.moonAngularRadius =
            kDefaultMoonAngularRadiusRad * std::clamp(comp.MoonSize, 0.1f, 6.0f);
        Rendering::ComputeSimpleSunMoon(settings, config, solarFrame, state);
        // The night's source is the moonlight's, on the same scale as the day's: the default
        // moonlight comes from a clear 100 000 lx sun, so the night dome and the moon disc follow
        // Moonlight and are left where they are when the sun is dimmed.
        const float moonSourceUnitless =
            Components::LightIntensityToUnitless(Components::kClearNoonSunIlluminanceLux *
                                                     Components::SkySunDrive::MoonlightRatio(comp),
                                                 Components::LightUnit::Lux) *
            kSkyIrradianceScale;
        Rendering::ApplySimpleLightingToSkySettings(state, config, sunTint, moonSourceUnitless, settings);
        settings.scatteringSunDir[0] = state.sunDirWS[0];
        settings.scatteringSunDir[1] = state.sunDirWS[1];
        settings.scatteringSunDir[2] = state.sunDirWS[2];

        settings.moonDirWS[0] = state.moonDirWS[0];
        settings.moonDirWS[1] = state.moonDirWS[1];
        settings.moonDirWS[2] = state.moonDirWS[2];
        settings.moonIntensity = settings.primarySunIntensity * config.moonIntensityScale;
    }
    else
    {
        settings.moonAngularRadius =
            kDefaultMoonAngularRadiusRad * std::clamp(comp.MoonSize, 0.1f, 6.0f);

        float dir[3] = { comp.SunDirOverride[0], comp.SunDirOverride[1], comp.SunDirOverride[2] };
        ::GameEngine::Rendering::Normalize3(dir);

        settings.primarySunDir[0] = dir[0];
        settings.primarySunDir[1] = dir[1];
        settings.primarySunDir[2] = dir[2];
        settings.scatteringSunDir[0] = dir[0];
        settings.scatteringSunDir[1] = dir[1];
        settings.scatteringSunDir[2] = dir[2];

        state.sunDirWS[0] = dir[0];
        state.sunDirWS[1] = dir[1];
        state.sunDirWS[2] = dir[2];

        // No automated moon when the sun direction is manually overridden:
        // place it antipodal to the override and disable the disk so it
        // never appears in unexpected places.
        settings.moonDirWS[0] = -dir[0];
        settings.moonDirWS[1] = -dir[1];
        settings.moonDirWS[2] = -dir[2];
        settings.moonIntensity = 0.0f;

        // This branch never runs the sun/moon blend, so the source is the sun's alone and the tint
        // is the whole of it.
        settings.primarySunColor[0] = sunTint[0];
        settings.primarySunColor[1] = sunTint[1];
        settings.primarySunColor[2] = sunTint[2];
    }

    // What the ground is lit by: each celestial body's source colour minus the extra air mass its
    // own light travels through, the two crossfaded on the same blend the intensity uses. The
    // sky's passes attenuate separately (per march step in the sky-view LUT, along the view ray
    // for the disc), so this is derived here rather than folded into primarySunColor — a warm low
    // sun and a blue-to-warm sky are the same optical depth applied once each.
    //
    // It takes `sunTint`, not the already-mixed primarySunColor: the tint belongs to the light going
    // IN, so it is handed to the extinction as a source and comes out the far side attenuated
    // exactly once, and the moon's share stays untinted on this path exactly as it does on the sky's.
    // Deriving it from primarySunColor instead would carry the moon blend across the handoff a second
    // time. Deliberately NOT evaluated at primarySunDir either: that is the blended direction, which
    // flips to the moon's side of the sky mid-handoff.
    const Rendering::SkyBodyGroundColors bodies =
        Rendering::EvaluateBodyGroundColors(ScatteringAtmosphere(), config, state, sunTint);
    Rendering::MixPrimaryGroundColor(bodies, settings.primarySunGroundColor);

    if (drivenLight.IsValid())
    {
        // Output mode: the sky writes the resolved time-of-day source to the linked light, the
        // fields FieldsSkyDrives names. Nothing reads those fields back.
        WriteLinkedSunDirection(world, drivenLight, settings.primarySunDir);
        const auto* light = world.GetComponent<Components::Light>(drivenLight);
        if (curveDrives)
        {
            // The curve owns the illuminance: Intensity = the energy blend (1 - w) C(t) + w M(t) in
            // lux, written in the light's own unit, so by day it is the curve's value exactly. The
            // colour, when the sky writes it, is a hue at unit luminance.
            const float moonBlend = bodies.MoonBlend;
            const float curveLux =
                Components::SkySunDrive::CurveIlluminanceLux(comp.SunIlluminanceCurve, comp.TimeOfDayHours);
            const float moonLux = Components::SkySunDrive::MoonlightLuxFromGround(comp, bodies.Moon);
            const float deliveredLux = (1.0f - moonBlend) * curveLux + moonBlend * moonLux;
            WriteLinkedSunIntensity(world, drivenLight,
                                    Components::SkySunIlluminance::LightIntensityFromLux(deliveredLux, light->IntensityUnit),
                                    m_DrivenLightIntensity);
            if (drivenFields & Components::SkySunIlluminance::kDrivesColor)
            {
                // The sun's hue with its elevation held at the horizon once it sets, so a set sun keeps
                // the deepest horizon red rather than a near-zero colour over a near-zero luminance.
                float sunHueSource[3] = {bodies.Sun[0], bodies.Sun[1], bodies.Sun[2]};
                if (state.sunDirWS[1] < 0.0f)
                    Rendering::EvaluateGroundLevelSunColor(ScatteringAtmosphere(), sunTint, 0.0f, sunHueSource);
                float sunHue[3];
                float moonHue[3];
                const bool sunHasHue = HueOf(sunHueSource, sunHue);
                const bool moonHasHue = HueOf(bodies.Moon, moonHue);
                float hue[3];
                CurveDrivenHue(sunHue, sunHasHue, moonHue, moonHasHue, moonBlend, curveLux, moonLux, hue);
                WriteLinkedSunColor(world, drivenLight, hue, m_DrivenLightColor);
            }
        }
        else
        {
            // The light owns the illuminance, its Intensity the author's clear-noon value, so the
            // colour carries the rest. Across the handover it is the energy blend of the two bodies,
            // each at what it delivers: the sun's ground colour, and the moon's scaled so that at full
            // night the light delivers Moonlight, whatever the light's own value (the stylized
            // fraction of the day SkySystemConfig owns, times Moonlight over its default, times a
            // clear sun over this light's). By day the blend is exactly zero, so the colour is the
            // sun's ground colour to the bit; at the defaults the moon's scale is that fraction
            // exactly.
            const float lightUnitless = Components::LightIntensityToUnitless(light->Intensity, light->IntensityUnit);
            const float clearSunOverLight =
                lightUnitless > 0.0f
                    ? Components::LightIntensityToUnitless(Components::kClearNoonSunIlluminanceLux,
                                                           Components::LightUnit::Lux) /
                          lightUnitless
                    : 0.0f;
            const float moonScale = Rendering::LinkedLightMoonScale(config, comp.ShowMoon) *
                                    Components::SkySunDrive::MoonlightRatio(comp) * clearSunOverLight;
            if (drivenFields & Components::SkySunIlluminance::kDrivesColor)
            {
                float linkedLightColor[3];
                Rendering::MixLinkedLightGroundColor(bodies, moonScale, linkedLightColor);
                WriteLinkedSunColor(world, drivenLight, linkedLightColor, m_DrivenLightColor);
            }
        }
    }

    settings.iblIntensity = std::max(0.0f, comp.IblIntensity);
    settings.iblLowerHemisphereDarkness = std::clamp(comp.IblLowerHemisphereDarkness, 0.0f, 1.0f);
    for (int i = 0; i < 3; ++i)
    {
        settings.ambientTintSky[i]     = comp.AmbientTintSky[i];
        settings.ambientTintEquator[i] = comp.AmbientTintEquator[i];
        settings.ambientTintGround[i]  = comp.AmbientTintGround[i];
    }

    // Gradient sky: premultiply the authored 0..1 colors by the luminance (nits) on the shared
    // 203-nit reference-white anchor, so the gradient dome + its baked IBL sit on the SAME
    // scene-linear scale as the physical sky and the camera auto-exposure.
    settings.skyMode = static_cast<uint32_t>(comp.Mode);
    const float gradientScale = std::max(0.0f, comp.GradientSkyIntensity) / Components::kReferenceWhiteNits;
    for (int i = 0; i < 3; ++i)
    {
        settings.gradientSkyTopColor[i]     = comp.GradientSkyTopColor[i] * gradientScale;
        settings.gradientSkyHorizonColor[i] = comp.GradientSkyHorizonColor[i] * gradientScale;
        settings.gradientSkyBottomColor[i]  = comp.GradientSkyBottomColor[i] * gradientScale;
    }

    // nightSkyBlend must always be driven by the real sun elevation, not
    // primarySunDir (which can blend toward the moon near sunset).
    float sunDotUp = state.sunDirWS[1];
    settings.nightSkyBlend = 1.0f - std::clamp((sunDotUp + 0.15f) / 0.15f, 0.0f, 1.0f);

    // Accumulate sky time for star animation
    m_AccumulatedSkyTime += deltaTime;
    settings.skyTimeSeconds = m_AccumulatedSkyTime;

    sky.SetSettings(settings, state);
}

void SkyEnvironmentSystem::TrackDrive(ECS::World& world, ECS::EntityHandle candidate,
                                      ECS::EntityHandle drivenThisFrame, uint8 fieldsThisFrame)
{
    using Components::SkySunIlluminance::kDrivesIntensity;
    if (m_TrackedWorld != world.GetWorldId() || m_TrackedReset != world.GetLifecycleResetGeneration())
    {
        m_TrackedWorld = world.GetWorldId();
        m_TrackedReset = world.GetLifecycleResetGeneration();
        m_DrivenLight = {};
        m_DrivenFields = 0;
        m_PreviousCandidate = {};
        m_HasPreviousUpdate = false;
        m_AuthoredIntensityLight = {};
    }
    const bool sameLight = m_DrivenLight.IsValid() && m_DrivenLight == drivenThisFrame;
    if (m_DrivenLight.IsValid())
        ReleaseEndedFields(world, m_DrivenLight, sameLight ? uint8(m_DrivenFields & ~fieldsThisFrame) : m_DrivenFields);

    const bool intensityStarts =
        (fieldsThisFrame & kDrivesIntensity) && !(sameLight && (m_DrivenFields & kDrivesIntensity));
    if (intensityStarts)
    {
        m_AuthoredIntensityLight = {};
        const auto* light = world.GetComponent<Components::Light>(drivenThisFrame);
        if (m_HasPreviousUpdate && m_PreviousCandidate == drivenThisFrame && light)
        {
            m_AuthoredIntensityLight = drivenThisFrame;
            m_AuthoredIntensityLux =
                Components::SkySunIlluminance::LuxFromLightIntensity(light->Intensity, light->IntensityUnit);
        }
    }

    m_DrivenLight = drivenThisFrame;
    m_DrivenFields = fieldsThisFrame;
    m_PreviousCandidate = candidate;
    m_HasPreviousUpdate = true;
}

void SkyEnvironmentSystem::ReleaseEndedFields(ECS::World& world, ECS::EntityHandle light, uint8 endedFields)
{
    using Components::SkySunIlluminance::kDrivesColor;
    using Components::SkySunIlluminance::kDrivesIntensity;
    const auto* current = world.IsValid(light) ? world.GetComponent<Components::Light>(light) : nullptr;
    const bool releasesIntensity = (endedFields & kDrivesIntensity) != 0;
    if (current && (endedFields & kDrivesColor) && current->Color[0] == m_DrivenLightColor[0] &&
        current->Color[1] == m_DrivenLightColor[1] && current->Color[2] == m_DrivenLightColor[2])
        Components::SkySunIlluminance::ReleaseSunLightColor(world, light);
    if (current && releasesIntensity && current->Intensity == m_DrivenLightIntensity)
    {
        const float lux = m_AuthoredIntensityLight == light ? m_AuthoredIntensityLux : m_DrivenNoonReferenceLux;
        Components::SkySunIlluminance::ReleaseSunLightIntensity(world, light, lux);
    }
    if (releasesIntensity && m_AuthoredIntensityLight == light)
        m_AuthoredIntensityLight = {};
}

void SkyEnvironmentSystem::SyncSunLight(ECS::World& world, Components::SkyEnvironment& comp,
                                        const Rendering::SolarFrame& solarFrame)
{
    const ECS::EntityHandle lightEntity = comp.SunLight;
    if (!world.IsValid(lightEntity))
        return;

    // TimeOfDayDrivesSunLight is the DRIVING path: the sky owns the sun and
    // WriteLinkedSunDirection writes the light's LOCAL transform every frame, so it
    // keeps requiring an unparented directional light (enforced there). Nothing
    // for this read-only follow path to do in that mode.
    if (comp.TimeOfDayDrivesSunLight)
        return;

    // FOLLOW path: the linked directional light is authoritative for the sun. We
    // read its WORLD direction (parent-composition-aware via WorldTransform) and
    // render the sky sun there, never writing the light — so a parented light is
    // fully supported; only the driving path needs an unparented light.
    const Components::Light* light = world.GetComponent<Components::Light>(lightEntity);
    if (!light || light->Type != Components::LightType::Directional)
    {
        static bool warnedUnsupportedSunLink = false;
        if (!warnedUnsupportedSunLink)
        {
            LOG_WARNING("SkyEnvironment.SunLight must reference a Directional light; ignoring the link.");
            warnedUnsupportedSunLink = true;
        }
        return;
    }

    const Components::WorldTransform* worldXf =
        world.GetComponent<Components::WorldTransform>(lightEntity);
    if (!worldXf)
        return; // Ensured by TransformHierarchySystem (runs earlier); pick it up next frame.

    float lightSunDir[3];
    ReadLightSunDir(*worldXf, lightSunDir);

    // Adopt the light's world direction as a manual sun override for this frame.
    // Mutating only the local `comp` (which builds this frame's settings) keeps
    // the light the single source of truth and never stamps the component. The
    // implied time of day drives the sky's color-key sampling so the dome tint
    // matches the sun's elevation.
    comp.AutoSunMoon = false;
    comp.SunDirOverride[0] = lightSunDir[0];
    comp.SunDirOverride[1] = lightSunDir[1];
    comp.SunDirOverride[2] = lightSunDir[2];
    comp.TimeOfDayHours = Rendering::SolarHourFromDirection(solarFrame, lightSunDir);
}

} } // namespace GameEngine::Engine::Renderer
