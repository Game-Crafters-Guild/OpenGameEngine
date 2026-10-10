// Built-in post-process EffectDescriptor registrations (PP-ARCH Phases 1-2).
//
// One block per effect, carrying everything the generic consumers read:
//   - FieldIO: ONLY what plain reflection cannot express for scene-IO (load
//     clamps, split-vector / tuple float[3] encodings, enum-as-int on-disk
//     contracts, legacy key renames). Every value mirrors the deleted
//     hand-written schema in BuiltInSceneSchemas.cpp and is locked by the
//     reflection-parity tests in SceneIOTests.cpp before that schema's deletion.
//     Effects that KEEP a hand-written schema (Bloom, CubeLut, HeightFog,
//     VolumetricFog, AtmosphericCloudLayer) register with empty FieldIO — hand
//     schemas win name resolution in SceneSchemaRegistry, so their descriptors
//     are scene-IO-inert and exist for the chrome + settings-write consumers.
//   - Chrome: display name / tooltip / icon / package gate for the Inspector.
//   - SettingsFields: the shader-name -> PostProcessSettings-member tables
//     (successor of the PP_SHARED_FIELDS X-macro) consumed by TryWriteField /
//     TryReadField / ReadableFieldNames, and — since Phase 2 — by the
//     BlendPostProcessSettings fold (each entry carries its blend rule and
//     contributes-group). Volume-core names (exposure, tonemapMode,
//     ditherMode) and the fog-glow group shared by both fog effects stay in
//     PostProcessSettings.cpp appendices.
//   - Gates: the computed "<effect>Active" skipWhen names this effect alone
//     decides, with their predicates. Gates that combine effects live in the
//     core gate table in PostProcessSettings.cpp.
//   - Neutralize/Extract hooks (Phase 2): RenderExtractionSystem folds each
//     effect component into the extracted volume settings via these, in
//     registration order.
//
// CrtEffect and ExposureAdjustmentEffect (collapsed in Phase 0) carry no field
// metadata: the descriptor's presence restores the hand schemas' lowerCamel key
// dialect on save, keeping historical scene text byte-stable.

#include "Engine/Rendering/ColorGradeParamsUBO.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"

#include "ECS/ComponentFactory.h"
#include "ECS/ECS.h" // GetComponentTypeId
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"
#include "Core/Engine.h"
#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/PostProcessVolumeExtract.h"
#include "Engine/Rendering/RenderServices.h"
#include "AssetCore/AssetRegistry.h"

#include "Components/Rendering/PostProcessEffects/AmbientOcclusionEffect.h"
#include "Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h"
#include "Components/Rendering/PostProcessEffects/BloomEffect.h"
#include "Components/Rendering/PostProcessEffects/ChromaticAberrationEffect.h"
#include "Components/Rendering/PostProcessEffects/ColorFilterEffect.h"
#include "Components/Rendering/PostProcessEffects/ColorGradeEffect.h"
#include "Components/Rendering/PostProcessEffects/ContrastAdaptiveSharpenEffect.h"
#include "Components/Rendering/PostProcessEffects/CrtEffect.h"
#include "Components/Rendering/PostProcessEffects/CubeLutEffect.h"
#include "Components/Rendering/PostProcessEffects/DebandEffect.h"
#include "Components/Rendering/PostProcessEffects/DepthOfFieldEffect.h"
#include "Components/Rendering/PostProcessEffects/ExposureAdjustmentEffect.h"
#include "Components/Rendering/PostProcessEffects/FastBlurEffect.h"
#include "Components/Rendering/PostProcessEffects/FilmSimulationEffect.h"
#include "Components/Rendering/PostProcessEffects/HeatDistortionEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffectPresets.h"
#include "Components/Rendering/PostProcessEffects/ScreenSpaceReflectionsEffect.h"
#include "Components/Rendering/PostProcessEffects/ShadowSettingsEffect.h"
#include "Components/Rendering/PostProcessEffects/VhsEffect.h"
#include "Components/Rendering/PostProcessEffects/VolumetricClouds.h"
#include "Components/Transform.h"
#include "Components/Rendering/PostProcessEffects/VignetteEffect.h"
#include "Components/Rendering/PostProcessEffects/VolumetricFogEffect.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <type_traits>

namespace GameEngine::Rendering
{
namespace
{
// FLT_MAX upper bound == a min()-only clamp.
constexpr float kNoMax = std::numeric_limits<float>::max();

// Resolve the effect's type id AND register its default bytes with the
// type-erased ComponentFactory. The volume inspector's Add Post FX picker
// creates effects by type id (ComponentFactory::Create), and this registration
// is what makes that succeed: nothing else registers these components natively.
// (The managed side re-registers reflected components with the factory when the
// CLR is hosted, which used to mask this on desktop — a CLR-less target such as
// the web editor gets its factory entries only from here.) addable=true matches
// the managed registration; the Add Component menu still excludes effects by
// descriptor lookup, so they add only through the volume picker.
template <class T>
ECS::ComponentTypeId RegisterEffectComponent()
{
    const ECS::ComponentTypeId id = ECS::GetComponentTypeId<T>();
    const T defaults{};
    ECS::ComponentFactory::RegisterDefaultBytes(id, &defaults, sizeof(T), /*addable=*/true);
    return id;
}

using PPS = Engine::Renderer::PostProcessSettings;
static_assert(std::is_standard_layout_v<PPS>,
              "PostProcessSettings must stay standard-layout: the settings-write "
              "tables address its members by offsetof");
static_assert(sizeof(PPS) <= std::numeric_limits<std::uint16_t>::max(),
              "EffectSettingsField::Offset is uint16_t and must be able to "
              "address every PostProcessSettings member");

// Field kind derived from the member's declared type, so a table entry cannot
// mistype a member (int written as float or vice versa).
template <class T>
consteval EffectSettingsField::Kind SettingsKindFor()
{
    static_assert(std::is_same_v<T, float32> || std::is_same_v<T, int32>,
                  "settings-write tables carry only float32/int32 members");
    return std::is_same_v<T, float32> ? EffectSettingsField::Kind::Float32
                                      : EffectSettingsField::Kind::Int32;
}

// Default blend rule derived from the member's type: every continuous (float)
// control lerps, every discrete (int enum/toggle/count) control dominant-picks
// (w >= 0.5 takes b — the higher-priority volume in the ascending fold). The
// shipped ladder followed this convention without exception; the exceptions
// that exist are Skip entries (per-view derived or camera-stamped members),
// declared explicitly below.
template <class T>
consteval SettingsBlendRule SettingsRuleFor()
{
    return std::is_same_v<T, float32> ? SettingsBlendRule::Lerp : SettingsBlendRule::Dominant;
}

#define PP_FIELD(shaderName, Member, rule, group)                                 \
    EffectSettingsField{shaderName,                                               \
                        static_cast<std::uint16_t>(offsetof(PPS, Member)),        \
                        SettingsKindFor<decltype(PPS::Member)>(), rule, group}

// Shader-visible member, type-derived blend rule.
#define PP_SETTING(shaderName, Member) \
    PP_FIELD(shaderName, Member, SettingsRuleFor<decltype(PPS::Member)>(), SettingsBlendGroup::Always)
#define PP_SETTING_IN(shaderName, Member, group) \
    PP_FIELD(shaderName, Member, SettingsRuleFor<decltype(PPS::Member)>(), SettingsBlendGroup::group)
// Shader-visible member the volume blend must NOT touch.
#define PP_SETTING_SKIP(shaderName, Member) \
    PP_FIELD(shaderName, Member, SettingsBlendRule::Skip, SettingsBlendGroup::Always)
// Blend-only member: folded across volumes, no shader reads it by name.
#define PP_BLEND(Member) \
    PP_FIELD("", Member, SettingsRuleFor<decltype(PPS::Member)>(), SettingsBlendGroup::Always)
#define PP_BLEND_IN(Member, group) \
    PP_FIELD("", Member, SettingsRuleFor<decltype(PPS::Member)>(), SettingsBlendGroup::group)
// Member that neither blends nor pushes by name — listed so the equivalence
// coverage stays total (per-view derived outputs).
#define PP_BLEND_SKIP(Member) \
    PP_FIELD("", Member, SettingsBlendRule::Skip, SettingsBlendGroup::Always)

// --- Height-fog fold helpers (moved from RenderExtractionSystem with the
// Extract hooks; the axis resolve + time-of-day preset application belong to
// the effect's fold, not the extraction walk) ----------------------------

void NormalizeAxis3(float& x, float& y, float& z)
{
    const float len2 = x * x + y * y + z * z;
    if (len2 <= 1e-12f)
    {
        x = 0.0f;
        y = 1.0f;
        z = 0.0f;
        return;
    }
    const float invLen = 1.0f / std::sqrt(len2);
    x *= invLen;
    y *= invLen;
    z *= invLen;
}

void ResolveHeightFogAxis(const GameEngine::Components::HeightFogEffect& effect, float outAxis[3])
{
    using GameEngine::Components::HeightFogAxisMode;
    switch (effect.AxisMode)
    {
    case HeightFogAxisMode::WorldX:
        outAxis[0] = 1.0f;
        outAxis[1] = 0.0f;
        outAxis[2] = 0.0f;
        break;
    case HeightFogAxisMode::WorldZ:
        outAxis[0] = 0.0f;
        outAxis[1] = 0.0f;
        outAxis[2] = 1.0f;
        break;
    case HeightFogAxisMode::Custom:
        outAxis[0] = effect.CustomAxis[0];
        outAxis[1] = effect.CustomAxis[1];
        outAxis[2] = effect.CustomAxis[2];
        NormalizeAxis3(outAxis[0], outAxis[1], outAxis[2]);
        break;
    case HeightFogAxisMode::WorldY:
    default:
        outAxis[0] = 0.0f;
        outAxis[1] = 1.0f;
        outAxis[2] = 0.0f;
        break;
    }
}


void CopyHeightFogEffectToSettings(
    const GameEngine::Components::HeightFogEffect& src,
    const GameEngine::Engine::Renderer::PostProcessExtractContext& ctx,
    GameEngine::Engine::Renderer::PostProcessSettings& settings)
{
    using HF = GameEngine::Components::HeightFogEffect;
    HF effect = src;
    if (effect.UseTimeOfDay)
        GameEngine::Components::ApplyHeightFogTimeOfDay(effect, ctx.TimeOfDayHours, ctx.DayKeyTimesHours);

    settings.HeightFogIntensity = std::clamp(effect.Intensity, 0.0f, 1.0f);
    settings.HeightFogDensity = std::clamp(effect.Density, 0.0f, 1.0f);
    settings.HeightFogMaxOpacity = std::clamp(effect.MaxOpacity, 0.0f, 1.0f);
    settings.HeightFogMinDistance = std::max(effect.MinDistance, 0.0f);
    settings.HeightFogSmoothLength = std::max(effect.SmoothLength, 0.0001f);
    settings.HeightFogBaseHeight = effect.BaseHeight;
    settings.HeightFogTransitionLength = std::max(effect.TransitionLength, 0.01f);
    settings.HeightFogEmissiveR = std::max(effect.Emissive[0], 0.0f);
    settings.HeightFogEmissiveG = std::max(effect.Emissive[1], 0.0f);
    settings.HeightFogEmissiveB = std::max(effect.Emissive[2], 0.0f);
    settings.HeightFogSunDirX = effect.SunDirection[0];
    settings.HeightFogSunDirY = effect.SunDirection[1];
    settings.HeightFogSunDirZ = effect.SunDirection[2];
    settings.HeightFogSunColorR = std::max(effect.SunColor[0], 0.0f);
    settings.HeightFogSunColorG = std::max(effect.SunColor[1], 0.0f);
    settings.HeightFogSunColorB = std::max(effect.SunColor[2], 0.0f);
    settings.HeightFogSunIntensity = std::max(effect.SunIntensity, 0.0f);
    settings.HeightFogPhase = std::clamp(effect.Phase, -0.95f, 0.95f);
    settings.HeightFogPhaseWeight0 = std::max(effect.PhaseWeight0, 0.0f);
    settings.HeightFogPhaseWeight1 = std::max(effect.PhaseWeight1, 0.0f);
    settings.HeightFogSkyEnabled = effect.SkyEnabled ? 1 : 0;
    settings.HeightFogSkyPower = std::max(effect.SkyPower, 0.001f);
    settings.HeightFogSkyFillStart = std::clamp(effect.SkyFillStart, 0.0f, 1.0f);
    settings.HeightFogSkyFillEnd = std::clamp(effect.SkyFillEnd, 0.0f, 1.0f);
    settings.HeightFogDistanceFogEnabled = effect.DistanceFogEnabled ? 1 : 0;
    settings.HeightFogHeightFogEnabled = effect.HeightFogEnabled ? 1 : 0;
    settings.HeightFogMaxDistance = std::max(effect.MaxDistance, 0.01f);
    settings.HeightFogLayerMode = static_cast<int32_t>(effect.LayerMode);
    settings.HeightFogHorizonHeightOffset = effect.HorizonHeightOffset;
    settings.HeightFogHorizonHeightBlendStart = std::max(effect.HorizonHeightBlendStart, 0.0f);
    settings.HeightFogHorizonHeightBlendEnd = std::max(effect.HorizonHeightBlendEnd, 0.0f);
    settings.HeightFogAxisMode = static_cast<int32_t>(effect.AxisMode);
    float axis[3]{};
    ResolveHeightFogAxis(effect, axis);
    settings.HeightFogAxisX = axis[0];
    settings.HeightFogAxisY = axis[1];
    settings.HeightFogAxisZ = axis[2];
    settings.HeightFogGradientMode = static_cast<int32_t>(effect.GradientMode);
    settings.HeightFogGradientStrength = std::clamp(effect.GradientStrength, 0.0f, 1.0f);
    settings.HeightFogGradientLowR = std::max(effect.GradientLowColor[0], 0.0f);
    settings.HeightFogGradientLowG = std::max(effect.GradientLowColor[1], 0.0f);
    settings.HeightFogGradientLowB = std::max(effect.GradientLowColor[2], 0.0f);
    settings.HeightFogGradientHighR = std::max(effect.GradientHighColor[0], 0.0f);
    settings.HeightFogGradientHighG = std::max(effect.GradientHighColor[1], 0.0f);
    settings.HeightFogGradientHighB = std::max(effect.GradientHighColor[2], 0.0f);
    settings.HeightFogTrackDirectionalLight = effect.TrackDirectionalLight ? 1 : 0;
    settings.HeightFogSunIntensityScale = std::max(effect.SunIntensityScale, 0.0f);
    settings.HeightFogNoiseEnabled = effect.NoiseEnabled ? 1 : 0;
    settings.HeightFogNoiseScale = std::max(effect.NoiseScale, 0.01f);
    settings.HeightFogNoiseStrength = std::clamp(effect.NoiseStrength, 0.0f, 1.0f);
    settings.HeightFogNoiseContrast = std::max(effect.NoiseContrast, 0.05f);
    settings.HeightFogNoiseVelX = effect.NoiseVelocity[0];
    settings.HeightFogNoiseVelY = effect.NoiseVelocity[1];
    settings.HeightFogNoiseVelZ = effect.NoiseVelocity[2];
    settings.HeightFogNoiseMin = std::clamp(effect.NoiseMin, 0.0f, 1.0f);
    settings.HeightFogNoiseMax = std::clamp(effect.NoiseMax, 0.0f, 1.0f);
    settings.HeightFogNoiseFadeStart = std::max(effect.NoiseFadeStart, 0.0f);
    settings.HeightFogNoiseFadeEnd = std::max(effect.NoiseFadeEnd, 0.0f);
    settings.HeightFogUseTimeOfDay = effect.UseTimeOfDay ? 1 : 0;
    settings.HeightFogSkyHorizonOffset = std::clamp(effect.SkyHorizonOffset, -1.0f, 1.0f);
    settings.HeightFogSkyBottomStrength = std::clamp(effect.SkyBottomStrength, 0.0f, 1.0f);

    // A volume can contain both fog components. They intentionally share one
    // post-fog pyramid; the stronger enabled authoring block owns its shape.
    if (src.FogGlowEnabled &&
        (!settings.FogGlowEnabled || src.FogGlowIntensity >= settings.FogGlowIntensity))
    {
        settings.FogGlowEnabled = 1;
        settings.FogGlowQuality = std::clamp(static_cast<int>(src.FogGlowQualityLevel), 0, 3);
        settings.FogGlowIntensity = std::max(src.FogGlowIntensity, 0.0f);
        settings.FogGlowRadius = std::clamp(src.FogGlowRadius, 1.0f, 7.0f);
        settings.FogGlowOctaves = std::clamp(src.FogGlowOctaves, 3, 8);
        settings.FogGlowScatter = std::clamp(src.FogGlowScatter, 0.0f, 1.0f);
        settings.FogGlowThreshold = std::max(src.FogGlowThreshold, 0.0f);
        settings.FogGlowKnee = std::max(src.FogGlowKnee, 0.0f);
        settings.FogGlowFadeStart = std::clamp(src.FogGlowFadeStart, 0.0f, 0.999f);
        settings.FogGlowFadeEnd = std::clamp(src.FogGlowFadeEnd, settings.FogGlowFadeStart + 0.001f, 1.0f);
        settings.FogGlowTintR = std::max(src.FogGlowTint[0], 0.0f);
        settings.FogGlowTintG = std::max(src.FogGlowTint[1], 0.0f);
        settings.FogGlowTintB = std::max(src.FogGlowTint[2], 0.0f);
        settings.FogGlowAntiFlicker = src.FogGlowAntiFlicker ? 1 : 0;
    }
}

// --- Scene-IO field metadata -------------------------------------------------

constexpr EffectFieldIO kAmbientOcclusionIO[]{
    {.FieldName = "Intensity", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = kNoMax},
    {.FieldName = "Radius", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = kNoMax},
    {.FieldName = "Thickness", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = kNoMax},
};

// Load clamps mirror the inspector's edit clamps and the Extract fold, so a
// hand-edited scene cannot feed the traversal a zero-thickness hit test or an
// unbounded step budget. Quality keeps the enum's integer on-disk form.
constexpr EffectFieldIO kScreenSpaceReflectionsIO[]{
    {.FieldName = "Intensity",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = Components::ScreenSpaceReflectionsEffect::kIntensityMax},
    {.FieldName = "MaxDistance", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = kNoMax},
    {.FieldName = "Thickness",
     .HasClamp = true,
     .ClampMin = Components::ScreenSpaceReflectionsEffect::kMinThickness,
     .ClampMax = kNoMax},
    {.FieldName = "EdgeFade",
     .HasClamp = true,
     .ClampMin = Components::ScreenSpaceReflectionsEffect::kMinEdgeFade,
     .ClampMax = Components::ScreenSpaceReflectionsEffect::kMaxEdgeFade},
    {.FieldName = "MaxSteps",
     .HasClamp = true,
     .ClampMin = static_cast<float>(Components::ScreenSpaceReflectionsEffect::kMinSteps),
     .ClampMax = static_cast<float>(Components::ScreenSpaceReflectionsEffect::kMaxSteps)},
    {.FieldName = "SampleQuality",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = static_cast<float>(Components::ScreenSpaceReflectionsEffect::kSampleQualityLast),
     .EnumAsInt = true},
};

// Extraction/env gate range; the 0..16 clamp is pinned by
// SceneIO.Load_DebandEffectThresholdDefaultsAndClamps.
constexpr EffectFieldIO kDebandIO[]{
    {.FieldName = "ThresholdLsb", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 16.0f},
};

constexpr EffectFieldIO kContrastAdaptiveSharpenIO[]{
    {.FieldName = "Strength", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 1.0f},
};

constexpr EffectFieldIO kFastBlurIO[]{
    {.FieldName = "Intensity", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 1.0f},
    {.FieldName = "FocusDistance", .HasClamp = true, .ClampMin = 0.01f, .ClampMax = kNoMax},
    {.FieldName = "FocusRange", .HasClamp = true, .ClampMin = 0.01f, .ClampMax = kNoMax},
    {.FieldName = "MaxRadius", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 24.0f},
};

constexpr EffectFieldIO kHeatDistortionIO[]{
    {.FieldName = "Strength", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = kNoMax},
    {.FieldName = "Scale", .HasClamp = true, .ClampMin = 0.01f, .ClampMax = kNoMax},
    {.FieldName = "MaskStrength", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = kNoMax},
    {.FieldName = "DistanceStart", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = kNoMax},
    {.FieldName = "DistanceEnd", .HasClamp = true, .ClampMin = 0.001f, .ClampMax = kNoMax},
    {.FieldName = "DirectionalFalloff", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = kNoMax},
    {.FieldName = "Softness", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 3.0f},
};

constexpr EffectFieldIO kVignetteIO[]{
    {.FieldName = "Intensity", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 1.0f},
    {.FieldName = "Smoothness", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 1.0f},
    {.FieldName = "Color", .SplitRgb = true},
};

// The hand schema hard-failed the load on an out-of-table blendMode; the
// descriptor clamps into the table instead (load resilience over hard failure).
constexpr EffectFieldIO kColorFilterIO[]{
    {.FieldName = "BlendMode",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = static_cast<float>(Components::kColorFilterBlendModeCount - 1),
     .EnumAsInt = true},
    {.FieldName = "Color", .SplitRgb = true},
};

constexpr EffectFieldIO kColorGradeIO[]{
    {.FieldName = "ShadowsColor", .SplitRgb = true},
    {.FieldName = "MidtonesColor", .SplitRgb = true},
    {.FieldName = "HighlightsColor", .SplitRgb = true},
    {.FieldName = "HueShift",
     .HasClamp = true,
     .ClampMin = -Engine::Renderer::kColorGradeHueShiftRangeDegrees,
     .ClampMax = Engine::Renderer::kColorGradeHueShiftRangeDegrees},
    {.FieldName = "Temperature",
     .HasClamp = true,
     .ClampMin = -Engine::Renderer::kColorGradeTemperatureTintRange,
     .ClampMax = Engine::Renderer::kColorGradeTemperatureTintRange},
    {.FieldName = "Tint",
     .HasClamp = true,
     .ClampMin = -Engine::Renderer::kColorGradeTemperatureTintRange,
     .ClampMax = Engine::Renderer::kColorGradeTemperatureTintRange},
};
// Pre-unification single-range keys: the 3-way model that replaced them has no
// equivalent, so stray scenes that still carry them load with the keys dropped.
constexpr std::string_view kColorGradeDroppedKeys[]{"brightness", "gamma", "hue"};

// DebugMode/DebugAlpha are legacy load-only keys (focus visualization moved to
// Camera): accepted from older scenes, never serialized. DebugMode's 0..1 clamp
// nets the hand schema's !=0 -> 1 normalization for the values scenes carry.
constexpr EffectFieldIO kDepthOfFieldIO[]{
    {.FieldName = "MaxRadius",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = Components::DepthOfFieldEffect::kMaxRadiusMax},
    {.FieldName = "SamplingQuality", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 2.0f, .EnumAsInt = true},
    {.FieldName = "DebugMode", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 1.0f, .SkipSerialize = true},
    {.FieldName = "DebugAlpha", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 1.0f, .SkipSerialize = true},
};

constexpr EffectFieldIO kFilmSimulationIO[]{
    {.FieldName = "HalationTint", .TupleVec3 = true},
    {.FieldName = "GrainMode",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = static_cast<float>(Components::FilmSimulationEffect::kGrainModeLast),
     .EnumAsInt = true},
    {.FieldName = "GateMask",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = static_cast<float>(Components::FilmSimulationEffect::kGateMaskLast),
     .EnumAsInt = true},
};

// A mode above what this build understands (a future mode) falls back to
// Cascades — the only mode with full caster coverage.
constexpr EffectFieldIO kShadowSettingsIO[]{
    {.FieldName = "Mode",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = static_cast<float>(Components::ShadowSettingsEffect::kDirectionalShadowModeLast),
     .EnumAsInt = true,
     .HasEnumFallback = true,
     .EnumFallbackValue = static_cast<std::int64_t>(Components::DirectionalShadowMode::Cascades)},
    {.FieldName = "RayTracedQuality",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = static_cast<float>(Components::ShadowSettingsEffect::kRayTracedShadowQualityLast),
     .EnumAsInt = true,
     .HasEnumFallback = true,
     .EnumFallbackValue = static_cast<std::int64_t>(Components::RayTracedShadowQuality::Performance)},
    {.FieldName = "Filter",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = static_cast<float>(Components::ShadowSettingsEffect::kDirectionalShadowFilterLast),
     .EnumAsInt = true,
     .HasEnumFallback = true,
     .EnumFallbackValue = static_cast<std::int64_t>(Components::DirectionalShadowFilter::PCSS)},
    {.FieldName = "MaxShadowDistance", .HasClamp = true, .ClampMin = 0.01f, .ClampMax = kNoMax},
    {.FieldName = "DistanceFadeFraction", .HasClamp = true, .ClampMin = 0.0f,
     .ClampMax = Components::ShadowSettingsEffect::kDistanceFadeFractionMax},
    {.FieldName = "SplitLambda", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 1.0f},
    {.FieldName = "DepthBias", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 0.01f},
    {.FieldName = "NormalBias", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 5.0f},
    {.FieldName = "ScreenSpaceShadowThickness", .HasClamp = true, .ClampMin = 0.0001f, .ClampMax = 0.05f},
};

constexpr EffectFieldIO kChromaticAberrationIO[]{
    {.FieldName = "Intensity",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = Components::ChromaticAberrationEffect::kIntensityMax},
    {.FieldName = "StartOffset", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 1.0f},
    {.FieldName = "Saturation", .HasClamp = true, .ClampMin = 0.0f, .ClampMax = 2.0f},
    {.FieldName = "LongitudinalIntensity",
     .SerializedKey = "longitudinal",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = Components::ChromaticAberrationEffect::kLongitudinalMax},
    {.FieldName = "ComaIntensity",
     .SerializedKey = "coma",
     .HasClamp = true,
     .ClampMin = 0.0f,
     .ClampMax = Components::ChromaticAberrationEffect::kComaMax},
};

// --- Settings-write tables (was PP_SHARED_FIELDS) ---------------------------
// "threshold" / "knee" / "intensity" are bloom's legacy shader-side names,
// preserved verbatim: the shader is the source of truth for these keys.

constexpr EffectSettingsField kBloomSettings[]{
    PP_SETTING("threshold", BloomThreshold),
    PP_SETTING("knee", BloomKnee),
    PP_SETTING("bloomAntiFlicker", BloomAntiFlicker),
    PP_SETTING("intensity", BloomIntensity),
    PP_SETTING("bloomScatteringAmount", BloomScatteringAmount),
    PP_SETTING("bloomTintR", BloomTintR),
    PP_SETTING("bloomTintG", BloomTintG),
    PP_SETTING("bloomTintB", BloomTintB),
    PP_SETTING("bloomRadius", BloomRadius),
    PP_SETTING("bloomOctaves", BloomOctaves),
    // Per-view reconstruction scale: ResolveBloomPyramid derives it from render
    // height after blending; pushed via the write-only appendix, never blended.
    PP_BLEND_SKIP(BloomSampleScale),
    PP_BLEND_SKIP(BloomOctaveBlend),
    PP_SETTING("bloomScatter", BloomScatter),
    PP_SETTING("bloomDepthVeilEnabled", BloomDepthVeilEnabled),
    PP_SETTING("bloomDepthVeilIntensity", BloomDepthVeilIntensity),
    PP_SETTING("bloomDepthVeilStart", BloomDepthVeilStart),
    PP_SETTING("bloomDepthVeilEnd", BloomDepthVeilEnd),
    PP_SETTING("bloomDepthVeilTintR", BloomDepthVeilTintR),
    PP_SETTING("bloomDepthVeilTintG", BloomDepthVeilTintG),
    PP_SETTING("bloomDepthVeilTintB", BloomDepthVeilTintB),
    PP_SETTING("bloomLensDirtEnabled", BloomLensDirtEnabled),
    PP_SETTING("bloomLensDirtVignette", BloomLensDirtVignette),
    PP_SETTING("bloomLensDirtVignetteIntensity", BloomLensDirtVignetteIntensity),
    PP_SETTING("bloomLensDirtVignetteRadius", BloomLensDirtVignetteRadius),
    PP_SETTING("bloomLensDirtVignetteSmoothness", BloomLensDirtVignetteSmoothness),
    PP_SETTING("bloomLensDirtVignetteRounded", BloomLensDirtVignetteRounded),
    PP_SETTING("bloomLensDirtVignetteColorR", BloomLensDirtVignetteColorR),
    PP_SETTING("bloomLensDirtVignetteColorG", BloomLensDirtVignetteColorG),
    PP_SETTING("bloomLensDirtVignetteColorB", BloomLensDirtVignetteColorB),
    PP_SETTING("bloomLensDirtIntensity", BloomLensDirtIntensity),
    PP_SETTING("bloomLensDirtScatter", BloomLensDirtScatter),
};

// Halation is film simulation's bloom-adjacent lobe; its settings ride this table.
constexpr EffectSettingsField kFilmSimulationSettings[]{
    PP_SETTING("halationIntensity", HalationIntensity),
    PP_SETTING("halationRadius", HalationRadius),
    PP_SETTING("halationTintR", HalationTintR),
    PP_SETTING("halationTintG", HalationTintG),
    PP_SETTING("halationTintB", HalationTintB),
    PP_SETTING("filmSimulationFrameRate", FilmSimulationFrameRate),
    PP_SETTING("filmSimulationGrainMode", FilmSimulationGrainMode),
    PP_SETTING("filmSimulationGrainIntensity", FilmSimulationGrainIntensity),
    PP_SETTING("filmSimulationGrainSize", FilmSimulationGrainSize),
    PP_SETTING("filmSimulationGrainSmooth", FilmSimulationGrainSmooth),
    PP_SETTING("filmSimulationGrainDensity", FilmSimulationGrainDensity),
    PP_SETTING("filmSimulationGrainShadowResponse", FilmSimulationGrainShadowResponse),
    PP_SETTING("filmSimulationGrainMidtoneResponse", FilmSimulationGrainMidtoneResponse),
    PP_SETTING("filmSimulationGrainHighlightResponse", FilmSimulationGrainHighlightResponse),
    PP_SETTING("filmSimulationGrainColored", FilmSimulationGrainColored),
    PP_SETTING("filmSimulationHairEnabled", FilmSimulationHairEnabled),
    PP_SETTING("filmSimulationHairAmount", FilmSimulationHairAmount),
    PP_SETTING("filmSimulationHairIntensity", FilmSimulationHairIntensity),
    PP_SETTING("filmSimulationHairWidth", FilmSimulationHairWidth),
    PP_SETTING("filmSimulationHairLength", FilmSimulationHairLength),
    PP_SETTING("filmSimulationHairRandomSize", FilmSimulationHairRandomSize),
    PP_SETTING("filmSimulationHairCurl", FilmSimulationHairCurl),
    PP_SETTING("filmSimulationHairCurlRandomness", FilmSimulationHairCurlRandomness),
    PP_SETTING("filmSimulationScratchesEnabled", FilmSimulationScratchesEnabled),
    PP_SETTING("filmSimulationScratchAmount", FilmSimulationScratchAmount),
    PP_SETTING("filmSimulationScratchIntensity", FilmSimulationScratchIntensity),
    PP_SETTING("filmSimulationScratchWidth", FilmSimulationScratchWidth),
    PP_SETTING("filmSimulationScratchLength", FilmSimulationScratchLength),
    PP_SETTING("filmSimulationDustEnabled", FilmSimulationDustEnabled),
    PP_SETTING("filmSimulationDustAmount", FilmSimulationDustAmount),
    PP_SETTING("filmSimulationDustIntensity", FilmSimulationDustIntensity),
    PP_SETTING("filmSimulationDustSize", FilmSimulationDustSize),
    PP_SETTING("filmSimulationDustRandomSize", FilmSimulationDustRandomSize),
    PP_SETTING("filmSimulationGateWeaveEnabled", FilmSimulationGateWeaveEnabled),
    PP_SETTING("filmSimulationGateWeaveHorizontal", FilmSimulationGateWeaveHorizontal),
    PP_SETTING("filmSimulationGateWeaveVertical", FilmSimulationGateWeaveVertical),
    PP_SETTING("filmSimulationGateWeaveRotation", FilmSimulationGateWeaveRotation),
    PP_SETTING("filmSimulationGateMask", FilmSimulationGateMask),
    PP_SETTING("filmSimulationGateMaskFeather", FilmSimulationGateMaskFeather),
    PP_SETTING("filmSimulationGateMaskRoundness", FilmSimulationGateMaskRoundness),
};

constexpr EffectSettingsField kChromaticAberrationSettings[]{
    PP_SETTING("chromaticAberrationIntensity", ChromaticAberrationIntensity),
    PP_SETTING("chromaticAberrationStartOffset", ChromaticAberrationStartOffset),
    PP_SETTING("chromaticAberrationSaturation", ChromaticAberrationSaturation),
    PP_SETTING("chromaticAberrationLongitudinal", ChromaticAberrationLongitudinal),
    PP_SETTING("chromaticAberrationComa", ChromaticAberrationComa),
};

// Focus/lens members are camera-stamped after blending but remain part of the
// dof push-constant surface, so they live here with the volume-authored pair.
constexpr EffectSettingsField kDepthOfFieldSettings[]{
    PP_SETTING("dofIntensity", DofIntensity),
    PP_SETTING("dofMaxRadius", DofMaxRadius),
    PP_SETTING("dofSamplingQuality", DofSamplingQuality),
    PP_SETTING("dofDebugMode", DofDebugMode),
    PP_SETTING("dofDebugAlpha", DofDebugAlpha),
    PP_SETTING_SKIP("dofFocusDistance", DofFocusDistance),
    PP_SETTING_SKIP("dofFocalLengthMm", DofFocalLengthMm),
    PP_SETTING_SKIP("dofAperture", DofAperture),
    PP_SETTING_SKIP("dofSensorHeightMm", DofSensorHeightMm),
    PP_SETTING_SKIP("dofApertureBladeCount", DofApertureBladeCount),
    PP_SETTING_SKIP("dofApertureRoundness", DofApertureRoundness),
    PP_SETTING_SKIP("dofApertureRotation", DofApertureRotation),
    PP_SETTING_SKIP("dofAnamorphicSqueeze", DofAnamorphicSqueeze),
};

constexpr EffectSettingsField kColorFilterSettings[]{
    PP_SETTING("colorFilterR", ColorFilterR),
    PP_SETTING("colorFilterG", ColorFilterG),
    PP_SETTING("colorFilterB", ColorFilterB),
    PP_SETTING("colorFilterIntensity", ColorFilterIntensity),
    PP_SETTING("colorFilterBlendMode", ColorFilterBlendMode),
    PP_SETTING("colorFilterStackOrder", ColorFilterStackOrder),
};

constexpr EffectSettingsField kContrastAdaptiveSharpenSettings[]{
    PP_SETTING("casStrength", CasStrength),
    PP_SETTING("casStackOrder", CasStackOrder),
};

constexpr EffectSettingsField kCubeLutSettings[]{
    PP_SETTING("lutIntensity", LutIntensity),
    PP_SETTING("lutStackOrder", LutStackOrder),
    PP_SETTING("lutInputEncoding", LutInputEncoding),
    PP_SETTING("lutTextureFormat", LutTextureFormat),
};

constexpr EffectSettingsField kVignetteSettings[]{
    PP_SETTING("vignetteIntensity", VignetteIntensity),
    PP_SETTING("vignetteSmoothness", VignetteSmoothness),
    PP_SETTING("vignetteRounded", VignetteRounded),
    PP_SETTING("vignetteColorR", VignetteColorR),
    PP_SETTING("vignetteColorG", VignetteColorG),
    PP_SETTING("vignetteColorB", VignetteColorB),
    PP_SETTING("vignetteStackOrder", VignetteStackOrder),
};

constexpr EffectSettingsField kCrtSettings[]{
    PP_SETTING("crtIntensity", CrtIntensity),
    PP_SETTING("crtCurvature", CrtCurvature),
    PP_SETTING("crtScanlines", CrtScanlines),
    PP_SETTING("crtVignette", CrtVignette),
    PP_SETTING("crtAberration", CrtAberration),
    PP_SETTING("crtSoftness", CrtSoftness),
    PP_SETTING("crtExposureCompensation", CrtExposureCompensation),
    PP_SETTING("crtEmulatedResolutionDiv", CrtEmulatedResolutionDiv),
};

constexpr EffectSettingsField kVhsSettings[]{
    PP_SETTING("vhsIntensity", VhsIntensity),
    PP_SETTING("vhsWobble", VhsWobble),
    PP_SETTING("vhsTracking", VhsTracking),
    PP_SETTING("vhsSignalGlitches", VhsSignalGlitches),
    PP_SETTING("vhsGlitchOffsets", VhsGlitchOffsets),
    PP_SETTING("vhsInterference", VhsInterference),
    PP_SETTING("vhsFrameFeedback", VhsFrameFeedback),
    PP_SETTING("vhsFeedbackDecay", VhsFeedbackDecay),
    PP_SETTING("vhsFeedbackMotionThreshold", VhsFeedbackMotionThreshold),
    PP_SETTING("vhsFeedbackTrailLength", VhsFeedbackTrailLength),
    PP_FIELD("vhsCompositeSignalMode", VhsCompositeSignalMode,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_SETTING("vhsDotCrawl", VhsDotCrawl),
    PP_SETTING("vhsColorBleed", VhsColorBleed),
    PP_SETTING("vhsColorBleedOffset", VhsColorBleedOffset),
    PP_SETTING("vhsTapeNoise", VhsTapeNoise),
    PP_SETTING("vhsChromaStreaks", VhsChromaStreaks),
    PP_SETTING("vhsPreFilterChromaStreaks", VhsPreFilterChromaStreaks),
    PP_SETTING("vhsDropouts", VhsDropouts),
    PP_SETTING("vhsRfDropouts", VhsRfDropouts),
    PP_SETTING("vhsScanlines", VhsScanlines),
    PP_SETTING("vhsSpeed", VhsSpeed),
    PP_FIELD("vhsOverlayEnabled", VhsOverlayEnabled,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_SETTING("vhsOverlayColorR", VhsOverlayColorR),
    PP_SETTING("vhsOverlayColorG", VhsOverlayColorG),
    PP_SETTING("vhsOverlayColorB", VhsOverlayColorB),
    PP_SETTING("vhsOverlayOpacity", VhsOverlayOpacity),
    PP_SETTING("vhsOverlaySize", VhsOverlaySize),
    PP_FIELD("vhsOverlayFont", VhsOverlayFont,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_SETTING("vhsOverlayPositionX", VhsOverlayPositionX),
    PP_SETTING("vhsOverlayPositionY", VhsOverlayPositionY),
    PP_FIELD("vhsOverlayTextLength", VhsOverlayTextLength,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsOverlayText0", VhsOverlayText0,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsOverlayText1", VhsOverlayText1,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsOverlayText2", VhsOverlayText2,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsOverlayText3", VhsOverlayText3,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsOverlayText4", VhsOverlayText4,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsOverlayText5", VhsOverlayText5,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsOverlayText6", VhsOverlayText6,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsOverlayText7", VhsOverlayText7,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsDateBurnEnabled", VhsDateBurnEnabled,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_SETTING("vhsDateBurnColorR", VhsDateBurnColorR),
    PP_SETTING("vhsDateBurnColorG", VhsDateBurnColorG),
    PP_SETTING("vhsDateBurnColorB", VhsDateBurnColorB),
    PP_SETTING("vhsDateBurnSize", VhsDateBurnSize),
    PP_SETTING("vhsDateBurnPositionX", VhsDateBurnPositionX),
    PP_SETTING("vhsDateBurnPositionY", VhsDateBurnPositionY),
    PP_FIELD("vhsDateBurnYear", VhsDateBurnYear,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsDateBurnMonth", VhsDateBurnMonth,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsDateBurnDay", VhsDateBurnDay,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsDateBurnHour", VhsDateBurnHour,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsDateBurnMinute", VhsDateBurnMinute,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_FIELD("vhsTransportMode", VhsTransportMode,
             SettingsBlendRule::Dominant, SettingsBlendGroup::Always),
    PP_SETTING("vhsTransportStrength", VhsTransportStrength),
};

constexpr EffectSettingsField kFastBlurSettings[]{
    PP_SETTING("fastBlurIntensity", FastBlurIntensity),
    PP_SETTING("fastBlurFocusDistance", FastBlurFocusDistance),
    PP_SETTING("fastBlurFocusRange", FastBlurFocusRange),
    PP_SETTING("fastBlurMaxRadius", FastBlurMaxRadius),
    PP_SETTING("fastBlurNearBlur", FastBlurNearBlur),
};

constexpr EffectSettingsField kHeatDistortionSettings[]{
    PP_SETTING("heatDistortionStrength", HeatDistortionStrength),
    PP_SETTING("heatDistortionSpeed", HeatDistortionSpeed),
    PP_SETTING("heatDistortionScale", HeatDistortionScale),
    PP_SETTING("heatDistortionMaskStrength", HeatDistortionMaskStrength),
    PP_SETTING("heatDistortionDistanceStart", HeatDistortionDistanceStart),
    PP_SETTING("heatDistortionDistanceEnd", HeatDistortionDistanceEnd),
    PP_SETTING("heatDistortionDirectionalFalloff", HeatDistortionDirectionalFalloff),
    PP_SETTING("heatDistortionUseAbsoluteY", HeatDistortionUseAbsoluteY),
    PP_SETTING("heatDistortionSoftness", HeatDistortionSoftness),
};

// Every HeightFog member belongs to the HeightFog contributes-group: a volume
// without a HeightFogEffect keeps a's values for the whole block. Members past
// the named ones are blend-only — the height-fog UBO fill and the TryWriteField
// appendix read them from the resolved struct without a registry name.
constexpr EffectSettingsField kHeightFogSettings[]{
    PP_SETTING_IN("heightFogIntensity", HeightFogIntensity, HeightFog),
    PP_SETTING_IN("heightFogDensity", HeightFogDensity, HeightFog),
    PP_SETTING_IN("heightFogMaxOpacity", HeightFogMaxOpacity, HeightFog),
    PP_SETTING_IN("heightFogMinDistance", HeightFogMinDistance, HeightFog),
    PP_SETTING_IN("heightFogSmoothLength", HeightFogSmoothLength, HeightFog),
    PP_SETTING_IN("heightFogBaseHeight", HeightFogBaseHeight, HeightFog),
    PP_SETTING_IN("heightFogTransitionLength", HeightFogTransitionLength, HeightFog),
    PP_SETTING_IN("heightFogHorizonHeightOffset", HeightFogHorizonHeightOffset, HeightFog),
    PP_SETTING_IN("heightFogHorizonHeightBlendStart", HeightFogHorizonHeightBlendStart, HeightFog),
    PP_SETTING_IN("heightFogHorizonHeightBlendEnd", HeightFogHorizonHeightBlendEnd, HeightFog),
    PP_SETTING_IN("heightFogSunIntensity", HeightFogSunIntensity, HeightFog),
    PP_SETTING_IN("heightFogSkyEnabled", HeightFogSkyEnabled, HeightFog),
    PP_SETTING_IN("heightFogSkyHorizonOffset", HeightFogSkyHorizonOffset, HeightFog),
    PP_SETTING_IN("heightFogSkyBottomStrength", HeightFogSkyBottomStrength, HeightFog),
    PP_SETTING_IN("heightFogLayerMode", HeightFogLayerMode, HeightFog),
    PP_SETTING_IN("heightFogNoiseMin", HeightFogNoiseMin, HeightFog),
    PP_SETTING_IN("heightFogNoiseMax", HeightFogNoiseMax, HeightFog),
    PP_SETTING_IN("heightFogNoiseFadeStart", HeightFogNoiseFadeStart, HeightFog),
    PP_SETTING_IN("heightFogNoiseFadeEnd", HeightFogNoiseFadeEnd, HeightFog),
    PP_BLEND_IN(HeightFogEmissiveR, HeightFog),
    PP_BLEND_IN(HeightFogEmissiveG, HeightFog),
    PP_BLEND_IN(HeightFogEmissiveB, HeightFog),
    PP_BLEND_IN(HeightFogSunDirX, HeightFog),
    PP_BLEND_IN(HeightFogSunDirY, HeightFog),
    PP_BLEND_IN(HeightFogSunDirZ, HeightFog),
    PP_BLEND_IN(HeightFogSunColorR, HeightFog),
    PP_BLEND_IN(HeightFogSunColorG, HeightFog),
    PP_BLEND_IN(HeightFogSunColorB, HeightFog),
    PP_BLEND_IN(HeightFogPhase, HeightFog),
    PP_BLEND_IN(HeightFogPhaseWeight0, HeightFog),
    PP_BLEND_IN(HeightFogPhaseWeight1, HeightFog),
    PP_BLEND_IN(HeightFogSkyPower, HeightFog),
    PP_BLEND_IN(HeightFogSkyFillStart, HeightFog),
    PP_BLEND_IN(HeightFogSkyFillEnd, HeightFog),
    PP_BLEND_IN(HeightFogDistanceFogEnabled, HeightFog),
    PP_BLEND_IN(HeightFogHeightFogEnabled, HeightFog),
    PP_BLEND_IN(HeightFogMaxDistance, HeightFog),
    PP_BLEND_IN(HeightFogAxisMode, HeightFog),
    PP_BLEND_IN(HeightFogAxisX, HeightFog),
    PP_BLEND_IN(HeightFogAxisY, HeightFog),
    PP_BLEND_IN(HeightFogAxisZ, HeightFog),
    PP_BLEND_IN(HeightFogGradientMode, HeightFog),
    PP_BLEND_IN(HeightFogGradientStrength, HeightFog),
    PP_BLEND_IN(HeightFogGradientLowR, HeightFog),
    PP_BLEND_IN(HeightFogGradientLowG, HeightFog),
    PP_BLEND_IN(HeightFogGradientLowB, HeightFog),
    PP_BLEND_IN(HeightFogGradientHighR, HeightFog),
    PP_BLEND_IN(HeightFogGradientHighG, HeightFog),
    PP_BLEND_IN(HeightFogGradientHighB, HeightFog),
    PP_BLEND_IN(HeightFogTrackDirectionalLight, HeightFog),
    PP_BLEND_IN(HeightFogSunIntensityScale, HeightFog),
    PP_BLEND_IN(HeightFogNoiseEnabled, HeightFog),
    PP_BLEND_IN(HeightFogNoiseScale, HeightFog),
    PP_BLEND_IN(HeightFogNoiseStrength, HeightFog),
    PP_BLEND_IN(HeightFogNoiseContrast, HeightFog),
    PP_BLEND_IN(HeightFogNoiseVelX, HeightFog),
    PP_BLEND_IN(HeightFogNoiseVelY, HeightFog),
    PP_BLEND_IN(HeightFogNoiseVelZ, HeightFog),
    PP_BLEND_IN(HeightFogUseTimeOfDay, HeightFog),
};

constexpr EffectSettingsField kColorGradeSettings[]{
    PP_SETTING_IN("colorGradeShadowsR", ColorGradeShadowsR, ColorGrade),
    PP_SETTING_IN("colorGradeShadowsG", ColorGradeShadowsG, ColorGrade),
    PP_SETTING_IN("colorGradeShadowsB", ColorGradeShadowsB, ColorGrade),
    PP_SETTING_IN("colorGradeShadowsMaster", ColorGradeShadowsMaster, ColorGrade),
    PP_SETTING_IN("colorGradeMidtonesR", ColorGradeMidtonesR, ColorGrade),
    PP_SETTING_IN("colorGradeMidtonesG", ColorGradeMidtonesG, ColorGrade),
    PP_SETTING_IN("colorGradeMidtonesB", ColorGradeMidtonesB, ColorGrade),
    PP_SETTING_IN("colorGradeMidtonesMaster", ColorGradeMidtonesMaster, ColorGrade),
    PP_SETTING_IN("colorGradeHighlightsR", ColorGradeHighlightsR, ColorGrade),
    PP_SETTING_IN("colorGradeHighlightsG", ColorGradeHighlightsG, ColorGrade),
    PP_SETTING_IN("colorGradeHighlightsB", ColorGradeHighlightsB, ColorGrade),
    PP_SETTING_IN("colorGradeHighlightsMaster", ColorGradeHighlightsMaster, ColorGrade),
    PP_SETTING_IN("colorGradeContrast", ColorGradeContrast, ColorGrade),
    PP_SETTING_IN("colorGradeSaturation", ColorGradeSaturation, ColorGrade),
    PP_SETTING_IN("colorGradeHueShift", ColorGradeHueShift, ColorGrade),
    PP_SETTING_IN("colorGradeTemperature", ColorGradeTemperature, ColorGrade),
    PP_SETTING_IN("colorGradeTint", ColorGradeTint, ColorGrade),
    PP_SETTING_IN("colorGradeInLog", ColorGradeInLog, ColorGrade),
    PP_SETTING_IN("colorGradeShadowsStart", ColorGradeShadowsStart, ColorGrade),
    PP_SETTING_IN("colorGradeShadowsEnd", ColorGradeShadowsEnd, ColorGrade),
    PP_SETTING_IN("colorGradeHighlightsStart", ColorGradeHighlightsStart, ColorGrade),
    PP_SETTING_IN("colorGradeHighlightsEnd", ColorGradeHighlightsEnd, ColorGrade),
};

constexpr EffectSettingsField kAtmosphericCloudSettings[]{
    PP_SETTING_IN("atmosSkyFill", AtmosphericCloudSkyFill, AtmosphericCloud),
    PP_SETTING_IN("atmosVaporMass", AtmosphericCloudVaporMass, AtmosphericCloud),
    PP_SETTING_IN("atmosCloudColorR", AtmosphericCloudColorR, AtmosphericCloud),
    PP_SETTING_IN("atmosCloudColorG", AtmosphericCloudColorG, AtmosphericCloud),
    PP_SETTING_IN("atmosCloudColorB", AtmosphericCloudColorB, AtmosphericCloud),
    PP_SETTING_IN("atmosOpacity", AtmosphericCloudOpacity, AtmosphericCloud),
    PP_SETTING_IN("atmosFloorHeight", AtmosphericCloudFloorHeight, AtmosphericCloud),
    PP_SETTING_IN("atmosLayerDepth", AtmosphericCloudLayerDepth, AtmosphericCloud),
    PP_SETTING_IN("atmosBodyFrequency", AtmosphericCloudBodyFrequency, AtmosphericCloud),
    PP_SETTING_IN("atmosEdgeFrequency", AtmosphericCloudEdgeFrequency, AtmosphericCloud),
    PP_SETTING_IN("atmosEdgeBreakup", AtmosphericCloudEdgeBreakup, AtmosphericCloud),
    PP_SETTING_IN("atmosDriftAngle", AtmosphericCloudDriftAngle, AtmosphericCloud),
    PP_SETTING_IN("atmosDriftRate", AtmosphericCloudDriftRate, AtmosphericCloud),
    PP_SETTING_IN("atmosSunFade", AtmosphericCloudSunFade, AtmosphericCloud),
    PP_SETTING_IN("atmosSkyBounce", AtmosphericCloudSkyBounce, AtmosphericCloud),
    PP_SETTING_IN("atmosRimBoost", AtmosphericCloudRimBoost, AtmosphericCloud),
    PP_SETTING_IN("atmosOcclusion", AtmosphericCloudOcclusion, AtmosphericCloud),
    PP_SETTING_IN("atmosHistoryWeight", AtmosphericCloudHistoryWeight, AtmosphericCloud),
    PP_SETTING_IN("atmosPixelScale", AtmosphericCloudPixelScale, AtmosphericCloud),
};

// Raymarched volumetric clouds are consumed through the CloudParams UBO
// (VolumetricCloudsParamsUploadNode), not named push constants, so the whole
// block is blend-only. Bounds members are stamped by the Extract hook from the
// owning volume entity's world transform, not authored on the component.
constexpr EffectSettingsField kVolumetricCloudsSettings[]{
    PP_BLEND_IN(CloudsRadius, VolumetricClouds),
    PP_BLEND_IN(CloudsAltitude, VolumetricClouds),
    PP_BLEND_IN(CloudsThickness, VolumetricClouds),
    PP_BLEND_IN(CloudsNumStepsLight, VolumetricClouds),
    PP_BLEND_IN(CloudsStepSize, VolumetricClouds),
    PP_BLEND_IN(CloudsRayOffsetStrength, VolumetricClouds),
    PP_BLEND_IN(CloudsScale, VolumetricClouds),
    PP_BLEND_IN(CloudsDensityMultiplier, VolumetricClouds),
    PP_BLEND_IN(CloudsDensityOffset, VolumetricClouds),
    PP_BLEND_IN(CloudsShapeOffsetX, VolumetricClouds),
    PP_BLEND_IN(CloudsShapeOffsetY, VolumetricClouds),
    PP_BLEND_IN(CloudsShapeOffsetZ, VolumetricClouds),
    PP_BLEND_IN(CloudsShapeWeightR, VolumetricClouds),
    PP_BLEND_IN(CloudsShapeWeightG, VolumetricClouds),
    PP_BLEND_IN(CloudsShapeWeightB, VolumetricClouds),
    PP_BLEND_IN(CloudsShapeWeightA, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailScale, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailWeight, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailWeightR, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailWeightG, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailWeightB, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailOffsetX, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailOffsetY, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailOffsetZ, VolumetricClouds),
    PP_BLEND_IN(CloudsAbsorptionThroughCloud, VolumetricClouds),
    PP_BLEND_IN(CloudsAbsorptionTowardSun, VolumetricClouds),
    PP_BLEND_IN(CloudsDarknessThreshold, VolumetricClouds),
    PP_BLEND_IN(CloudsPhaseForward, VolumetricClouds),
    PP_BLEND_IN(CloudsPhaseBack, VolumetricClouds),
    PP_BLEND_IN(CloudsPhaseBase, VolumetricClouds),
    PP_BLEND_IN(CloudsPhaseFactor, VolumetricClouds),
    PP_BLEND_IN(CloudsTimeScale, VolumetricClouds),
    PP_BLEND_IN(CloudsBaseSpeed, VolumetricClouds),
    PP_BLEND_IN(CloudsDetailSpeed, VolumetricClouds),
    PP_BLEND_IN(CloudsHistoryWeight, VolumetricClouds),
};

// Volumetric fog is consumed through ToVolumetricFogSettings / the froxel UBO,
// not named push constants, so its whole block is blend-only. The shared
// fog-glow members both fog effects author live in the core table
// (PostProcessSettings.cpp) with the FogGlow group.
constexpr EffectSettingsField kVolumetricFogSettings[]{
    PP_BLEND_IN(VolumetricFogIntensity, VolumetricFog),
    PP_BLEND_IN(VolumetricFogIsGlobal, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeShape, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeValid, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeBlendDistance, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeCenterX, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeCenterY, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeCenterZ, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisXX, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisXY, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisXZ, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisYX, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisYY, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisYZ, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisZX, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisZY, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeAxisZZ, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeHalfExtentX, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeHalfExtentY, VolumetricFog),
    PP_BLEND_IN(VolumetricFogVolumeHalfExtentZ, VolumetricFog),
    PP_BLEND_IN(VolumetricFogMaxDistance, VolumetricFog),
    PP_BLEND_IN(VolumetricFogXYCellSizePixels, VolumetricFog),
    PP_BLEND_IN(VolumetricFogZSliceCount, VolumetricFog),
    PP_BLEND_IN(VolumetricFogDepthDistribution, VolumetricFog),
    PP_BLEND_IN(VolumetricFogDensity, VolumetricFog),
    PP_BLEND_IN(VolumetricFogBaseHeight, VolumetricFog),
    PP_BLEND_IN(VolumetricFogHeightFalloff, VolumetricFog),
    PP_BLEND_IN(VolumetricFogSkyFade, VolumetricFog),
    PP_BLEND_IN(VolumetricFogAlbedoR, VolumetricFog),
    PP_BLEND_IN(VolumetricFogAlbedoG, VolumetricFog),
    PP_BLEND_IN(VolumetricFogAlbedoB, VolumetricFog),
    PP_BLEND_IN(VolumetricFogEmissionR, VolumetricFog),
    PP_BLEND_IN(VolumetricFogEmissionG, VolumetricFog),
    PP_BLEND_IN(VolumetricFogEmissionB, VolumetricFog),
    PP_BLEND_IN(VolumetricFogAnisotropy, VolumetricFog),
    PP_BLEND_IN(VolumetricFogTrackDirectionalLight, VolumetricFog),
    PP_BLEND_IN(VolumetricFogSunIntensityScale, VolumetricFog),
    PP_BLEND_IN(VolumetricFogSunTintR, VolumetricFog),
    PP_BLEND_IN(VolumetricFogSunTintG, VolumetricFog),
    PP_BLEND_IN(VolumetricFogSunTintB, VolumetricFog),
    PP_BLEND_IN(VolumetricFogAmbientTintR, VolumetricFog),
    PP_BLEND_IN(VolumetricFogAmbientTintG, VolumetricFog),
    PP_BLEND_IN(VolumetricFogAmbientTintB, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseEnabled, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseScale, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseStrength, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseVelocityX, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseVelocityY, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseVelocityZ, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseContrast, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseChannelWeightR, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseChannelWeightG, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseChannelWeightB, VolumetricFog),
    PP_BLEND_IN(VolumetricFogNoiseChannelWeightA, VolumetricFog),
    PP_BLEND_IN(VolumetricFogDensityThreshold, VolumetricFog),
    PP_BLEND_IN(VolumetricFogDensityThresholdSoftness, VolumetricFog),
    PP_BLEND_IN(VolumetricFogTemporalEnabled, VolumetricFog),
    PP_BLEND_IN(VolumetricFogTemporalBlend, VolumetricFog),
    PP_BLEND_IN(VolumetricFogJitterStrength, VolumetricFog),
    PP_BLEND_IN(VolumetricFogJitterMotion, VolumetricFog),
    PP_BLEND_IN(VolumetricFogCompositeDepthBias, VolumetricFog),
    PP_BLEND_IN(VolumetricFogShadowBias, VolumetricFog),
};

constexpr EffectSettingsField kDebandSettings[]{
    // Absent component = "baseline applies here": the gate lerps so a
    // blended-in deband volume fades against the baseline, not a neighbor.
    PP_BLEND(DebandThresholdLsb),
};

// AO pushes through the write-only TryWriteField appendix (aoIntensity /
// aoRadius / aoThickness stay unreadable by design); the blend still lerps.
constexpr EffectSettingsField kAmbientOcclusionSettings[]{
    PP_BLEND(AOIntensity),
    PP_BLEND(AORadius),
    PP_BLEND(AOThickness),
};

// Blend-only: ScreenSpaceReflectionsNode writes its own push constants by
// literal name and gates on IsSSSRActive(), so nothing resolves these through
// TryWriteField / skipWhen. They still must fold across volumes, or a
// crossfade would snap the reflection instead of ramping it.
constexpr EffectSettingsField kScreenSpaceReflectionsSettings[]{
    PP_BLEND(SSSRIntensity),
    PP_BLEND(SSSRMaxDistance),
    PP_BLEND(SSSRThickness),
    PP_BLEND(SSSREdgeFade),
    PP_BLEND(SSSRMaxSteps),
    PP_BLEND(SSSRSampleQuality),
    PP_BLEND(SSSRMultiBounce),
};

// EV-space lerps: the no-op clamp endpoints are the identity, so a fading
// volume's compensation/clamps engage proportionally with its weight.
constexpr EffectSettingsField kExposureAdjustmentSettings[]{
    PP_BLEND(ExposureCompensationEv),
    PP_BLEND(ExposureClampMinEv),
    PP_BLEND(ExposureClampMaxEv),
};

// A lens-dirt composite samples the full bloom pyramid, so it keeps every
// highlight octave alive regardless of the authored depth.
template <int32 Octave>
bool IsBloomOctaveActive(const PPS& s)
{
    return s.IsBloomHighlightsActive() && (s.BloomOctaves >= Octave || s.IsBloomLensDirtActive());
}

template <int32 Octave>
bool IsScatteringOctaveActive(const PPS& s)
{
    return s.IsBloomScatteringActive() && s.BloomOctaves >= Octave;
}

// Skip gates each effect decides alone; gates that combine effects are in the
// core table in PostProcessSettings.cpp.
constexpr EffectSettingsGate kBloomGates[]{
    {"bloomActive", [](const PPS& s) { return s.IsBloomActive(); }},
    {"bloomHighlightsActive", [](const PPS& s) { return s.IsBloomHighlightsActive(); }},
    {"bloomChainActive", [](const PPS& s) { return s.IsBloomChainActive(); }},
    {"bloomScatteringActive", [](const PPS& s) { return s.IsBloomScatteringActive(); }},
    {"bloomLensDirtActive", [](const PPS& s) { return s.IsBloomLensDirtActive(); }},
    {"bloomOctave4Active", &IsBloomOctaveActive<4>},
    {"bloomOctave5Active", &IsBloomOctaveActive<5>},
    {"bloomOctave6Active", &IsBloomOctaveActive<6>},
    {"bloomOctave7Active", &IsBloomOctaveActive<7>},
    {"bloomOctave8Active", &IsBloomOctaveActive<8>},
    {"scatteringOctave4Active", &IsScatteringOctaveActive<4>},
    {"scatteringOctave5Active", &IsScatteringOctaveActive<5>},
    {"scatteringOctave6Active", &IsScatteringOctaveActive<6>},
    {"scatteringOctave7Active", &IsScatteringOctaveActive<7>},
    {"scatteringOctave8Active", &IsScatteringOctaveActive<8>},
};
constexpr EffectSettingsGate kCrtGates[]{
    {"crtActive", [](const PPS& s) { return s.IsCrtActive(); }},
};
constexpr EffectSettingsGate kVhsGates[]{
    {"vhsActive", [](const PPS& s) { return s.IsVhsActive(); }},
};
constexpr EffectSettingsGate kAmbientOcclusionGates[]{
    {"aoActive", [](const PPS& s) { return s.IsAOActive(); }},
};
constexpr EffectSettingsGate kContrastAdaptiveSharpenGates[]{
    {"casActive", [](const PPS& s) { return s.IsCasActive(); }},
};
constexpr EffectSettingsGate kFastBlurGates[]{
    {"fastBlurActive", [](const PPS& s) { return s.IsFastBlurActive(); }},
};
constexpr EffectSettingsGate kHeatDistortionGates[]{
    {"heatDistortionActive", [](const PPS& s) { return s.IsHeatDistortionActive(); }},
};
constexpr EffectSettingsGate kColorFilterGates[]{
    {"colorFilterActive", [](const PPS& s) { return s.IsColorFilterActive(); }},
};
constexpr EffectSettingsGate kColorGradeGates[]{
    {"colorGradeActive", [](const PPS& s) { return s.IsColorGradeActive(); }},
};
constexpr EffectSettingsGate kChromaticAberrationGates[]{
    {"chromaticAberrationActive", [](const PPS& s) { return s.IsChromaticAberrationActive(); }},
};
constexpr EffectSettingsGate kDepthOfFieldGates[]{
    {"dofActive", [](const PPS& s) { return s.IsDofActive(); }},
};
constexpr EffectSettingsGate kFilmSimulationGates[]{
    {"filmSimulationGrainActive", [](const PPS& s) { return s.IsFilmSimulationGrainActive(); }},
    {"filmSimulationArtifactsActive", [](const PPS& s) { return s.IsFilmSimulationArtifactsActive(); }},
    {"halationActive", [](const PPS& s) { return s.IsHalationActive(); }},
};
constexpr EffectSettingsGate kCubeLutGates[]{
    {"lutActive", [](const PPS& s) { return s.IsLutActive(); }},
};
constexpr EffectSettingsGate kVolumetricFogGates[]{
    {"volumetricFogActive", [](const PPS& s) { return s.IsVolumetricFogActive(); }},
};
constexpr EffectSettingsGate kHeightFogGates[]{
    {"heightFogActive", [](const PPS& s) { return s.IsHeightFogActive(); }},
};
constexpr EffectSettingsGate kAtmosphericCloudGates[]{
    {"atmosphericCloudActive", [](const PPS& s) { return s.IsAtmosphericCloudActive(); }},
};
constexpr EffectSettingsGate kVolumetricCloudsGates[]{
    {"volumetricCloudsActive", [](const PPS& s) { return s.IsVolumetricCloudsActive(); }},
};

#undef PP_SETTING
#undef PP_SETTING_IN
#undef PP_SETTING_SKIP
#undef PP_BLEND
#undef PP_BLEND_IN
#undef PP_BLEND_SKIP
#undef PP_FIELD
} // namespace

void RegisterBuiltInPostProcessEffectDescriptors()
{
    using namespace Components;
    using Registry = PostProcessEffectRegistry;

    Registry::Register({
        .Type = RegisterEffectComponent<BloomEffect>(),
        .ComponentName = "BloomEffect",
        .DisplayName = "Bloom Effect",
        .Description = "HDR bloom with tint, depth veil, and lens-dirt overlays",
        .IconPath = "Icons/PostFX/Bloom.svg",
        .SettingsFields = kBloomSettings,
        .Gates = kBloomGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* bloom = world.GetComponent<GameEngine::Components::BloomEffect>(entity); bloom && bloom->Enabled)
            {
                out.Settings.BloomThreshold = bloom->Threshold;
                out.Settings.BloomKnee      = bloom->Knee;
                out.Settings.BloomAntiFlicker = bloom->AntiFlicker ? 1 : 0;
                out.Settings.BloomIntensity = bloom->Intensity;
                out.Settings.BloomScatteringAmount = std::clamp(bloom->ScatteringAmount, 0.0f, 1.0f);
                out.Settings.BloomTintR     = std::clamp(bloom->Tint[0], 0.0f, 1.0f);
                out.Settings.BloomTintG     = std::clamp(bloom->Tint[1], 0.0f, 1.0f);
                out.Settings.BloomTintB     = std::clamp(bloom->Tint[2], 0.0f, 1.0f);
                out.Settings.BloomRadius    = std::clamp(bloom->Radius, 1.0f, 7.0f);
                out.Settings.BloomOctaves   = std::clamp(bloom->Octaves, 3, 8);
                out.Settings.BloomScatter   = std::clamp(bloom->Scatter, 0.0f, 1.0f);
                out.Settings.BloomDepthVeilEnabled = bloom->DepthVeilEnabled ? 1 : 0;
                out.Settings.BloomDepthVeilIntensity = std::clamp(bloom->DepthVeilIntensity, 0.0f, 10.0f);
                out.Settings.BloomDepthVeilStart = std::max(bloom->DepthVeilStart, 0.0f);
                out.Settings.BloomDepthVeilEnd = std::max(bloom->DepthVeilEnd, 0.0f);
                out.Settings.BloomDepthVeilTintR = std::clamp(bloom->DepthVeilTint[0], 0.0f, 1.0f);
                out.Settings.BloomDepthVeilTintG = std::clamp(bloom->DepthVeilTint[1], 0.0f, 1.0f);
                out.Settings.BloomDepthVeilTintB = std::clamp(bloom->DepthVeilTint[2], 0.0f, 1.0f);
                out.Settings.BloomLensDirtEnabled = bloom->LensDirtEnabled ? 1 : 0;
                out.Settings.BloomLensDirtVignette = bloom->LensDirtVignette ? 1 : 0;
                out.Settings.BloomLensDirtVignetteIntensity = std::clamp(bloom->LensDirtVignetteIntensity, 0.0f, 1.0f);
                out.Settings.BloomLensDirtVignetteRadius = std::clamp(bloom->LensDirtVignetteRadius, 0.0f, 1.0f);
                out.Settings.BloomLensDirtVignetteSmoothness = std::clamp(bloom->LensDirtVignetteSmoothness, 0.0f, 1.0f);
                out.Settings.BloomLensDirtVignetteRounded = bloom->LensDirtVignetteRounded ? 1 : 0;
                out.Settings.BloomLensDirtVignetteColorR = std::clamp(bloom->LensDirtVignetteColor[0], 0.0f, 1.0f);
                out.Settings.BloomLensDirtVignetteColorG = std::clamp(bloom->LensDirtVignetteColor[1], 0.0f, 1.0f);
                out.Settings.BloomLensDirtVignetteColorB = std::clamp(bloom->LensDirtVignetteColor[2], 0.0f, 1.0f);
                out.Settings.BloomLensDirtIntensity = std::clamp(bloom->LensDirtIntensity, 0.0f, 10.0f);
                out.Settings.BloomLensDirtScatter = std::clamp(bloom->LensDirtScatter, 0.0f, 1.0f);
                out.BloomLensDirtAssetGuid = bloom->LensDirtTexture.ToGuid();
                out.HasBloomLensDirtAsset = !out.BloomLensDirtAssetGuid.IsNull();

                // Warm the selected dirt texture as soon as the control is enabled,
                // even while intensity is zero and the composite pass is gated out.
                // This removes the first-use decode/upload hitch when the artist raises
                // intensity. An empty override warms the built-in Sonic Ether texture.
                if (bloom->LensDirtEnabled && ctx.Services)
                {
                    GUID preloadGuid = out.BloomLensDirtAssetGuid;
                    if (preloadGuid.IsNull())
                    {
                        if (GameEngine::AssetManager* assetManager =
                                GameEngine::EngineCore::GetInstance().TryGetAssetManager())
                        {
                            preloadGuid =
                                assetManager->ResolveAssetGuid("Textures/Bloom/lensDirt1.png");
                        }
                    }
                    if (!preloadGuid.IsNull())
                        (void)ctx.Services->Textures().GetOrUpload(preloadGuid);
                }
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<CrtEffect>(),
        .ComponentName = "CrtEffect",
        .DisplayName = "CRT Effect",
        .Description = "CRT display emulation: curvature, scanlines, and phosphor softness",
        .IconPath = "Icons/PostFX/CRT.svg",
        .SettingsFields = kCrtSettings,
        .Gates = kCrtGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* crt = world.GetComponent<GameEngine::Components::CrtEffect>(entity); crt && crt->Enabled)
            {
                out.Settings.CrtIntensity             = crt->Intensity;
                out.Settings.CrtCurvature             = crt->Curvature;
                out.Settings.CrtScanlines             = crt->Scanlines;
                out.Settings.CrtVignette              = crt->Vignette;
                out.Settings.CrtAberration            = crt->Aberration;
                out.Settings.CrtSoftness              = crt->Softness;
                out.Settings.CrtExposureCompensation  = crt->ExposureCompensation ? 1.0f : 0.0f;
                out.Settings.CrtEmulatedResolutionDiv = (crt->EmulatedResolutionDiv > 1e-5f)
                                                       ? crt->EmulatedResolutionDiv
                                                       : 6.0f;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<VhsEffect>(),
        .ComponentName = "VhsEffect",
        .DisplayName = "VHS Effect",
        .Description = "Analog tape transport, composite-signal artifacts, and recorded overlays",
        .IconPath = "Icons/PostFX/VHS.svg",
        .SettingsFields = kVhsSettings,
        .Gates = kVhsGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            using VHS = GameEngine::Components::VhsEffect;
            if (auto* vhs = world.GetComponent<VHS>(entity); vhs && vhs->Enabled)
            {
                out.Settings.VhsIntensity =
                    std::clamp(vhs->Intensity, 0.0f, VHS::kIntensityMax);
                out.Settings.VhsWobble =
                    std::clamp(vhs->Wobble, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsTracking =
                    std::clamp(vhs->Tracking, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsSignalGlitches =
                    std::clamp(vhs->SignalGlitches, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsGlitchOffsets =
                    std::clamp(vhs->GlitchOffsets, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsInterference =
                    std::clamp(vhs->Interference, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsFrameFeedback =
                    std::clamp(vhs->FrameFeedback, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsFeedbackDecay =
                    std::clamp(vhs->FeedbackDecay, 0.0f, 0.98f);
                out.Settings.VhsFeedbackMotionThreshold =
                    std::clamp(vhs->FeedbackMotionThreshold, 0.0f, 1.0f);
                out.Settings.VhsFeedbackTrailLength =
                    std::clamp(vhs->FeedbackTrailLength, 0.0f, 24.0f);
                out.Settings.VhsCompositeSignalMode =
                    static_cast<float32>(std::min(vhs->CompositeSignalMode,
                                                  VHS::kCompositeSignalModeMax));
                out.Settings.VhsDotCrawl =
                    std::clamp(vhs->DotCrawl, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsColorBleed =
                    std::clamp(vhs->ColorBleed, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsColorBleedOffset =
                    std::clamp(vhs->ColorBleedOffset, 0.0f, VHS::kColorBleedOffsetMax);
                out.Settings.VhsTapeNoise =
                    std::clamp(vhs->TapeNoise, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsChromaStreaks =
                    std::clamp(vhs->ChromaStreaks, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsPreFilterChromaStreaks =
                    out.Settings.VhsChromaStreaks;
                out.Settings.VhsDropouts =
                    std::clamp(vhs->Dropouts, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsRfDropouts = out.Settings.VhsDropouts;
                out.Settings.VhsScanlines =
                    std::clamp(vhs->Scanlines, 0.0f, VHS::kStrengthMax);
                out.Settings.VhsSpeed =
                    std::clamp(vhs->Speed, 0.0f, VHS::kSpeedMax);
                out.Settings.VhsOverlayEnabled = vhs->OverlayEnabled ? 1.0f : 0.0f;
                out.Settings.VhsOverlayColorR =
                    std::clamp(vhs->OverlayColor[0], 0.0f, 1.0f);
                out.Settings.VhsOverlayColorG =
                    std::clamp(vhs->OverlayColor[1], 0.0f, 1.0f);
                out.Settings.VhsOverlayColorB =
                    std::clamp(vhs->OverlayColor[2], 0.0f, 1.0f);
                out.Settings.VhsOverlayOpacity =
                    std::clamp(vhs->OverlayOpacity, 0.0f, 1.0f);
                out.Settings.VhsOverlaySize =
                    std::clamp(vhs->OverlaySize, VHS::kOverlaySizeMin,
                               VHS::kOverlaySizeMax);
                out.Settings.VhsOverlayFont =
                    static_cast<float32>(std::min(vhs->OverlayFont, VHS::kOverlayFontMax));
                out.Settings.VhsOverlayPositionX =
                    std::clamp(vhs->OverlayPositionX, 0.0f, 1.0f);
                out.Settings.VhsOverlayPositionY =
                    std::clamp(vhs->OverlayPositionY, 0.0f, 1.0f);

                const std::string_view overlayText = vhs->GetOverlayText();
                out.Settings.VhsOverlayTextLength =
                    static_cast<float32>(overlayText.size());
                float32* overlayCharacters[] = {
                    &out.Settings.VhsOverlayText0, &out.Settings.VhsOverlayText1,
                    &out.Settings.VhsOverlayText2, &out.Settings.VhsOverlayText3,
                    &out.Settings.VhsOverlayText4, &out.Settings.VhsOverlayText5,
                    &out.Settings.VhsOverlayText6, &out.Settings.VhsOverlayText7,
                };
                for (uint32 index = 0; index < VHS::kOverlayTextCapacity; ++index)
                {
                    *overlayCharacters[index] = index < overlayText.size()
                        ? static_cast<float32>(static_cast<uint8>(overlayText[index]))
                        : 0.0f;
                }

                out.Settings.VhsDateBurnEnabled =
                    vhs->DateBurnEnabled ? 1.0f : 0.0f;
                out.Settings.VhsDateBurnColorR =
                    std::clamp(vhs->DateBurnColor[0], 0.0f, 1.0f);
                out.Settings.VhsDateBurnColorG =
                    std::clamp(vhs->DateBurnColor[1], 0.0f, 1.0f);
                out.Settings.VhsDateBurnColorB =
                    std::clamp(vhs->DateBurnColor[2], 0.0f, 1.0f);
                out.Settings.VhsDateBurnSize =
                    std::clamp(vhs->DateBurnSize, VHS::kOverlaySizeMin,
                               VHS::kOverlaySizeMax);
                out.Settings.VhsDateBurnPositionX =
                    std::clamp(vhs->DateBurnPositionX, 0.0f, 1.0f);
                out.Settings.VhsDateBurnPositionY =
                    std::clamp(vhs->DateBurnPositionY, 0.0f, 1.0f);
                out.Settings.VhsDateBurnYear =
                    static_cast<float32>(std::clamp(vhs->DateBurnYear,
                                                    VHS::kDateBurnYearMin,
                                                    VHS::kDateBurnYearMax));
                out.Settings.VhsDateBurnMonth =
                    static_cast<float32>(std::clamp(vhs->DateBurnMonth, 1u, 12u));
                out.Settings.VhsDateBurnDay =
                    static_cast<float32>(std::clamp(vhs->DateBurnDay, 1u, 31u));
                out.Settings.VhsDateBurnHour =
                    static_cast<float32>(std::min(vhs->DateBurnHour, 23u));
                out.Settings.VhsDateBurnMinute =
                    static_cast<float32>(std::min(vhs->DateBurnMinute, 59u));
                out.Settings.VhsTransportMode =
                    static_cast<float32>(std::min(vhs->TransportMode,
                                                  VHS::kTransportModeMax));
                out.Settings.VhsTransportStrength =
                    std::clamp(vhs->TransportStrength, 0.0f, VHS::kStrengthMax);
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<ExposureAdjustmentEffect>(),
        .ComponentName = "ExposureAdjustmentEffect",
        .DisplayName = "Exposure Adjustment",
        .Description = "Per-volume exposure compensation and adaptation clamps on top of the camera sensor",
        .IconPath = "Icons/PostFX/ExposureAdjustment.svg",
        .SettingsFields = kExposureAdjustmentSettings,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* exposureAdjust =
                    world.GetComponent<GameEngine::Components::ExposureAdjustmentEffect>(entity);
                exposureAdjust && exposureAdjust->Enabled)
            {
                out.Settings.ExposureCompensationEv = exposureAdjust->Compensation;
                if (exposureAdjust->ClampMin)
                    out.Settings.ExposureClampMinEv = exposureAdjust->MinEv;
                if (exposureAdjust->ClampMax)
                    out.Settings.ExposureClampMaxEv = exposureAdjust->MaxEv;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<AmbientOcclusionEffect>(),
        .ComponentName = "AmbientOcclusionEffect",
        .DisplayName = "Ambient Occlusion",
        .Description = "Screen-space ground-truth ambient occlusion (GTAO)",
        .IconPath = "Icons/PostFX/AmbientOcclusion.svg",
        .FieldIO = kAmbientOcclusionIO,
        .SettingsFields = kAmbientOcclusionSettings,
        .Gates = kAmbientOcclusionGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* ao = world.GetComponent<GameEngine::Components::AmbientOcclusionEffect>(entity); ao && ao->Enabled)
            {
                out.Settings.AOIntensity = ao->Intensity;
                out.Settings.AORadius    = ao->Radius;
                out.Settings.AOThickness = ao->Thickness;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<ScreenSpaceReflectionsEffect>(),
        .ComponentName = "ScreenSpaceReflectionsEffect",
        .DisplayName = "Screen Space Reflections",
        .Description = "Stochastic screen-space reflections traced against the scene HZB",
        .IconPath = "Icons/PostFX/ScreenSpaceReflections.svg",
        .FieldIO = kScreenSpaceReflectionsIO,
        .SettingsFields = kScreenSpaceReflectionsSettings,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            using SSSR = GameEngine::Components::ScreenSpaceReflectionsEffect;
            if (auto* sssr = world.GetComponent<SSSR>(entity); sssr && sssr->Enabled)
            {
                out.Settings.SSSRIntensity = std::clamp(sssr->Intensity, 0.0f, SSSR::kIntensityMax);
                out.Settings.SSSRMaxDistance = std::max(sssr->MaxDistance, 0.0f);
                out.Settings.SSSRThickness = std::max(sssr->Thickness, SSSR::kMinThickness);
                out.Settings.SSSREdgeFade =
                    std::clamp(sssr->EdgeFade, SSSR::kMinEdgeFade, SSSR::kMaxEdgeFade);
                out.Settings.SSSRMaxSteps =
                    std::clamp(sssr->MaxSteps, SSSR::kMinSteps, SSSR::kMaxSteps);
                out.Settings.SSSRSampleQuality =
                    std::clamp(static_cast<int32>(sssr->SampleQuality), 0, SSSR::kSampleQualityLast);
                out.Settings.SSSRMultiBounce = sssr->MultiBounce ? 1 : 0;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<DebandEffect>(),
        .ComponentName = "DebandEffect",
        .DisplayName = "Deband",
        .Description = "Terminal-encode dither gate that hides gradient banding",
        .IconPath = "Icons/PostFX/Deband.svg",
        .FieldIO = kDebandIO,
        .SettingsFields = kDebandSettings,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
// Unlike siblings, checked without && Enabled: a PRESENT
            // component is an explicit policy either way — Enabled=false
            // folds to 0 rather than falling through like an absent
            // component. (The settings default is 0/off — the deband is
            // opt-in — so today both routes land off; keeping the
            // explicit fold preserves the contract if the default ever
            // moves again.)
            if (auto* deband = world.GetComponent<GameEngine::Components::DebandEffect>(entity))
            {
                // Clamp mirrors the GE_DEBAND_THRESHOLD parser's range.
                out.Settings.DebandThresholdLsb =
                    deband->Enabled ? std::clamp(deband->ThresholdLsb, 0.0f, 16.0f) : 0.0f;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<ContrastAdaptiveSharpenEffect>(),
        .ComponentName = "ContrastAdaptiveSharpenEffect",
        .DisplayName = "Contrast Adaptive Sharpening",
        .Description = "AMD FidelityFX contrast-adaptive sharpening",
        .IconPath = "Icons/PostFX/ContrastAdaptiveSharpening.svg",
        .FieldIO = kContrastAdaptiveSharpenIO,
        .SettingsFields = kContrastAdaptiveSharpenSettings,
        .Gates = kContrastAdaptiveSharpenGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* cas = world.GetComponent<GameEngine::Components::ContrastAdaptiveSharpenEffect>(entity); cas && cas->Enabled)
            {
                out.Settings.CasStrength = std::clamp(cas->Strength, 0.0f, 1.0f);
                out.Settings.CasStackOrder = cas->StackOrder;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<FastBlurEffect>(),
        .ComponentName = "FastBlurEffect",
        .DisplayName = "Fast Blur",
        .Description = "Fast single-pass depth-of-field blur",
        .IconPath = "Icons/PostFX/FastBlur.svg",
        .FieldIO = kFastBlurIO,
        .SettingsFields = kFastBlurSettings,
        .Gates = kFastBlurGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* fastBlur = world.GetComponent<GameEngine::Components::FastBlurEffect>(entity); fastBlur && fastBlur->Enabled)
            {
                out.Settings.FastBlurIntensity = std::clamp(fastBlur->Intensity, 0.0f, 1.0f);
                out.Settings.FastBlurFocusDistance = std::max(fastBlur->FocusDistance, 0.01f);
                out.Settings.FastBlurFocusRange = std::max(fastBlur->FocusRange, 0.01f);
                out.Settings.FastBlurMaxRadius = std::clamp(fastBlur->MaxRadius, 0.0f, 24.0f);
                out.Settings.FastBlurNearBlur = fastBlur->NearBlur ? 1 : 0;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<HeatDistortionEffect>(),
        .ComponentName = "HeatDistortionEffect",
        .DisplayName = "Heat Distortion",
        .Description = "Screen-space heat shimmer distortion",
        .IconPath = "Icons/PostFX/HeatDistortion.svg",
        .FieldIO = kHeatDistortionIO,
        .SettingsFields = kHeatDistortionSettings,
        .Gates = kHeatDistortionGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* heat = world.GetComponent<GameEngine::Components::HeatDistortionEffect>(entity);
                heat && heat->Enabled)
            {
                out.Settings.HeatDistortionStrength = std::max(heat->Strength, 0.0f);
                out.Settings.HeatDistortionSpeed = heat->Speed;
                out.Settings.HeatDistortionScale = std::max(heat->Scale, 0.01f);
                out.Settings.HeatDistortionMaskStrength = std::max(heat->MaskStrength, 0.0f);
                out.Settings.HeatDistortionDistanceStart = std::max(heat->DistanceStart, 0.0f);
                out.Settings.HeatDistortionDistanceEnd =
                    std::max(heat->DistanceEnd, out.Settings.HeatDistortionDistanceStart + 0.001f);
                out.Settings.HeatDistortionDirectionalFalloff = std::max(heat->DirectionalFalloff, 0.0f);
                out.Settings.HeatDistortionUseAbsoluteY = heat->UseAbsoluteY ? 1 : 0;
                out.Settings.HeatDistortionSoftness = std::clamp(heat->Softness, 0.0f, 3.0f);
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<VignetteEffect>(),
        .ComponentName = "VignetteEffect",
        .DisplayName = "Vignette",
        .Description = "Optical vignette darkening toward the frame corners",
        .IconPath = "Icons/PostFX/Vignette.svg",
        .FieldIO = kVignetteIO,
        .SettingsFields = kVignetteSettings,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* vig = world.GetComponent<GameEngine::Components::VignetteEffect>(entity);
                vig && vig->Enabled)
            {
                out.Settings.VignetteIntensity  = std::clamp(vig->Intensity, 0.0f, 1.0f);
                out.Settings.VignetteSmoothness = std::clamp(vig->Smoothness, 0.0f, 1.0f);
                out.Settings.VignetteRounded    = vig->Rounded ? 1 : 0;
                out.Settings.VignetteColorR     = vig->Color[0];
                out.Settings.VignetteColorG     = vig->Color[1];
                out.Settings.VignetteColorB     = vig->Color[2];
                out.Settings.VignetteStackOrder = vig->StackOrder;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<ColorFilterEffect>(),
        .ComponentName = "ColorFilterEffect",
        .DisplayName = "Color Filter Effect",
        .Description = "Per-channel color multiplier before HDR color grading and tonemapping",
        .IconPath = "Icons/PostFX/ColorFilter.svg",
        .FieldIO = kColorFilterIO,
        .SettingsFields = kColorFilterSettings,
        .Gates = kColorFilterGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* cf = world.GetComponent<GameEngine::Components::ColorFilterEffect>(entity); cf && cf->Enabled)
            {
                out.Settings.ColorFilterR         = cf->Color[0];
                out.Settings.ColorFilterG         = cf->Color[1];
                out.Settings.ColorFilterB         = cf->Color[2];
                out.Settings.ColorFilterIntensity = cf->Intensity;
                out.Settings.ColorFilterBlendMode = static_cast<int32>(cf->BlendMode);
                out.Settings.ColorFilterStackOrder = cf->StackOrder;
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<ColorGradeEffect>(),
        .ComponentName = "ColorGradeEffect",
        .DisplayName = "Color Grade",
        .Description = "Three-way HDR color corrector (shadows / midtones / highlights) before tonemap",
        .IconPath = "Icons/PostFX/ColorGrade.svg",
        .FieldIO = kColorGradeIO,
        .DroppedLegacyKeys = kColorGradeDroppedKeys,
        .SettingsFields = kColorGradeSettings,
        .Gates = kColorGradeGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* grade = world.GetComponent<GameEngine::Components::ColorGradeEffect>(entity);
                grade && grade->Enabled)
            {
                out.HasColorGrade                     = true;
                out.Settings.ColorGradeShadowsR       = grade->ShadowsColor[0];
                out.Settings.ColorGradeShadowsG       = grade->ShadowsColor[1];
                out.Settings.ColorGradeShadowsB       = grade->ShadowsColor[2];
                out.Settings.ColorGradeShadowsMaster  = grade->ShadowsLightness;
                out.Settings.ColorGradeMidtonesR      = grade->MidtonesColor[0];
                out.Settings.ColorGradeMidtonesG      = grade->MidtonesColor[1];
                out.Settings.ColorGradeMidtonesB      = grade->MidtonesColor[2];
                out.Settings.ColorGradeMidtonesMaster = grade->MidtonesLightness;
                out.Settings.ColorGradeHighlightsR    = grade->HighlightsColor[0];
                out.Settings.ColorGradeHighlightsG    = grade->HighlightsColor[1];
                out.Settings.ColorGradeHighlightsB    = grade->HighlightsColor[2];
                out.Settings.ColorGradeHighlightsMaster = grade->HighlightsLightness;
                out.Settings.ColorGradeContrast       = std::max(grade->Contrast, 0.0f);
                out.Settings.ColorGradeSaturation     = std::max(grade->Saturation, 0.0f);
                out.Settings.ColorGradeHueShift       = std::clamp(grade->HueShift,
                    -Engine::Renderer::kColorGradeHueShiftRangeDegrees,
                    Engine::Renderer::kColorGradeHueShiftRangeDegrees);
                out.Settings.ColorGradeTemperature    = std::clamp(grade->Temperature,
                    -Engine::Renderer::kColorGradeTemperatureTintRange,
                    Engine::Renderer::kColorGradeTemperatureTintRange);
                out.Settings.ColorGradeTint           = std::clamp(grade->Tint,
                    -Engine::Renderer::kColorGradeTemperatureTintRange,
                    Engine::Renderer::kColorGradeTemperatureTintRange);
                out.Settings.ColorGradeInLog          = grade->GradeInLog ? 1 : 0;
                // Band limits clamp to the encoded axis here; ordering is
                // sanitized once in FillColorGradeParamsUBO.
                out.Settings.ColorGradeShadowsStart    = std::clamp(grade->ShadowsStart, 0.0f, 1.0f);
                out.Settings.ColorGradeShadowsEnd      = std::clamp(grade->ShadowsEnd, 0.0f, 1.0f);
                out.Settings.ColorGradeHighlightsStart = std::clamp(grade->HighlightsStart, 0.0f, 1.0f);
                out.Settings.ColorGradeHighlightsEnd   = std::clamp(grade->HighlightsEnd, 0.0f, 1.0f);
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<ChromaticAberrationEffect>(),
        .ComponentName = "ChromaticAberrationEffect",
        .DisplayName = "Chromatic Aberration",
        .Description = "Radial RGB fringing toward the frame edges, as from lens dispersion",
        .IconPath = "Icons/PostFX/ChromaticAberration.svg",
        .RequiredPackage = "chromatic-aberration",
        .FieldIO = kChromaticAberrationIO,
        .SettingsFields = kChromaticAberrationSettings,
        .Gates = kChromaticAberrationGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            // Package gate, hoisted per-extraction before Phase 2: absent services
            // (tests, headless) count as available, matching the old !rs || check.
            if (ctx.Services && !ctx.Services->IsPackageAvailable("chromatic-aberration"))
                return;
            if (auto* chromatic = world.GetComponent<GameEngine::Components::ChromaticAberrationEffect>(entity);
                chromatic && chromatic->Enabled)
            {
                out.Settings.ChromaticAberrationIntensity = std::clamp(chromatic->Intensity, 0.0f, GameEngine::Components::ChromaticAberrationEffect::kIntensityMax);
                out.Settings.ChromaticAberrationStartOffset = std::clamp(chromatic->StartOffset, 0.0f, 1.0f);
                out.Settings.ChromaticAberrationSaturation = std::clamp(chromatic->Saturation, 0.0f, 2.0f);
                out.Settings.ChromaticAberrationLongitudinal = std::clamp(chromatic->LongitudinalIntensity, 0.0f, GameEngine::Components::ChromaticAberrationEffect::kLongitudinalMax);
                out.Settings.ChromaticAberrationComa = std::clamp(chromatic->ComaIntensity, 0.0f, GameEngine::Components::ChromaticAberrationEffect::kComaMax);
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<DepthOfFieldEffect>(),
        .ComponentName = "DepthOfFieldEffect",
        .DisplayName = "Depth of Field",
        .Description = "Physical depth of field: focus distance and f-number come from the Camera",
        .IconPath = "Icons/PostFX/DepthOfField.svg",
        .RequiredPackage = "fidelityfx-dof",
        .FieldIO = kDepthOfFieldIO,
        .SettingsFields = kDepthOfFieldSettings,
        .Gates = kDepthOfFieldGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* dof = world.GetComponent<GameEngine::Components::DepthOfFieldEffect>(entity);
                dof && dof->Enabled)
            {
                // The physical lens determines DoF strength. Volume Weight
                // supplies the transition blend, so no second authored
                // intensity multiplier is necessary on the effect.
                out.Settings.DofIntensity = 1.0f;
                out.Settings.DofMaxRadius = std::clamp(dof->MaxRadius, 0.0f, GameEngine::Components::DepthOfFieldEffect::kMaxRadiusMax);
                out.Settings.DofSamplingQuality = std::clamp(static_cast<int32>(dof->SamplingQuality), 0, 2);
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<FilmSimulationEffect>(),
        .ComponentName = "FilmSimulationEffect",
        .DisplayName = "Film Simulation",
        .Description = "Simulate film halation, grain, gate weave, hairs, scratches, and aperture masks",
        .IconPath = "Icons/PostFX/FilmSimulation.svg",
        .RequiredPackage = "film-simulation",
        .FieldIO = kFilmSimulationIO,
        .SettingsFields = kFilmSimulationSettings,
        .Gates = kFilmSimulationGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            // Package gate, hoisted per-extraction before Phase 2: absent services
            // (tests, headless) count as available, matching the old !rs || check.
            if (ctx.Services && !ctx.Services->IsPackageAvailable("film-simulation"))
                return;
            if (auto* film = world.GetComponent<GameEngine::Components::FilmSimulationEffect>(entity);
                film && film->Enabled)
            {
                out.Settings.FilmSimulationFrameRate = std::clamp(film->FilmFrameRate, GameEngine::Components::FilmSimulationEffect::kFrameRateMin, GameEngine::Components::FilmSimulationEffect::kFrameRateMax);
                if (film->HalationEnabled)
                {
                    out.Settings.HalationIntensity = std::max(film->HalationIntensity, 0.0f);
                    out.Settings.HalationRadius = std::max(film->HalationRadius, 0.0f);
                    out.Settings.HalationTintR = std::max(film->HalationTint[0], 0.0f);
                    out.Settings.HalationTintG = std::max(film->HalationTint[1], 0.0f);
                    out.Settings.HalationTintB = std::max(film->HalationTint[2], 0.0f);
                }
                if (film->GrainEnabled)
                {
                    out.Settings.FilmSimulationGrainMode = std::clamp(
                        static_cast<int32>(film->GrainMode), 0,
                        static_cast<int32>(GameEngine::Components::FilmSimulationEffect::kGrainModeLast));
                    out.Settings.FilmSimulationGrainIntensity = std::clamp(film->GrainIntensity, 0.0f, GameEngine::Components::FilmSimulationEffect::kGrainIntensityMax);
                    out.Settings.FilmSimulationGrainSize = std::clamp(film->GrainSize, GameEngine::Components::FilmSimulationEffect::kGrainSizeMin, GameEngine::Components::FilmSimulationEffect::kGrainSizeMax);
                    out.Settings.FilmSimulationGrainSmooth = film->GrainSmooth ? 1 : 0;
                    out.Settings.FilmSimulationGrainDensity = std::clamp(film->GrainDensity, 0.0f, 1.0f);
                    out.Settings.FilmSimulationGrainShadowResponse = std::clamp(film->GrainShadowResponse, 0.0f, GameEngine::Components::FilmSimulationEffect::kGrainResponseMax);
                    out.Settings.FilmSimulationGrainMidtoneResponse = std::clamp(film->GrainMidtoneResponse, 0.0f, GameEngine::Components::FilmSimulationEffect::kGrainResponseMax);
                    out.Settings.FilmSimulationGrainHighlightResponse = std::clamp(film->GrainHighlightResponse, 0.0f, GameEngine::Components::FilmSimulationEffect::kGrainResponseMax);
                    out.Settings.FilmSimulationGrainColored = film->GrainColored ? 1 : 0;
                }
                out.Settings.FilmSimulationHairEnabled = film->HairEnabled ? 1 : 0;
                out.Settings.FilmSimulationHairAmount =
                    std::clamp(film->HairAmount, 0.0f, GameEngine::Components::FilmSimulationEffect::kHairAmountMax) /
                    GameEngine::Components::FilmSimulationEffect::kArtifactCandidateCount;
                out.Settings.FilmSimulationHairIntensity =
                    std::clamp(film->HairIntensity, 0.0f, GameEngine::Components::FilmSimulationEffect::kArtifactIntensityMax) /
                    GameEngine::Components::FilmSimulationEffect::kArtifactIntensityMax;
                out.Settings.FilmSimulationHairWidth = std::clamp(film->HairWidth, GameEngine::Components::FilmSimulationEffect::kHairWidthMin, GameEngine::Components::FilmSimulationEffect::kHairWidthMax);
                out.Settings.FilmSimulationHairLength = std::clamp(film->HairLength, GameEngine::Components::FilmSimulationEffect::kHairLengthMin, GameEngine::Components::FilmSimulationEffect::kHairLengthMax);
                out.Settings.FilmSimulationHairRandomSize = std::clamp(film->HairRandomSize, 0.0f, 1.0f);
                out.Settings.FilmSimulationHairCurl = std::clamp(film->HairCurl, -1.0f, 1.0f);
                out.Settings.FilmSimulationHairCurlRandomness = std::clamp(film->HairCurlRandomness, 0.0f, 1.0f);
                out.Settings.FilmSimulationScratchesEnabled = film->ScratchesEnabled ? 1 : 0;
                out.Settings.FilmSimulationScratchAmount =
                    std::clamp(film->ScratchAmount, 0.0f, GameEngine::Components::FilmSimulationEffect::kScratchAmountMax) /
                    GameEngine::Components::FilmSimulationEffect::kArtifactCandidateCount;
                out.Settings.FilmSimulationScratchIntensity =
                    std::clamp(film->ScratchIntensity, 0.0f, GameEngine::Components::FilmSimulationEffect::kArtifactIntensityMax) /
                    GameEngine::Components::FilmSimulationEffect::kArtifactIntensityMax;
                out.Settings.FilmSimulationScratchWidth = std::clamp(film->ScratchWidth, GameEngine::Components::FilmSimulationEffect::kScratchWidthMin, GameEngine::Components::FilmSimulationEffect::kScratchWidthMax);
                out.Settings.FilmSimulationScratchLength = std::clamp(film->ScratchLength, GameEngine::Components::FilmSimulationEffect::kScratchLengthMin, GameEngine::Components::FilmSimulationEffect::kScratchLengthMax);
                out.Settings.FilmSimulationDustEnabled = film->DustEnabled ? 1 : 0;
                out.Settings.FilmSimulationDustAmount =
                    std::clamp(film->DustAmount, 0.0f, GameEngine::Components::FilmSimulationEffect::kDustAmountMax) /
                    GameEngine::Components::FilmSimulationEffect::kArtifactCandidateCount;
                out.Settings.FilmSimulationDustIntensity =
                    std::clamp(film->DustIntensity, 0.0f, GameEngine::Components::FilmSimulationEffect::kArtifactIntensityMax) /
                    GameEngine::Components::FilmSimulationEffect::kArtifactIntensityMax;
                out.Settings.FilmSimulationDustSize = std::clamp(film->DustSize, GameEngine::Components::FilmSimulationEffect::kDustSizeMin, GameEngine::Components::FilmSimulationEffect::kDustSizeMax);
                out.Settings.FilmSimulationDustRandomSize = std::clamp(film->DustRandomSize, 0.0f, 1.0f);
                out.Settings.FilmSimulationGateWeaveEnabled = film->GateWeaveEnabled ? 1 : 0;
                out.Settings.FilmSimulationGateWeaveHorizontal = std::clamp(film->GateWeaveHorizontal, 0.0f, GameEngine::Components::FilmSimulationEffect::kGateWeaveOffsetMax);
                out.Settings.FilmSimulationGateWeaveVertical = std::clamp(film->GateWeaveVertical, 0.0f, GameEngine::Components::FilmSimulationEffect::kGateWeaveOffsetMax);
                out.Settings.FilmSimulationGateWeaveRotation = std::clamp(film->GateWeaveRotation, 0.0f, GameEngine::Components::FilmSimulationEffect::kGateWeaveRotationMax);
                out.Settings.FilmSimulationGateMask = static_cast<int32>(film->GateMask);
                out.Settings.FilmSimulationGateMaskFeather = std::clamp(film->GateMaskFeather, 0.0f, GameEngine::Components::FilmSimulationEffect::kGateMaskFeatherMax);
                out.Settings.FilmSimulationGateMaskRoundness = std::clamp(film->GateMaskRoundness, 0.0f, GameEngine::Components::FilmSimulationEffect::kGateMaskRoundnessMax);
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<ShadowSettingsEffect>(),
        .ComponentName = "ShadowSettingsEffect",
        .DisplayName = "Shadow Settings",
        .Description = "Per-volume directional shadow overrides (mode, distance, cascade split, biases)",
        .IconPath = "Icons/PostFX/ShadowSettings.svg",
        .FieldIO = kShadowSettingsIO,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* shadow = world.GetComponent<GameEngine::Components::ShadowSettingsEffect>(entity);
                shadow && shadow->Enabled)
            {
                out.HasShadowSettings = true;
                out.ShadowSettings.HasOverride = true;
                out.ShadowSettings.Mode = shadow->Mode;
                out.ShadowSettings.RayTracedQuality = shadow->RayTracedQuality;
                out.ShadowSettings.Filter = shadow->Filter;
                out.ShadowSettings.ScreenSpaceShadows = shadow->ScreenSpaceShadows;
                out.ShadowSettings.ScreenSpaceShadowThickness =
                    std::clamp(shadow->ScreenSpaceShadowThickness, 0.0001f, 0.05f);
                // MaxShadowDistance must stay positive so the cascade fit's
                // min(cameraFar, MaxShadowDistance) can't collapse the range.
                out.ShadowSettings.MaxShadowDistance = std::max(shadow->MaxShadowDistance, 0.01f);
                out.ShadowSettings.DistanceFadeFraction = std::clamp(shadow->DistanceFadeFraction, 0.0f,
                    Components::ShadowSettingsEffect::kDistanceFadeFractionMax);
                out.ShadowSettings.SplitLambda = std::clamp(shadow->SplitLambda, 0.0f, 1.0f);
                out.ShadowSettings.DepthBias = std::clamp(shadow->DepthBias, 0.0f, 0.01f);
                out.ShadowSettings.NormalBias = std::clamp(shadow->NormalBias, 0.0f, 5.0f);
            }
        },
    });

    // Hand-schema keeps: scene-IO stays with the custom ISceneComponentSchema
    // (asset refs, enums with bespoke encodings, legacy key re-homing), so no
    // FieldIO here — these registrations serve chrome + settings-write only.
    Registry::Register({
        .Type = RegisterEffectComponent<CubeLutEffect>(),
        .ComponentName = "CubeLutEffect",
        .DisplayName = "LUT",
        .Description = "Apply a LUT post-processing effect from a Resolve .cube asset",
        .IconPath = "Icons/PostFX/LutPostFX.svg",
        .SettingsFields = kCubeLutSettings,
        .Gates = kCubeLutGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* clut = world.GetComponent<GameEngine::Components::CubeLutEffect>(entity);
                clut && clut->Enabled)
            {
                out.Settings.LutIntensity = std::clamp(clut->Intensity, 0.0f, 1.0f);
                out.Settings.LutStackOrder = clut->StackOrder;
                out.Settings.LutInputEncoding = static_cast<int32>(std::min<uint32>(clut->InputEncoding, 5u));
                out.Settings.LutTextureFormat = static_cast<int32>(std::min<uint32>(clut->TextureFormat, 1u));
                out.CubeLutAssetGuid = clut->LutAssetGuid.ToGuid();
                out.HasCubeLutAsset = !out.CubeLutAssetGuid.IsNull();
            }
        },
    });
    // Registered (and therefore extracted) BEFORE HeightFog: both fog effects
    // author the shared fog-glow block, volumetric writes it unconditionally and
    // height fog merges by strongest-enabled � so volumetric must fold first.
    Registry::Register({
        .Type = RegisterEffectComponent<VolumetricFogEffect>(),
        .ComponentName = "VolumetricFogEffect",
        .DisplayName = "Volumetric Fog",
        .Description = "Froxel-grid volumetric fog resolved through the post-FX stack",
        .IconPath = "Icons/PostFX/VolumetricFog.svg",
        .SettingsFields = kVolumetricFogSettings,
        .Gates = kVolumetricFogGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* fog = world.GetComponent<GameEngine::Components::VolumetricFogEffect>(entity);
                fog && fog->Enabled)
            {
                out.HasVolumetricFog = true;
                out.Settings.VolumetricFogIntensity = std::max(fog->Intensity, 0.0f);
                out.Settings.VolumetricFogIsGlobal = out.IsGlobal ? 1 : 0;
                out.Settings.VolumetricFogVolumeShape = out.Shape;
                out.Settings.VolumetricFogVolumeValid = (!out.IsGlobal && out.ValidSpatial) ? 1 : 0;
                out.Settings.VolumetricFogVolumeBlendDistance = std::max(out.BlendDistance, 0.0f);
                out.Settings.VolumetricFogVolumeCenterX = out.Center[0];
                out.Settings.VolumetricFogVolumeCenterY = out.Center[1];
                out.Settings.VolumetricFogVolumeCenterZ = out.Center[2];
                out.Settings.VolumetricFogVolumeAxisXX = out.AxisX[0];
                out.Settings.VolumetricFogVolumeAxisXY = out.AxisX[1];
                out.Settings.VolumetricFogVolumeAxisXZ = out.AxisX[2];
                out.Settings.VolumetricFogVolumeAxisYX = out.AxisY[0];
                out.Settings.VolumetricFogVolumeAxisYY = out.AxisY[1];
                out.Settings.VolumetricFogVolumeAxisYZ = out.AxisY[2];
                out.Settings.VolumetricFogVolumeAxisZX = out.AxisZ[0];
                out.Settings.VolumetricFogVolumeAxisZY = out.AxisZ[1];
                out.Settings.VolumetricFogVolumeAxisZZ = out.AxisZ[2];
                out.Settings.VolumetricFogVolumeHalfExtentX = out.HalfExtents[0];
                out.Settings.VolumetricFogVolumeHalfExtentY = out.HalfExtents[1];
                out.Settings.VolumetricFogVolumeHalfExtentZ = out.HalfExtents[2];
                out.Settings.VolumetricFogMaxDistance = std::max(fog->MaxDistance, 0.01f);
                out.Settings.VolumetricFogXYCellSizePixels = std::max(fog->XYCellSizePixels, 1);
                out.Settings.VolumetricFogZSliceCount = std::max(fog->ZSliceCount, 1);
                out.Settings.VolumetricFogDepthDistribution = std::max(fog->DepthDistribution, 0.05f);
                out.Settings.VolumetricFogDensity = std::max(fog->Density, 0.0f);
                out.Settings.VolumetricFogBaseHeight = fog->BaseHeight;
                out.Settings.VolumetricFogHeightFalloff = std::max(fog->HeightFalloff, 0.01f);
                out.Settings.VolumetricFogSkyFade = std::clamp(fog->SkyFade, 0.0f, 1.0f);
                out.Settings.VolumetricFogAlbedoR = fog->Albedo[0];
                out.Settings.VolumetricFogAlbedoG = fog->Albedo[1];
                out.Settings.VolumetricFogAlbedoB = fog->Albedo[2];
                out.Settings.VolumetricFogEmissionR = fog->Emission[0];
                out.Settings.VolumetricFogEmissionG = fog->Emission[1];
                out.Settings.VolumetricFogEmissionB = fog->Emission[2];
                out.Settings.VolumetricFogAnisotropy = fog->Anisotropy;
                out.Settings.VolumetricFogTrackDirectionalLight = fog->TrackDirectionalLight ? 1 : 0;
                out.Settings.VolumetricFogSunIntensityScale = std::max(fog->SunIntensityScale, 0.0f);
                out.Settings.VolumetricFogSunTintR = fog->SunScatteringTint[0];
                out.Settings.VolumetricFogSunTintG = fog->SunScatteringTint[1];
                out.Settings.VolumetricFogSunTintB = fog->SunScatteringTint[2];
                out.Settings.VolumetricFogAmbientTintR = fog->AmbientScatteringTint[0];
                out.Settings.VolumetricFogAmbientTintG = fog->AmbientScatteringTint[1];
                out.Settings.VolumetricFogAmbientTintB = fog->AmbientScatteringTint[2];
                out.Settings.VolumetricFogNoiseEnabled = fog->NoiseEnabled ? 1 : 0;
                out.Settings.VolumetricFogNoiseScale = std::max(fog->NoiseScale, 0.01f);
                out.Settings.VolumetricFogNoiseStrength = std::clamp(fog->NoiseStrength, 0.0f, 1.0f);
                out.Settings.VolumetricFogNoiseVelocityX = fog->NoiseVelocity[0];
                out.Settings.VolumetricFogNoiseVelocityY = fog->NoiseVelocity[1];
                out.Settings.VolumetricFogNoiseVelocityZ = fog->NoiseVelocity[2];
                out.Settings.VolumetricFogNoiseContrast = std::max(fog->NoiseContrast, 0.01f);
                out.Settings.VolumetricFogNoiseChannelWeightR = std::max(fog->NoiseChannelWeights[0], 0.0f);
                out.Settings.VolumetricFogNoiseChannelWeightG = std::max(fog->NoiseChannelWeights[1], 0.0f);
                out.Settings.VolumetricFogNoiseChannelWeightB = std::max(fog->NoiseChannelWeights[2], 0.0f);
                out.Settings.VolumetricFogNoiseChannelWeightA = std::max(fog->NoiseChannelWeights[3], 0.0f);
                out.Settings.VolumetricFogDensityThreshold = std::clamp(fog->DensityThreshold, 0.0f, 1.0f);
                out.Settings.VolumetricFogDensityThresholdSoftness = std::max(fog->DensityThresholdSoftness, 0.0001f);
                out.Settings.VolumetricFogTemporalEnabled = fog->TemporalEnabled ? 1 : 0;
                out.Settings.VolumetricFogTemporalBlend = std::clamp(fog->TemporalBlend, 0.0f, 0.99f);
                out.Settings.VolumetricFogJitterStrength = std::clamp(fog->JitterStrength, 0.0f, 1.0f);
                out.Settings.VolumetricFogJitterMotion = fog->JitterMotion ? 1 : 0;
                out.Settings.VolumetricFogCompositeDepthBias = fog->CompositeDepthBias;
                out.Settings.VolumetricFogShadowBias = fog->ShadowBias;
                if (fog->FogGlowEnabled)
                {
                    out.Settings.FogGlowEnabled = 1;
                    out.Settings.FogGlowQuality = std::clamp(static_cast<int>(fog->FogGlowQualityLevel), 0, 3);
                    out.Settings.FogGlowIntensity = std::max(fog->FogGlowIntensity, 0.0f);
                    out.Settings.FogGlowRadius = std::clamp(fog->FogGlowRadius, 1.0f, 7.0f);
                    out.Settings.FogGlowOctaves = std::clamp(fog->FogGlowOctaves, 3, 8);
                    out.Settings.FogGlowScatter = std::clamp(fog->FogGlowScatter, 0.0f, 1.0f);
                    out.Settings.FogGlowThreshold = std::max(fog->FogGlowThreshold, 0.0f);
                    out.Settings.FogGlowKnee = std::max(fog->FogGlowKnee, 0.0f);
                    out.Settings.FogGlowFadeStart = std::clamp(fog->FogGlowFadeStart, 0.0f, 0.999f);
                    out.Settings.FogGlowFadeEnd = std::clamp(fog->FogGlowFadeEnd,
                                                          out.Settings.FogGlowFadeStart + 0.001f, 1.0f);
                    out.Settings.FogGlowTintR = std::max(fog->FogGlowTint[0], 0.0f);
                    out.Settings.FogGlowTintG = std::max(fog->FogGlowTint[1], 0.0f);
                    out.Settings.FogGlowTintB = std::max(fog->FogGlowTint[2], 0.0f);
                    out.Settings.FogGlowAntiFlicker = fog->FogGlowAntiFlicker ? 1 : 0;
                }

                out.LocalFogVolume.enabled = !out.IsGlobal && out.ValidSpatial;
                out.LocalFogVolume.shape = out.Shape;
                out.LocalFogVolume.densityMode = static_cast<Engine::Renderer::VolumetricFogDensityMode>(fog->DensityMode);
                out.LocalFogVolume.gradientMode = static_cast<Engine::Renderer::VolumetricFogGradientMode>(fog->GradientMode);
                out.LocalFogVolume.weight = std::max(fog->Intensity, 0.0f) * std::max(out.BaseWeight, 0.0f);
                out.LocalFogVolume.density = std::max(fog->Density, 0.0f);
                out.LocalFogVolume.blendDistance = std::max(out.BlendDistance, 0.0f);
                std::memcpy(out.LocalFogVolume.center, out.Center, sizeof(out.Center));
                std::memcpy(out.LocalFogVolume.axisX, out.AxisX, sizeof(out.AxisX));
                std::memcpy(out.LocalFogVolume.axisY, out.AxisY, sizeof(out.AxisY));
                std::memcpy(out.LocalFogVolume.axisZ, out.AxisZ, sizeof(out.AxisZ));
                std::memcpy(out.LocalFogVolume.halfExtents, out.HalfExtents, sizeof(out.HalfExtents));
                std::memcpy(out.LocalFogVolume.albedo, fog->Albedo, sizeof(out.LocalFogVolume.albedo));
                std::memcpy(out.LocalFogVolume.emission, fog->Emission, sizeof(out.LocalFogVolume.emission));
                std::memcpy(out.LocalFogVolume.gradientLowTint, fog->GradientLowTint, sizeof(out.LocalFogVolume.gradientLowTint));
                std::memcpy(out.LocalFogVolume.gradientHighTint, fog->GradientHighTint, sizeof(out.LocalFogVolume.gradientHighTint));
                out.LocalFogVolume.gradientStrength = std::clamp(fog->GradientStrength, 0.0f, 1.0f);
                out.LocalFogVolume.densityThreshold = std::clamp(fog->DensityThreshold, 0.0f, 1.0f);
                out.LocalFogVolume.densityThresholdSoftness = std::max(fog->DensityThresholdSoftness, 0.0001f);
            }
        },
        .NeutralizeSettings =
            +[](Engine::Renderer::PostProcessSettings& settings)
        {
            // Struct defaults are a usable fog, not "authored off": baseline them
            // per volume so a fog-less volume folds as no-fog.
            settings.VolumetricFogIntensity = 0.0f;
            settings.VolumetricFogIsGlobal = 1;
            settings.VolumetricFogVolumeShape = 0;
            settings.VolumetricFogVolumeValid = 0;
            settings.VolumetricFogVolumeBlendDistance = 0.0f;
            settings.VolumetricFogDensity = 0.0f;
            settings.VolumetricFogTemporalEnabled = 0;
            settings.VolumetricFogTemporalBlend = 0.0f;
            settings.VolumetricFogJitterStrength = 0.0f;
        },
    });

    Registry::Register({
        .Type = RegisterEffectComponent<HeightFogEffect>(),
        .ComponentName = "HeightFogEffect",
        .DisplayName = "Height Fog",
        .Description = "Analytical screen-space height fog",
        .IconPath = "Icons/PostFX/HeightFog.svg",
        .SettingsFields = kHeightFogSettings,
        .Gates = kHeightFogGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            if (auto* heightFog = world.GetComponent<GameEngine::Components::HeightFogEffect>(entity);
                heightFog && heightFog->Enabled)
            {
                out.HasHeightFog = true;
                CopyHeightFogEffectToSettings(*heightFog, ctx, out.Settings);
            }
        },
    });
    Registry::Register({
        .Type = RegisterEffectComponent<AtmosphericCloudLayer>(),
        .ComponentName = "AtmosphericCloudLayer",
        .DisplayName = "Atmospheric Cloud Layer",
        .Description = "World-space atmospheric cloud volume",
        .IconPath = "Icons/PostFX/AtmosphericCloudLayer.svg",
        .SettingsFields = kAtmosphericCloudSettings,
        .Gates = kAtmosphericCloudGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            if (auto* layer = world.GetComponent<GameEngine::Components::AtmosphericCloudLayer>(entity);
                layer && layer->Enabled)
            {
                out.HasAtmosphericCloud = true;
                out.Settings.AtmosphericCloudSkyFill = std::clamp(layer->SkyFill, 0.0f, 1.0f);
                out.Settings.AtmosphericCloudVaporMass = std::clamp(layer->VaporMass, 0.0f, 1.0f);
                out.Settings.AtmosphericCloudColorR = std::clamp(layer->CloudColor[0], 0.0f, kAtmosphericCloudColorMaxIntensity);
                out.Settings.AtmosphericCloudColorG = std::clamp(layer->CloudColor[1], 0.0f, kAtmosphericCloudColorMaxIntensity);
                out.Settings.AtmosphericCloudColorB = std::clamp(layer->CloudColor[2], 0.0f, kAtmosphericCloudColorMaxIntensity);
                out.Settings.AtmosphericCloudOpacity = std::clamp(layer->Opacity, 0.0f, 8.0f);
                out.Settings.AtmosphericCloudFloorHeight = std::max(layer->FloorHeight, 0.0f);
                out.Settings.AtmosphericCloudLayerDepth = std::max(layer->LayerDepth, 1.0f);
                out.Settings.AtmosphericCloudBodyFrequency = std::max(layer->BodyFrequency, 0.01f);
                out.Settings.AtmosphericCloudEdgeFrequency = std::max(layer->EdgeFrequency, 0.01f);
                out.Settings.AtmosphericCloudEdgeBreakup = std::clamp(layer->EdgeBreakup, 0.0f, 1.0f);
                out.Settings.AtmosphericCloudDriftAngle = layer->DriftAngle;
                out.Settings.AtmosphericCloudDriftRate = layer->DriftRate;
                out.Settings.AtmosphericCloudSunFade = std::clamp(layer->SunFade, 0.0f, 1.0f);
                out.Settings.AtmosphericCloudSkyBounce = std::clamp(layer->SkyBounce, 0.0f, 2.0f);
                out.Settings.AtmosphericCloudRimBoost = std::clamp(layer->RimBoost, 0.0f, 2.0f);
                out.Settings.AtmosphericCloudOcclusion = std::clamp(layer->Occlusion, 0.0f, 1.0f);
                out.Settings.AtmosphericCloudHistoryWeight = std::clamp(layer->HistoryWeight, 0.0f, 0.99f);
                out.Settings.AtmosphericCloudPixelScale = std::clamp(layer->PixelScale, 0.25f, 1.0f);
            }
        },
        .NeutralizeSettings =
            +[](Engine::Renderer::PostProcessSettings& settings)
        {
            settings.AtmosphericCloudSkyFill = 0.0f;
            settings.AtmosphericCloudVaporMass = 0.0f;
        },
    });
    Registry::Register({
        .Type = ECS::GetComponentTypeId<VolumetricClouds>(),
        .ComponentName = "VolumetricClouds",
        .DisplayName = "Volumetric Clouds",
        .Description = "Raymarched cloud container box (the volume entity's transform)",
        .IconPath = "Icons/PostFX/VolumetricClouds.svg",
        .SettingsFields = kVolumetricCloudsSettings,
        .Gates = kVolumetricCloudsGates,
        .Extract =
            +[](ECS::World& world, const ECS::EntityHandle& entity,
                const Engine::Renderer::PostProcessExtractContext& ctx,
                Engine::Renderer::PostProcessExtractedVolume& out)
        {
            (void)ctx;
            auto* clouds = world.GetComponent<GameEngine::Components::VolumetricClouds>(entity);
            if (!clouds || !clouds->Enabled)
                return;

            out.HasVolumetricClouds = true;
            auto& s = out.Settings;
            // The volume is authored on the effect, not derived from the
            // entity transform: a viewer-centered disc of this radius spanning
            // [Altitude, Altitude + Thickness].
            s.CloudsRadius = std::max(clouds->Radius, 0.0f);
            s.CloudsAltitude = clouds->Altitude;
            s.CloudsThickness = std::max(clouds->Thickness, 0.0f);
            s.CloudsNumStepsLight = std::clamp(clouds->NumStepsLight, 1, 64);
            s.CloudsStepSize = std::clamp(clouds->StepSize, 0.5f, 1000.0f);
            s.CloudsRayOffsetStrength = std::max(clouds->RayOffsetStrength, 0.0f);
            s.CloudsScale = std::max(clouds->CloudScale, 0.01f);
            s.CloudsDensityMultiplier = std::max(clouds->DensityMultiplier, 0.0f);
            s.CloudsDensityOffset = clouds->DensityOffset;
            s.CloudsShapeOffsetX = clouds->ShapeOffset[0];
            s.CloudsShapeOffsetY = clouds->ShapeOffset[1];
            s.CloudsShapeOffsetZ = clouds->ShapeOffset[2];
            s.CloudsShapeWeightR = std::max(clouds->ShapeNoiseWeights[0], 0.0f);
            s.CloudsShapeWeightG = std::max(clouds->ShapeNoiseWeights[1], 0.0f);
            s.CloudsShapeWeightB = std::max(clouds->ShapeNoiseWeights[2], 0.0f);
            s.CloudsShapeWeightA = std::max(clouds->ShapeNoiseWeights[3], 0.0f);
            s.CloudsDetailScale = std::max(clouds->DetailNoiseScale, 0.01f);
            s.CloudsDetailWeight = std::max(clouds->DetailNoiseWeight, 0.0f);
            s.CloudsDetailWeightR = std::max(clouds->DetailNoiseWeights[0], 0.0f);
            s.CloudsDetailWeightG = std::max(clouds->DetailNoiseWeights[1], 0.0f);
            s.CloudsDetailWeightB = std::max(clouds->DetailNoiseWeights[2], 0.0f);
            s.CloudsDetailOffsetX = clouds->DetailOffset[0];
            s.CloudsDetailOffsetY = clouds->DetailOffset[1];
            s.CloudsDetailOffsetZ = clouds->DetailOffset[2];
            s.CloudsAbsorptionThroughCloud = std::max(clouds->LightAbsorptionThroughCloud, 0.0f);
            s.CloudsAbsorptionTowardSun = std::max(clouds->LightAbsorptionTowardSun, 0.0f);
            s.CloudsDarknessThreshold = std::clamp(clouds->DarknessThreshold, 0.0f, 1.0f);
            s.CloudsPhaseForward = std::clamp(clouds->ForwardScattering, 0.0f, 1.0f);
            s.CloudsPhaseBack = std::clamp(clouds->BackScattering, 0.0f, 1.0f);
            s.CloudsPhaseBase = std::clamp(clouds->BaseBrightness, 0.0f, 1.0f);
            s.CloudsPhaseFactor = std::clamp(clouds->PhaseFactor, 0.0f, 1.0f);
            s.CloudsTimeScale = clouds->TimeScale;
            s.CloudsBaseSpeed = clouds->BaseSpeed;
            s.CloudsDetailSpeed = clouds->DetailSpeed;
            s.CloudsHistoryWeight = std::clamp(clouds->HistoryWeight, 0.0f, 0.98f);
        },
        .NeutralizeSettings =
            +[](Engine::Renderer::PostProcessSettings& settings)
        {
            settings.CloudsDensityMultiplier = 0.0f;
        },
    });
}

} // namespace GameEngine::Rendering
