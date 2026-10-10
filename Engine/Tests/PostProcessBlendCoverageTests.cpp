#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"
#include "Engine/Rendering/PostProcessVolumeExtract.h"
#include "Components/Rendering/PostProcessEffects/AtmosphericCloudLayer.h"
#include "Components/Rendering/PostProcessEffects/BloomEffect.h"
#include "Components/Rendering/PostProcessEffects/ChromaticAberrationEffect.h"
#include "Components/Rendering/PostProcessEffects/ColorGradeEffect.h"
#include "Components/Rendering/PostProcessEffects/CrtEffect.h"
#include "Components/Rendering/PostProcessEffects/DebandEffect.h"
#include "Components/Rendering/PostProcessEffects/DepthOfFieldEffect.h"
#include "Components/Rendering/PostProcessEffects/ExposureAdjustmentEffect.h"
#include "Components/Rendering/PostProcessEffects/HeightFogEffect.h"
#include "Components/Rendering/PostProcessEffects/ScreenSpaceReflectionsEffect.h"
#include "Components/Rendering/PostProcessEffects/VolumetricFogEffect.h"
#include "Components/Rendering/TonemapMode.h"
#include "Engine/Rendering/ColorGradeParamsUBO.h"
#include "Rendering/Core/NamedPushConstantWriter.h"
#include "Rendering/Materials/ShaderMeta.h"
#include "ECS/ComponentFactory.h"
#include "ECS/ECSTemplates.h"
#include "ECS/World.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using GameEngine::Engine::Renderer::BlendPostProcessSettings;
using GameEngine::Engine::Renderer::PostProcessSettings;
using GameEngine::Engine::Renderer::SettingsBlendGroupMask;

namespace
{

// The blend's contributes mask from one flag per contributes-group.
SettingsBlendGroupMask ContributingGroups(bool colorGrade, bool heightFog, bool volumetricFog,
                                          bool atmosphericCloud, bool volumetricClouds)
{
    using GameEngine::Rendering::SettingsBlendGroup;
    SettingsBlendGroupMask groups = SettingsBlendGroupMask::None();
    groups.Set(SettingsBlendGroup::ColorGrade, colorGrade);
    groups.Set(SettingsBlendGroup::HeightFog, heightFog);
    groups.Set(SettingsBlendGroup::VolumetricFog, volumetricFog);
    groups.Set(SettingsBlendGroup::AtmosphericCloud, atmosphericCloud);
    groups.Set(SettingsBlendGroup::VolumetricClouds, volumetricClouds);
    return groups;
}

// Fields PP_SHARED_FIELDS reflects that BlendPostProcessSettings deliberately
// does NOT carry from b. Adding a field here is a statement that it must not
// blend across volumes — anything reflected but neither blended nor listed
// fails the coverage test below.
const std::unordered_set<std::string> kDeliberatelyUnblended = {
    // Camera-stamped lens fields: applied after blending from the active
    // camera (applyCameraExposure), never volume-blended.
    "dofFocusDistance",
    "dofFocalLengthMm",
    "dofAperture",
    "dofSensorHeightMm",
    "dofApertureBladeCount",
    "dofApertureRoundness",
    "dofApertureRotation",
    "dofAnamorphicSqueeze",
};

// Derived gates and aliases: computed from stored members at read time, so
// their blend coverage is their members'.
const std::unordered_set<std::string> kComputedReads = {
    "bloomLensDirtActive", "bloomHighlightsActive", "halationActive", "bloomScatteringActive",
    "scatteringOctave4Active", "scatteringOctave5Active", "scatteringOctave6Active", "scatteringOctave7Active", "scatteringOctave8Active",
    "bloomOctave4Active", "bloomOctave5Active", "bloomOctave6Active",
    "bloomOctave7Active", "bloomOctave8Active",
    "fogGlowActive", "fogGlowFastActive", "fogGlowPyramidActive",
    "fogGlowOctave4Active", "fogGlowOctave5Active",
    "fogGlowOctave6Active", "fogGlowOctave7Active", "fogGlowOctave8Active",
    "colorGradeActive", "hdrColorFxActive", "casActive", "lutActive",
    "ldrStackActive", "vhsActive", "chromaticAberrationActive", "filmSimulationGrainActive",
    "filmSimulationArtifactsActive", "crtActive", "fastBlurActive", "dofActive",
    "heatDistortionActive", "heightFogActive", "volumetricFogActive", "atmosphericCloudActive",
    "volumetricCloudsActive",
    "bloomIntensity", "volumetricFogIntensity",
};

// Word-fill a settings struct with a sentinel that is a valid, unusual float
// (and a nonzero int) so every stored field differs from its default.
PostProcessSettings MakeSentinelSettings()
{
    static_assert(sizeof(PostProcessSettings) % sizeof(uint32_t) == 0,
                  "PostProcessSettings must stay 4-byte-member layout for the word fill");
    PostProcessSettings s{};
    constexpr uint32_t kSentinelBits = 0x402A0000u; // 2.65625f
    unsigned char bytes[sizeof(PostProcessSettings)];
    for (size_t i = 0; i < sizeof(bytes); i += sizeof(uint32_t))
        std::memcpy(bytes + i, &kSentinelBits, sizeof(uint32_t));
    std::memcpy(&s, bytes, sizeof(s));
    return s;
}

float PhysicalCocPixels(float distance, float focusDistance, float focalLengthMm,
                        float aperture, float sensorHeightMm, float imageHeight)
{
    const float focal = focalLengthMm * 0.001f;
    const float focus = std::max(focusDistance, focal + 0.001f);
    const float apertureDiameter = focal / std::max(aperture, 0.95f);
    const float cocMeters = apertureDiameter * focal * (focus - distance) /
                            std::max(distance * (focus - focal), 1e-6f);
    return 0.5f * cocMeters * (imageHeight / (sensorHeightMm * 0.001f));
}

} // namespace

// Every reflected field must survive Blend(a, b, w=1) as b's value. This is
// the drift guard for PP_SHARED_FIELDS vs the hand-written blend list: a field
// added to the macro but forgotten in BlendPostProcessSettings surfaces here
// instead of as a silent non-interpolating volume boundary.
TEST(PostProcessBlendCoverage, EveryReflectedFieldBlendsOrIsExcluded)
{
    const PostProcessSettings a{};
    const PostProcessSettings b = MakeSentinelSettings();

    const PostProcessSettings out =
        BlendPostProcessSettings(a, b, 1.0f, SettingsBlendGroupMask::All());

    for (const std::string& name : PostProcessSettings::ReadableFieldNames())
    {
        if (kDeliberatelyUnblended.count(name) || kComputedReads.count(name))
            continue;
        float expected = 0.0f;
        float actual = 0.0f;
        ASSERT_TRUE(b.TryReadField(name, expected)) << name;
        ASSERT_TRUE(out.TryReadField(name, actual)) << name;
        // Exposure blends in log2 space; the w=1 roundtrip is only
        // ulp-accurate, so compare with a relative tolerance.
        EXPECT_NEAR(actual, expected, std::abs(expected) * 1e-4f + 1e-6f)
            << "field '" << name << "' is reflected in PP_SHARED_FIELDS but not carried by "
            << "BlendPostProcessSettings (add the blend line or list it as deliberately unblended)";
    }
}


// --- Settings-write registry parity (PP-ARCH Phase 1b) ----------------------
// The PP_SHARED_FIELDS X-macro became per-effect EffectSettingsField tables in
// PostProcessEffectDescriptors.cpp plus a small core appendix. These two tests
// pin the migration: the exact name set cannot drift, and every registered
// (name, offset, kind) triple must resolve the member TryReadField reports.

// Golden set of every readable name, 303 in total: 44 computed gates (11 core +
// 33 registered) + 257 shared members (16 core appendix + 241 registered) + 2
// read-only aliases. A new effect field extends this list in the same change
// that registers it; a missing name here means a registration line was lost,
// not that the list is stale.
TEST(PostProcessSettingsWrite, ReadableFieldNamesMatchGoldenSet)
{
    static const char* const kGolden[] = {
    "bloomActive", "bloomHighlightsActive", "bloomChainActive", "halationActive", "bloomScatteringActive",
    "scatteringOctave4Active", "scatteringOctave5Active", "scatteringOctave6Active", "scatteringOctave7Active", "scatteringOctave8Active",
    "bloomLensDirtActive", "bloomOctave4Active",
    "bloomOctave5Active", "bloomOctave6Active", "bloomOctave7Active", "bloomOctave8Active",
    "fogGlowActive", "fogGlowFastActive", "fogGlowPyramidActive", "fogGlowOctave4Active",
    "fogGlowOctave5Active", "fogGlowOctave6Active", "fogGlowOctave7Active", "fogGlowOctave8Active",
    "aoActive", "useAutoExposure", "colorFilterActive", "colorGradeActive",
    "hdrColorFxActive", "casActive", "lutActive", "ldrStackActive",
    "chromaticAberrationActive", "filmSimulationGrainActive", "filmSimulationArtifactsActive", "crtActive",
    "fastBlurActive", "dofActive", "heatDistortionActive", "heightFogActive",
    "volumetricFogActive", "atmosphericCloudActive", "volumetricCloudsActive", "exposure", "threshold",
    "knee", "bloomAntiFlicker", "intensity", "bloomScatteringAmount",
    "bloomTintR", "bloomTintG", "bloomTintB", "bloomRadius",
    "bloomOctaves", "bloomScatter", "bloomDepthVeilEnabled", "bloomDepthVeilIntensity",
    "bloomDepthVeilStart", "bloomDepthVeilEnd", "bloomDepthVeilTintR", "bloomDepthVeilTintG",
    "bloomDepthVeilTintB", "bloomLensDirtEnabled",
    "bloomLensDirtVignette", "bloomLensDirtVignetteIntensity", "bloomLensDirtVignetteRadius", "bloomLensDirtVignetteSmoothness",
    "bloomLensDirtVignetteRounded", "bloomLensDirtVignetteColorR", "bloomLensDirtVignetteColorG", "bloomLensDirtVignetteColorB",
    "bloomLensDirtIntensity", "bloomLensDirtScatter", "fogGlowIntensity", "fogGlowQuality",
    "fogGlowThreshold", "fogGlowKnee", "fogGlowAntiFlicker", "fogGlowOctaves",
    "fogGlowScatter", "fogGlowFadeStart", "fogGlowFadeEnd", "fogGlowTintR",
    "fogGlowTintG", "fogGlowTintB", "halationIntensity", "halationRadius",
    "halationTintR", "halationTintG", "halationTintB", "tonemapMode",
    "ditherMode", "ictcpChromaCompression", "chromaticAberrationIntensity", "chromaticAberrationStartOffset", "chromaticAberrationSaturation",
    "chromaticAberrationLongitudinal", "chromaticAberrationComa", "dofIntensity", "dofMaxRadius",
    "dofSamplingQuality", "dofDebugMode", "dofDebugAlpha", "dofFocusDistance",
    "dofFocalLengthMm", "dofAperture", "dofSensorHeightMm", "dofApertureBladeCount",
    "dofApertureRoundness", "dofApertureRotation", "dofAnamorphicSqueeze", "filmSimulationFrameRate",
    "filmSimulationGrainMode", "filmSimulationGrainIntensity", "filmSimulationGrainSize", "filmSimulationGrainSmooth",
    "filmSimulationGrainDensity", "filmSimulationGrainShadowResponse", "filmSimulationGrainMidtoneResponse", "filmSimulationGrainHighlightResponse",
    "filmSimulationGrainColored", "filmSimulationHairEnabled", "filmSimulationHairAmount", "filmSimulationHairIntensity",
    "filmSimulationHairWidth", "filmSimulationHairLength", "filmSimulationHairRandomSize", "filmSimulationHairCurl",
    "filmSimulationHairCurlRandomness", "filmSimulationScratchesEnabled", "filmSimulationScratchAmount", "filmSimulationScratchIntensity",
    "filmSimulationScratchWidth", "filmSimulationScratchLength", "filmSimulationDustEnabled", "filmSimulationDustAmount",
    "filmSimulationDustIntensity", "filmSimulationDustSize", "filmSimulationDustRandomSize", "filmSimulationGateWeaveEnabled",
    "filmSimulationGateWeaveHorizontal", "filmSimulationGateWeaveVertical", "filmSimulationGateWeaveRotation", "filmSimulationGateMask",
    "filmSimulationGateMaskFeather", "filmSimulationGateMaskRoundness", "colorFilterR", "colorFilterG",
    "colorFilterB", "colorFilterIntensity", "colorFilterBlendMode", "colorFilterStackOrder",
    "casStrength", "casStackOrder", "lutIntensity", "lutStackOrder",
    "lutInputEncoding", "lutTextureFormat", "vignetteIntensity", "vignetteSmoothness",
    "vignetteRounded", "vignetteColorR", "vignetteColorG", "vignetteColorB",
    "vignetteStackOrder", "vhsActive", "vhsIntensity", "vhsWobble",
    "vhsTracking", "vhsSignalGlitches", "vhsGlitchOffsets", "vhsInterference",
    "vhsFrameFeedback", "vhsFeedbackDecay", "vhsFeedbackMotionThreshold", "vhsFeedbackTrailLength",
    "vhsCompositeSignalMode", "vhsDotCrawl", "vhsColorBleed", "vhsColorBleedOffset",
    "vhsTapeNoise", "vhsChromaStreaks", "vhsPreFilterChromaStreaks", "vhsDropouts",
    "vhsRfDropouts", "vhsScanlines", "vhsSpeed", "vhsOverlayEnabled",
    "vhsOverlayColorR", "vhsOverlayColorG", "vhsOverlayColorB", "vhsOverlayOpacity",
    "vhsOverlaySize", "vhsOverlayFont", "vhsOverlayPositionX", "vhsOverlayPositionY",
    "vhsOverlayTextLength", "vhsOverlayText0", "vhsOverlayText1", "vhsOverlayText2",
    "vhsOverlayText3", "vhsOverlayText4", "vhsOverlayText5", "vhsOverlayText6",
    "vhsOverlayText7", "vhsDateBurnEnabled", "vhsDateBurnColorR", "vhsDateBurnColorG",
    "vhsDateBurnColorB", "vhsDateBurnSize", "vhsDateBurnPositionX", "vhsDateBurnPositionY",
    "vhsDateBurnYear", "vhsDateBurnMonth", "vhsDateBurnDay", "vhsDateBurnHour",
    "vhsDateBurnMinute", "vhsTransportMode", "vhsTransportStrength",
    "crtIntensity", "crtCurvature", "crtScanlines",
    "crtVignette", "crtAberration", "crtSoftness", "crtExposureCompensation",
    "crtEmulatedResolutionDiv", "fastBlurIntensity", "fastBlurFocusDistance", "fastBlurFocusRange",
    "fastBlurMaxRadius", "fastBlurNearBlur", "heatDistortionStrength", "heatDistortionSpeed",
    "heatDistortionScale", "heatDistortionMaskStrength", "heatDistortionDistanceStart", "heatDistortionDistanceEnd",
    "heatDistortionDirectionalFalloff", "heatDistortionUseAbsoluteY", "heatDistortionSoftness", "heightFogIntensity",
    "heightFogDensity", "heightFogMaxOpacity", "heightFogMinDistance", "heightFogSmoothLength",
    "heightFogBaseHeight", "heightFogTransitionLength", "heightFogHorizonHeightOffset", "heightFogHorizonHeightBlendStart",
    "heightFogHorizonHeightBlendEnd", "heightFogSunIntensity", "heightFogSkyEnabled", "heightFogSkyHorizonOffset",
    "heightFogSkyBottomStrength", "heightFogLayerMode", "heightFogNoiseMin", "heightFogNoiseMax",
    "heightFogNoiseFadeStart", "heightFogNoiseFadeEnd", "colorGradeShadowsR", "colorGradeShadowsG",
    "colorGradeShadowsB", "colorGradeShadowsMaster", "colorGradeMidtonesR", "colorGradeMidtonesG",
    "colorGradeMidtonesB", "colorGradeMidtonesMaster", "colorGradeHighlightsR", "colorGradeHighlightsG",
    "colorGradeHighlightsB", "colorGradeHighlightsMaster", "colorGradeContrast", "colorGradeSaturation",
    "colorGradeHueShift", "colorGradeTemperature", "colorGradeTint",
    "colorGradeInLog", "colorGradeShadowsStart", "colorGradeShadowsEnd", "colorGradeHighlightsStart",
    "colorGradeHighlightsEnd", "atmosSkyFill", "atmosVaporMass", "atmosCloudColorR",
    "atmosCloudColorG", "atmosCloudColorB", "atmosOpacity", "atmosFloorHeight", "atmosLayerDepth",
    "atmosBodyFrequency", "atmosEdgeFrequency", "atmosEdgeBreakup", "atmosDriftAngle",
    "atmosDriftRate", "atmosSunFade", "atmosSkyBounce", "atmosRimBoost",
    "atmosOcclusion", "atmosHistoryWeight", "atmosPixelScale", "bloomIntensity",
    "volumetricFogIntensity",
    };

    std::unordered_set<std::string> expected;
    for (const char* name : kGolden)
        EXPECT_TRUE(expected.insert(name).second) << "duplicate golden name: " << name;

    std::unordered_set<std::string> actual;
    for (const std::string& name : PostProcessSettings::ReadableFieldNames())
        EXPECT_TRUE(actual.insert(name).second) << "duplicate readable name: " << name;

    for (const std::string& name : expected)
        EXPECT_TRUE(actual.count(name)) << "readable name lost by the registry migration: " << name;
    for (const std::string& name : actual)
        EXPECT_TRUE(expected.count(name)) << "unexpected readable name (extend the golden list): " << name;
}

// Every registered settings field must address the member its shader name
// stands for: poke a sentinel through the registered (offset, kind) and require
// TryReadField to surface exactly that value. Catches a mistyped offsetof or a
// table entry pointing at a neighboring member. Blend-only entries (empty
// ShaderName, Phase 2) have no name to resolve; they are counted and
// bounds-checked here and behaviorally locked by the blend equivalence tests.
TEST(PostProcessSettingsWrite, RegistryFieldsResolveDeclaredMembersByName)
{
    namespace R = GameEngine::Rendering;
    std::unordered_set<std::string> seen;
    size_t namedCount = 0;
    size_t blendOnlyCount = 0;

    R::PostProcessEffectRegistry::ForEach(
        [&](const R::PostProcessEffectDescriptor& desc)
        {
            for (const R::EffectSettingsField& field : desc.SettingsFields)
            {
                ASSERT_LT(field.Offset + sizeof(float), sizeof(PostProcessSettings) + 1)
                    << desc.ComponentName;
                if (field.ShaderName.empty())
                {
                    ++blendOnlyCount;
                    continue;
                }
                ++namedCount;
                const std::string name(field.ShaderName);
                EXPECT_TRUE(seen.insert(name).second)
                    << "shader name registered by two effects: " << name;

                PostProcessSettings s{};
                auto* base = reinterpret_cast<unsigned char*>(&s);
                float expected = 0.0f;
                if (field.Type == R::EffectSettingsField::Kind::Int32)
                {
                    const int32_t sentinel = 40507;
                    std::memcpy(base + field.Offset, &sentinel, sizeof(sentinel));
                    expected = static_cast<float>(sentinel);
                }
                else
                {
                    const float sentinel = 40507.25f;
                    std::memcpy(base + field.Offset, &sentinel, sizeof(sentinel));
                    expected = sentinel;
                }

                float actual = 0.0f;
                ASSERT_TRUE(s.TryReadField(name, actual)) << name;
                EXPECT_EQ(actual, expected)
                    << "registered offset does not resolve the member behind '" << name << "'";
            }
        });

    // 257 shared members minus the 16 core/fog-glow appendix names
    // (PP_CORE_SHARED_FIELDS in PostProcessSettings.cpp). The named
    // set is an on-disk/shader contract: it must not move without extending the
    // golden name list above.
    EXPECT_EQ(namedCount, 241u);
    // Blend-only members (Phase 2): 2 bloom + 40 height fog + 62 volumetric fog
    // + 1 deband + 3 AO + 7 SSSR + 3 exposure adjustment + 35 volumetric clouds.
    EXPECT_EQ(blendOnlyCount, 153u);
}

// Every gate an effect registers is readable by name, listed once, and reads
// back its own predicate — for the default settings and for a settings struct
// where every member differs from its default.
TEST(PostProcessSettingsWrite, RegistryGatesReadTheirPredicatesByName)
{
    namespace R = GameEngine::Rendering;
    const auto& names = PostProcessSettings::ReadableFieldNames();
    const PostProcessSettings samples[] = {PostProcessSettings{}, MakeSentinelSettings()};
    std::unordered_set<std::string> seen;
    size_t gateCount = 0;

    R::PostProcessEffectRegistry::ForEach(
        [&](const R::PostProcessEffectDescriptor& desc)
        {
            for (const R::EffectSettingsGate& gate : desc.Gates)
            {
                ++gateCount;
                const std::string name(gate.ShaderName);
                ASSERT_NE(gate.IsActive, nullptr) << name;
                EXPECT_TRUE(seen.insert(name).second) << "gate registered by two effects: " << name;
                const R::EffectSettingsGate* found = R::PostProcessEffectRegistry::FindGate(gate.ShaderName);
                ASSERT_NE(found, nullptr) << name;
                EXPECT_EQ(found->IsActive, gate.IsActive) << name;
                EXPECT_EQ(std::count(names.begin(), names.end(), name), 1) << name;
                for (const PostProcessSettings& settings : samples)
                {
                    float actual = -1.0f;
                    ASSERT_TRUE(settings.TryReadField(name, actual)) << name;
                    EXPECT_EQ(actual, gate.IsActive(settings) ? 1.0f : 0.0f) << name;
                }
            }
        });

    // 44 gates in total: 33 decided by one effect, 11 in the core table.
    EXPECT_EQ(gateCount, 33u);
    EXPECT_EQ(R::PostProcessEffectRegistry::FindGate("fogGlowActive"), nullptr)
        << "a gate combining effects belongs in the core table, not one effect's registration";
}

// A skipWhen gate follows its effect's contribution, not a raw member.
TEST(PostProcessSettingsWrite, EffectGateFollowsEffectContribution)
{
    PostProcessSettings settings{};
    float crtActive = -1.0f;
    ASSERT_TRUE(settings.TryReadField("crtActive", crtActive));
    EXPECT_EQ(crtActive, 0.0f);

    settings.CrtIntensity = 1.0f;
    ASSERT_TRUE(settings.TryReadField("crtActive", crtActive));
    EXPECT_EQ(crtActive, 1.0f);

    float ldrStackActive = -1.0f;
    settings.VignetteIntensity = 0.5f;
    ASSERT_TRUE(settings.TryReadField("ldrStackActive", ldrStackActive));
    EXPECT_EQ(ldrStackActive, 1.0f);
}

// Blend metadata sanity over every registered field (registry tables; the core
// table is locked behaviorally by the equivalence tests): int members must
// carry a discrete rule — a Lerp on an int would reinterpret its bits as float.
TEST(PostProcessSettingsWrite, RegistryBlendRulesMatchFieldKinds)
{
    namespace R = GameEngine::Rendering;
    size_t checked = 0;
    R::PostProcessEffectRegistry::ForEach(
        [&](const R::PostProcessEffectDescriptor& desc)
        {
            for (const R::EffectSettingsField& field : desc.SettingsFields)
            {
                ++checked;
                if (field.Type == R::EffectSettingsField::Kind::Int32)
                {
                    EXPECT_TRUE(field.Blend == R::SettingsBlendRule::Dominant ||
                                field.Blend == R::SettingsBlendRule::Skip)
                        << desc.ComponentName << ": int32 field '" << field.ShaderName
                        << "' must dominant-pick or skip, never lerp";
                }
            }
        });
    EXPECT_EQ(checked, 394u);
    EXPECT_EQ(R::PostProcessEffectRegistry::AllSettingsFields().size(), 394u);
}

// The three-way grade runs in an ACEScct-shaped log space; the toe branch is
// where a sign/breakpoint typo hides and turns a deep black into a NaN. Pin the
// encode<->decode round-trip finite and near-identity across the full HDR range,
// especially at 0.0 (the toe) and 1e4 (deep in the log body).
TEST(PostProcessColorGrade, GradeLogRoundTripsWithoutNaN)
{
    using GameEngine::Engine::Renderer::EncodeGradeLog;
    using GameEngine::Engine::Renderer::DecodeGradeLog;

    // Exact-at-black: EncodeGradeLog(0) sits on the linear toe, DecodeGradeLog must
    // return exactly 0 (no log2(0) = -inf leaking through).
    const float encodedBlack = EncodeGradeLog(0.0f);
    EXPECT_TRUE(std::isfinite(encodedBlack));
    EXPECT_NEAR(DecodeGradeLog(encodedBlack), 0.0f, 1e-6f);

    for (float x : {0.0f, 1e-6f, 0.0078125f, 0.05f, 0.18f, 1.0f, 10.0f, 100.0f, 1e4f})
    {
        const float encoded = EncodeGradeLog(x);
        const float decoded = DecodeGradeLog(encoded);
        ASSERT_TRUE(std::isfinite(encoded)) << "encode NaN/inf at x=" << x;
        ASSERT_TRUE(std::isfinite(decoded)) << "decode NaN/inf at x=" << x;
        // Relative tolerance: the log body is only ulp-exact through a log2/exp2 pair.
        EXPECT_NEAR(decoded, x, std::abs(x) * 1e-3f + 1e-5f) << "round-trip drift at x=" << x;
    }
}

// FillColorGradeParamsUBO delta-encodes Contrast/Saturation so a zero-filled
// (unbound / placeholder) UBO is an identity grade; a default settings block must
// therefore produce an all-zero UBO.
TEST(PostProcessColorGrade, DefaultSettingsFillIdentityUBO)
{
    using GameEngine::Engine::Renderer::ColorGradeParamsUBO;
    using GameEngine::Engine::Renderer::FillColorGradeParamsUBO;

    const PostProcessSettings defaults{};
    ColorGradeParamsUBO ubo{};
    FillColorGradeParamsUBO(defaults, ubo);

    EXPECT_FLOAT_EQ(ubo.contrastMinusOne, 0.0f);
    EXPECT_FLOAT_EQ(ubo.saturationMinusOne, 0.0f);
    EXPECT_FLOAT_EQ(ubo.gradeInLinear, 0.0f) << "default grade is log";
    EXPECT_FLOAT_EQ(ubo.shadowsR, 0.0f);
    EXPECT_FLOAT_EQ(ubo.shadowsMaster, 0.0f);
    EXPECT_FLOAT_EQ(ubo.highlightsB, 0.0f);

    // Band limits are delta-encoded against the kColorGrade*Default constants,
    // so the default partition also lands at zero (zero-filled == identity at
    // the default partition).
    EXPECT_FLOAT_EQ(ubo.shadowsStartDelta, 0.0f);
    EXPECT_FLOAT_EQ(ubo.shadowsEndDelta, 0.0f);
    EXPECT_FLOAT_EQ(ubo.highlightsStartDelta, 0.0f);
    EXPECT_FLOAT_EQ(ubo.highlightsEndDelta, 0.0f);

    // Neutral white balance / hue must be EXACT zeros (not the ~1-ulp noise a
    // computed D65/D65 ratio would leave) so the shader's per-lane gates and the
    // zero-filled placeholder read identity.
    EXPECT_EQ(ubo.whiteBalanceLMinusOne, 0.0f);
    EXPECT_EQ(ubo.whiteBalanceMMinusOne, 0.0f);
    EXPECT_EQ(ubo.whiteBalanceSMinusOne, 0.0f);
    EXPECT_EQ(ubo.hueShiftTurns, 0.0f);

    // A non-default grade must round its way into the delta-encoded fields.
    PostProcessSettings graded{};
    graded.ColorGradeContrast = 1.25f;
    graded.ColorGradeSaturation = 0.5f;
    graded.ColorGradeShadowsB = 0.3f;
    graded.ColorGradeInLog = 0;
    graded.ColorGradeHighlightsStart = 0.6f;
    graded.ColorGradeHighlightsEnd = 0.8f;
    FillColorGradeParamsUBO(graded, ubo);
    EXPECT_FLOAT_EQ(ubo.contrastMinusOne, 0.25f);
    EXPECT_FLOAT_EQ(ubo.saturationMinusOne, -0.5f);
    EXPECT_FLOAT_EQ(ubo.shadowsB, 0.3f);
    EXPECT_FLOAT_EQ(ubo.gradeInLinear, 1.0f) << "linear toggle sets the flag";
    using GameEngine::Engine::Renderer::kColorGradeHighlightsStartDefault;
    using GameEngine::Engine::Renderer::kColorGradeHighlightsEndDefault;
    EXPECT_FLOAT_EQ(ubo.highlightsStartDelta, 0.6f - kColorGradeHighlightsStartDefault);
    EXPECT_FLOAT_EQ(ubo.highlightsEndDelta, 0.8f - kColorGradeHighlightsEndDefault);
}

// White balance resolves to URP's ColorBalanceToLMSCoeffs on the CPU. Pin the
// physical directions — warm temperature must gain L (long/red) against S
// (short/blue): adapting to a bluer target white pushes the render warm; and
// positive tint (a greener target white) must suppress M (medium/green)
// relative to both L and S, pushing magenta. Also pin hue degrees -> turns and
// the out-of-range clamps sharing the descriptor bounds.
TEST(PostProcessColorGrade, WhiteBalanceCoeffsFollowUrpDirections)
{
    using GameEngine::Engine::Renderer::ColorGradeParamsUBO;
    using GameEngine::Engine::Renderer::FillColorGradeParamsUBO;

    PostProcessSettings warm{};
    warm.ColorGradeTemperature = 50.0f;
    ColorGradeParamsUBO ubo{};
    FillColorGradeParamsUBO(warm, ubo);
    const float warmL = ubo.whiteBalanceLMinusOne + 1.0f;
    const float warmM = ubo.whiteBalanceMMinusOne + 1.0f;
    const float warmS = ubo.whiteBalanceSMinusOne + 1.0f;
    EXPECT_GT(warmL, warmS) << "warm push must gain red over blue";
    EXPECT_GT(warmL, 1.0f);
    EXPECT_LT(warmS, 1.0f);
    // Exact gains independently derived from URP ColorUtils.ColorBalanceToLMSCoeffs:
    // directional checks alone would survive a constant transcription slip
    // (illuminant-Y polynomial, slope asymmetry, CAT02 rows); these do not.
    EXPECT_NEAR(warmL, 1.07597f, 1e-4f);
    EXPECT_NEAR(warmM, 1.00232f, 1e-4f);
    EXPECT_NEAR(warmS, 0.71279f, 1e-4f);

    PostProcessSettings magenta{};
    magenta.ColorGradeTint = 50.0f;
    FillColorGradeParamsUBO(magenta, ubo);
    const float tintL = ubo.whiteBalanceLMinusOne + 1.0f;
    const float tintM = ubo.whiteBalanceMMinusOne + 1.0f;
    const float tintS = ubo.whiteBalanceSMinusOne + 1.0f;
    EXPECT_LT(tintM, tintL) << "positive tint must suppress green against red";
    EXPECT_LT(tintM, tintS) << "positive tint must suppress green against blue";
    EXPECT_NEAR(tintL, 1.04100f, 1e-4f);
    EXPECT_NEAR(tintM, 0.93781f, 1e-4f);
    EXPECT_NEAR(tintS, 1.24691f, 1e-4f);

    PostProcessSettings hue{};
    hue.ColorGradeHueShift = 90.0f;
    FillColorGradeParamsUBO(hue, ubo);
    EXPECT_FLOAT_EQ(ubo.hueShiftTurns, 0.25f);

    PostProcessSettings wild{};
    wild.ColorGradeTemperature = 500.0f; // clamps to +100
    wild.ColorGradeTint = -500.0f;       // clamps to -100
    wild.ColorGradeHueShift = -900.0f;   // clamps to -180 -> -0.5 turns
    FillColorGradeParamsUBO(wild, ubo);
    EXPECT_FLOAT_EQ(ubo.hueShiftTurns, -0.5f);
    PostProcessSettings edge{};
    edge.ColorGradeTemperature = 100.0f;
    edge.ColorGradeTint = -100.0f;
    ColorGradeParamsUBO edgeUbo{};
    FillColorGradeParamsUBO(edge, edgeUbo);
    EXPECT_FLOAT_EQ(ubo.whiteBalanceLMinusOne, edgeUbo.whiteBalanceLMinusOne);
    EXPECT_FLOAT_EQ(ubo.whiteBalanceMMinusOne, edgeUbo.whiteBalanceMMinusOne);
    EXPECT_FLOAT_EQ(ubo.whiteBalanceSMinusOne, edgeUbo.whiteBalanceSMinusOne);
}

// FillColorGradeParamsUBO is the single sanitize choke point for the band
// limits: starts clamp to [0,1] and each end is raised to at least
// start + kColorGradeMinBandWidth, so a reversed/degenerate authored pair can
// never reach the shader as an undefined smoothstep (edge0 >= edge1) and no
// weight can go negative.
TEST(PostProcessColorGrade, BandLimitSanitizerRepairsDegenerateOrderings)
{
    using GameEngine::Engine::Renderer::ColorGradeParamsUBO;
    using GameEngine::Engine::Renderer::FillColorGradeParamsUBO;
    using GameEngine::Engine::Renderer::kColorGradeMinBandWidth;
    using GameEngine::Engine::Renderer::kColorGradeShadowsStartDefault;
    using GameEngine::Engine::Renderer::kColorGradeShadowsEndDefault;
    using GameEngine::Engine::Renderer::kColorGradeHighlightsStartDefault;
    using GameEngine::Engine::Renderer::kColorGradeHighlightsEndDefault;

    // Reversed shadows pair + out-of-range highlights pair.
    PostProcessSettings s{};
    s.ColorGradeShadowsStart = 0.7f;
    s.ColorGradeShadowsEnd = 0.2f;      // < start: raised to start + min width
    s.ColorGradeHighlightsStart = -1.0f; // clamps to 0
    s.ColorGradeHighlightsEnd = 3.0f;    // clamps to 1
    ColorGradeParamsUBO ubo{};
    FillColorGradeParamsUBO(s, ubo);

    const float shadowsStart = kColorGradeShadowsStartDefault + ubo.shadowsStartDelta;
    const float shadowsEnd = kColorGradeShadowsEndDefault + ubo.shadowsEndDelta;
    const float highlightsStart = kColorGradeHighlightsStartDefault + ubo.highlightsStartDelta;
    const float highlightsEnd = kColorGradeHighlightsEndDefault + ubo.highlightsEndDelta;
    EXPECT_FLOAT_EQ(shadowsStart, 0.7f);
    EXPECT_FLOAT_EQ(shadowsEnd, 0.7f + kColorGradeMinBandWidth);
    EXPECT_FLOAT_EQ(highlightsStart, 0.0f);
    EXPECT_FLOAT_EQ(highlightsEnd, 1.0f);
}

// The component's band-limit defaults, the UBO delta baseline and the shader's
// kDefault* constants must be ONE partition. The first two compare directly;
// the shader is pinned by source (same style as the bloom/DoF shader guards) —
// a drifting copy would silently re-anchor every authored grade.
TEST(PostProcessColorGrade, BandLimitDefaultsAgreeAcrossComponentUboAndShader)
{
    using GameEngine::Engine::Renderer::kColorGradeShadowsStartDefault;
    using GameEngine::Engine::Renderer::kColorGradeShadowsEndDefault;
    using GameEngine::Engine::Renderer::kColorGradeHighlightsStartDefault;
    using GameEngine::Engine::Renderer::kColorGradeHighlightsEndDefault;

    const GameEngine::Components::ColorGradeEffect grade{};
    EXPECT_FLOAT_EQ(grade.ShadowsStart, kColorGradeShadowsStartDefault);
    EXPECT_FLOAT_EQ(grade.ShadowsEnd, kColorGradeShadowsEndDefault);
    EXPECT_FLOAT_EQ(grade.HighlightsStart, kColorGradeHighlightsStartDefault);
    EXPECT_FLOAT_EQ(grade.HighlightsEnd, kColorGradeHighlightsEndDefault);

    const PostProcessSettings defaults{};
    EXPECT_FLOAT_EQ(defaults.ColorGradeShadowsStart, kColorGradeShadowsStartDefault);
    EXPECT_FLOAT_EQ(defaults.ColorGradeShadowsEnd, kColorGradeShadowsEndDefault);
    EXPECT_FLOAT_EQ(defaults.ColorGradeHighlightsStart, kColorGradeHighlightsStartDefault);
    EXPECT_FLOAT_EQ(defaults.ColorGradeHighlightsEnd, kColorGradeHighlightsEndDefault);

    const std::filesystem::path shader = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Engine/Modules/Rendering/Shaders/hdr_color_fx.frag";
    std::ifstream stream(shader);
    ASSERT_TRUE(stream.is_open()) << shader;
    const std::string source((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());
    EXPECT_NE(source.find("const float kDefaultShadowsStart = 0.0;"), std::string::npos);
    EXPECT_NE(source.find("const float kDefaultShadowsEnd = 0.45;"), std::string::npos);
    EXPECT_NE(source.find("const float kDefaultHighlightsStart = 0.45;"), std::string::npos);
    EXPECT_NE(source.find("const float kDefaultHighlightsEnd = 0.95;"), std::string::npos);
    // Band masks must consume the delta-encoded limits, and the partition of
    // unity must be enforced by renormalization (wS + wH <= 1, wMid >= 0).
    EXPECT_NE(source.find("kDefaultShadowsStart + uBandLimits.x"), std::string::npos);
    EXPECT_NE(source.find("kDefaultHighlightsEnd + uBandLimits.w"), std::string::npos);
    EXPECT_NE(source.find("float wNorm = max(wShadow + wHighlight, 1.0);"), std::string::npos);
    EXPECT_NE(source.find("float wMid = max(1.0 - wShadow - wHighlight, 0.0);"), std::string::npos);
}

// The unified grade fields must survive a volume blend (they are reflected in
// PP_SHARED_FIELDS; the coverage test above catches an unblended one, this pins
// the actual interpolation).
TEST(PostProcessColorGrade, BandOffsetsAndGlobalsBlend)
{
    PostProcessSettings a{};
    PostProcessSettings b{};
    b.ColorGradeShadowsR = 0.4f;
    b.ColorGradeHighlightsB = 0.8f;
    b.ColorGradeMidtonesMaster = 0.2f;
    b.ColorGradeContrast = 1.5f;
    b.ColorGradeSaturation = 0.6f;
    b.ColorGradeShadowsEnd = 0.25f;      // a: 0.45
    b.ColorGradeHighlightsStart = 0.65f; // a: 0.45
    b.ColorGradeHighlightsEnd = 0.75f;   // a: 0.95

    const PostProcessSettings mid = BlendPostProcessSettings(a, b, 0.5f, SettingsBlendGroupMask::All());
    EXPECT_FLOAT_EQ(mid.ColorGradeShadowsR, 0.2f);
    EXPECT_FLOAT_EQ(mid.ColorGradeHighlightsB, 0.4f);
    EXPECT_FLOAT_EQ(mid.ColorGradeMidtonesMaster, 0.1f);
    EXPECT_FLOAT_EQ(mid.ColorGradeContrast, 1.25f);
    EXPECT_FLOAT_EQ(mid.ColorGradeSaturation, 0.8f);
    // Band limits interpolate like any other grade field (two ordered pairs
    // lerp to an ordered pair).
    EXPECT_FLOAT_EQ(mid.ColorGradeShadowsStart, 0.0f);
    EXPECT_FLOAT_EQ(mid.ColorGradeShadowsEnd, 0.35f);
    EXPECT_FLOAT_EQ(mid.ColorGradeHighlightsStart, 0.55f);
    EXPECT_FLOAT_EQ(mid.ColorGradeHighlightsEnd, 0.85f);

    // A volume that does not author a grade must not drag the grade toward identity.
    PostProcessSettings base{};
    base.ColorGradeShadowsR = 0.4f;
    base.ColorGradeHighlightsStart = 0.6f;
    const PostProcessSettings kept =
        BlendPostProcessSettings(base, PostProcessSettings{}, 1.0f,
                                 ContributingGroups(/*colorGrade=*/false, /*heightFog=*/true,
                                                    /*volumetricFog=*/true, /*atmosphericCloud=*/true,
                                                    /*volumetricClouds=*/true));
    EXPECT_FLOAT_EQ(kept.ColorGradeShadowsR, 0.4f);
    EXPECT_FLOAT_EQ(kept.ColorGradeHighlightsStart, 0.6f)
        << "band limits are grade fields: a non-contributing volume must not reset them";
}

TEST(PostProcessBloomControls, IndependentHalationAndScatteringGates)
{
    PostProcessSettings s{};
    s.HalationIntensity = 2.0f; s.HalationRadius = 3.0f;
    EXPECT_TRUE(s.IsHalationActive());
    EXPECT_FALSE(s.IsBloomChainActive());
    float gate = 0;
    ASSERT_TRUE(s.TryReadField("halationActive", gate));
    EXPECT_FLOAT_EQ(gate, 1);
    s.BloomScatteringAmount = 0.3f;
    EXPECT_TRUE(s.IsBloomChainActive());
    ASSERT_TRUE(s.TryReadField("bloomScatteringActive", gate));
    EXPECT_FLOAT_EQ(gate, 1);
    EXPECT_FLOAT_EQ(s.BloomIntensity, 0);
    EXPECT_FALSE(s.IsBloomHighlightsActive());
    s.BloomLensDirtEnabled = 1;
    s.BloomLensDirtIntensity = 1;
    EXPECT_FALSE(s.IsBloomLensDirtActive()) << "pure scattering must not wake highlight-only lens dirt";
}

// The combine mixes scattering in only where the scattering passes run: the gate,
// the bloom chain and the pushed amount all switch at the one threshold.
TEST(PostProcessBloomControls, PushedScatteringAmountFollowsTheScatteringGate)
{
    constexpr float kGateEpsilon = 1.0f / 1024.0f; // PostProcessSettings' default gate epsilon
    GameEngine::Rendering::ShaderMeta meta{};
    GameEngine::Rendering::PushConstantRangeMeta pc{};
    pc.Name = "PC";
    pc.Size = 4;
    pc.Block.Size = 4;
    GameEngine::Rendering::Member member{};
    member.Name = "bloomScatteringAmount";
    member.Size = 4;
    pc.Block.Members.push_back(member);
    meta.PushConstants.push_back(pc);

    for (const float amount : {0.0f, 0.9f * kGateEpsilon, 1.1f * kGateEpsilon})
    {
        PostProcessSettings s{};
        s.BloomScatteringAmount = amount;
        const bool active = amount > kGateEpsilon;
        float gate = -1.0f;
        ASSERT_TRUE(s.TryReadField("bloomScatteringActive", gate));
        EXPECT_EQ(gate, active ? 1.0f : 0.0f) << amount;
        EXPECT_EQ(s.IsBloomChainActive(), active) << amount;
        GameEngine::Rendering::NamedPushConstantWriter pcw(meta, "PC");
        ASSERT_TRUE(s.TryWriteField("bloomScatteringAmount", pcw));
        float pushed = -1.0f;
        std::memcpy(&pushed, pcw.GetBuffer().data(), sizeof(pushed));
        EXPECT_EQ(pushed, active ? amount : 0.0f) << amount;
    }
}

TEST(PostProcessChromaticAberration, UsesDepthAwareFocusAndPixelCorrectSeparation)
{
    const std::filesystem::path root = std::filesystem::path(GE_RENDERER_REPO_ROOT);
    const std::filesystem::path shader = root /
        "Packages/chromatic-aberration/Assets/Shaders/chromatic_aberration.frag";
    const std::filesystem::path graph = root / "Assets/RenderPipelines/ForwardPlus.rendergraph";

    std::ifstream shaderStream(shader);
    ASSERT_TRUE(shaderStream.is_open()) << shader;
    const std::string shaderSource((std::istreambuf_iterator<char>(shaderStream)),
                                   std::istreambuf_iterator<char>());
    EXPECT_NE(shaderSource.find("float AxialDepthWeight()"), std::string::npos);
    EXPECT_NE(shaderSource.find("LinearizeReverseZ(rawDepth)"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.dofFocusDistance"), std::string::npos);
    EXPECT_NE(shaderSource.find("normalize(radial)"), std::string::npos);
    EXPECT_NE(shaderSource.find("0.5 * lateral"), std::string::npos);
    EXPECT_NE(shaderSource.find("const vec2 offsets[8]"), std::string::npos);
    EXPECT_NE(shaderSource.find("ceil(tailLength * 0.25)"), std::string::npos);
    EXPECT_NE(shaderSource.find("i < 16"), std::string::npos);

    std::ifstream graphStream(graph);
    ASSERT_TRUE(graphStream.is_open()) << graph;
    const std::string graphSource((std::istreambuf_iterator<char>(graphStream)),
                                  std::istreambuf_iterator<char>());
    const size_t pass = graphSource.find("\"id\": \"ChromaticAberration\"");
    ASSERT_NE(pass, std::string::npos);
    const size_t nextPass = graphSource.find("\"id\":", pass + 6);
    const std::string chromaticPass = graphSource.substr(pass, nextPass - pass);
    EXPECT_NE(chromaticPass.find("\"uDepth\": \"View.DepthResolved\""), std::string::npos);
    EXPECT_NE(chromaticPass.find("\"ViewParams\": \"ViewParams\""), std::string::npos);
    EXPECT_NE(chromaticPass.find("\"dofFocusDistance\": 10.0"), std::string::npos);
}

TEST(PostProcessVhs, UsesSkippableMitLicensedPixelCorrectLdrPass)
{
    const std::filesystem::path root = std::filesystem::path(GE_RENDERER_REPO_ROOT);
    const std::filesystem::path shader =
        root / "Engine/Modules/Rendering/Shaders/vhs.frag";
    const std::filesystem::path dateBurnShader =
        root / "Engine/Modules/Rendering/Shaders/vhs_date_burn.frag";
    const std::filesystem::path frameFeedbackShader =
        root / "Engine/Modules/Rendering/Shaders/vhs_frame_feedback.frag";
    const std::filesystem::path compositeSignalShader =
        root / "Engine/Modules/Rendering/Shaders/vhs_composite_signal.frag";
    const std::filesystem::path interferenceShader =
        root / "Engine/Modules/Rendering/Shaders/vhs_interference.frag";
    const std::filesystem::path signalGlitchesShader =
        root / "Engine/Modules/Rendering/Shaders/vhs_signal_glitches.frag";
    const std::filesystem::path rfDropoutsShader =
        root / "Engine/Modules/Rendering/Shaders/vhs_rf_dropouts.frag";
    const std::filesystem::path chromaBandsShader =
        root / "Engine/Modules/Rendering/Shaders/vhs_chroma_bands.frag";
    const std::filesystem::path graph =
        root / "Assets/RenderPipelines/ForwardPlus.rendergraph";

    std::ifstream shaderStream(shader);
    ASSERT_TRUE(shaderStream.is_open()) << shader;
    const std::string shaderSource((std::istreambuf_iterator<char>(shaderStream)),
                                   std::istreambuf_iterator<char>());
    EXPECT_NE(shaderSource.find("godot-shader-crt-vhs"), std::string::npos);
    EXPECT_NE(shaderSource.find("PunikontaVhs/LICENSE.md"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsIntensity"), std::string::npos);
    EXPECT_NE(shaderSource.find("textureSize(uSceneColor, 0)"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsColorBleedOffset"), std::string::npos);
    EXPECT_NE(shaderSource.find("headSwitchMask"), std::string::npos);
    EXPECT_NE(shaderSource.find("dropoutYiq"), std::string::npos);
    EXPECT_NE(shaderSource.find("DrawVhsOverlay"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsOverlayColorR"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsOverlaySize"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsOverlayFont"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsOverlayPositionX"), std::string::npos);
    EXPECT_NE(shaderSource.find("OverlayTextCode"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsOverlayTextLength"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsTransportMode"), std::string::npos);
    EXPECT_NE(shaderSource.find("transportLocalSlip"), std::string::npos);
    EXPECT_NE(shaderSource.find("transportYiq"), std::string::npos);
    EXPECT_NE(shaderSource.find("uFieldHistory"), std::string::npos);
    EXPECT_NE(shaderSource.find("pc.vhsFieldHistoryValid"), std::string::npos);
    EXPECT_NE(shaderSource.find("retainedFieldLine"), std::string::npos);
    EXPECT_NE(shaderSource.find("mix(source.rgb, clamp(color"), std::string::npos);
    EXPECT_NE(shaderSource.find("float scanlinePhase = vUV.y * 480.0"), std::string::npos);
    EXPECT_NE(shaderSource.find("fieldParity * 0.5"), std::string::npos);
    EXPECT_NE(shaderSource.find("0.042 * scanlineStrength"), std::string::npos);
    const size_t fieldReconstruction = shaderSource.find(
        "Reconstruct alternating VHS fields");
    const size_t deckOverlay = shaderSource.find(
        "REC, the counter, rewind, and fast-forward are generated");
    const size_t scanlineResponse = shaderSource.find(
        "float scanlinePhase = vUV.y * 480.0");
    ASSERT_NE(fieldReconstruction, std::string::npos);
    ASSERT_NE(deckOverlay, std::string::npos);
    ASSERT_NE(scanlineResponse, std::string::npos);
    EXPECT_LT(fieldReconstruction, deckOverlay);
    EXPECT_LT(deckOverlay, scanlineResponse);
    EXPECT_NE(shaderSource.find("if (pc.vhsOverlayEnabled > 0.5)"),
              std::string::npos);
    EXPECT_EQ(shaderSource.find(
        "pc.vhsOverlayEnabled > 0.5 && pc.vhsTransportMode < 0.5"),
        std::string::npos);

    std::ifstream dateBurnStream(dateBurnShader);
    ASSERT_TRUE(dateBurnStream.is_open()) << dateBurnShader;
    const std::string dateBurnSource(
        (std::istreambuf_iterator<char>(dateBurnStream)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(dateBurnSource.find("pc.vhsDateBurnEnabled"), std::string::npos);
    EXPECT_NE(dateBurnSource.find("MonthCharacter"), std::string::npos);
    EXPECT_NE(dateBurnSource.find("pc.vhsDateBurnYear"), std::string::npos);
    EXPECT_NE(dateBurnSource.find("pc.vhsDateBurnColorR"), std::string::npos);
    EXPECT_NE(dateBurnSource.find("pc.vhsDateBurnSize"), std::string::npos);
    EXPECT_NE(dateBurnSource.find("pc.vhsDateBurnPositionX"), std::string::npos);
    EXPECT_NE(dateBurnSource.find("pc.vhsDateBurnPositionY"), std::string::npos);

    std::ifstream frameFeedbackStream(frameFeedbackShader);
    ASSERT_TRUE(frameFeedbackStream.is_open()) << frameFeedbackShader;
    const std::string frameFeedbackSource(
        (std::istreambuf_iterator<char>(frameFeedbackStream)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(frameFeedbackSource.find("uFeedbackHistory"), std::string::npos);
    EXPECT_NE(frameFeedbackSource.find(
        "pc.vhsFeedbackMotionThreshold"), std::string::npos);
    EXPECT_NE(frameFeedbackSource.find("previousYiq.yz"), std::string::npos);

    std::ifstream compositeSignalStream(compositeSignalShader);
    ASSERT_TRUE(compositeSignalStream.is_open()) << compositeSignalShader;
    const std::string compositeSignalSource(
        (std::istreambuf_iterator<char>(compositeSignalStream)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(compositeSignalSource.find(
        "pc.vhsCompositeSignalMode"), std::string::npos);
    EXPECT_NE(compositeSignalSource.find("float linePhase"), std::string::npos);
    EXPECT_NE(compositeSignalSource.find("float crawl"), std::string::npos);

    std::ifstream interferenceStream(interferenceShader);
    ASSERT_TRUE(interferenceStream.is_open()) << interferenceShader;
    const std::string interferenceSource(
        (std::istreambuf_iterator<char>(interferenceStream)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(interferenceSource.find("pc.vhsInterference"), std::string::npos);
    EXPECT_NE(interferenceSource.find("float humBar"), std::string::npos);

    std::ifstream signalGlitchesStream(signalGlitchesShader);
    ASSERT_TRUE(signalGlitchesStream.is_open()) << signalGlitchesShader;
    const std::string signalGlitchesSource(
        (std::istreambuf_iterator<char>(signalGlitchesStream)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(signalGlitchesSource.find("pc.vhsSignalGlitches"), std::string::npos);
    EXPECT_NE(signalGlitchesSource.find("pc.vhsGlitchOffsets"), std::string::npos);
    EXPECT_NE(signalGlitchesSource.find("for (int bandIndex = 0; bandIndex < 3; ++bandIndex)"),
              std::string::npos);
    EXPECT_NE(signalGlitchesSource.find("float slip"), std::string::npos);

    std::ifstream rfDropoutsStream(rfDropoutsShader);
    ASSERT_TRUE(rfDropoutsStream.is_open()) << rfDropoutsShader;
    const std::string rfDropoutsSource(
        (std::istreambuf_iterator<char>(rfDropoutsStream)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(rfDropoutsSource.find("pc.vhsRfDropouts"), std::string::npos);
    EXPECT_NE(rfDropoutsSource.find("float frameTick"), std::string::npos);
    EXPECT_NE(rfDropoutsSource.find("float densityScale"), std::string::npos);
    EXPECT_NE(rfDropoutsSource.find("float rowCluster"), std::string::npos);
    EXPECT_NE(rfDropoutsSource.find("float halfLength"), std::string::npos);

    std::ifstream chromaBandsStream(chromaBandsShader);
    ASSERT_TRUE(chromaBandsStream.is_open()) << chromaBandsShader;
    const std::string chromaBandsSource(
        (std::istreambuf_iterator<char>(chromaBandsStream)),
        std::istreambuf_iterator<char>());
    EXPECT_NE(chromaBandsSource.find(
        "pc.vhsPreFilterChromaStreaks"), std::string::npos);
    EXPECT_NE(chromaBandsSource.find("vec3 blur"), std::string::npos);

    std::ifstream graphStream(graph);
    ASSERT_TRUE(graphStream.is_open()) << graph;
    const std::string graphSource((std::istreambuf_iterator<char>(graphStream)),
                                  std::istreambuf_iterator<char>());
    EXPECT_NE(graphSource.find("\"LDRBeforeVhs\": {"), std::string::npos)
        << "The LDR post-FX stack output must be backed by a render-graph resource";
    const size_t pass = graphSource.find("\"id\": \"Vhs\"");
    ASSERT_NE(pass, std::string::npos);
    const size_t nextPass = graphSource.find("\"id\":", pass + 6);
    const std::string vhsPass = graphSource.substr(pass, nextPass - pass);
    EXPECT_NE(vhsPass.find("\"shaderPkg\": \"Shaders/vhs.shaderpkg\""), std::string::npos);
    EXPECT_NE(vhsPass.find("\"vhsActive\": 0.0"), std::string::npos);
    EXPECT_NE(vhsPass.find("\"passthroughInput\": \"uSceneColor\""), std::string::npos);
    EXPECT_NE(vhsPass.find(
        "\"uSceneColor\": \"LDRBeforeVhsWithChromaBands\""),
        std::string::npos);
    EXPECT_NE(vhsPass.find("\"vhsOverlayTextLength\": 3.0"), std::string::npos);
    EXPECT_NE(vhsPass.find("\"vhsTransportMode\": 0.0"), std::string::npos);
    EXPECT_NE(vhsPass.find("\"type\": \"TemporalFullscreenShader\""),
              std::string::npos);
    EXPECT_NE(vhsPass.find("\"temporalHistory\""), std::string::npos);
    EXPECT_NE(vhsPass.find("\"uFieldHistory\""), std::string::npos);

    const size_t datePass = graphSource.find("\"id\": \"VhsDateBurn\"");
    ASSERT_NE(datePass, std::string::npos);
    const size_t dateNextPass = graphSource.find("\"id\":", datePass + 6);
    const std::string vhsDatePass =
        graphSource.substr(datePass, dateNextPass - datePass);
    EXPECT_NE(vhsDatePass.find(
        "\"shaderPkg\": \"Shaders/vhs_date_burn.shaderpkg\""),
        std::string::npos);
    EXPECT_NE(vhsDatePass.find(
        "\"output\": \"LDRBeforeVhsWithDateBurn\""), std::string::npos);
    EXPECT_NE(vhsDatePass.find(
        "\"vhsDateBurnEnabled\": 0.0"), std::string::npos);

    const size_t feedbackPass =
        graphSource.find("\"id\": \"VhsFrameFeedback\"");
    const size_t compositePass =
        graphSource.find("\"id\": \"VhsCompositeSignal\"");
    const size_t interferencePass =
        graphSource.find("\"id\": \"VhsInterference\"");
    ASSERT_NE(feedbackPass, std::string::npos);
    ASSERT_NE(compositePass, std::string::npos);
    ASSERT_NE(interferencePass, std::string::npos);
    EXPECT_LT(feedbackPass, datePass);
    EXPECT_LT(datePass, compositePass);
    EXPECT_LT(compositePass, interferencePass);

    const size_t glitchesPass = graphSource.find("\"id\": \"VhsSignalGlitches\"");
    ASSERT_NE(glitchesPass, std::string::npos);
    EXPECT_LT(interferencePass, glitchesPass);
    const size_t glitchesNextPass =
        graphSource.find("\"id\":", glitchesPass + 6);
    const std::string vhsGlitchesPass =
        graphSource.substr(glitchesPass, glitchesNextPass - glitchesPass);
    EXPECT_NE(vhsGlitchesPass.find(
        "\"shaderPkg\": \"Shaders/vhs_signal_glitches.shaderpkg\""),
        std::string::npos);
    EXPECT_NE(vhsGlitchesPass.find(
        "\"output\": \"LDRBeforeVhsWithSignalGlitches\""),
        std::string::npos);

    const size_t rfPass = graphSource.find("\"id\": \"VhsRfDropouts\"");
    const size_t chromaPass = graphSource.find("\"id\": \"VhsChromaBands\"");
    ASSERT_NE(rfPass, std::string::npos);
    ASSERT_NE(chromaPass, std::string::npos);
    EXPECT_LT(rfPass, chromaPass);
    EXPECT_LT(chromaPass, pass);
}

TEST(PostProcessBloomControls, RadiusResolvesOctavesAndFractionalScalePerRenderHeight)
{
    PostProcessSettings settings{};
    settings.BloomRadius = 2.5f;
    settings.BloomOctaves = 8;

    settings.ResolveBloomPyramid(720);
    EXPECT_EQ(settings.BloomOctaves, 4);
    EXPECT_FLOAT_EQ(settings.BloomSampleScale, 1.0f);

    settings.BloomOctaves = 8;
    settings.ResolveBloomPyramid(1080);
    EXPECT_EQ(settings.BloomOctaves, 5);
    EXPECT_FLOAT_EQ(settings.BloomSampleScale, 1.0f);

    settings.BloomOctaves = 3;
    settings.ResolveBloomPyramid(2160);
    EXPECT_EQ(settings.BloomOctaves, 3) << "authored octaves remain the quality ceiling";
    EXPECT_FLOAT_EQ(settings.BloomSampleScale, 1.0f)
        << "fractional scale saturates instead of wrapping after the quality ceiling";
}

// Audit H4: with the old default octave ceiling of 3, the Radius slider went
// inert past ~1.9 at 1080p (octave count and BloomSampleScale both pinned).
// Bloom width is carried geometrically by the octave count (BloomSampleScale is
// a bounded sub-octave tent the gather clamps to [0.5, 1.5]), so the ceiling is
// what bounds how far Radius can widen. Defaulting the ceiling to the full
// pyramid keeps Radius responsive across its whole [1, 7] range.
TEST(PostProcessBloomControls, DefaultOctaveCeilingKeepsRadiusResponsiveAcrossRange)
{
    // The component default is the full pyramid; do not silently regress it.
    EXPECT_EQ(GameEngine::Components::BloomEffect{}.Octaves, 8);

    const uint32_t kHeight = 1080;
    // octaves + sampleScale - 0.5 reconstructs the continuous footprint the
    // KinoBloom mapping targets (== logExtent while below the ceiling).
    const auto compositeFootprint = [](const PostProcessSettings& s) {
        return static_cast<float>(s.BloomOctaves - 1) + s.BloomOctaveBlend + s.BloomSampleScale - 1.0f;
    };

    float prevDefault = -1.0f; // new default ceiling (full pyramid, 8)
    float prevCapped = -1.0f;  // old default ceiling (3)
    int comparedSteps = 0;
    int defaultRisingSteps = 0;
    int cappedFlatSteps = 0;
    for (int i = 0; i <= 24; ++i)
    {
        const float radius = 1.0f + static_cast<float>(i) * 0.25f;

        PostProcessSettings def{};
        def.BloomRadius = radius;
        def.BloomOctaves = 8;
        def.ResolveBloomPyramid(kHeight);

        PostProcessSettings cap{};
        cap.BloomRadius = radius;
        cap.BloomOctaves = 3;
        cap.ResolveBloomPyramid(kHeight);

        const float cDef = compositeFootprint(def);
        const float cCap = compositeFootprint(cap);
        if (i > 0)
        {
            ++comparedSteps;
            EXPECT_GE(cDef, prevDefault - 1e-4f)
                << "default-ceiling footprint must be monotonic; radius " << radius;
            if (cDef > prevDefault + 1e-4f)
                ++defaultRisingSteps;
            if (std::fabs(cCap - prevCapped) < 1e-4f)
                ++cappedFlatSteps;
        }
        prevDefault = cDef;
        prevCapped = cCap;
    }
    // At 1080p the eighth octave saturates at radius 16-log2(1080) ~= 5.92.
    // The final four quarter-steps are correctly capped by the quality ceiling.
    EXPECT_EQ(defaultRisingSteps, comparedSteps - 4)
        << "Radius must widen bloom until the eighth-octave quality ceiling";
    EXPECT_GE(cappedFlatSteps, 16)
        << "the old ceiling-3 default was inert (flat footprint) across most of the range";
}

// Existing scenes serialize BloomEffect.octaves explicitly, so they keep their
// stored ceiling and the raised component default cannot move them. The resolve
// for an explicit ceiling of 3 is unchanged by the fix — pinned here at the
// default Radius so no shipped scene shifts.
TEST(PostProcessBloomControls, ExplicitOctaveCeilingUnchangedAtDefaultRadius)
{
    PostProcessSettings s{};
    s.BloomRadius = 2.5f; // default radius
    s.BloomOctaves = 3;   // what an existing scene stores
    s.ResolveBloomPyramid(1080);
    EXPECT_EQ(s.BloomOctaves, 3);
    EXPECT_FLOAT_EQ(s.BloomSampleScale, 1.0f);
}

TEST(PostProcessFogGlowControls, RequiresFogAndResolvesSharedPyramid)
{
    PostProcessSettings settings{};
    settings.FogGlowEnabled = 1;
    settings.FogGlowIntensity = 0.5f;
    settings.FogGlowRadius = 2.5f;
    settings.FogGlowOctaves = 8;

    EXPECT_FALSE(settings.IsFogGlowActive()) << "glow cannot become a scene-wide bloom without fog";
    settings.HeightFogIntensity = 1.0f;
    settings.HeightFogDensity = 0.4f;
    settings.HeightFogDistanceFogEnabled = 1;
    EXPECT_TRUE(settings.IsFogGlowActive());

    settings.ResolveFogGlowPyramid(1080);
    EXPECT_EQ(settings.FogGlowOctaves, 4);
    EXPECT_NEAR(settings.FogGlowSampleScale, 1.07682f, 1e-4f);

    float gate = 0.0f;
    ASSERT_TRUE(settings.TryReadField("fogGlowOctave4Active", gate));
    EXPECT_FLOAT_EQ(gate, 1.0f);
    ASSERT_TRUE(settings.TryReadField("fogGlowOctave5Active", gate));
    EXPECT_FLOAT_EQ(gate, 0.0f);
}

TEST(PostProcessFogGlowControls, QualitySelectsFastPassOrPyramidCeiling)
{
    PostProcessSettings settings{};
    settings.FogGlowEnabled = 1;
    settings.FogGlowIntensity = 0.5f;
    settings.HeightFogIntensity = 1.0f;
    settings.HeightFogDensity = 0.4f;
    settings.HeightFogDistanceFogEnabled = 1;
    settings.FogGlowRadius = 7.0f;

    settings.FogGlowQuality = 0;
    EXPECT_TRUE(settings.IsFogGlowFastActive());
    EXPECT_FALSE(settings.IsFogGlowPyramidActive());

    settings.FogGlowQuality = 1;
    settings.ResolveFogGlowPyramid(2160);
    EXPECT_FALSE(settings.IsFogGlowFastActive());
    EXPECT_TRUE(settings.IsFogGlowPyramidActive());
    EXPECT_EQ(settings.FogGlowOctaves, 4);

    settings.FogGlowQuality = 2;
    settings.ResolveFogGlowPyramid(2160);
    EXPECT_EQ(settings.FogGlowOctaves, 6);

    settings.FogGlowQuality = 3;
    settings.ResolveFogGlowPyramid(2160);
    EXPECT_EQ(settings.FogGlowOctaves, 8);
}

TEST(PostProcessFogGlowControls, ExtractsFogRadianceWithoutSceneSilhouettes)
{
    const std::filesystem::path root = std::filesystem::path(GE_RENDERER_REPO_ROOT);
    const std::filesystem::path fastShader = root /
        "Engine/Modules/Rendering/Shaders/fog_glow_fast.frag";
    const std::filesystem::path prefilterShader = root /
        "Engine/Modules/Rendering/Shaders/fog_glow_prefilter.frag";
    const std::filesystem::path graph = root / "Assets/RenderPipelines/ForwardPlus.rendergraph";

    auto readText = [](const std::filesystem::path& path) {
        std::ifstream stream(path);
        EXPECT_TRUE(stream.is_open()) << path;
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    };

    const std::string fastSource = readText(fastShader);
    const std::string prefilterSource = readText(prefilterShader);
    const std::string graphSource = readText(graph);
    for (const std::string* source : {&fastSource, &prefilterSource})
    {
        EXPECT_NE(source->find("uSceneSource"), std::string::npos);
        EXPECT_NE(source->find("fogged.rgb - scene.rgb * transmittance"), std::string::npos);
    }
    EXPECT_NE(fastSource.find("fogGlowAntiFlicker"), std::string::npos);
    EXPECT_NE(fastSource.find("Median3"), std::string::npos);
    EXPECT_NE(graphSource.find("\"id\": \"FogGlowSceneSource\""), std::string::npos);
    EXPECT_NE(graphSource.find("\"uSceneSource\": \"FogGlowSceneSource\""),
              std::string::npos);
    // The glow source taps the post-clouds chain output: VolumetricClouds when
    // active, stitched through to AtmosphericCloudLayer when skipped.
    EXPECT_NE(graphSource.find("\"inputs\": { \"uTex\": \"VolumetricClouds\" }"),
              std::string::npos);

    const size_t cloudPos = graphSource.find("\"id\": \"AtmosphericCloudLayer\"");
    const size_t sourcePos = graphSource.find("\"id\": \"FogGlowSceneSource\"");
    const size_t volumetricFogPos = graphSource.find("\"id\": \"VolumetricFog\"");
    const size_t heightFogPos = graphSource.find("\"id\": \"HeightFog\"");
    ASSERT_NE(cloudPos, std::string::npos);
    ASSERT_NE(sourcePos, std::string::npos);
    ASSERT_NE(volumetricFogPos, std::string::npos);
    ASSERT_NE(heightFogPos, std::string::npos);
    EXPECT_LT(cloudPos, sourcePos);
    EXPECT_LT(sourcePos, volumetricFogPos);
    EXPECT_LT(volumetricFogPos, heightFogPos);
}

// The SebLague/Clouds port: pins the ported density/lighting math, the MIT
// provenance pointer, the temporal march wiring, and the runOnce noise bakes.
TEST(PostProcessVolumetricClouds, PortsLagueMarchWithTemporalHistoryAndBakedNoise)
{
    const std::filesystem::path root = std::filesystem::path(GE_RENDERER_REPO_ROOT);
    const std::filesystem::path marchShader = root /
        "Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_march.frag";
    const std::filesystem::path resolveShader = root /
        "Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_resolve.frag";
    const std::filesystem::path compositeShader = root /
        "Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_composite.frag";
    const std::filesystem::path shapeBake = root /
        "Engine/Modules/VolumetricClouds/Shaders/volumetric_clouds_shape_noise.comp";
    const std::filesystem::path upstream = root /
        "Engine/Modules/VolumetricClouds/ThirdParty/SebLague-Clouds/UPSTREAM.md";
    const std::filesystem::path graph = root / "Assets/RenderPipelines/ForwardPlus.rendergraph";

    auto readText = [](const std::filesystem::path& path) {
        std::ifstream stream(path);
        EXPECT_TRUE(stream.is_open()) << path;
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    };

    const std::string marchSource = readText(marchShader);
    // Ported upstream math: Beer's law extinction, two-lobe HG phase, detail
    // erosion weighted by (1-shape)^3, and the depth-limited march.
    EXPECT_NE(marchSource.find("exp(-density * stepSize * CloudParams.lightAbsorptionThroughCloud)"),
              std::string::npos);
    EXPECT_NE(marchSource.find("HenyeyGreenstein(a, -CloudParams.phaseBack)"), std::string::npos);
    EXPECT_NE(marchSource.find("oneMinusShape * oneMinusShape * oneMinusShape"), std::string::npos);
    EXPECT_NE(marchSource.find("min(sceneDst - dstToVolume, dstInsideVolume)"), std::string::npos);
    // The volume is a viewer-centered disc, not a box: no corners, no edge to reach.
    EXPECT_NE(marchSource.find("RayCloudVolumeDst"), std::string::npos);
    EXPECT_NE(marchSource.find("ge_cameraPosWS.xz"), std::string::npos);

    // Temporal additions live in the resolve pass: reprojection through the
    // previous view-proj at the march's mean cloud distance, and neighborhood
    // variance clamping of the history.
    const std::string resolveSource = readText(resolveShader);
    EXPECT_NE(resolveSource.find("ge_prevViewProj"), std::string::npos);
    EXPECT_NE(resolveSource.find("cloudsHistoryValid"), std::string::npos);
    EXPECT_NE(resolveSource.find("SampleCatmullRom"), std::string::npos);
    EXPECT_NE(resolveSource.find("minLumT"), std::string::npos);

    const std::string compositeSource = readText(compositeShader);
    EXPECT_NE(compositeSource.find("scene.rgb * clamp(clouds.g, 0.0, 1.0) + clouds.r * sunColor"),
              std::string::npos);

    const std::string shapeSource = readText(shapeBake);
    EXPECT_NE(shapeSource.find("VcLayeredWorley"), std::string::npos);
    EXPECT_NE(shapeSource.find("NoiseMinMax"), std::string::npos);

    const std::string upstreamSource = readText(upstream);
    EXPECT_NE(upstreamSource.find("https://github.com/SebLague/Clouds"), std::string::npos);
    EXPECT_NE(upstreamSource.find("MIT"), std::string::npos);

    const std::string graphSource = readText(graph);
    // Bakes run once into the persistent 3D noise textures.
    EXPECT_NE(graphSource.find("\"id\": \"CloudShapeNoiseBake\""), std::string::npos);
    EXPECT_NE(graphSource.find("\"targetTexture\": \"CloudShapeNoise\""), std::string::npos);
    EXPECT_NE(graphSource.find("\"id\": \"CloudDetailNoiseBake\""), std::string::npos);
    // The march feeds a temporal resolve; both gate on the resolved settings.
    const size_t marchPos = graphSource.find("\"id\": \"VolumetricCloudsMarch\"");
    ASSERT_NE(marchPos, std::string::npos);
    const size_t marchEnd = graphSource.find("\"id\":", marchPos + 6);
    const std::string marchPass = graphSource.substr(marchPos, marchEnd - marchPos);
    EXPECT_NE(marchPass.find("\"uDepth\": \"View.DepthResolved\""), std::string::npos);
    EXPECT_NE(marchPass.find("\"CloudParams\": \"CloudParams\""), std::string::npos);
    EXPECT_NE(marchPass.find("\"volumetricCloudsActive\": 0.0"), std::string::npos);
    const size_t resolvePos = graphSource.find("\"id\": \"VolumetricCloudsResolve\"");
    ASSERT_NE(resolvePos, std::string::npos);
    const size_t resolveEnd = graphSource.find("\"id\":", resolvePos + 6);
    const std::string resolvePass = graphSource.substr(resolvePos, resolveEnd - resolvePos);
    EXPECT_NE(resolvePass.find("\"uMarch\": \"CloudMarch\""), std::string::npos);
    EXPECT_NE(resolvePass.find("\"binding\": \"uHistory\""), std::string::npos);
    EXPECT_NE(resolvePass.find("TemporalFullscreenShader"), std::string::npos);
    // Composite sits between the atmospheric layer and the fog chain.
    const size_t compositePos = graphSource.find("\"id\": \"VolumetricCloudsComposite\"");
    const size_t volumetricFogPos = graphSource.find("\"id\": \"VolumetricFog\"");
    ASSERT_NE(compositePos, std::string::npos);
    ASSERT_NE(volumetricFogPos, std::string::npos);
    EXPECT_LT(marchPos, resolvePos);
    EXPECT_LT(resolvePos, compositePos);
    EXPECT_LT(compositePos, volumetricFogPos);
}

TEST(PostProcessAtmosphericCloudControls, AuthoredColorOwnsHueAndOpacityControlsTransmission)
{
    const std::filesystem::path shader = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Engine/Modules/Rendering/Shaders/atmospheric_cloud_layer.frag";
    std::ifstream stream(shader);
    ASSERT_TRUE(stream.is_open()) << shader;
    const std::string source{std::istreambuf_iterator<char>(stream),
                             std::istreambuf_iterator<char>()};

    EXPECT_EQ(source.find("warmLight"), std::string::npos);
    EXPECT_EQ(source.find("coolLight"), std::string::npos);
    EXPECT_NE(source.find("cloudColor * max(cloudLighting, 0.0)"), std::string::npos);
    EXPECT_NE(source.find("clamp(pc.atmosOpacity, 0.0, 8.0)"), std::string::npos);
    EXPECT_NE(source.find("scene.rgb * transmittance + scattering"), std::string::npos);
    EXPECT_EQ(source.find("mix(scene.rgb, rgb"), std::string::npos)
        << "Beer-Lambert transmittance must not be alpha-composited a second time";
}

TEST(PostProcessTonemapControls, ExposesReferenceOperators)
{
    using GameEngine::Components::TonemapMode;
    EXPECT_EQ(static_cast<int32_t>(TonemapMode::GranTurismo7), 6);
    EXPECT_EQ(static_cast<int32_t>(TonemapMode::ACES2), 7);
    EXPECT_EQ(GameEngine::Components::kTonemapModeCount, 8);
    // Display names carry the operator's published provenance, so the label is
    // the pinned surface: the enumerator names stay bare (Neutral,
    // GranTurismo7) and only what the inspector shows spells out the source.
    EXPECT_STREQ(GameEngine::Components::kTonemapModeNames[4], "Khronos PBR Neutral");
    EXPECT_STREQ(GameEngine::Components::kTonemapModeNames[6], "ICtCp Tonemapper (2025, GT7)");
    EXPECT_STREQ(GameEngine::Components::kTonemapModeNames[7], "ACES 2");
    EXPECT_TRUE(GameEngine::Components::IsValidTonemapModeValue(6));
    EXPECT_TRUE(GameEngine::Components::IsValidTonemapModeValue(7));

    const std::filesystem::path shaderRoot =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Engine/Modules/Rendering/Shaders";
    auto readText = [](const std::filesystem::path& path) {
        std::ifstream stream(path);
        EXPECT_TRUE(stream.is_open()) << path;
        return std::string(std::istreambuf_iterator<char>(stream),
                           std::istreambuf_iterator<char>());
    };

    const std::string tonemap = readText(shaderRoot / "tonemap.frag");
    const std::string gt7 = readText(shaderRoot / "Includes/tonemap_gt7.glsl");
    const std::string aces2 = readText(shaderRoot / "Includes/tonemap_aces2.glsl");
    const std::string aces2Tables =
        readText(shaderRoot / "Includes/tonemap_aces2_tables.glsl");

    EXPECT_NE(tonemap.find("case 6:  return TonemapGranTurismo7(hdr)"),
              std::string::npos);
    EXPECT_NE(tonemap.find("case 7:  return TonemapACES2(hdr, Aces2TargetOutputMax())"),
              std::string::npos);
    // ACES 2 is peak-parameterised: it routes display-referred (clamped, never
    // extended) so its tier ladder owns the headroom rendering.
    EXPECT_NE(tonemap.find("if (pc.tonemapMode == 7)\n        return min(sdrTonemapped, vec3(outputMax))"),
              std::string::npos);
    // GT7 must remain the published 2025 ICtCp operator, not the older GT Sport
    // curve commonly distributed online under the generic Gran Turismo name.
    EXPECT_NE(gt7.find("GT7RgbToICtCp"), std::string::npos);
    EXPECT_NE(gt7.find("sourceICtCp.yz * chromaScale"), std::string::npos);
    EXPECT_NE(gt7.find("mix(skewedRgb, scaledRgb, 0.6)"), std::string::npos);
    EXPECT_NE(gt7.find("Copyright (c) 2025 Polyphony Digital Inc."),
              std::string::npos);

    // ACES 2's reference transform includes CAM/JMh tone scaling, chroma
    // compression, and hue-dependent gamut compression, tiered over six peak
    // luminances (100*2^k nits) with runtime blending. Its lookup tables are
    // embedded so selecting it adds no descriptor or external-LUT dependency;
    // the tier data is generated by Tools/ShaderGen/gen_aces2_tables.py.
    EXPECT_NE(aces2.find("aces2_tonescale_fwd0"), std::string::npos);
    EXPECT_NE(aces2.find("aces2_gamut_compress0"), std::string::npos);
    EXPECT_NE(aces2.find("Aces2SelectTier"), std::string::npos);
    EXPECT_NE(aces2.find("Aces2MixGeo"), std::string::npos);
    EXPECT_NE(aces2Tables.find("aces2_reach_m_tables[6][363]"), std::string::npos);
    EXPECT_NE(aces2Tables.find("aces2_gamut_cusp_tables[6][363]"), std::string::npos);
    EXPECT_NE(aces2Tables.find("aces2_tier_scalars[6][12]"), std::string::npos);
    EXPECT_EQ(aces2.find("sampler1D"), std::string::npos);
    EXPECT_EQ(aces2Tables.find("sampler1D"), std::string::npos);
}

TEST(PostProcessFogGlowControls, HeightFogOnlyVolumeCarriesSharedGlowControls)
{
    PostProcessSettings base{};
    PostProcessSettings volume{};
    volume.FogGlowEnabled = 1;
    volume.FogGlowIntensity = 0.65f;
    volume.FogGlowScatter = 0.8f;

    const PostProcessSettings out = BlendPostProcessSettings(
        base, volume, 1.0f,
        ContributingGroups(/*colorGrade=*/false, /*heightFog=*/true, /*volumetricFog=*/false,
                           /*atmosphericCloud=*/false, /*volumetricClouds=*/true));

    EXPECT_EQ(out.FogGlowEnabled, 1);
    EXPECT_FLOAT_EQ(out.FogGlowIntensity, 0.65f);
    EXPECT_FLOAT_EQ(out.FogGlowScatter, 0.8f);
}

TEST(PostProcessFogGlowControls, VolumetricFogOnlyVolumeCarriesSharedGlowControls)
{
    PostProcessSettings base{};
    PostProcessSettings volume{};
    volume.FogGlowEnabled = 1;
    volume.FogGlowIntensity = 0.65f;
    volume.FogGlowScatter = 0.8f;

    const PostProcessSettings out = BlendPostProcessSettings(
        base, volume, 1.0f,
        ContributingGroups(/*colorGrade=*/false, /*heightFog=*/false, /*volumetricFog=*/true,
                           /*atmosphericCloud=*/false, /*volumetricClouds=*/true));

    EXPECT_EQ(out.FogGlowEnabled, 1);
    EXPECT_FLOAT_EQ(out.FogGlowIntensity, 0.65f);
    EXPECT_FLOAT_EQ(out.FogGlowScatter, 0.8f);
}

TEST(PostProcessFogGlowControls, UnrelatedVolumeDoesNotDisableFogGlow)
{
    PostProcessSettings base{};
    base.FogGlowEnabled = 1;
    base.FogGlowIntensity = 0.65f;
    base.FogGlowScatter = 0.8f;

    const PostProcessSettings out = BlendPostProcessSettings(
        base, PostProcessSettings{}, 1.0f,
        ContributingGroups(/*colorGrade=*/true, /*heightFog=*/false, /*volumetricFog=*/false,
                           /*atmosphericCloud=*/false, /*volumetricClouds=*/true));

    EXPECT_EQ(out.FogGlowEnabled, base.FogGlowEnabled);
    EXPECT_FLOAT_EQ(out.FogGlowIntensity, base.FogGlowIntensity);
    EXPECT_FLOAT_EQ(out.FogGlowScatter, base.FogGlowScatter);
}

TEST(PostProcessExposureControls, SceneViewToggleOverridesStickyViewState)
{
    const std::filesystem::path sourcePath = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Apps/Editor/Source/SceneViewController.cpp";
    std::ifstream stream(sourcePath);
    ASSERT_TRUE(stream.is_open()) << sourcePath;
    const std::string source((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());

    // SceneViewController republishes a complete post-process override every
    // frame. Re-enabling Auto with default tuning must therefore overwrite the
    // prior override's false value explicitly rather than relying on extraction
    // to replace that sticky state first.
    EXPECT_NE(source.find("pp.AutoExposureActive = autoExposureOn;"), std::string::npos);
}

TEST(PostProcessBloomControls, LensDirtKeepsWidePyramidAtThreeBloomOctaves)
{
    PostProcessSettings settings{};
    settings.BloomIntensity = 1.0f;
    settings.BloomOctaves = 3;

    float gate = -1.0f;
    ASSERT_TRUE(settings.TryReadField("bloomOctave8Active", gate));
    EXPECT_FLOAT_EQ(gate, 0.0f);

    settings.BloomLensDirtEnabled = 1;
    settings.BloomLensDirtIntensity = 1.0f;
    ASSERT_TRUE(settings.TryReadField("bloomLensDirtActive", gate));
    EXPECT_FLOAT_EQ(gate, 1.0f);
    ASSERT_TRUE(settings.TryReadField("bloomOctave8Active", gate));
    EXPECT_FLOAT_EQ(gate, 1.0f);

    settings.BloomLensDirtEnabled = 0;
    ASSERT_TRUE(settings.TryReadField("bloomLensDirtActive", gate));
    EXPECT_FLOAT_EQ(gate, 0.0f);
    ASSERT_TRUE(settings.TryReadField("bloomOctave8Active", gate));
    EXPECT_FLOAT_EQ(gate, 0.0f);
}

TEST(PostProcessBloomControls, DepthVeilCanDriveBloomWithoutBrightPass)
{
    PostProcessSettings settings{};
    settings.BloomIntensity = 0.0f;

    float gate = -1.0f;
    ASSERT_TRUE(settings.TryReadField("bloomChainActive", gate));
    EXPECT_FLOAT_EQ(gate, 0.0f);

    settings.BloomDepthVeilEnabled = 1;
    settings.BloomDepthVeilIntensity = 0.5f;
    settings.BloomDepthVeilStart = 10.0f;
    settings.BloomDepthVeilEnd = 100.0f;
    ASSERT_TRUE(settings.TryReadField("bloomChainActive", gate));
    EXPECT_FLOAT_EQ(gate, 1.0f);

    settings.BloomDepthVeilEnd = settings.BloomDepthVeilStart;
    ASSERT_TRUE(settings.TryReadField("bloomChainActive", gate));
    EXPECT_FLOAT_EQ(gate, 0.0f);
}

TEST(PostProcessDofControls, EveryAuthoredControlIsReadableByItsShaderName)
{
    struct Case
    {
        const char* Name;
        float Expected;
        std::function<void(PostProcessSettings&)> Set;
    };
    const std::vector<Case> cases = {
        {"dofIntensity", 0.73f, [](auto& s) { s.DofIntensity = 0.73f; }},
        {"dofMaxRadius", 31.0f, [](auto& s) { s.DofMaxRadius = 31.0f; }},
        {"dofSamplingQuality", 2.0f, [](auto& s) { s.DofSamplingQuality = 2; }},
        {"dofDebugMode", 1.0f, [](auto& s) { s.DofDebugMode = 1; }},
        {"dofDebugAlpha", 0.42f, [](auto& s) { s.DofDebugAlpha = 0.42f; }},
        {"dofFocusDistance", 7.25f, [](auto& s) { s.DofFocusDistance = 7.25f; }},
        {"dofFocalLengthMm", 85.0f, [](auto& s) { s.DofFocalLengthMm = 85.0f; }},
        {"dofAperture", 1.4f, [](auto& s) { s.DofAperture = 1.4f; }},
        {"dofSensorHeightMm", 36.0f, [](auto& s) { s.DofSensorHeightMm = 36.0f; }},
        {"dofApertureBladeCount", 11.0f, [](auto& s) { s.DofApertureBladeCount = 11; }},
        {"dofApertureRoundness", 0.35f, [](auto& s) { s.DofApertureRoundness = 0.35f; }},
        {"dofApertureRotation", 27.5f, [](auto& s) { s.DofApertureRotation = 27.5f; }},
        {"dofAnamorphicSqueeze", 1.8f, [](auto& s) { s.DofAnamorphicSqueeze = 1.8f; }},
    };

    const auto& names = PostProcessSettings::ReadableFieldNames();
    for (const Case& test : cases)
    {
        PostProcessSettings settings{};
        test.Set(settings);
        float actual = 0.0f;
        EXPECT_TRUE(settings.TryReadField(test.Name, actual)) << test.Name;
        EXPECT_FLOAT_EQ(actual, test.Expected) << test.Name;
        EXPECT_NE(std::find(names.begin(), names.end(), test.Name), names.end()) << test.Name;
    }
}

TEST(PostProcessDofControls, DebugVisualizerKeepsPipelineActiveAtZeroBlurIntensity)
{
    PostProcessSettings settings{};
    settings.DofIntensity = 0.0f;
    settings.DofMaxRadius = 16.0f;
    settings.DofDebugMode = 0;
    EXPECT_FALSE(settings.IsDofActive());

    settings.DofDebugMode = 1;
    EXPECT_TRUE(settings.IsDofActive());

    float active = 0.0f;
    ASSERT_TRUE(settings.TryReadField("dofActive", active));
    EXPECT_FLOAT_EQ(active, 1.0f);

    settings.DofDebugMode = 0;
    settings.DofIntensity = 1.0f;
    settings.DofMaxRadius = 0.0f;
    EXPECT_FALSE(settings.IsDofActive());
    settings.DofMaxRadius = 16.0f;
    EXPECT_TRUE(settings.IsDofActive());
}

TEST(PostProcessDofControls, FocusVisualizerShaderPreservesNearFocusFarConvention)
{
    const std::filesystem::path shader =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Packages/fidelityfx-dof/Assets/Shaders/ffx_dof_composite.comp";
    std::ifstream stream(shader);
    ASSERT_TRUE(stream.is_open()) << shader;
    const std::string source((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());

    // The visualizer must remain useful while blur intensity is zero, show
    // focus in green, near blur in blue, and far blur in red. Pin these to the
    // actual packaged shader so a control-side unit test cannot pass while the
    // GPU implementation silently changes convention.
    EXPECT_NE(source.find("float debugCoc = PhysicalCocPx(rawDepth, float(size.y))"), std::string::npos);
    EXPECT_NE(source.find("apertureDiameter = focal / max(pc.dofAperture, 0.95)"), std::string::npos);
    EXPECT_NE(source.find("smoothstep(0.1, 1.0, abs(debugCoc))"), std::string::npos);
    EXPECT_NE(source.find("vec3 sharp = vec3(0.05, 0.75, 0.15)"), std::string::npos);
    EXPECT_NE(source.find("debugCoc < 0.0 ? vec3(0.85, 0.12, 0.1) : vec3(0.15, 0.3, 0.9)"),
              std::string::npos);
    EXPECT_NE(source.find("clamp(pc.dofDebugAlpha, 0.0, 1.0)"), std::string::npos);
}

TEST(PostProcessDofControls, ReverseZSkyUsesPhysicalInfinityCoc)
{
    const std::filesystem::path shaderRoot =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Packages/fidelityfx-dof/Assets/Shaders";
    for (const char* shaderName : {"ffx_dof_prepare.comp", "ffx_dof_composite.comp"})
    {
        const std::filesystem::path shader = shaderRoot / shaderName;
        std::ifstream stream(shader);
        ASSERT_TRUE(stream.is_open()) << shader;
        const std::string source((std::istreambuf_iterator<char>(stream)),
                                 std::istreambuf_iterator<char>());

        EXPECT_NE(source.find("if (rawDepth <= 0.0)"), std::string::npos) << shader;
        EXPECT_NE(source.find("return -apertureDiameter * focal / max(focus - focal, 1e-6)"),
                  std::string::npos) << shader;
        EXPECT_EQ(source.find("return -65504.0"), std::string::npos) << shader;
        EXPECT_EQ(source.find("return -0.5 * pc.dofMaxRadius"), std::string::npos) << shader;
    }
}

TEST(PostProcessDofControls, CameraFocusDistanceDeclaresPositiveEditorMinimum)
{
    const std::filesystem::path root = std::filesystem::path(GE_RENDERER_REPO_ROOT);
    const std::filesystem::path inspector =
        root / "Apps/Editor/Source/Inspectors/CameraInspector.cpp";
    const std::filesystem::path schema =
        root / "Engine/Source/Scene/BuiltInSceneSchemas.cpp";

    std::ifstream inspectorStream(inspector);
    ASSERT_TRUE(inspectorStream.is_open()) << inspector;
    const std::string inspectorSource((std::istreambuf_iterator<char>(inspectorStream)),
                                      std::istreambuf_iterator<char>());
    EXPECT_NE(inspectorSource.find("extras, Components::Camera::kFocusDistanceMin"),
              std::string::npos);

    std::ifstream schemaStream(schema);
    ASSERT_TRUE(schemaStream.is_open()) << schema;
    const std::string schemaSource((std::istreambuf_iterator<char>(schemaStream)),
                                   std::istreambuf_iterator<char>());
    EXPECT_NE(schemaSource.find(
                  "c.FocusDistance = std::max(c.FocusDistance, Components::Camera::kFocusDistanceMin)"),
              std::string::npos);
}

TEST(PostProcessDofControls, NearFieldDilationFadesAcrossItsActivationThreshold)
{
    const std::filesystem::path shader =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Packages/fidelityfx-dof/Assets/Shaders/ffx_dof_blur.comp";
    std::ifstream stream(shader);
    ASSERT_TRUE(stream.is_open()) << shader;
    const std::string source((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());

    // Candidate radii come from a low-resolution dilated tile texture. Its
    // half-pixel activation boundary must not become a visible tile contour.
    EXPECT_NE(source.find("float kernelFade = smoothstep(0.5, 1.5, kernelRadius)"),
              std::string::npos);
    EXPECT_NE(source.find("nearOpacity *= kernelFade"), std::string::npos);
    EXPECT_NE(source.find("farOpacity *= kernelFade"), std::string::npos);
}

TEST(PostProcessDofControls, QualityUsesDecorrelatedFidelityFxStyleRings)
{
    const std::filesystem::path shader =
        std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Packages/fidelityfx-dof/Assets/Shaders/ffx_dof_blur.comp";
    std::ifstream stream(shader);
    ASSERT_TRUE(stream.is_open()) << shader;
    const std::string source((std::istreambuf_iterator<char>(stream)),
                             std::istreambuf_iterator<char>());

    EXPECT_NE(source.find("const int kMaxSampleRings = 20"), std::string::npos);
    EXPECT_NE(source.find("const int kMaxSamplesPerRing = 256"), std::string::npos);
    EXPECT_NE(source.find("int quality = clamp(pc.dofSamplingQuality, 0, 2)"), std::string::npos);
    EXPECT_NE(source.find("quality == 0 ? 6"), std::string::npos);
    EXPECT_NE(source.find("quality == 1 ? 10 : kMaxSampleRings"), std::string::npos);
    EXPECT_NE(source.find("int ringCount = clamp(int(ceil(kernelRadius)), 1, maxRings)"),
              std::string::npos);
    EXPECT_NE(source.find("int(ceil(6.25 * float(ringOrdinal) * shapeDensity * anamorphicDensity))"),
              std::string::npos);
    EXPECT_NE(source.find("float ringPhase = samplePhase + (ring & 1) * 0.5 * angleStep"),
              std::string::npos);
    EXPECT_NE(source.find("float samplePhase = SamplePhase(p)"), std::string::npos);
    EXPECT_NE(source.find("ShapedOffset(angle, radius)"), std::string::npos);
    EXPECT_NE(source.find("vec4 SampleHighlightSoftened(vec2 uv, float kernelRadius)"), std::string::npos);
    EXPECT_NE(source.find("return vec4(mix(center.rgb, filtered, highlightMask), center.a)"), std::string::npos);
}

TEST(PostProcessDofControls, LowerFNumberNarrowsPhysicalFocusRange)
{
    constexpr float focusDistance = 10.0f;
    constexpr float sampleDistance = 5.0f;
    const float narrowApertureCoc =
        std::abs(PhysicalCocPixels(sampleDistance, focusDistance, 50.0f, 16.0f, 24.0f, 1080.0f));
    const float wideApertureCoc =
        std::abs(PhysicalCocPixels(sampleDistance, focusDistance, 50.0f, 1.4f, 24.0f, 1080.0f));

    EXPECT_GT(wideApertureCoc, narrowApertureCoc * 10.0f);
    EXPECT_NEAR(PhysicalCocPixels(focusDistance, focusDistance, 50.0f, 1.4f, 24.0f, 1080.0f),
                0.0f, 1e-6f);
}

// ===========================================================================
// PP-ARCH Phase 2 — blend equivalence lock.
//
// BlendPostProcessSettings moves from a hand-written per-field ladder to a
// registry-driven fold over EffectSettingsField tables. The migration contract
// is BIT-EXACT equivalence, pinned two independent ways:
//   1. BlendReference below is a frozen, byte-for-byte transcription of the
//      shipped ladder (main @ 4695c76d2, extended with the white-balance trio
//      exactly as the WB ladder shipped them in main @ e06cb4d8a) — the spec
//      the registry fold must reproduce across randomized settings pairs,
//      weights, and every contributes combination.
//   2. Engine/Tests/Fixtures/PostProcessBlendGoldens.txt carries committed
//      blend outputs, guarding the reference transcription itself. Regenerate
//      (intentional semantic changes only) with GE_PP_BLEND_GOLDEN_REGEN=1.
//
// The fixture is partitioned by what a golden can actually hold across
// platforms. PP_BLEND_LOCK_EXACT_FIELDS blends to exactly representable floats
// on the test's input grid, so those members are locked bit-for-bit by hash.
// PP_BLEND_LOCK_TOLERANCE_FIELDS (Exposure) blends through libm and is pinned
// to a ULP bound. A member in the wrong half is a false red on whichever
// platform did not author the fixture, so both halves carry a proof test rather
// than a claim.
//
// Their union, PP_BLEND_LOCK_FIELDS, is the complete member inventory of
// PostProcessSettings; the static_asserts below force it back in sync when the
// struct grows.
// ===========================================================================

namespace BlendLock
{

using GameEngine::float32;
using GameEngine::int32;
using GameEngine::uint32;
using GameEngine::uint64;

// Exposure is the one member whose blend output is not bit-lockable. Its
// Log2Lerp fold is exp2(lx + (ly - lx) * w) over lx/ly from std::log2, and
// neither libm call is required to be correctly rounded — the result depends on
// the platform's math library and on whether the compiler contracts the
// multiply-add. The golden fixture pins it to a ULP bound instead of a hash.
#define PP_BLEND_LOCK_TOLERANCE_FIELDS(X) \
    X(Exposure)

// Every other member's blend output is exactly representable on the input grid
// FillValue draws from, so its bits are the same on every platform and the
// fixture locks them by hash. LerpFoldIsExactOverTheGoldenGrid and
// BitLockedFieldsCarryOnlyExactArithmetic below prove that property rather than
// assuming it.
#define PP_BLEND_LOCK_EXACT_FIELDS(X) \
    X(BloomThreshold) \
    X(BloomKnee) \
    X(BloomAntiFlicker) \
    X(BloomIntensity) \
    X(BloomScatteringAmount) \
    X(BloomTintR) \
    X(BloomTintG) \
    X(BloomTintB) \
    X(BloomRadius) \
    X(BloomOctaves) \
    X(BloomSampleScale) \
    X(BloomScatter) \
    X(BloomDepthVeilEnabled) \
    X(BloomDepthVeilIntensity) \
    X(BloomDepthVeilStart) \
    X(BloomDepthVeilEnd) \
    X(BloomDepthVeilTintR) \
    X(BloomDepthVeilTintG) \
    X(BloomDepthVeilTintB) \
    X(BloomLensDirtEnabled) \
    X(BloomLensDirtVignette) \
    X(BloomLensDirtVignetteIntensity) \
    X(BloomLensDirtVignetteRadius) \
    X(BloomLensDirtVignetteSmoothness) \
    X(BloomLensDirtVignetteRounded) \
    X(BloomLensDirtVignetteColorR) \
    X(BloomLensDirtVignetteColorG) \
    X(BloomLensDirtVignetteColorB) \
    X(BloomLensDirtIntensity) \
    X(BloomLensDirtScatter) \
    X(BloomLensDirtAssetGuidWords) \
    X(HalationIntensity) \
    X(HalationRadius) \
    X(HalationTintR) \
    X(HalationTintG) \
    X(HalationTintB) \
    X(TonemapMode) \
    X(DitherMode) \
    X(IctcpChromaCompression) \
    X(ChromaticAberrationIntensity) \
    X(ChromaticAberrationStartOffset) \
    X(ChromaticAberrationSaturation) \
    X(FilmSimulationFrameRate) \
    X(FilmSimulationGrainMode) \
    X(FilmSimulationGrainIntensity) \
    X(FilmSimulationGrainSize) \
    X(FilmSimulationGrainSmooth) \
    X(FilmSimulationGrainDensity) \
    X(FilmSimulationGrainShadowResponse) \
    X(FilmSimulationGrainMidtoneResponse) \
    X(FilmSimulationGrainHighlightResponse) \
    X(FilmSimulationGrainColored) \
    X(FilmSimulationHairEnabled) \
    X(FilmSimulationHairAmount) \
    X(FilmSimulationHairIntensity) \
    X(FilmSimulationHairWidth) \
    X(FilmSimulationHairLength) \
    X(FilmSimulationHairRandomSize) \
    X(FilmSimulationHairCurl) \
    X(FilmSimulationHairCurlRandomness) \
    X(FilmSimulationScratchesEnabled) \
    X(FilmSimulationScratchAmount) \
    X(FilmSimulationScratchIntensity) \
    X(FilmSimulationScratchWidth) \
    X(FilmSimulationScratchLength) \
    X(ChromaticAberrationLongitudinal) \
    X(ChromaticAberrationComa) \
    X(DofIntensity) \
    X(DofMaxRadius) \
    X(DofSamplingQuality) \
    X(DofDebugMode) \
    X(DofDebugAlpha) \
    X(DofFocusDistance) \
    X(DofFocalLengthMm) \
    X(DofAperture) \
    X(DofSensorHeightMm) \
    X(DofApertureBladeCount) \
    X(DofApertureRoundness) \
    X(DofApertureRotation) \
    X(DofAnamorphicSqueeze) \
    X(FilmSimulationDustEnabled) \
    X(FilmSimulationDustAmount) \
    X(FilmSimulationDustIntensity) \
    X(FilmSimulationDustSize) \
    X(FilmSimulationDustRandomSize) \
    X(FilmSimulationGateWeaveEnabled) \
    X(FilmSimulationGateWeaveHorizontal) \
    X(FilmSimulationGateWeaveVertical) \
    X(FilmSimulationGateWeaveRotation) \
    X(FilmSimulationGateMask) \
    X(FilmSimulationGateMaskFeather) \
    X(FilmSimulationGateMaskRoundness) \
    X(DebandThresholdLsb) \
    X(ColorFilterR) \
    X(ColorFilterG) \
    X(ColorFilterB) \
    X(ColorFilterIntensity) \
    X(ColorFilterBlendMode) \
    X(ColorFilterStackOrder) \
    X(CasStrength) \
    X(CasStackOrder) \
    X(LutIntensity) \
    X(LutStackOrder) \
    X(LutInputEncoding) \
    X(LutTextureFormat) \
    X(LutAssetGuidWords) \
    X(VignetteIntensity) \
    X(VignetteSmoothness) \
    X(VignetteRounded) \
    X(VignetteColorR) \
    X(VignetteColorG) \
    X(VignetteColorB) \
    X(VignetteStackOrder) \
    X(VhsIntensity) \
    X(VhsWobble) \
    X(VhsTracking) \
    X(VhsSignalGlitches) \
    X(VhsGlitchOffsets) \
    X(VhsInterference) \
    X(VhsFrameFeedback) \
    X(VhsFeedbackDecay) \
    X(VhsFeedbackMotionThreshold) \
    X(VhsFeedbackTrailLength) \
    X(VhsCompositeSignalMode) \
    X(VhsDotCrawl) \
    X(VhsColorBleed) \
    X(VhsColorBleedOffset) \
    X(VhsTapeNoise) \
    X(VhsChromaStreaks) \
    X(VhsPreFilterChromaStreaks) \
    X(VhsDropouts) \
    X(VhsRfDropouts) \
    X(VhsScanlines) \
    X(VhsSpeed) \
    X(VhsOverlayEnabled) \
    X(VhsOverlayColorR) \
    X(VhsOverlayColorG) \
    X(VhsOverlayColorB) \
    X(VhsOverlayOpacity) \
    X(VhsOverlaySize) \
    X(VhsOverlayFont) \
    X(VhsOverlayPositionX) \
    X(VhsOverlayPositionY) \
    X(VhsOverlayTextLength) \
    X(VhsOverlayText0) \
    X(VhsOverlayText1) \
    X(VhsOverlayText2) \
    X(VhsOverlayText3) \
    X(VhsOverlayText4) \
    X(VhsOverlayText5) \
    X(VhsOverlayText6) \
    X(VhsOverlayText7) \
    X(VhsDateBurnEnabled) \
    X(VhsDateBurnColorR) \
    X(VhsDateBurnColorG) \
    X(VhsDateBurnColorB) \
    X(VhsDateBurnSize) \
    X(VhsDateBurnPositionX) \
    X(VhsDateBurnPositionY) \
    X(VhsDateBurnYear) \
    X(VhsDateBurnMonth) \
    X(VhsDateBurnDay) \
    X(VhsDateBurnHour) \
    X(VhsDateBurnMinute) \
    X(VhsTransportMode) \
    X(VhsTransportStrength) \
    X(CrtIntensity) \
    X(CrtCurvature) \
    X(CrtScanlines) \
    X(CrtVignette) \
    X(CrtAberration) \
    X(CrtSoftness) \
    X(CrtExposureCompensation) \
    X(CrtEmulatedResolutionDiv) \
    X(FastBlurIntensity) \
    X(FastBlurFocusDistance) \
    X(FastBlurFocusRange) \
    X(FastBlurMaxRadius) \
    X(FastBlurNearBlur) \
    X(HeatDistortionStrength) \
    X(HeatDistortionSpeed) \
    X(HeatDistortionScale) \
    X(HeatDistortionMaskStrength) \
    X(HeatDistortionDistanceStart) \
    X(HeatDistortionDistanceEnd) \
    X(HeatDistortionDirectionalFalloff) \
    X(HeatDistortionUseAbsoluteY) \
    X(HeatDistortionSoftness) \
    X(HeightFogIntensity) \
    X(HeightFogDensity) \
    X(HeightFogMaxOpacity) \
    X(HeightFogMinDistance) \
    X(HeightFogSmoothLength) \
    X(HeightFogBaseHeight) \
    X(HeightFogTransitionLength) \
    X(HeightFogEmissiveR) \
    X(HeightFogEmissiveG) \
    X(HeightFogEmissiveB) \
    X(HeightFogSunDirX) \
    X(HeightFogSunDirY) \
    X(HeightFogSunDirZ) \
    X(HeightFogSunColorR) \
    X(HeightFogSunColorG) \
    X(HeightFogSunColorB) \
    X(HeightFogSunIntensity) \
    X(HeightFogPhase) \
    X(HeightFogPhaseWeight0) \
    X(HeightFogPhaseWeight1) \
    X(HeightFogSkyEnabled) \
    X(HeightFogSkyPower) \
    X(HeightFogSkyFillStart) \
    X(HeightFogSkyFillEnd) \
    X(HeightFogDistanceFogEnabled) \
    X(HeightFogHeightFogEnabled) \
    X(HeightFogMaxDistance) \
    X(HeightFogLayerMode) \
    X(HeightFogHorizonHeightOffset) \
    X(HeightFogHorizonHeightBlendStart) \
    X(HeightFogHorizonHeightBlendEnd) \
    X(HeightFogAxisMode) \
    X(HeightFogAxisX) \
    X(HeightFogAxisY) \
    X(HeightFogAxisZ) \
    X(HeightFogGradientMode) \
    X(HeightFogGradientStrength) \
    X(HeightFogGradientLowR) \
    X(HeightFogGradientLowG) \
    X(HeightFogGradientLowB) \
    X(HeightFogGradientHighR) \
    X(HeightFogGradientHighG) \
    X(HeightFogGradientHighB) \
    X(HeightFogTrackDirectionalLight) \
    X(HeightFogSunIntensityScale) \
    X(HeightFogNoiseEnabled) \
    X(HeightFogNoiseScale) \
    X(HeightFogNoiseStrength) \
    X(HeightFogNoiseContrast) \
    X(HeightFogNoiseVelX) \
    X(HeightFogNoiseVelY) \
    X(HeightFogNoiseVelZ) \
    X(HeightFogNoiseMin) \
    X(HeightFogNoiseMax) \
    X(HeightFogNoiseFadeStart) \
    X(HeightFogNoiseFadeEnd) \
    X(HeightFogUseTimeOfDay) \
    X(HeightFogSkyHorizonOffset) \
    X(HeightFogSkyBottomStrength) \
    X(ColorGradeShadowsR) \
    X(ColorGradeShadowsG) \
    X(ColorGradeShadowsB) \
    X(ColorGradeShadowsMaster) \
    X(ColorGradeMidtonesR) \
    X(ColorGradeMidtonesG) \
    X(ColorGradeMidtonesB) \
    X(ColorGradeMidtonesMaster) \
    X(ColorGradeHighlightsR) \
    X(ColorGradeHighlightsG) \
    X(ColorGradeHighlightsB) \
    X(ColorGradeHighlightsMaster) \
    X(ColorGradeContrast) \
    X(ColorGradeSaturation) \
    X(ColorGradeHueShift) \
    X(ColorGradeTemperature) \
    X(ColorGradeTint) \
    X(ColorGradeInLog) \
    X(ColorGradeShadowsStart) \
    X(ColorGradeShadowsEnd) \
    X(ColorGradeHighlightsStart) \
    X(ColorGradeHighlightsEnd) \
    X(VolumetricFogIntensity) \
    X(VolumetricFogIsGlobal) \
    X(VolumetricFogVolumeShape) \
    X(VolumetricFogVolumeValid) \
    X(VolumetricFogVolumeBlendDistance) \
    X(VolumetricFogVolumeCenterX) \
    X(VolumetricFogVolumeCenterY) \
    X(VolumetricFogVolumeCenterZ) \
    X(VolumetricFogVolumeAxisXX) \
    X(VolumetricFogVolumeAxisXY) \
    X(VolumetricFogVolumeAxisXZ) \
    X(VolumetricFogVolumeAxisYX) \
    X(VolumetricFogVolumeAxisYY) \
    X(VolumetricFogVolumeAxisYZ) \
    X(VolumetricFogVolumeAxisZX) \
    X(VolumetricFogVolumeAxisZY) \
    X(VolumetricFogVolumeAxisZZ) \
    X(VolumetricFogVolumeHalfExtentX) \
    X(VolumetricFogVolumeHalfExtentY) \
    X(VolumetricFogVolumeHalfExtentZ) \
    X(VolumetricFogMaxDistance) \
    X(VolumetricFogXYCellSizePixels) \
    X(VolumetricFogZSliceCount) \
    X(VolumetricFogDepthDistribution) \
    X(VolumetricFogDensity) \
    X(VolumetricFogBaseHeight) \
    X(VolumetricFogHeightFalloff) \
    X(VolumetricFogSkyFade) \
    X(VolumetricFogAlbedoR) \
    X(VolumetricFogAlbedoG) \
    X(VolumetricFogAlbedoB) \
    X(VolumetricFogEmissionR) \
    X(VolumetricFogEmissionG) \
    X(VolumetricFogEmissionB) \
    X(VolumetricFogAnisotropy) \
    X(VolumetricFogTrackDirectionalLight) \
    X(VolumetricFogSunIntensityScale) \
    X(VolumetricFogSunTintR) \
    X(VolumetricFogSunTintG) \
    X(VolumetricFogSunTintB) \
    X(VolumetricFogAmbientTintR) \
    X(VolumetricFogAmbientTintG) \
    X(VolumetricFogAmbientTintB) \
    X(VolumetricFogNoiseEnabled) \
    X(VolumetricFogNoiseScale) \
    X(VolumetricFogNoiseStrength) \
    X(VolumetricFogNoiseVelocityX) \
    X(VolumetricFogNoiseVelocityY) \
    X(VolumetricFogNoiseVelocityZ) \
    X(VolumetricFogNoiseContrast) \
    X(VolumetricFogNoiseChannelWeightR) \
    X(VolumetricFogNoiseChannelWeightG) \
    X(VolumetricFogNoiseChannelWeightB) \
    X(VolumetricFogNoiseChannelWeightA) \
    X(VolumetricFogDensityThreshold) \
    X(VolumetricFogDensityThresholdSoftness) \
    X(VolumetricFogTemporalEnabled) \
    X(VolumetricFogTemporalBlend) \
    X(VolumetricFogJitterStrength) \
    X(VolumetricFogJitterMotion) \
    X(VolumetricFogCompositeDepthBias) \
    X(VolumetricFogShadowBias) \
    X(FogGlowEnabled) \
    X(FogGlowQuality) \
    X(FogGlowIntensity) \
    X(FogGlowRadius) \
    X(FogGlowOctaves) \
    X(FogGlowSampleScale) \
    X(FogGlowScatter) \
    X(FogGlowThreshold) \
    X(FogGlowKnee) \
    X(FogGlowFadeStart) \
    X(FogGlowFadeEnd) \
    X(FogGlowTintR) \
    X(FogGlowTintG) \
    X(FogGlowTintB) \
    X(FogGlowAntiFlicker) \
    X(AtmosphericCloudSkyFill) \
    X(AtmosphericCloudVaporMass) \
    X(AtmosphericCloudColorR) \
    X(AtmosphericCloudColorG) \
    X(AtmosphericCloudColorB) \
    X(AtmosphericCloudOpacity) \
    X(AtmosphericCloudFloorHeight) \
    X(AtmosphericCloudLayerDepth) \
    X(AtmosphericCloudBodyFrequency) \
    X(AtmosphericCloudEdgeFrequency) \
    X(AtmosphericCloudEdgeBreakup) \
    X(AtmosphericCloudDriftAngle) \
    X(AtmosphericCloudDriftRate) \
    X(AtmosphericCloudSunFade) \
    X(AtmosphericCloudSkyBounce) \
    X(AtmosphericCloudRimBoost) \
    X(AtmosphericCloudOcclusion) \
    X(AtmosphericCloudHistoryWeight) \
    X(AtmosphericCloudPixelScale) \
    X(CloudsRadius) \
    X(CloudsAltitude) \
    X(CloudsThickness) \
    X(CloudsNumStepsLight) \
    X(CloudsStepSize) \
    X(CloudsRayOffsetStrength) \
    X(CloudsScale) \
    X(CloudsDensityMultiplier) \
    X(CloudsDensityOffset) \
    X(CloudsShapeOffsetX) \
    X(CloudsShapeOffsetY) \
    X(CloudsShapeOffsetZ) \
    X(CloudsShapeWeightR) \
    X(CloudsShapeWeightG) \
    X(CloudsShapeWeightB) \
    X(CloudsShapeWeightA) \
    X(CloudsDetailScale) \
    X(CloudsDetailWeight) \
    X(CloudsDetailWeightR) \
    X(CloudsDetailWeightG) \
    X(CloudsDetailWeightB) \
    X(CloudsDetailOffsetX) \
    X(CloudsDetailOffsetY) \
    X(CloudsDetailOffsetZ) \
    X(CloudsAbsorptionThroughCloud) \
    X(CloudsAbsorptionTowardSun) \
    X(CloudsDarknessThreshold) \
    X(CloudsPhaseForward) \
    X(CloudsPhaseBack) \
    X(CloudsPhaseBase) \
    X(CloudsPhaseFactor) \
    X(CloudsTimeScale) \
    X(CloudsBaseSpeed) \
    X(CloudsDetailSpeed) \
    X(CloudsHistoryWeight) \
    X(AOIntensity) \
    X(AORadius) \
    X(AOThickness) \
    X(SSSRIntensity) \
    X(SSSRMaxDistance) \
    X(SSSRThickness) \
    X(SSSREdgeFade) \
    X(SSSRMaxSteps) \
    X(SSSRSampleQuality) \
    X(SSSRMultiBounce) \
    X(AutoExposureActive) \
    X(AutoExposureMinEv) \
    X(AutoExposureMaxEv) \
    X(AutoExposureSpeedUp) \
    X(AutoExposureSpeedDown) \
    X(AutoExposureBiasEv) \
    X(ExposureCompensationEv) \
    X(ExposureClampMinEv) \
    X(ExposureClampMaxEv) \
    X(BloomOctaveBlend)

// Keep this inventory order stable when extending it: randomized input values
// depend on ordinal, so appending fields preserves existing per-field goldens.
#define PP_BLEND_LOCK_FIELDS(X) \
    PP_BLEND_LOCK_TOLERANCE_FIELDS(X) \
    PP_BLEND_LOCK_EXACT_FIELDS(X)

#define PP_LOCK_COUNT(M) +1
constexpr std::size_t kFieldCount = 0 PP_BLEND_LOCK_FIELDS(PP_LOCK_COUNT);
constexpr std::size_t kExactFieldCount = 0 PP_BLEND_LOCK_EXACT_FIELDS(PP_LOCK_COUNT);
constexpr std::size_t kToleranceFieldCount = 0 PP_BLEND_LOCK_TOLERANCE_FIELDS(PP_LOCK_COUNT);
#undef PP_LOCK_COUNT
static_assert(kFieldCount == 421, "PostProcessSettings member count moved: regenerate PP_BLEND_LOCK_FIELDS");
static_assert(kToleranceFieldCount == 1, "the ULP-pinned set is Exposure alone");
static_assert(kExactFieldCount + kToleranceFieldCount == kFieldCount,
              "the lock partition must cover every member exactly once");

constexpr std::size_t kFieldBytes = 0
#define PP_LOCK_BYTES(M) +sizeof(PostProcessSettings::M)
    PP_BLEND_LOCK_FIELDS(PP_LOCK_BYTES)
#undef PP_LOCK_BYTES
    ;
// Exact padding accounting: the only padding in the struct is after the lone
// bool and (if the ABI inserts any) before/after the 8-aligned GUID word
// arrays. Accounting for it exactly means a NEW member missing from the lock
// list cannot hide inside a padding allowance.
constexpr std::size_t kPadAfterAutoExposureActive =
    offsetof(PostProcessSettings, AutoExposureMinEv) -
    offsetof(PostProcessSettings, AutoExposureActive) - sizeof(bool);
constexpr std::size_t kPadBeforeBloomDirtGuid =
    offsetof(PostProcessSettings, BloomLensDirtAssetGuidWords) -
    offsetof(PostProcessSettings, BloomLensDirtScatter) - sizeof(float32);
constexpr std::size_t kPadBeforeLutGuid =
    offsetof(PostProcessSettings, LutAssetGuidWords) -
    offsetof(PostProcessSettings, LutTextureFormat) - sizeof(int32);
constexpr std::size_t kTailPad = sizeof(PostProcessSettings) -
    offsetof(PostProcessSettings, ExposureClampMaxEv) - sizeof(float32);
// The pads are pinned to literals: a derived pad would silently absorb a NEW
// member appended after the anchor it is measured from.
static_assert(kPadAfterAutoExposureActive == 3, "layout moved: re-derive the pad pins");
static_assert(kPadBeforeBloomDirtGuid == 0, "layout moved: re-derive the pad pins");
static_assert(kPadBeforeLutGuid == 0, "layout moved: re-derive the pad pins");
// The current layout ends 4 bytes short of its 8-byte boundary (the 8-aligned GUID
// word arrays set the struct's alignment), so the tail pad is 4. Pinning it still
// ensures an appended member cannot be silently absorbed by an ABI alignment hole.
static_assert(kTailPad == 4, "layout moved: extend PP_BLEND_LOCK_FIELDS and re-derive the pad pins");
static_assert(kFieldBytes + kPadAfterAutoExposureActive + kPadBeforeBloomDirtGuid +
                  kPadBeforeLutGuid + kTailPad == sizeof(PostProcessSettings),
              "PP_BLEND_LOCK_FIELDS is stale: a PostProcessSettings member is missing from the equivalence lock");

struct LockRng
{
    uint32 State;
    explicit LockRng(uint32 seed) : State(seed != 0u ? seed : 0xA341316Cu) {}
    uint32 Next()
    {
        uint32 x = State;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        State = x;
        return x;
    }
};

// Float inputs are drawn from a dyadic grid — 4096 multiples of 1/32 centred on
// zero. No NaN/Inf, no decimal-parse ambiguity, and (proved by
// LerpFoldIsExactOverTheGoldenGrid) a lerp between any two of them at any golden
// weight lands on an exactly representable float, which is what lets the fixture
// lock those outputs by hash across compilers and configs. It buys Exposure
// nothing: log2 leaves the grid.
constexpr int kGridSteps = 4096;
constexpr int kGridBias = 2048;
constexpr float32 kGridDivisor = 32.0f;

constexpr float32 GridValue(int step)
{
    return static_cast<float32>(step - kGridBias) / kGridDivisor;
}

inline void FillValue(float32& v, LockRng& r)
{
    v = GridValue(static_cast<int>(r.Next() % static_cast<uint32>(kGridSteps)));
}
inline void FillValue(int32& v, LockRng& r) { v = static_cast<int32>(r.Next() % 9u) - 4; }
inline void FillValue(bool& v, LockRng& r) { v = (r.Next() & 1u) != 0u; }
inline void FillValue(uint64 (&v)[2], LockRng& r)
{
    v[0] = (static_cast<uint64>(r.Next()) << 32) | r.Next();
    v[1] = (static_cast<uint64>(r.Next()) << 32) | r.Next();
}

template <class T>
void FillLockMember(const char* name, T& value, LockRng& sequence, uint32 seed)
{
    // New fields get an independent stream so extending the lock does not
    // renumber every established randomized value in the committed fixture.
    if constexpr (std::is_same_v<T, float32>)
    {
        if (std::strcmp(name, "IctcpChromaCompression") == 0)
        {
            LockRng independent(seed ^ 0x49435443u); // "ICTC"
            FillValue(value, independent);
            return;
        }
    }
    FillValue(value, sequence);
}

PostProcessSettings MakeRandomSettings(uint32 seed)
{
    PostProcessSettings s{};
    LockRng r(seed);
#define PP_LOCK_FILL(M) FillLockMember(#M, s.M, r, seed);
    PP_BLEND_LOCK_FIELDS(PP_LOCK_FILL)
#undef PP_LOCK_FILL
    return s;
}

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

void FnvAccumulate(uint64_t& h, const void* p, std::size_t n)
{
    const auto* bytes = static_cast<const unsigned char*>(p);
    for (std::size_t i = 0; i < n; ++i)
    {
        h ^= bytes[i];
        h *= kFnvPrime;
    }
}

// FNV-1a 64 over the bit-locked members' object bytes in declaration order
// (padding excluded — it is indeterminate in a returned struct; Exposure
// excluded — it is ULP-pinned, not bit-locked).
uint64_t HashBitLockedFields(const PostProcessSettings& s)
{
    uint64_t h = kFnvOffsetBasis;
#define PP_LOCK_HASH(M) FnvAccumulate(h, &s.M, sizeof(s.M));
    PP_BLEND_LOCK_EXACT_FIELDS(PP_LOCK_HASH)
#undef PP_LOCK_HASH
    return h;
}

// Signed-magnitude ULP distance between two finite floats. Saturates rather
// than wrapping so a wildly wrong value reports as wildly wrong.
int64_t UlpDistance(float32 x, float32 y)
{
    if (!std::isfinite(x) || !std::isfinite(y))
        return std::numeric_limits<int64_t>::max();
    const auto ordered = [](float32 v)
    {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof(bits));
        // Map the sign-magnitude encoding onto a monotone integer line so
        // distances stay meaningful across zero.
        return (bits & 0x80000000u) != 0u ? static_cast<int64_t>(0x80000000u) - static_cast<int64_t>(bits)
                                          : static_cast<int64_t>(bits);
    };
    const int64_t d = ordered(x) - ordered(y);
    return d < 0 ? -d : d;
}

// How far Exposure may sit from its pinned golden.
//
// std::log2 and std::exp2 are faithfully but not correctly rounded, so each is
// free by one ULP of ITS OWN result — and a ULP of the exp2 argument is not a
// ULP of the output. An absolute error d in that argument lands as a relative
// error of ln2 * d, i.e. ln2 * d * 2^23 output ULP.
//
// The golden grid's worst case is the {1e-7, 1e6} exposure hatch at w = 0.5,
// where the 1e-7 clamps to the fold's 1e-6 floor: lx and ly are -/+19.93 and
// cancel to an argument of ~0, so the error in that argument is set by
// ulp(19.93) = 2^-19, not by a ULP of the argument itself. Summing log2's
// freedom (weighted 1-w and w), the fold's freedom to contract its
// multiply-add, and exp2's own ULP gives 35 ULP of spread between two
// conforming platforms; the median golden case spreads 8.
//
// 64 is that worst case with a shade under 2x headroom.
// ExposureUlpBoundCoversConformingPlatforms measures the spread rather than
// trusting this note: it fails if a golden case ever widens past the bound, and
// equally if the bound drifts far above what the fold actually needs.
constexpr int64_t kExposureUlpBound = 64;

const char* FirstMismatch(const PostProcessSettings& x, const PostProcessSettings& y)
{
#define PP_LOCK_CMP(M) if (std::memcmp(&x.M, &y.M, sizeof(x.M)) != 0) return #M;
    PP_BLEND_LOCK_FIELDS(PP_LOCK_CMP)
#undef PP_LOCK_CMP
    return nullptr;
}

std::string HexBytes(const void* p, std::size_t n)
{
    static const char* kDigits = "0123456789abcdef";
    const auto* bytes = static_cast<const unsigned char*>(p);
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i)
    {
        out.push_back(kDigits[bytes[i] >> 4]);
        out.push_back(kDigits[bytes[i] & 0xF]);
    }
    return out;
}

// --- Frozen reference: byte-for-byte transcription of the shipped
// BlendPostProcessSettings and its Copy* group helpers (main @ 4695c76d2).
// Do not "fix" or modernize this code — it IS the spec.

void RefCopyFogGlowFields(PostProcessSettings& dst, const PostProcessSettings& src)
{
    dst.FogGlowEnabled = src.FogGlowEnabled;
    dst.FogGlowQuality = src.FogGlowQuality;
    dst.FogGlowIntensity = src.FogGlowIntensity;
    dst.FogGlowRadius = src.FogGlowRadius;
    dst.FogGlowOctaves = src.FogGlowOctaves;
    dst.FogGlowSampleScale = src.FogGlowSampleScale;
    dst.FogGlowScatter = src.FogGlowScatter;
    dst.FogGlowThreshold = src.FogGlowThreshold;
    dst.FogGlowKnee = src.FogGlowKnee;
    dst.FogGlowFadeStart = src.FogGlowFadeStart;
    dst.FogGlowFadeEnd = src.FogGlowFadeEnd;
    dst.FogGlowTintR = src.FogGlowTintR;
    dst.FogGlowTintG = src.FogGlowTintG;
    dst.FogGlowTintB = src.FogGlowTintB;
    dst.FogGlowAntiFlicker = src.FogGlowAntiFlicker;
}

void RefCopyVolumetricFogFields(PostProcessSettings& dst, const PostProcessSettings& src)
{
    dst.VolumetricFogIntensity = src.VolumetricFogIntensity;
    dst.VolumetricFogIsGlobal = src.VolumetricFogIsGlobal;
    dst.VolumetricFogVolumeShape = src.VolumetricFogVolumeShape;
    dst.VolumetricFogVolumeValid = src.VolumetricFogVolumeValid;
    dst.VolumetricFogVolumeBlendDistance = src.VolumetricFogVolumeBlendDistance;
    dst.VolumetricFogVolumeCenterX = src.VolumetricFogVolumeCenterX;
    dst.VolumetricFogVolumeCenterY = src.VolumetricFogVolumeCenterY;
    dst.VolumetricFogVolumeCenterZ = src.VolumetricFogVolumeCenterZ;
    dst.VolumetricFogVolumeAxisXX = src.VolumetricFogVolumeAxisXX;
    dst.VolumetricFogVolumeAxisXY = src.VolumetricFogVolumeAxisXY;
    dst.VolumetricFogVolumeAxisXZ = src.VolumetricFogVolumeAxisXZ;
    dst.VolumetricFogVolumeAxisYX = src.VolumetricFogVolumeAxisYX;
    dst.VolumetricFogVolumeAxisYY = src.VolumetricFogVolumeAxisYY;
    dst.VolumetricFogVolumeAxisYZ = src.VolumetricFogVolumeAxisYZ;
    dst.VolumetricFogVolumeAxisZX = src.VolumetricFogVolumeAxisZX;
    dst.VolumetricFogVolumeAxisZY = src.VolumetricFogVolumeAxisZY;
    dst.VolumetricFogVolumeAxisZZ = src.VolumetricFogVolumeAxisZZ;
    dst.VolumetricFogVolumeHalfExtentX = src.VolumetricFogVolumeHalfExtentX;
    dst.VolumetricFogVolumeHalfExtentY = src.VolumetricFogVolumeHalfExtentY;
    dst.VolumetricFogVolumeHalfExtentZ = src.VolumetricFogVolumeHalfExtentZ;
    dst.VolumetricFogMaxDistance = src.VolumetricFogMaxDistance;
    dst.VolumetricFogXYCellSizePixels = src.VolumetricFogXYCellSizePixels;
    dst.VolumetricFogZSliceCount = src.VolumetricFogZSliceCount;
    dst.VolumetricFogDepthDistribution = src.VolumetricFogDepthDistribution;
    dst.VolumetricFogDensity = src.VolumetricFogDensity;
    dst.VolumetricFogBaseHeight = src.VolumetricFogBaseHeight;
    dst.VolumetricFogHeightFalloff = src.VolumetricFogHeightFalloff;
    dst.VolumetricFogSkyFade = src.VolumetricFogSkyFade;
    dst.VolumetricFogAlbedoR = src.VolumetricFogAlbedoR;
    dst.VolumetricFogAlbedoG = src.VolumetricFogAlbedoG;
    dst.VolumetricFogAlbedoB = src.VolumetricFogAlbedoB;
    dst.VolumetricFogEmissionR = src.VolumetricFogEmissionR;
    dst.VolumetricFogEmissionG = src.VolumetricFogEmissionG;
    dst.VolumetricFogEmissionB = src.VolumetricFogEmissionB;
    dst.VolumetricFogAnisotropy = src.VolumetricFogAnisotropy;
    dst.VolumetricFogTrackDirectionalLight = src.VolumetricFogTrackDirectionalLight;
    dst.VolumetricFogSunIntensityScale = src.VolumetricFogSunIntensityScale;
    dst.VolumetricFogSunTintR = src.VolumetricFogSunTintR;
    dst.VolumetricFogSunTintG = src.VolumetricFogSunTintG;
    dst.VolumetricFogSunTintB = src.VolumetricFogSunTintB;
    dst.VolumetricFogAmbientTintR = src.VolumetricFogAmbientTintR;
    dst.VolumetricFogAmbientTintG = src.VolumetricFogAmbientTintG;
    dst.VolumetricFogAmbientTintB = src.VolumetricFogAmbientTintB;
    dst.VolumetricFogNoiseEnabled = src.VolumetricFogNoiseEnabled;
    dst.VolumetricFogNoiseScale = src.VolumetricFogNoiseScale;
    dst.VolumetricFogNoiseStrength = src.VolumetricFogNoiseStrength;
    dst.VolumetricFogNoiseVelocityX = src.VolumetricFogNoiseVelocityX;
    dst.VolumetricFogNoiseVelocityY = src.VolumetricFogNoiseVelocityY;
    dst.VolumetricFogNoiseVelocityZ = src.VolumetricFogNoiseVelocityZ;
    dst.VolumetricFogNoiseContrast = src.VolumetricFogNoiseContrast;
    dst.VolumetricFogNoiseChannelWeightR = src.VolumetricFogNoiseChannelWeightR;
    dst.VolumetricFogNoiseChannelWeightG = src.VolumetricFogNoiseChannelWeightG;
    dst.VolumetricFogNoiseChannelWeightB = src.VolumetricFogNoiseChannelWeightB;
    dst.VolumetricFogNoiseChannelWeightA = src.VolumetricFogNoiseChannelWeightA;
    dst.VolumetricFogDensityThreshold = src.VolumetricFogDensityThreshold;
    dst.VolumetricFogDensityThresholdSoftness = src.VolumetricFogDensityThresholdSoftness;
    dst.VolumetricFogTemporalEnabled = src.VolumetricFogTemporalEnabled;
    dst.VolumetricFogTemporalBlend = src.VolumetricFogTemporalBlend;
    dst.VolumetricFogJitterStrength = src.VolumetricFogJitterStrength;
    dst.VolumetricFogJitterMotion = src.VolumetricFogJitterMotion;
    dst.VolumetricFogCompositeDepthBias = src.VolumetricFogCompositeDepthBias;
    dst.VolumetricFogShadowBias = src.VolumetricFogShadowBias;
}

void RefCopyHeightFogFields(PostProcessSettings& dst, const PostProcessSettings& src)
{
    dst.HeightFogIntensity = src.HeightFogIntensity;
    dst.HeightFogDensity = src.HeightFogDensity;
    dst.HeightFogMaxOpacity = src.HeightFogMaxOpacity;
    dst.HeightFogMinDistance = src.HeightFogMinDistance;
    dst.HeightFogSmoothLength = src.HeightFogSmoothLength;
    dst.HeightFogBaseHeight = src.HeightFogBaseHeight;
    dst.HeightFogTransitionLength = src.HeightFogTransitionLength;
    dst.HeightFogEmissiveR = src.HeightFogEmissiveR;
    dst.HeightFogEmissiveG = src.HeightFogEmissiveG;
    dst.HeightFogEmissiveB = src.HeightFogEmissiveB;
    dst.HeightFogSunDirX = src.HeightFogSunDirX;
    dst.HeightFogSunDirY = src.HeightFogSunDirY;
    dst.HeightFogSunDirZ = src.HeightFogSunDirZ;
    dst.HeightFogSunColorR = src.HeightFogSunColorR;
    dst.HeightFogSunColorG = src.HeightFogSunColorG;
    dst.HeightFogSunColorB = src.HeightFogSunColorB;
    dst.HeightFogSunIntensity = src.HeightFogSunIntensity;
    dst.HeightFogPhase = src.HeightFogPhase;
    dst.HeightFogPhaseWeight0 = src.HeightFogPhaseWeight0;
    dst.HeightFogPhaseWeight1 = src.HeightFogPhaseWeight1;
    dst.HeightFogSkyEnabled = src.HeightFogSkyEnabled;
    dst.HeightFogSkyPower = src.HeightFogSkyPower;
    dst.HeightFogSkyFillStart = src.HeightFogSkyFillStart;
    dst.HeightFogSkyFillEnd = src.HeightFogSkyFillEnd;
    dst.HeightFogDistanceFogEnabled = src.HeightFogDistanceFogEnabled;
    dst.HeightFogHeightFogEnabled = src.HeightFogHeightFogEnabled;
    dst.HeightFogMaxDistance = src.HeightFogMaxDistance;
    dst.HeightFogLayerMode = src.HeightFogLayerMode;
    dst.HeightFogHorizonHeightOffset = src.HeightFogHorizonHeightOffset;
    dst.HeightFogHorizonHeightBlendStart = src.HeightFogHorizonHeightBlendStart;
    dst.HeightFogHorizonHeightBlendEnd = src.HeightFogHorizonHeightBlendEnd;
    dst.HeightFogAxisMode = src.HeightFogAxisMode;
    dst.HeightFogAxisX = src.HeightFogAxisX;
    dst.HeightFogAxisY = src.HeightFogAxisY;
    dst.HeightFogAxisZ = src.HeightFogAxisZ;
    dst.HeightFogGradientMode = src.HeightFogGradientMode;
    dst.HeightFogGradientStrength = src.HeightFogGradientStrength;
    dst.HeightFogGradientLowR = src.HeightFogGradientLowR;
    dst.HeightFogGradientLowG = src.HeightFogGradientLowG;
    dst.HeightFogGradientLowB = src.HeightFogGradientLowB;
    dst.HeightFogGradientHighR = src.HeightFogGradientHighR;
    dst.HeightFogGradientHighG = src.HeightFogGradientHighG;
    dst.HeightFogGradientHighB = src.HeightFogGradientHighB;
    dst.HeightFogTrackDirectionalLight = src.HeightFogTrackDirectionalLight;
    dst.HeightFogSunIntensityScale = src.HeightFogSunIntensityScale;
    dst.HeightFogNoiseEnabled = src.HeightFogNoiseEnabled;
    dst.HeightFogNoiseScale = src.HeightFogNoiseScale;
    dst.HeightFogNoiseStrength = src.HeightFogNoiseStrength;
    dst.HeightFogNoiseContrast = src.HeightFogNoiseContrast;
    dst.HeightFogNoiseVelX = src.HeightFogNoiseVelX;
    dst.HeightFogNoiseVelY = src.HeightFogNoiseVelY;
    dst.HeightFogNoiseVelZ = src.HeightFogNoiseVelZ;
    dst.HeightFogNoiseMin = src.HeightFogNoiseMin;
    dst.HeightFogNoiseMax = src.HeightFogNoiseMax;
    dst.HeightFogNoiseFadeStart = src.HeightFogNoiseFadeStart;
    dst.HeightFogNoiseFadeEnd = src.HeightFogNoiseFadeEnd;
    dst.HeightFogUseTimeOfDay = src.HeightFogUseTimeOfDay;
    dst.HeightFogSkyHorizonOffset = src.HeightFogSkyHorizonOffset;
    dst.HeightFogSkyBottomStrength = src.HeightFogSkyBottomStrength;
}

void RefCopyAtmosphericCloudFields(PostProcessSettings& dst, const PostProcessSettings& src)
{
    dst.AtmosphericCloudSkyFill = src.AtmosphericCloudSkyFill;
    dst.AtmosphericCloudVaporMass = src.AtmosphericCloudVaporMass;
    dst.AtmosphericCloudColorR = src.AtmosphericCloudColorR;
    dst.AtmosphericCloudColorG = src.AtmosphericCloudColorG;
    dst.AtmosphericCloudColorB = src.AtmosphericCloudColorB;
    dst.AtmosphericCloudOpacity = src.AtmosphericCloudOpacity;
    dst.AtmosphericCloudFloorHeight = src.AtmosphericCloudFloorHeight;
    dst.AtmosphericCloudLayerDepth = src.AtmosphericCloudLayerDepth;
    dst.AtmosphericCloudBodyFrequency = src.AtmosphericCloudBodyFrequency;
    dst.AtmosphericCloudEdgeFrequency = src.AtmosphericCloudEdgeFrequency;
    dst.AtmosphericCloudEdgeBreakup = src.AtmosphericCloudEdgeBreakup;
    dst.AtmosphericCloudDriftAngle = src.AtmosphericCloudDriftAngle;
    dst.AtmosphericCloudDriftRate = src.AtmosphericCloudDriftRate;
    dst.AtmosphericCloudSunFade = src.AtmosphericCloudSunFade;
    dst.AtmosphericCloudSkyBounce = src.AtmosphericCloudSkyBounce;
    dst.AtmosphericCloudRimBoost = src.AtmosphericCloudRimBoost;
    dst.AtmosphericCloudOcclusion = src.AtmosphericCloudOcclusion;
    dst.AtmosphericCloudHistoryWeight = src.AtmosphericCloudHistoryWeight;
    dst.AtmosphericCloudPixelScale = src.AtmosphericCloudPixelScale;
}
void RefCopyVolumetricCloudsFields(PostProcessSettings& dst, const PostProcessSettings& src)
{
    dst.CloudsRadius = src.CloudsRadius;
    dst.CloudsAltitude = src.CloudsAltitude;
    dst.CloudsThickness = src.CloudsThickness;
    dst.CloudsNumStepsLight = src.CloudsNumStepsLight;
    dst.CloudsStepSize = src.CloudsStepSize;
    dst.CloudsRayOffsetStrength = src.CloudsRayOffsetStrength;
    dst.CloudsScale = src.CloudsScale;
    dst.CloudsDensityMultiplier = src.CloudsDensityMultiplier;
    dst.CloudsDensityOffset = src.CloudsDensityOffset;
    dst.CloudsShapeOffsetX = src.CloudsShapeOffsetX;
    dst.CloudsShapeOffsetY = src.CloudsShapeOffsetY;
    dst.CloudsShapeOffsetZ = src.CloudsShapeOffsetZ;
    dst.CloudsShapeWeightR = src.CloudsShapeWeightR;
    dst.CloudsShapeWeightG = src.CloudsShapeWeightG;
    dst.CloudsShapeWeightB = src.CloudsShapeWeightB;
    dst.CloudsShapeWeightA = src.CloudsShapeWeightA;
    dst.CloudsDetailScale = src.CloudsDetailScale;
    dst.CloudsDetailWeight = src.CloudsDetailWeight;
    dst.CloudsDetailWeightR = src.CloudsDetailWeightR;
    dst.CloudsDetailWeightG = src.CloudsDetailWeightG;
    dst.CloudsDetailWeightB = src.CloudsDetailWeightB;
    dst.CloudsDetailOffsetX = src.CloudsDetailOffsetX;
    dst.CloudsDetailOffsetY = src.CloudsDetailOffsetY;
    dst.CloudsDetailOffsetZ = src.CloudsDetailOffsetZ;
    dst.CloudsAbsorptionThroughCloud = src.CloudsAbsorptionThroughCloud;
    dst.CloudsAbsorptionTowardSun = src.CloudsAbsorptionTowardSun;
    dst.CloudsDarknessThreshold = src.CloudsDarknessThreshold;
    dst.CloudsPhaseForward = src.CloudsPhaseForward;
    dst.CloudsPhaseBack = src.CloudsPhaseBack;
    dst.CloudsPhaseBase = src.CloudsPhaseBase;
    dst.CloudsPhaseFactor = src.CloudsPhaseFactor;
    dst.CloudsTimeScale = src.CloudsTimeScale;
    dst.CloudsBaseSpeed = src.CloudsBaseSpeed;
    dst.CloudsDetailSpeed = src.CloudsDetailSpeed;
    dst.CloudsHistoryWeight = src.CloudsHistoryWeight;
}

PostProcessSettings BlendReference(
    const PostProcessSettings& a,
    const PostProcessSettings& b,
    float w,
    bool bContributesColorGrade,
    bool bContributesHeightFog,
    bool bContributesVolumetricFog,
    bool bContributesAtmosphericCloud,
    bool bContributesVolumetricClouds = true)
{
    auto lerp = [](float x, float y, float t) { return x + (y - x) * t; };

    PostProcessSettings out;
    // Exposure blends in log/stop space (geometric) so a volume crossfade is perceptually even — a
    // fade halfway between 1x and 4x exposure lands at 2x (one stop), not the arithmetic 2.5x.
    out.Exposure       = std::exp2(lerp(std::log2(std::max(a.Exposure, 1e-6f)),
                                        std::log2(std::max(b.Exposure, 1e-6f)), w));
    out.BloomThreshold = lerp(a.BloomThreshold, b.BloomThreshold, w);
    out.BloomKnee      = lerp(a.BloomKnee, b.BloomKnee, w);
    out.BloomAntiFlicker = (w >= 0.5f) ? b.BloomAntiFlicker : a.BloomAntiFlicker;
    out.BloomIntensity = lerp(a.BloomIntensity, b.BloomIntensity, w);
    out.BloomScatteringAmount  = lerp(a.BloomScatteringAmount, b.BloomScatteringAmount, w);
    out.BloomTintR     = lerp(a.BloomTintR, b.BloomTintR, w);
    out.BloomTintG     = lerp(a.BloomTintG, b.BloomTintG, w);
    out.BloomTintB     = lerp(a.BloomTintB, b.BloomTintB, w);
    out.BloomRadius    = lerp(a.BloomRadius, b.BloomRadius, w);
    out.BloomOctaves   = (w >= 0.5f) ? b.BloomOctaves : a.BloomOctaves;
    out.BloomScatter   = lerp(a.BloomScatter, b.BloomScatter, w);
    out.BloomDepthVeilEnabled = (w >= 0.5f) ? b.BloomDepthVeilEnabled : a.BloomDepthVeilEnabled;
    out.BloomDepthVeilIntensity = lerp(a.BloomDepthVeilIntensity, b.BloomDepthVeilIntensity, w);
    out.BloomDepthVeilStart = lerp(a.BloomDepthVeilStart, b.BloomDepthVeilStart, w);
    out.BloomDepthVeilEnd = lerp(a.BloomDepthVeilEnd, b.BloomDepthVeilEnd, w);
    out.BloomDepthVeilTintR = lerp(a.BloomDepthVeilTintR, b.BloomDepthVeilTintR, w);
    out.BloomDepthVeilTintG = lerp(a.BloomDepthVeilTintG, b.BloomDepthVeilTintG, w);
    out.BloomDepthVeilTintB = lerp(a.BloomDepthVeilTintB, b.BloomDepthVeilTintB, w);
    out.BloomLensDirtEnabled = (w >= 0.5f) ? b.BloomLensDirtEnabled : a.BloomLensDirtEnabled;
    out.BloomLensDirtVignette = (w >= 0.5f) ? b.BloomLensDirtVignette : a.BloomLensDirtVignette;
    out.BloomLensDirtVignetteIntensity =
        lerp(a.BloomLensDirtVignetteIntensity, b.BloomLensDirtVignetteIntensity, w);
    out.BloomLensDirtVignetteRadius =
        lerp(a.BloomLensDirtVignetteRadius, b.BloomLensDirtVignetteRadius, w);
    out.BloomLensDirtVignetteSmoothness =
        lerp(a.BloomLensDirtVignetteSmoothness, b.BloomLensDirtVignetteSmoothness, w);
    out.BloomLensDirtVignetteRounded =
        (w >= 0.5f) ? b.BloomLensDirtVignetteRounded : a.BloomLensDirtVignetteRounded;
    out.BloomLensDirtVignetteColorR =
        lerp(a.BloomLensDirtVignetteColorR, b.BloomLensDirtVignetteColorR, w);
    out.BloomLensDirtVignetteColorG =
        lerp(a.BloomLensDirtVignetteColorG, b.BloomLensDirtVignetteColorG, w);
    out.BloomLensDirtVignetteColorB =
        lerp(a.BloomLensDirtVignetteColorB, b.BloomLensDirtVignetteColorB, w);
    out.BloomLensDirtIntensity = std::clamp(
        lerp(a.BloomLensDirtIntensity, b.BloomLensDirtIntensity, w), 0.0f, 10.0f);
    out.BloomLensDirtScatter = lerp(a.BloomLensDirtScatter, b.BloomLensDirtScatter, w);
    out.BloomLensDirtAssetGuidWords[0] = 0;
    out.BloomLensDirtAssetGuidWords[1] = 0;
    out.FogGlowEnabled = (w >= 0.5f) ? b.FogGlowEnabled : a.FogGlowEnabled;
    out.FogGlowQuality = (w >= 0.5f) ? b.FogGlowQuality : a.FogGlowQuality;
    out.FogGlowIntensity = lerp(a.FogGlowIntensity, b.FogGlowIntensity, w);
    out.FogGlowRadius = lerp(a.FogGlowRadius, b.FogGlowRadius, w);
    out.FogGlowOctaves = (w >= 0.5f) ? b.FogGlowOctaves : a.FogGlowOctaves;
    // FogGlowSampleScale is a derived pyramid output (ResolveFogGlowPyramid),
    // re-resolved per view before use — no blend line (its authored value never
    // feeds the resolve, so blending it would be dead). Matches BloomSampleScale.
    out.FogGlowScatter = lerp(a.FogGlowScatter, b.FogGlowScatter, w);
    out.FogGlowThreshold = lerp(a.FogGlowThreshold, b.FogGlowThreshold, w);
    out.FogGlowKnee = lerp(a.FogGlowKnee, b.FogGlowKnee, w);
    out.FogGlowFadeStart = lerp(a.FogGlowFadeStart, b.FogGlowFadeStart, w);
    out.FogGlowFadeEnd = lerp(a.FogGlowFadeEnd, b.FogGlowFadeEnd, w);
    out.FogGlowTintR = lerp(a.FogGlowTintR, b.FogGlowTintR, w);
    out.FogGlowTintG = lerp(a.FogGlowTintG, b.FogGlowTintG, w);
    out.FogGlowTintB = lerp(a.FogGlowTintB, b.FogGlowTintB, w);
    out.FogGlowAntiFlicker = (w >= 0.5f) ? b.FogGlowAntiFlicker : a.FogGlowAntiFlicker;
    out.HalationIntensity = lerp(a.HalationIntensity, b.HalationIntensity, w);
    out.HalationRadius = lerp(a.HalationRadius, b.HalationRadius, w);
    out.HalationTintR = lerp(a.HalationTintR, b.HalationTintR, w);
    out.HalationTintG = lerp(a.HalationTintG, b.HalationTintG, w);
    out.HalationTintB = lerp(a.HalationTintB, b.HalationTintB, w);
    out.AOIntensity    = lerp(a.AOIntensity, b.AOIntensity, w);
    out.AORadius       = lerp(a.AORadius, b.AORadius, w);
    out.AOThickness    = lerp(a.AOThickness, b.AOThickness, w);
    out.SSSRIntensity   = lerp(a.SSSRIntensity, b.SSSRIntensity, w);
    out.SSSRMaxDistance = lerp(a.SSSRMaxDistance, b.SSSRMaxDistance, w);
    out.SSSRThickness   = lerp(a.SSSRThickness, b.SSSRThickness, w);
    out.SSSREdgeFade    = lerp(a.SSSREdgeFade, b.SSSREdgeFade, w);
    out.SSSRMaxSteps      = (w >= 0.5f) ? b.SSSRMaxSteps : a.SSSRMaxSteps;
    out.SSSRSampleQuality = (w >= 0.5f) ? b.SSSRSampleQuality : a.SSSRSampleQuality;
    out.SSSRMultiBounce   = (w >= 0.5f) ? b.SSSRMultiBounce : a.SSSRMultiBounce;
    out.AutoExposureMinEv     = lerp(a.AutoExposureMinEv, b.AutoExposureMinEv, w);
    out.AutoExposureMaxEv     = lerp(a.AutoExposureMaxEv, b.AutoExposureMaxEv, w);
    out.AutoExposureSpeedUp   = lerp(a.AutoExposureSpeedUp, b.AutoExposureSpeedUp, w);
    out.AutoExposureSpeedDown = lerp(a.AutoExposureSpeedDown, b.AutoExposureSpeedDown, w);
    out.AutoExposureBiasEv    = lerp(a.AutoExposureBiasEv, b.AutoExposureBiasEv, w);
    // Volume exposure modifiers: EV-space lerps fade a volume's compensation/clamps in and
    // out with its blend weight (the no-op clamp endpoints are the identity). A fading clamp
    // engages nonlinearly — it only bites once the lerped value crosses the camera envelope —
    // but the adaptation speed smooths the visible transition when crossing a volume boundary.
    out.ExposureCompensationEv = lerp(a.ExposureCompensationEv, b.ExposureCompensationEv, w);
    out.ExposureClampMinEv     = lerp(a.ExposureClampMinEv, b.ExposureClampMinEv, w);
    out.ExposureClampMaxEv     = lerp(a.ExposureClampMaxEv, b.ExposureClampMaxEv, w);
    // Discrete settings: use B when weight >= 0.5 (highest priority wins)
    out.AutoExposureActive = (w >= 0.5f) ? b.AutoExposureActive : a.AutoExposureActive;
    out.TonemapMode    = (w >= 0.5f) ? b.TonemapMode : a.TonemapMode;
    out.DitherMode     = (w >= 0.5f) ? b.DitherMode : a.DitherMode;
    out.IctcpChromaCompression = lerp(a.IctcpChromaCompression, b.IctcpChromaCompression, w);
    out.ChromaticAberrationIntensity = lerp(a.ChromaticAberrationIntensity, b.ChromaticAberrationIntensity, w);
    out.ChromaticAberrationStartOffset = lerp(a.ChromaticAberrationStartOffset, b.ChromaticAberrationStartOffset, w);
    out.ChromaticAberrationSaturation = lerp(a.ChromaticAberrationSaturation, b.ChromaticAberrationSaturation, w);
    out.ChromaticAberrationLongitudinal = lerp(a.ChromaticAberrationLongitudinal, b.ChromaticAberrationLongitudinal, w);
    out.ChromaticAberrationComa = lerp(a.ChromaticAberrationComa, b.ChromaticAberrationComa, w);
    // Only the volume-authored DoF fields blend; the camera-stamped lens fields
    // (focus distance / focal length / aperture) are applied after blending.
    out.DofIntensity = lerp(a.DofIntensity, b.DofIntensity, w);
    out.DofMaxRadius = lerp(a.DofMaxRadius, b.DofMaxRadius, w);
    out.DofSamplingQuality = (w >= 0.5f) ? b.DofSamplingQuality : a.DofSamplingQuality;
    out.DofDebugMode = (w >= 0.5f) ? b.DofDebugMode : a.DofDebugMode;
    out.DofDebugAlpha = lerp(a.DofDebugAlpha, b.DofDebugAlpha, w);
    out.FilmSimulationFrameRate = lerp(a.FilmSimulationFrameRate, b.FilmSimulationFrameRate, w);
    out.FilmSimulationGrainMode = (w >= 0.5f) ? b.FilmSimulationGrainMode : a.FilmSimulationGrainMode;
    out.FilmSimulationGrainIntensity = lerp(a.FilmSimulationGrainIntensity, b.FilmSimulationGrainIntensity, w);
    out.FilmSimulationGrainSize = lerp(a.FilmSimulationGrainSize, b.FilmSimulationGrainSize, w);
    out.FilmSimulationGrainSmooth = (w >= 0.5f) ? b.FilmSimulationGrainSmooth : a.FilmSimulationGrainSmooth;
    out.FilmSimulationGrainDensity = lerp(a.FilmSimulationGrainDensity, b.FilmSimulationGrainDensity, w);
    out.FilmSimulationGrainShadowResponse = lerp(a.FilmSimulationGrainShadowResponse, b.FilmSimulationGrainShadowResponse, w);
    out.FilmSimulationGrainMidtoneResponse = lerp(a.FilmSimulationGrainMidtoneResponse, b.FilmSimulationGrainMidtoneResponse, w);
    out.FilmSimulationGrainHighlightResponse = lerp(a.FilmSimulationGrainHighlightResponse, b.FilmSimulationGrainHighlightResponse, w);
    out.FilmSimulationGrainColored = (w >= 0.5f) ? b.FilmSimulationGrainColored : a.FilmSimulationGrainColored;
    out.FilmSimulationHairEnabled = (w >= 0.5f) ? b.FilmSimulationHairEnabled : a.FilmSimulationHairEnabled;
    out.FilmSimulationHairAmount = lerp(a.FilmSimulationHairAmount, b.FilmSimulationHairAmount, w);
    out.FilmSimulationHairIntensity = lerp(a.FilmSimulationHairIntensity, b.FilmSimulationHairIntensity, w);
    out.FilmSimulationHairWidth = lerp(a.FilmSimulationHairWidth, b.FilmSimulationHairWidth, w);
    out.FilmSimulationHairLength = lerp(a.FilmSimulationHairLength, b.FilmSimulationHairLength, w);
    out.FilmSimulationHairRandomSize = lerp(a.FilmSimulationHairRandomSize, b.FilmSimulationHairRandomSize, w);
    out.FilmSimulationHairCurl = lerp(a.FilmSimulationHairCurl, b.FilmSimulationHairCurl, w);
    out.FilmSimulationHairCurlRandomness = lerp(a.FilmSimulationHairCurlRandomness, b.FilmSimulationHairCurlRandomness, w);
    out.FilmSimulationScratchesEnabled = (w >= 0.5f) ? b.FilmSimulationScratchesEnabled : a.FilmSimulationScratchesEnabled;
    out.FilmSimulationScratchAmount = lerp(a.FilmSimulationScratchAmount, b.FilmSimulationScratchAmount, w);
    out.FilmSimulationScratchIntensity = lerp(a.FilmSimulationScratchIntensity, b.FilmSimulationScratchIntensity, w);
    out.FilmSimulationScratchWidth = lerp(a.FilmSimulationScratchWidth, b.FilmSimulationScratchWidth, w);
    out.FilmSimulationScratchLength = lerp(a.FilmSimulationScratchLength, b.FilmSimulationScratchLength, w);
    out.FilmSimulationDustEnabled = (w >= 0.5f) ? b.FilmSimulationDustEnabled : a.FilmSimulationDustEnabled;
    out.FilmSimulationDustAmount = lerp(a.FilmSimulationDustAmount, b.FilmSimulationDustAmount, w);
    out.FilmSimulationDustIntensity = lerp(a.FilmSimulationDustIntensity, b.FilmSimulationDustIntensity, w);
    out.FilmSimulationDustSize = lerp(a.FilmSimulationDustSize, b.FilmSimulationDustSize, w);
    out.FilmSimulationDustRandomSize = lerp(a.FilmSimulationDustRandomSize, b.FilmSimulationDustRandomSize, w);
    out.FilmSimulationGateWeaveEnabled = (w >= 0.5f) ? b.FilmSimulationGateWeaveEnabled : a.FilmSimulationGateWeaveEnabled;
    out.FilmSimulationGateWeaveHorizontal = lerp(a.FilmSimulationGateWeaveHorizontal, b.FilmSimulationGateWeaveHorizontal, w);
    out.FilmSimulationGateWeaveVertical = lerp(a.FilmSimulationGateWeaveVertical, b.FilmSimulationGateWeaveVertical, w);
    out.FilmSimulationGateWeaveRotation = lerp(a.FilmSimulationGateWeaveRotation, b.FilmSimulationGateWeaveRotation, w);
    out.FilmSimulationGateMask = (w >= 0.5f) ? b.FilmSimulationGateMask : a.FilmSimulationGateMask;
    out.FilmSimulationGateMaskFeather = lerp(a.FilmSimulationGateMaskFeather, b.FilmSimulationGateMaskFeather, w);
    out.FilmSimulationGateMaskRoundness = lerp(a.FilmSimulationGateMaskRoundness, b.FilmSimulationGateMaskRoundness, w);
    // A volume without a DebandEffect carries the baseline gate, so blending
    // one in fades a disabling volume back toward baseline — absent component
    // means "baseline applies here", not "keep whatever the neighbor said".
    out.DebandThresholdLsb = lerp(a.DebandThresholdLsb, b.DebandThresholdLsb, w);
    out.ColorFilterR         = lerp(a.ColorFilterR,         b.ColorFilterR,         w);
    out.ColorFilterG         = lerp(a.ColorFilterG,         b.ColorFilterG,         w);
    out.ColorFilterB         = lerp(a.ColorFilterB,         b.ColorFilterB,         w);
    out.ColorFilterIntensity = lerp(a.ColorFilterIntensity, b.ColorFilterIntensity, w);
    out.ColorFilterBlendMode = (w >= 0.5f) ? b.ColorFilterBlendMode : a.ColorFilterBlendMode;
    out.ColorFilterStackOrder = (w >= 0.5f) ? b.ColorFilterStackOrder : a.ColorFilterStackOrder;
    out.CasStrength          = lerp(a.CasStrength,          b.CasStrength,          w);
    out.CasStackOrder        = (w >= 0.5f) ? b.CasStackOrder : a.CasStackOrder;
    out.LutIntensity         = lerp(a.LutIntensity,         b.LutIntensity,         w);
    out.LutStackOrder        = (w >= 0.5f) ? b.LutStackOrder : a.LutStackOrder;
    out.LutInputEncoding     = (w >= 0.5f) ? b.LutInputEncoding : a.LutInputEncoding;
    out.LutTextureFormat     = (w >= 0.5f) ? b.LutTextureFormat : a.LutTextureFormat;
    out.LutAssetGuidWords[0] = 0;
    out.LutAssetGuidWords[1] = 0;
    out.VignetteIntensity  = lerp(a.VignetteIntensity,  b.VignetteIntensity,  w);
    out.VignetteSmoothness = lerp(a.VignetteSmoothness, b.VignetteSmoothness, w);
    out.VignetteColorR     = lerp(a.VignetteColorR,     b.VignetteColorR,     w);
    out.VignetteColorG     = lerp(a.VignetteColorG,     b.VignetteColorG,     w);
    out.VignetteColorB     = lerp(a.VignetteColorB,     b.VignetteColorB,     w);
    out.VignetteRounded    = (w >= 0.5f) ? b.VignetteRounded : a.VignetteRounded;
    out.VignetteStackOrder = (w >= 0.5f) ? b.VignetteStackOrder : a.VignetteStackOrder;
    out.VhsIntensity = lerp(a.VhsIntensity, b.VhsIntensity, w);
    out.VhsWobble = lerp(a.VhsWobble, b.VhsWobble, w);
    out.VhsTracking = lerp(a.VhsTracking, b.VhsTracking, w);
    out.VhsSignalGlitches = lerp(a.VhsSignalGlitches, b.VhsSignalGlitches, w);
    out.VhsGlitchOffsets = lerp(a.VhsGlitchOffsets, b.VhsGlitchOffsets, w);
    out.VhsInterference = lerp(a.VhsInterference, b.VhsInterference, w);
    out.VhsFrameFeedback = lerp(a.VhsFrameFeedback, b.VhsFrameFeedback, w);
    out.VhsFeedbackDecay = lerp(a.VhsFeedbackDecay, b.VhsFeedbackDecay, w);
    out.VhsFeedbackMotionThreshold =
        lerp(a.VhsFeedbackMotionThreshold, b.VhsFeedbackMotionThreshold, w);
    out.VhsFeedbackTrailLength =
        lerp(a.VhsFeedbackTrailLength, b.VhsFeedbackTrailLength, w);
    out.VhsCompositeSignalMode =
        (w >= 0.5f) ? b.VhsCompositeSignalMode : a.VhsCompositeSignalMode;
    out.VhsDotCrawl = lerp(a.VhsDotCrawl, b.VhsDotCrawl, w);
    out.VhsColorBleed = lerp(a.VhsColorBleed, b.VhsColorBleed, w);
    out.VhsColorBleedOffset = lerp(a.VhsColorBleedOffset, b.VhsColorBleedOffset, w);
    out.VhsTapeNoise = lerp(a.VhsTapeNoise, b.VhsTapeNoise, w);
    out.VhsChromaStreaks = lerp(a.VhsChromaStreaks, b.VhsChromaStreaks, w);
    out.VhsPreFilterChromaStreaks =
        lerp(a.VhsPreFilterChromaStreaks, b.VhsPreFilterChromaStreaks, w);
    out.VhsDropouts = lerp(a.VhsDropouts, b.VhsDropouts, w);
    out.VhsRfDropouts = lerp(a.VhsRfDropouts, b.VhsRfDropouts, w);
    out.VhsScanlines = lerp(a.VhsScanlines, b.VhsScanlines, w);
    out.VhsSpeed = lerp(a.VhsSpeed, b.VhsSpeed, w);
    out.VhsOverlayEnabled =
        (w >= 0.5f) ? b.VhsOverlayEnabled : a.VhsOverlayEnabled;
    out.VhsOverlayColorR = lerp(a.VhsOverlayColorR, b.VhsOverlayColorR, w);
    out.VhsOverlayColorG = lerp(a.VhsOverlayColorG, b.VhsOverlayColorG, w);
    out.VhsOverlayColorB = lerp(a.VhsOverlayColorB, b.VhsOverlayColorB, w);
    out.VhsOverlayOpacity = lerp(a.VhsOverlayOpacity, b.VhsOverlayOpacity, w);
    out.VhsOverlaySize = lerp(a.VhsOverlaySize, b.VhsOverlaySize, w);
    out.VhsOverlayFont = (w >= 0.5f) ? b.VhsOverlayFont : a.VhsOverlayFont;
    out.VhsOverlayPositionX =
        lerp(a.VhsOverlayPositionX, b.VhsOverlayPositionX, w);
    out.VhsOverlayPositionY =
        lerp(a.VhsOverlayPositionY, b.VhsOverlayPositionY, w);
    out.VhsOverlayTextLength =
        (w >= 0.5f) ? b.VhsOverlayTextLength : a.VhsOverlayTextLength;
    out.VhsOverlayText0 = (w >= 0.5f) ? b.VhsOverlayText0 : a.VhsOverlayText0;
    out.VhsOverlayText1 = (w >= 0.5f) ? b.VhsOverlayText1 : a.VhsOverlayText1;
    out.VhsOverlayText2 = (w >= 0.5f) ? b.VhsOverlayText2 : a.VhsOverlayText2;
    out.VhsOverlayText3 = (w >= 0.5f) ? b.VhsOverlayText3 : a.VhsOverlayText3;
    out.VhsOverlayText4 = (w >= 0.5f) ? b.VhsOverlayText4 : a.VhsOverlayText4;
    out.VhsOverlayText5 = (w >= 0.5f) ? b.VhsOverlayText5 : a.VhsOverlayText5;
    out.VhsOverlayText6 = (w >= 0.5f) ? b.VhsOverlayText6 : a.VhsOverlayText6;
    out.VhsOverlayText7 = (w >= 0.5f) ? b.VhsOverlayText7 : a.VhsOverlayText7;
    out.VhsDateBurnEnabled =
        (w >= 0.5f) ? b.VhsDateBurnEnabled : a.VhsDateBurnEnabled;
    out.VhsDateBurnColorR =
        lerp(a.VhsDateBurnColorR, b.VhsDateBurnColorR, w);
    out.VhsDateBurnColorG =
        lerp(a.VhsDateBurnColorG, b.VhsDateBurnColorG, w);
    out.VhsDateBurnColorB =
        lerp(a.VhsDateBurnColorB, b.VhsDateBurnColorB, w);
    out.VhsDateBurnSize = lerp(a.VhsDateBurnSize, b.VhsDateBurnSize, w);
    out.VhsDateBurnPositionX =
        lerp(a.VhsDateBurnPositionX, b.VhsDateBurnPositionX, w);
    out.VhsDateBurnPositionY =
        lerp(a.VhsDateBurnPositionY, b.VhsDateBurnPositionY, w);
    out.VhsDateBurnYear =
        (w >= 0.5f) ? b.VhsDateBurnYear : a.VhsDateBurnYear;
    out.VhsDateBurnMonth =
        (w >= 0.5f) ? b.VhsDateBurnMonth : a.VhsDateBurnMonth;
    out.VhsDateBurnDay =
        (w >= 0.5f) ? b.VhsDateBurnDay : a.VhsDateBurnDay;
    out.VhsDateBurnHour =
        (w >= 0.5f) ? b.VhsDateBurnHour : a.VhsDateBurnHour;
    out.VhsDateBurnMinute =
        (w >= 0.5f) ? b.VhsDateBurnMinute : a.VhsDateBurnMinute;
    out.VhsTransportMode =
        (w >= 0.5f) ? b.VhsTransportMode : a.VhsTransportMode;
    out.VhsTransportStrength =
        lerp(a.VhsTransportStrength, b.VhsTransportStrength, w);
    out.CrtIntensity   = lerp(a.CrtIntensity,   b.CrtIntensity,   w);
    out.CrtCurvature   = lerp(a.CrtCurvature,   b.CrtCurvature,   w);
    out.CrtScanlines   = lerp(a.CrtScanlines,   b.CrtScanlines,   w);
    out.CrtVignette    = lerp(a.CrtVignette,    b.CrtVignette,    w);
    out.CrtAberration  = lerp(a.CrtAberration,  b.CrtAberration,  w);
    out.CrtSoftness    = lerp(a.CrtSoftness,    b.CrtSoftness,    w);
    out.CrtExposureCompensation = lerp(a.CrtExposureCompensation, b.CrtExposureCompensation, w);
    out.CrtEmulatedResolutionDiv = lerp(a.CrtEmulatedResolutionDiv, b.CrtEmulatedResolutionDiv, w);
    out.FastBlurIntensity = lerp(a.FastBlurIntensity, b.FastBlurIntensity, w);
    out.FastBlurFocusDistance = lerp(a.FastBlurFocusDistance, b.FastBlurFocusDistance, w);
    out.FastBlurFocusRange = lerp(a.FastBlurFocusRange, b.FastBlurFocusRange, w);
    out.FastBlurMaxRadius = lerp(a.FastBlurMaxRadius, b.FastBlurMaxRadius, w);
    out.FastBlurNearBlur = (w >= 0.5f) ? b.FastBlurNearBlur : a.FastBlurNearBlur;
    out.HeatDistortionStrength = lerp(a.HeatDistortionStrength, b.HeatDistortionStrength, w);
    out.HeatDistortionSpeed = lerp(a.HeatDistortionSpeed, b.HeatDistortionSpeed, w);
    out.HeatDistortionScale = lerp(a.HeatDistortionScale, b.HeatDistortionScale, w);
    out.HeatDistortionMaskStrength = lerp(a.HeatDistortionMaskStrength, b.HeatDistortionMaskStrength, w);
    out.HeatDistortionDistanceStart = lerp(a.HeatDistortionDistanceStart, b.HeatDistortionDistanceStart, w);
    out.HeatDistortionDistanceEnd = lerp(a.HeatDistortionDistanceEnd, b.HeatDistortionDistanceEnd, w);
    out.HeatDistortionDirectionalFalloff = lerp(a.HeatDistortionDirectionalFalloff, b.HeatDistortionDirectionalFalloff, w);
    out.HeatDistortionUseAbsoluteY = (w >= 0.5f) ? b.HeatDistortionUseAbsoluteY : a.HeatDistortionUseAbsoluteY;
    out.HeatDistortionSoftness = lerp(a.HeatDistortionSoftness, b.HeatDistortionSoftness, w);
    out.HeightFogIntensity = lerp(a.HeightFogIntensity, b.HeightFogIntensity, w);
    out.HeightFogDensity = lerp(a.HeightFogDensity, b.HeightFogDensity, w);
    out.HeightFogMaxOpacity = lerp(a.HeightFogMaxOpacity, b.HeightFogMaxOpacity, w);
    out.HeightFogMinDistance = lerp(a.HeightFogMinDistance, b.HeightFogMinDistance, w);
    out.HeightFogSmoothLength = lerp(a.HeightFogSmoothLength, b.HeightFogSmoothLength, w);
    out.HeightFogBaseHeight = lerp(a.HeightFogBaseHeight, b.HeightFogBaseHeight, w);
    out.HeightFogTransitionLength = lerp(a.HeightFogTransitionLength, b.HeightFogTransitionLength, w);
    out.HeightFogEmissiveR = lerp(a.HeightFogEmissiveR, b.HeightFogEmissiveR, w);
    out.HeightFogEmissiveG = lerp(a.HeightFogEmissiveG, b.HeightFogEmissiveG, w);
    out.HeightFogEmissiveB = lerp(a.HeightFogEmissiveB, b.HeightFogEmissiveB, w);
    out.HeightFogSunDirX = lerp(a.HeightFogSunDirX, b.HeightFogSunDirX, w);
    out.HeightFogSunDirY = lerp(a.HeightFogSunDirY, b.HeightFogSunDirY, w);
    out.HeightFogSunDirZ = lerp(a.HeightFogSunDirZ, b.HeightFogSunDirZ, w);
    out.HeightFogSunColorR = lerp(a.HeightFogSunColorR, b.HeightFogSunColorR, w);
    out.HeightFogSunColorG = lerp(a.HeightFogSunColorG, b.HeightFogSunColorG, w);
    out.HeightFogSunColorB = lerp(a.HeightFogSunColorB, b.HeightFogSunColorB, w);
    out.HeightFogSunIntensity = lerp(a.HeightFogSunIntensity, b.HeightFogSunIntensity, w);
    out.HeightFogPhase = lerp(a.HeightFogPhase, b.HeightFogPhase, w);
    out.HeightFogPhaseWeight0 = lerp(a.HeightFogPhaseWeight0, b.HeightFogPhaseWeight0, w);
    out.HeightFogPhaseWeight1 = lerp(a.HeightFogPhaseWeight1, b.HeightFogPhaseWeight1, w);
    out.HeightFogSkyEnabled = (w >= 0.5f) ? b.HeightFogSkyEnabled : a.HeightFogSkyEnabled;
    out.HeightFogSkyPower = lerp(a.HeightFogSkyPower, b.HeightFogSkyPower, w);
    out.HeightFogSkyFillStart = lerp(a.HeightFogSkyFillStart, b.HeightFogSkyFillStart, w);
    out.HeightFogSkyFillEnd = lerp(a.HeightFogSkyFillEnd, b.HeightFogSkyFillEnd, w);
    out.HeightFogDistanceFogEnabled = (w >= 0.5f) ? b.HeightFogDistanceFogEnabled : a.HeightFogDistanceFogEnabled;
    out.HeightFogHeightFogEnabled = (w >= 0.5f) ? b.HeightFogHeightFogEnabled : a.HeightFogHeightFogEnabled;
    out.HeightFogMaxDistance = lerp(a.HeightFogMaxDistance, b.HeightFogMaxDistance, w);
    out.HeightFogLayerMode = (w >= 0.5f) ? b.HeightFogLayerMode : a.HeightFogLayerMode;
    out.HeightFogHorizonHeightOffset = lerp(a.HeightFogHorizonHeightOffset, b.HeightFogHorizonHeightOffset, w);
    out.HeightFogHorizonHeightBlendStart = lerp(a.HeightFogHorizonHeightBlendStart, b.HeightFogHorizonHeightBlendStart, w);
    out.HeightFogHorizonHeightBlendEnd = lerp(a.HeightFogHorizonHeightBlendEnd, b.HeightFogHorizonHeightBlendEnd, w);
    out.HeightFogAxisMode = (w >= 0.5f) ? b.HeightFogAxisMode : a.HeightFogAxisMode;
    out.HeightFogAxisX = lerp(a.HeightFogAxisX, b.HeightFogAxisX, w);
    out.HeightFogAxisY = lerp(a.HeightFogAxisY, b.HeightFogAxisY, w);
    out.HeightFogAxisZ = lerp(a.HeightFogAxisZ, b.HeightFogAxisZ, w);
    out.HeightFogGradientMode = (w >= 0.5f) ? b.HeightFogGradientMode : a.HeightFogGradientMode;
    out.HeightFogGradientStrength = lerp(a.HeightFogGradientStrength, b.HeightFogGradientStrength, w);
    out.HeightFogGradientLowR = lerp(a.HeightFogGradientLowR, b.HeightFogGradientLowR, w);
    out.HeightFogGradientLowG = lerp(a.HeightFogGradientLowG, b.HeightFogGradientLowG, w);
    out.HeightFogGradientLowB = lerp(a.HeightFogGradientLowB, b.HeightFogGradientLowB, w);
    out.HeightFogGradientHighR = lerp(a.HeightFogGradientHighR, b.HeightFogGradientHighR, w);
    out.HeightFogGradientHighG = lerp(a.HeightFogGradientHighG, b.HeightFogGradientHighG, w);
    out.HeightFogGradientHighB = lerp(a.HeightFogGradientHighB, b.HeightFogGradientHighB, w);
    out.HeightFogTrackDirectionalLight = (w >= 0.5f) ? b.HeightFogTrackDirectionalLight : a.HeightFogTrackDirectionalLight;
    out.HeightFogSunIntensityScale = lerp(a.HeightFogSunIntensityScale, b.HeightFogSunIntensityScale, w);
    out.HeightFogNoiseEnabled = (w >= 0.5f) ? b.HeightFogNoiseEnabled : a.HeightFogNoiseEnabled;
    out.HeightFogNoiseScale = lerp(a.HeightFogNoiseScale, b.HeightFogNoiseScale, w);
    out.HeightFogNoiseStrength = lerp(a.HeightFogNoiseStrength, b.HeightFogNoiseStrength, w);
    out.HeightFogNoiseContrast = lerp(a.HeightFogNoiseContrast, b.HeightFogNoiseContrast, w);
    out.HeightFogNoiseVelX = lerp(a.HeightFogNoiseVelX, b.HeightFogNoiseVelX, w);
    out.HeightFogNoiseVelY = lerp(a.HeightFogNoiseVelY, b.HeightFogNoiseVelY, w);
    out.HeightFogNoiseVelZ = lerp(a.HeightFogNoiseVelZ, b.HeightFogNoiseVelZ, w);
    out.HeightFogNoiseMin = lerp(a.HeightFogNoiseMin, b.HeightFogNoiseMin, w);
    out.HeightFogNoiseMax = lerp(a.HeightFogNoiseMax, b.HeightFogNoiseMax, w);
    out.HeightFogNoiseFadeStart = lerp(a.HeightFogNoiseFadeStart, b.HeightFogNoiseFadeStart, w);
    out.HeightFogNoiseFadeEnd = lerp(a.HeightFogNoiseFadeEnd, b.HeightFogNoiseFadeEnd, w);
    out.HeightFogUseTimeOfDay = (w >= 0.5f) ? b.HeightFogUseTimeOfDay : a.HeightFogUseTimeOfDay;
    out.HeightFogSkyHorizonOffset = lerp(a.HeightFogSkyHorizonOffset, b.HeightFogSkyHorizonOffset, w);
    out.HeightFogSkyBottomStrength = lerp(a.HeightFogSkyBottomStrength, b.HeightFogSkyBottomStrength, w);
    out.VolumetricFogIntensity         = lerp(a.VolumetricFogIntensity, b.VolumetricFogIntensity, w);
    out.VolumetricFogIsGlobal          = (w >= 0.5f) ? b.VolumetricFogIsGlobal : a.VolumetricFogIsGlobal;
    out.VolumetricFogVolumeShape       = (w >= 0.5f) ? b.VolumetricFogVolumeShape : a.VolumetricFogVolumeShape;
    out.VolumetricFogVolumeValid       = (w >= 0.5f) ? b.VolumetricFogVolumeValid : a.VolumetricFogVolumeValid;
    out.VolumetricFogVolumeBlendDistance = lerp(a.VolumetricFogVolumeBlendDistance, b.VolumetricFogVolumeBlendDistance, w);
    out.VolumetricFogVolumeCenterX     = lerp(a.VolumetricFogVolumeCenterX, b.VolumetricFogVolumeCenterX, w);
    out.VolumetricFogVolumeCenterY     = lerp(a.VolumetricFogVolumeCenterY, b.VolumetricFogVolumeCenterY, w);
    out.VolumetricFogVolumeCenterZ     = lerp(a.VolumetricFogVolumeCenterZ, b.VolumetricFogVolumeCenterZ, w);
    out.VolumetricFogVolumeAxisXX      = lerp(a.VolumetricFogVolumeAxisXX, b.VolumetricFogVolumeAxisXX, w);
    out.VolumetricFogVolumeAxisXY      = lerp(a.VolumetricFogVolumeAxisXY, b.VolumetricFogVolumeAxisXY, w);
    out.VolumetricFogVolumeAxisXZ      = lerp(a.VolumetricFogVolumeAxisXZ, b.VolumetricFogVolumeAxisXZ, w);
    out.VolumetricFogVolumeAxisYX      = lerp(a.VolumetricFogVolumeAxisYX, b.VolumetricFogVolumeAxisYX, w);
    out.VolumetricFogVolumeAxisYY      = lerp(a.VolumetricFogVolumeAxisYY, b.VolumetricFogVolumeAxisYY, w);
    out.VolumetricFogVolumeAxisYZ      = lerp(a.VolumetricFogVolumeAxisYZ, b.VolumetricFogVolumeAxisYZ, w);
    out.VolumetricFogVolumeAxisZX      = lerp(a.VolumetricFogVolumeAxisZX, b.VolumetricFogVolumeAxisZX, w);
    out.VolumetricFogVolumeAxisZY      = lerp(a.VolumetricFogVolumeAxisZY, b.VolumetricFogVolumeAxisZY, w);
    out.VolumetricFogVolumeAxisZZ      = lerp(a.VolumetricFogVolumeAxisZZ, b.VolumetricFogVolumeAxisZZ, w);
    out.VolumetricFogVolumeHalfExtentX = lerp(a.VolumetricFogVolumeHalfExtentX, b.VolumetricFogVolumeHalfExtentX, w);
    out.VolumetricFogVolumeHalfExtentY = lerp(a.VolumetricFogVolumeHalfExtentY, b.VolumetricFogVolumeHalfExtentY, w);
    out.VolumetricFogVolumeHalfExtentZ = lerp(a.VolumetricFogVolumeHalfExtentZ, b.VolumetricFogVolumeHalfExtentZ, w);
    out.VolumetricFogMaxDistance       = lerp(a.VolumetricFogMaxDistance, b.VolumetricFogMaxDistance, w);
    out.VolumetricFogXYCellSizePixels  = (w >= 0.5f) ? b.VolumetricFogXYCellSizePixels : a.VolumetricFogXYCellSizePixels;
    out.VolumetricFogZSliceCount       = (w >= 0.5f) ? b.VolumetricFogZSliceCount : a.VolumetricFogZSliceCount;
    out.VolumetricFogDepthDistribution = lerp(a.VolumetricFogDepthDistribution, b.VolumetricFogDepthDistribution, w);
    out.VolumetricFogDensity           = lerp(a.VolumetricFogDensity, b.VolumetricFogDensity, w);
    out.VolumetricFogBaseHeight        = lerp(a.VolumetricFogBaseHeight, b.VolumetricFogBaseHeight, w);
    out.VolumetricFogHeightFalloff     = lerp(a.VolumetricFogHeightFalloff, b.VolumetricFogHeightFalloff, w);
    out.VolumetricFogSkyFade           = lerp(a.VolumetricFogSkyFade, b.VolumetricFogSkyFade, w);
    out.VolumetricFogAlbedoR           = lerp(a.VolumetricFogAlbedoR, b.VolumetricFogAlbedoR, w);
    out.VolumetricFogAlbedoG           = lerp(a.VolumetricFogAlbedoG, b.VolumetricFogAlbedoG, w);
    out.VolumetricFogAlbedoB           = lerp(a.VolumetricFogAlbedoB, b.VolumetricFogAlbedoB, w);
    out.VolumetricFogEmissionR         = lerp(a.VolumetricFogEmissionR, b.VolumetricFogEmissionR, w);
    out.VolumetricFogEmissionG         = lerp(a.VolumetricFogEmissionG, b.VolumetricFogEmissionG, w);
    out.VolumetricFogEmissionB         = lerp(a.VolumetricFogEmissionB, b.VolumetricFogEmissionB, w);
    out.VolumetricFogAnisotropy        = lerp(a.VolumetricFogAnisotropy, b.VolumetricFogAnisotropy, w);
    out.VolumetricFogTrackDirectionalLight = (w >= 0.5f) ? b.VolumetricFogTrackDirectionalLight : a.VolumetricFogTrackDirectionalLight;
    out.VolumetricFogSunIntensityScale = lerp(a.VolumetricFogSunIntensityScale, b.VolumetricFogSunIntensityScale, w);
    out.VolumetricFogSunTintR          = lerp(a.VolumetricFogSunTintR, b.VolumetricFogSunTintR, w);
    out.VolumetricFogSunTintG          = lerp(a.VolumetricFogSunTintG, b.VolumetricFogSunTintG, w);
    out.VolumetricFogSunTintB          = lerp(a.VolumetricFogSunTintB, b.VolumetricFogSunTintB, w);
    out.VolumetricFogAmbientTintR      = lerp(a.VolumetricFogAmbientTintR, b.VolumetricFogAmbientTintR, w);
    out.VolumetricFogAmbientTintG      = lerp(a.VolumetricFogAmbientTintG, b.VolumetricFogAmbientTintG, w);
    out.VolumetricFogAmbientTintB      = lerp(a.VolumetricFogAmbientTintB, b.VolumetricFogAmbientTintB, w);
    out.VolumetricFogNoiseEnabled      = (w >= 0.5f) ? b.VolumetricFogNoiseEnabled : a.VolumetricFogNoiseEnabled;
    out.VolumetricFogNoiseScale        = lerp(a.VolumetricFogNoiseScale, b.VolumetricFogNoiseScale, w);
    out.VolumetricFogNoiseStrength     = lerp(a.VolumetricFogNoiseStrength, b.VolumetricFogNoiseStrength, w);
    out.VolumetricFogNoiseVelocityX    = lerp(a.VolumetricFogNoiseVelocityX, b.VolumetricFogNoiseVelocityX, w);
    out.VolumetricFogNoiseVelocityY    = lerp(a.VolumetricFogNoiseVelocityY, b.VolumetricFogNoiseVelocityY, w);
    out.VolumetricFogNoiseVelocityZ    = lerp(a.VolumetricFogNoiseVelocityZ, b.VolumetricFogNoiseVelocityZ, w);
    out.VolumetricFogNoiseContrast     = lerp(a.VolumetricFogNoiseContrast, b.VolumetricFogNoiseContrast, w);
    out.VolumetricFogNoiseChannelWeightR = lerp(a.VolumetricFogNoiseChannelWeightR, b.VolumetricFogNoiseChannelWeightR, w);
    out.VolumetricFogNoiseChannelWeightG = lerp(a.VolumetricFogNoiseChannelWeightG, b.VolumetricFogNoiseChannelWeightG, w);
    out.VolumetricFogNoiseChannelWeightB = lerp(a.VolumetricFogNoiseChannelWeightB, b.VolumetricFogNoiseChannelWeightB, w);
    out.VolumetricFogNoiseChannelWeightA = lerp(a.VolumetricFogNoiseChannelWeightA, b.VolumetricFogNoiseChannelWeightA, w);
    out.VolumetricFogDensityThreshold  = lerp(a.VolumetricFogDensityThreshold, b.VolumetricFogDensityThreshold, w);
    out.VolumetricFogDensityThresholdSoftness = lerp(a.VolumetricFogDensityThresholdSoftness, b.VolumetricFogDensityThresholdSoftness, w);
    out.VolumetricFogTemporalEnabled   = (w >= 0.5f) ? b.VolumetricFogTemporalEnabled : a.VolumetricFogTemporalEnabled;
    out.VolumetricFogTemporalBlend     = lerp(a.VolumetricFogTemporalBlend, b.VolumetricFogTemporalBlend, w);
    out.VolumetricFogJitterStrength    = lerp(a.VolumetricFogJitterStrength, b.VolumetricFogJitterStrength, w);
    out.VolumetricFogJitterMotion      = (w >= 0.5f) ? b.VolumetricFogJitterMotion : a.VolumetricFogJitterMotion;
    out.VolumetricFogCompositeDepthBias = lerp(a.VolumetricFogCompositeDepthBias, b.VolumetricFogCompositeDepthBias, w);
    out.VolumetricFogShadowBias        = lerp(a.VolumetricFogShadowBias, b.VolumetricFogShadowBias, w);
    out.AtmosphericCloudSkyFill = lerp(a.AtmosphericCloudSkyFill, b.AtmosphericCloudSkyFill, w);
    out.AtmosphericCloudVaporMass = lerp(a.AtmosphericCloudVaporMass, b.AtmosphericCloudVaporMass, w);
    out.AtmosphericCloudColorR = lerp(a.AtmosphericCloudColorR, b.AtmosphericCloudColorR, w);
    out.AtmosphericCloudColorG = lerp(a.AtmosphericCloudColorG, b.AtmosphericCloudColorG, w);
    out.AtmosphericCloudColorB = lerp(a.AtmosphericCloudColorB, b.AtmosphericCloudColorB, w);
    out.AtmosphericCloudOpacity = lerp(a.AtmosphericCloudOpacity, b.AtmosphericCloudOpacity, w);
    out.AtmosphericCloudFloorHeight = lerp(a.AtmosphericCloudFloorHeight, b.AtmosphericCloudFloorHeight, w);
    out.AtmosphericCloudLayerDepth = lerp(a.AtmosphericCloudLayerDepth, b.AtmosphericCloudLayerDepth, w);
    out.AtmosphericCloudBodyFrequency = lerp(a.AtmosphericCloudBodyFrequency, b.AtmosphericCloudBodyFrequency, w);
    out.AtmosphericCloudEdgeFrequency = lerp(a.AtmosphericCloudEdgeFrequency, b.AtmosphericCloudEdgeFrequency, w);
    out.AtmosphericCloudEdgeBreakup = lerp(a.AtmosphericCloudEdgeBreakup, b.AtmosphericCloudEdgeBreakup, w);
    out.AtmosphericCloudDriftAngle = lerp(a.AtmosphericCloudDriftAngle, b.AtmosphericCloudDriftAngle, w);
    out.AtmosphericCloudDriftRate = lerp(a.AtmosphericCloudDriftRate, b.AtmosphericCloudDriftRate, w);
    out.AtmosphericCloudSunFade = lerp(a.AtmosphericCloudSunFade, b.AtmosphericCloudSunFade, w);
    out.AtmosphericCloudSkyBounce = lerp(a.AtmosphericCloudSkyBounce, b.AtmosphericCloudSkyBounce, w);
    out.AtmosphericCloudRimBoost = lerp(a.AtmosphericCloudRimBoost, b.AtmosphericCloudRimBoost, w);
    out.AtmosphericCloudOcclusion = lerp(a.AtmosphericCloudOcclusion, b.AtmosphericCloudOcclusion, w);
    out.AtmosphericCloudHistoryWeight = lerp(a.AtmosphericCloudHistoryWeight, b.AtmosphericCloudHistoryWeight, w);
    out.AtmosphericCloudPixelScale = lerp(a.AtmosphericCloudPixelScale, b.AtmosphericCloudPixelScale, w);
    out.CloudsRadius = lerp(a.CloudsRadius, b.CloudsRadius, w);
    out.CloudsAltitude = lerp(a.CloudsAltitude, b.CloudsAltitude, w);
    out.CloudsThickness = lerp(a.CloudsThickness, b.CloudsThickness, w);
    out.CloudsNumStepsLight = (w >= 0.5f) ? b.CloudsNumStepsLight : a.CloudsNumStepsLight;
    out.CloudsStepSize = lerp(a.CloudsStepSize, b.CloudsStepSize, w);
    out.CloudsRayOffsetStrength = lerp(a.CloudsRayOffsetStrength, b.CloudsRayOffsetStrength, w);
    out.CloudsScale = lerp(a.CloudsScale, b.CloudsScale, w);
    out.CloudsDensityMultiplier = lerp(a.CloudsDensityMultiplier, b.CloudsDensityMultiplier, w);
    out.CloudsDensityOffset = lerp(a.CloudsDensityOffset, b.CloudsDensityOffset, w);
    out.CloudsShapeOffsetX = lerp(a.CloudsShapeOffsetX, b.CloudsShapeOffsetX, w);
    out.CloudsShapeOffsetY = lerp(a.CloudsShapeOffsetY, b.CloudsShapeOffsetY, w);
    out.CloudsShapeOffsetZ = lerp(a.CloudsShapeOffsetZ, b.CloudsShapeOffsetZ, w);
    out.CloudsShapeWeightR = lerp(a.CloudsShapeWeightR, b.CloudsShapeWeightR, w);
    out.CloudsShapeWeightG = lerp(a.CloudsShapeWeightG, b.CloudsShapeWeightG, w);
    out.CloudsShapeWeightB = lerp(a.CloudsShapeWeightB, b.CloudsShapeWeightB, w);
    out.CloudsShapeWeightA = lerp(a.CloudsShapeWeightA, b.CloudsShapeWeightA, w);
    out.CloudsDetailScale = lerp(a.CloudsDetailScale, b.CloudsDetailScale, w);
    out.CloudsDetailWeight = lerp(a.CloudsDetailWeight, b.CloudsDetailWeight, w);
    out.CloudsDetailWeightR = lerp(a.CloudsDetailWeightR, b.CloudsDetailWeightR, w);
    out.CloudsDetailWeightG = lerp(a.CloudsDetailWeightG, b.CloudsDetailWeightG, w);
    out.CloudsDetailWeightB = lerp(a.CloudsDetailWeightB, b.CloudsDetailWeightB, w);
    out.CloudsDetailOffsetX = lerp(a.CloudsDetailOffsetX, b.CloudsDetailOffsetX, w);
    out.CloudsDetailOffsetY = lerp(a.CloudsDetailOffsetY, b.CloudsDetailOffsetY, w);
    out.CloudsDetailOffsetZ = lerp(a.CloudsDetailOffsetZ, b.CloudsDetailOffsetZ, w);
    out.CloudsAbsorptionThroughCloud = lerp(a.CloudsAbsorptionThroughCloud, b.CloudsAbsorptionThroughCloud, w);
    out.CloudsAbsorptionTowardSun = lerp(a.CloudsAbsorptionTowardSun, b.CloudsAbsorptionTowardSun, w);
    out.CloudsDarknessThreshold = lerp(a.CloudsDarknessThreshold, b.CloudsDarknessThreshold, w);
    out.CloudsPhaseForward = lerp(a.CloudsPhaseForward, b.CloudsPhaseForward, w);
    out.CloudsPhaseBack = lerp(a.CloudsPhaseBack, b.CloudsPhaseBack, w);
    out.CloudsPhaseBase = lerp(a.CloudsPhaseBase, b.CloudsPhaseBase, w);
    out.CloudsPhaseFactor = lerp(a.CloudsPhaseFactor, b.CloudsPhaseFactor, w);
    out.CloudsTimeScale = lerp(a.CloudsTimeScale, b.CloudsTimeScale, w);
    out.CloudsBaseSpeed = lerp(a.CloudsBaseSpeed, b.CloudsBaseSpeed, w);
    out.CloudsDetailSpeed = lerp(a.CloudsDetailSpeed, b.CloudsDetailSpeed, w);
    out.CloudsHistoryWeight = lerp(a.CloudsHistoryWeight, b.CloudsHistoryWeight, w);
    if (!bContributesVolumetricFog)
    {
        RefCopyVolumetricFogFields(out, a);
    }
    if (!bContributesHeightFog && !bContributesVolumetricFog)
    {
        // An unrelated grading/effect volume must not fade a fog component's
        // shared scattering controls toward their default-off values.
        RefCopyFogGlowFields(out, a);
    }
    if (!bContributesHeightFog)
    {
        RefCopyHeightFogFields(out, a);
    }
    if (!bContributesAtmosphericCloud)
    {
        RefCopyAtmosphericCloudFields(out, a);
    }
    if (!bContributesVolumetricClouds)
    {
        RefCopyVolumetricCloudsFields(out, a);
    }
    // The log working-space flag is a discrete choice, not an interpolant. Only a
    // volume that actually authors a grade may dictate it — a non-contributing
    // dominant volume must not flip the contributing volume's working space.
    out.ColorGradeInLog = (bContributesColorGrade && w >= 0.5f) ? b.ColorGradeInLog : a.ColorGradeInLog;
    if (bContributesColorGrade)
    {
        out.ColorGradeShadowsR       = lerp(a.ColorGradeShadowsR, b.ColorGradeShadowsR, w);
        out.ColorGradeShadowsG       = lerp(a.ColorGradeShadowsG, b.ColorGradeShadowsG, w);
        out.ColorGradeShadowsB       = lerp(a.ColorGradeShadowsB, b.ColorGradeShadowsB, w);
        out.ColorGradeShadowsMaster  = lerp(a.ColorGradeShadowsMaster, b.ColorGradeShadowsMaster, w);
        out.ColorGradeMidtonesR      = lerp(a.ColorGradeMidtonesR, b.ColorGradeMidtonesR, w);
        out.ColorGradeMidtonesG      = lerp(a.ColorGradeMidtonesG, b.ColorGradeMidtonesG, w);
        out.ColorGradeMidtonesB      = lerp(a.ColorGradeMidtonesB, b.ColorGradeMidtonesB, w);
        out.ColorGradeMidtonesMaster = lerp(a.ColorGradeMidtonesMaster, b.ColorGradeMidtonesMaster, w);
        out.ColorGradeHighlightsR    = lerp(a.ColorGradeHighlightsR, b.ColorGradeHighlightsR, w);
        out.ColorGradeHighlightsG    = lerp(a.ColorGradeHighlightsG, b.ColorGradeHighlightsG, w);
        out.ColorGradeHighlightsB    = lerp(a.ColorGradeHighlightsB, b.ColorGradeHighlightsB, w);
        out.ColorGradeHighlightsMaster = lerp(a.ColorGradeHighlightsMaster, b.ColorGradeHighlightsMaster, w);
        out.ColorGradeContrast       = lerp(a.ColorGradeContrast, b.ColorGradeContrast, w);
        out.ColorGradeSaturation     = lerp(a.ColorGradeSaturation, b.ColorGradeSaturation, w);
        // White balance and hue interpolate as authored values (Unity volume
        // semantics); the LMS coefficients are derived after blending.
        out.ColorGradeHueShift       = lerp(a.ColorGradeHueShift, b.ColorGradeHueShift, w);
        out.ColorGradeTemperature    = lerp(a.ColorGradeTemperature, b.ColorGradeTemperature, w);
        out.ColorGradeTint           = lerp(a.ColorGradeTint, b.ColorGradeTint, w);
        // Lerp of two ordered (start <= end) pairs stays ordered, so blending
        // cannot itself create a degenerate band.
        out.ColorGradeShadowsStart    = lerp(a.ColorGradeShadowsStart, b.ColorGradeShadowsStart, w);
        out.ColorGradeShadowsEnd      = lerp(a.ColorGradeShadowsEnd, b.ColorGradeShadowsEnd, w);
        out.ColorGradeHighlightsStart = lerp(a.ColorGradeHighlightsStart, b.ColorGradeHighlightsStart, w);
        out.ColorGradeHighlightsEnd   = lerp(a.ColorGradeHighlightsEnd, b.ColorGradeHighlightsEnd, w);
    }
    else
    {
        out.ColorGradeShadowsR       = a.ColorGradeShadowsR;
        out.ColorGradeShadowsG       = a.ColorGradeShadowsG;
        out.ColorGradeShadowsB       = a.ColorGradeShadowsB;
        out.ColorGradeShadowsMaster  = a.ColorGradeShadowsMaster;
        out.ColorGradeMidtonesR      = a.ColorGradeMidtonesR;
        out.ColorGradeMidtonesG      = a.ColorGradeMidtonesG;
        out.ColorGradeMidtonesB      = a.ColorGradeMidtonesB;
        out.ColorGradeMidtonesMaster = a.ColorGradeMidtonesMaster;
        out.ColorGradeHighlightsR    = a.ColorGradeHighlightsR;
        out.ColorGradeHighlightsG    = a.ColorGradeHighlightsG;
        out.ColorGradeHighlightsB    = a.ColorGradeHighlightsB;
        out.ColorGradeHighlightsMaster = a.ColorGradeHighlightsMaster;
        out.ColorGradeContrast       = a.ColorGradeContrast;
        out.ColorGradeSaturation     = a.ColorGradeSaturation;
        out.ColorGradeHueShift       = a.ColorGradeHueShift;
        out.ColorGradeTemperature    = a.ColorGradeTemperature;
        out.ColorGradeTint           = a.ColorGradeTint;
        out.ColorGradeShadowsStart    = a.ColorGradeShadowsStart;
        out.ColorGradeShadowsEnd      = a.ColorGradeShadowsEnd;
        out.ColorGradeHighlightsStart = a.ColorGradeHighlightsStart;
        out.ColorGradeHighlightsEnd   = a.ColorGradeHighlightsEnd;
    }
    return out;
}

struct BlendCase
{
    uint32 SeedA = 0;
    uint32 SeedB = 0;
    float Weight = 0.0f;
    bool ContributesColorGrade = true;
    bool ContributesHeightFog = true;
    bool ContributesVolumetricFog = true;
    bool ContributesAtmosphericCloud = true;
    bool OverrideExposure = false;
    float ExposureA = 1.0f;
    float ExposureB = 1.0f;
};

// Deterministic golden-case sweep: seeded pairs x weight endpoints/midpoints x
// the contributes combinations that select each group gate, plus the log2
// exposure escape hatch at its boundary values (the 1e-6 clamp, HDR extremes,
// a negative input).
std::vector<BlendCase> GoldenCases()
{
    std::vector<BlendCase> cases;
    constexpr float kWeights[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    constexpr bool kCombos[][4] = {
        {true, true, true, true},
        {false, true, true, true},
        {true, false, true, true},
        {true, true, false, true},
        {true, true, true, false},
        {true, false, false, true}, // fog-glow group gated: neither fog contributes
        {false, false, false, false},
    };
    for (uint32 i = 0; i < 6; ++i)
        for (float w : kWeights)
            for (const auto& c : kCombos)
                cases.push_back({100u + i, 200u + i, w, c[0], c[1], c[2], c[3]});
    const std::pair<float, float> kExposurePairs[] = {
        {0.0f, 1.0f}, {1e-7f, 1e6f}, {65504.0f, 1e-6f}, {-1.0f, 2.0f}};
    for (const auto& [ea, eb] : kExposurePairs)
        cases.push_back({7u, 8u, 0.5f, true, true, true, true, true, ea, eb});
    return cases;
}

PostProcessSettings RunCase(const BlendCase& c)
{
    PostProcessSettings a = MakeRandomSettings(c.SeedA);
    PostProcessSettings b = MakeRandomSettings(c.SeedB);
    if (c.OverrideExposure)
    {
        a.Exposure = c.ExposureA;
        b.Exposure = c.ExposureB;
    }
    return BlendPostProcessSettings(
        a, b, c.Weight,
        ContributingGroups(c.ContributesColorGrade, c.ContributesHeightFog,
                           c.ContributesVolumetricFog, c.ContributesAtmosphericCloud,
                           /*volumetricClouds=*/true));
}

} // namespace BlendLock

// The generic post-process node calls ResolveDerivedForRenderHeight so it never
// names an effect. It must be an exact stand-in for the inline
// ResolveBloomPyramid + ResolveFogGlowPyramid sequence — the two resolves touch
// disjoint Bloom*/FogGlow* fields, so ordering them behind one call cannot move
// a single resolved member (and therefore cannot move a pushed constant).
// Compared through the member inventory, not as object representations: the
// struct carries padding (pinned above), a copy leaves padding bytes
// indeterminate, and no pushed constant is ever built from them.
TEST(PostProcessBlendCoverage, ResolveDerivedForRenderHeightMatchesInlineSequence)
{
    using namespace BlendLock;
    for (uint32_t height : {0u, 480u, 720u, 1080u, 1440u, 2160u})
    {
        for (float radius : {1.0f, 2.5f, 4.0f, 7.0f})
        {
            for (int octaves : {3, 4, 6, 8})
            {
                for (int quality : {0, 1, 2, 3})
                {
                    PostProcessSettings base{};
                    base.BloomRadius = radius;
                    base.BloomOctaves = octaves;
                    base.FogGlowRadius = radius;
                    base.FogGlowQuality = quality;

                    PostProcessSettings sequenced = base;
                    sequenced.ResolveBloomPyramid(height);
                    sequenced.ResolveFogGlowPyramid(height);

                    PostProcessSettings combined = base;
                    combined.ResolveDerivedForRenderHeight(height);

                    const char* bad = FirstMismatch(sequenced, combined);
                    EXPECT_TRUE(bad == nullptr)
                        << "field '" << bad << "' differs between the inline resolve sequence and "
                        << "ResolveDerivedForRenderHeight (height=" << height << " radius=" << radius
                        << " octaves=" << octaves << " quality=" << quality << ")";
                }
            }
        }
    }
}

// The production blend must reproduce the frozen reference bit-exactly for
// every field, across randomized pairs, a weight sweep (including the 0.5
// dominant-pick tie, which picks b), and all 16 contributes combinations.
TEST(PostProcessBlendEquivalence, BlendMatchesFrozenReferenceBitExact)
{
    using namespace BlendLock;
    for (uint32 seed = 1; seed <= 12; ++seed)
    {
        for (float w : {0.0f, 0.125f, 0.25f, 0.5f, 0.503f, 0.75f, 1.0f})
        {
            for (int combo = 0; combo < 32; ++combo)
            {
                const bool cg = (combo & 1) != 0;
                const bool hf = (combo & 2) != 0;
                const bool vf = (combo & 4) != 0;
                const bool ac = (combo & 8) != 0;
                const bool vc = (combo & 16) != 0;
                const PostProcessSettings a = MakeRandomSettings(1000u + seed);
                const PostProcessSettings b = MakeRandomSettings(2000u + seed);
                const PostProcessSettings ref = BlendReference(a, b, w, cg, hf, vf, ac, vc);
                const PostProcessSettings got = BlendPostProcessSettings(a, b, w, ContributingGroups(cg, hf, vf, ac, vc));
                const char* bad = FirstMismatch(ref, got);
                ASSERT_TRUE(bad == nullptr)
                    << "field '" << bad << "' diverges from the frozen blend reference"
                    << " (seed=" << seed << " w=" << w << " cg=" << cg << " hf=" << hf
                    << " vf=" << vf << " ac=" << ac << " vc=" << vc << ")";
            }
        }
    }

    // Log2 exposure hatch at its boundaries (clamp at 1e-6, HDR extremes, negatives).
    for (float ea : {0.0f, 1e-7f, 1e-6f, 1.0f, 65504.0f, 1e6f, -1.0f})
    {
        for (float eb : {0.0f, 1e-6f, 4.0f, 1e6f})
        {
            for (float w : {0.0f, 0.25f, 0.5f, 1.0f})
            {
                PostProcessSettings a = MakeRandomSettings(31u);
                PostProcessSettings b = MakeRandomSettings(32u);
                a.Exposure = ea;
                b.Exposure = eb;
                const PostProcessSettings ref = BlendReference(a, b, w, true, true, true, true);
                const PostProcessSettings got = BlendPostProcessSettings(a, b, w, SettingsBlendGroupMask::All());
                const char* bad = FirstMismatch(ref, got);
                ASSERT_TRUE(bad == nullptr)
                    << "field '" << bad << "' diverges (exposure hatch, ea=" << ea
                    << " eb=" << eb << " w=" << w << ")";
            }
        }
    }
}

// Committed goldens for every blend case, projected two ways so a failure is
// actionable: a per-case row hash says WHICH case moved, a per-field column
// hash says WHICH member moved, and the intersection is the defect. Exposure
// sits outside both hashes with its own ULP-bounded pin. A full byte dump of
// two representative cases guards the reference transcription with concrete
// values. Regenerate only for an INTENTIONAL blend-semantics change
// (GE_PP_BLEND_GOLDEN_REGEN=1).
TEST(PostProcessBlendEquivalence, GoldenFixtureLocksShippedBlendOutputs)
{
    using namespace BlendLock;
    const std::filesystem::path fixture = std::filesystem::path(GE_RENDERER_REPO_ROOT) /
        "Engine/Tests/Fixtures/PostProcessBlendGoldens.txt";
    const std::vector<BlendCase> cases = GoldenCases();
    const std::size_t dumpA = 0;                 // plain randomized case
    const std::size_t dumpB = cases.size() - 4;  // first exposure-extreme case

    std::vector<PostProcessSettings> outputs;
    outputs.reserve(cases.size());
    for (const BlendCase& c : cases)
        outputs.push_back(RunCase(c));

    std::vector<const char*> columnNames;
    columnNames.reserve(kExactFieldCount);
#define PP_LOCK_NAME(M) columnNames.push_back(#M);
    PP_BLEND_LOCK_EXACT_FIELDS(PP_LOCK_NAME)
#undef PP_LOCK_NAME
    ASSERT_EQ(columnNames.size(), kExactFieldCount);

    std::vector<uint64_t> columnHashes(kExactFieldCount, kFnvOffsetBasis);
    for (const PostProcessSettings& s : outputs)
    {
        std::size_t column = 0;
#define PP_LOCK_COLUMN(M) FnvAccumulate(columnHashes[column++], &s.M, sizeof(s.M));
        PP_BLEND_LOCK_EXACT_FIELDS(PP_LOCK_COLUMN)
#undef PP_LOCK_COLUMN
    }

    const auto emitCaseFields =
        [&](std::size_t idx, const std::function<void(const char*, const std::string&)>& emit)
    {
        const PostProcessSettings& s = outputs[idx];
#define PP_LOCK_DUMP(M) emit(#M, HexBytes(&s.M, sizeof(s.M)));
        PP_BLEND_LOCK_EXACT_FIELDS(PP_LOCK_DUMP)
#undef PP_LOCK_DUMP
    };

    if (std::getenv("GE_PP_BLEND_GOLDEN_REGEN") != nullptr)
    {
        // Binary: .gitattributes pins this fixture to LF, so a text-mode stream
        // would make a Windows regeneration rewrite every line as CRLF and bury
        // the real delta.
        std::ofstream out(fixture, std::ios::trunc | std::ios::binary);
        ASSERT_TRUE(out.is_open()) << fixture;
        out << "# PostProcessBlendGoldens v2 — BlendPostProcessSettings outputs.\n";
        out << "# cases: GoldenCases() in PostProcessBlendCoverageTests.cpp.\n";
        out << "# case <i> <h>: FNV-1a64 over one case's PP_BLEND_LOCK_EXACT_FIELDS bytes.\n";
        out << "# fieldhash <name> <h>: FNV-1a64 over one member's bytes across every case.\n";
        out << "# exposure <i> <bits>: Exposure blends through libm and is pinned to a ULP\n";
        out << "#   bound, not to these bits — see kExposureUlpBound.\n";
        out << "# field <i> <name> <bytes>: full member dump of two representative cases.\n";
        for (std::size_t i = 0; i < outputs.size(); ++i)
            out << "case " << i << " " << std::hex << HashBitLockedFields(outputs[i]) << std::dec << "\n";
        for (std::size_t c = 0; c < columnNames.size(); ++c)
            out << "fieldhash " << columnNames[c] << " " << std::hex << columnHashes[c] << std::dec << "\n";
        for (std::size_t i = 0; i < outputs.size(); ++i)
            out << "exposure " << i << " " << HexBytes(&outputs[i].Exposure, sizeof(float32)) << "\n";
        for (std::size_t idx : {dumpA, dumpB})
            emitCaseFields(idx, [&](const char* name, const std::string& hex)
                           { out << "field " << idx << " " << name << " " << hex << "\n"; });
    }

    std::ifstream in(fixture);
    ASSERT_TRUE(in.is_open()) << fixture << " (run once with GE_PP_BLEND_GOLDEN_REGEN=1 to capture)";
    std::vector<uint64_t> goldenCaseHashes;
    std::map<std::string, uint64_t> goldenColumnHashes;
    std::vector<std::string> goldenExposure;
    std::map<std::pair<std::size_t, std::string>, std::string> goldenFields;
    std::string tag;
    while (in >> tag)
    {
        if (tag == "case")
        {
            std::size_t idx = 0;
            uint64_t h = 0;
            in >> idx >> std::hex >> h >> std::dec;
            goldenCaseHashes.resize(std::max(goldenCaseHashes.size(), idx + 1));
            goldenCaseHashes[idx] = h;
        }
        else if (tag == "fieldhash")
        {
            std::string name;
            uint64_t h = 0;
            in >> name >> std::hex >> h >> std::dec;
            goldenColumnHashes[name] = h;
        }
        else if (tag == "exposure")
        {
            std::size_t idx = 0;
            std::string hex;
            in >> idx >> hex;
            goldenExposure.resize(std::max(goldenExposure.size(), idx + 1));
            goldenExposure[idx] = hex;
        }
        else if (tag == "field")
        {
            std::size_t idx = 0;
            std::string name, hex;
            in >> idx >> name >> hex;
            goldenFields[{idx, name}] = hex;
        }
        else
        {
            in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        }
    }

    ASSERT_EQ(goldenCaseHashes.size(), outputs.size())
        << "golden case count drifted — GoldenCases() changed without regenerating the fixture";
    ASSERT_EQ(goldenColumnHashes.size(), kExactFieldCount)
        << "golden field count drifted — PP_BLEND_LOCK_EXACT_FIELDS changed without "
           "regenerating the fixture";
    ASSERT_EQ(goldenExposure.size(), outputs.size());

    // Columns first: this is the projection that names the member.
    for (std::size_t c = 0; c < columnNames.size(); ++c)
    {
        const auto it = goldenColumnHashes.find(columnNames[c]);
        ASSERT_TRUE(it != goldenColumnHashes.end()) << "fixture missing field: " << columnNames[c];
        EXPECT_EQ(columnHashes[c], it->second)
            << "member '" << columnNames[c] << "' blends to different bits than the fixture holds";
    }
    // Then rows, to name the case the member moved in.
    for (std::size_t i = 0; i < outputs.size(); ++i)
        EXPECT_EQ(HashBitLockedFields(outputs[i]), goldenCaseHashes[i]) << "golden case " << i;

    for (std::size_t i = 0; i < outputs.size(); ++i)
    {
        ASSERT_EQ(goldenExposure[i].size(), sizeof(float32) * 2) << "golden case " << i;
        // Exact inverse of HexBytes: object bytes in memory order.
        unsigned char referenceBytes[sizeof(float32)];
        for (std::size_t b = 0; b < sizeof(float32); ++b)
            referenceBytes[b] = static_cast<unsigned char>(
                std::stoul(goldenExposure[i].substr(b * 2, 2), nullptr, 16));
        float32 reference = 0.0f;
        std::memcpy(&reference, referenceBytes, sizeof(reference));
        EXPECT_LE(UlpDistance(outputs[i].Exposure, reference), kExposureUlpBound)
            << "member 'Exposure' in golden case " << i << ": got " << outputs[i].Exposure << " ("
            << HexBytes(&outputs[i].Exposure, sizeof(float32)) << "), pinned " << reference << " ("
            << goldenExposure[i] << ")";
    }

    std::size_t checkedFields = 0;
    for (std::size_t idx : {dumpA, dumpB})
    {
        emitCaseFields(idx, [&](const char* name, const std::string& hex)
        {
            const auto it = goldenFields.find({idx, std::string(name)});
            ASSERT_TRUE(it != goldenFields.end()) << "fixture missing field dump: " << name;
            EXPECT_EQ(hex, it->second) << "case " << idx << " member " << name;
            ++checkedFields;
        });
    }
    EXPECT_EQ(checkedFields, kExactFieldCount * 2);
}

// The fixture bit-locks kExactFieldCount members on the strength of one
// property: a lerp between two grid values at a golden weight lands on an
// exactly representable float, so nothing rounds and neither the platform's
// libm nor the compiler's choice to contract the multiply-add can change the
// bits. Check it over the whole grid rather than asserting it — a weight added
// to GoldenCases() that breaks exactness surfaces here, naming the weight,
// instead of as a fixture only its author can keep green.
TEST(PostProcessBlendEquivalence, LerpFoldIsExactOverTheGoldenGrid)
{
    using namespace BlendLock;
    std::vector<float> weights;
    for (const BlendCase& c : GoldenCases())
    {
        if (std::find(weights.begin(), weights.end(), c.Weight) == weights.end())
            weights.push_back(c.Weight);
    }
    ASSERT_FALSE(weights.empty());

    for (const float w : weights)
    {
        std::size_t rounded = 0;
        std::size_t contractionSplit = 0;
        for (int nx = 0; nx < kGridSteps; ++nx)
        {
            const float32 x = GridValue(nx);
            for (int ny = 0; ny < kGridSteps; ++ny)
            {
                const float32 y = GridValue(ny);
                const float32 folded = x + (y - x) * w;
                const float32 contracted = std::fma(y - x, w, x);
                const double wide = static_cast<double>(x) +
                                    (static_cast<double>(y) - static_cast<double>(x)) *
                                        static_cast<double>(w);
                if (static_cast<double>(folded) != wide)
                    ++rounded;
                if (std::memcmp(&folded, &contracted, sizeof(float32)) != 0)
                    ++contractionSplit;
            }
        }
        EXPECT_EQ(rounded, 0u)
            << "weight " << w << " rounds the lerp fold on the input grid — the bit-locked half "
            << "of the fixture is no longer platform-independent at this weight";
        EXPECT_EQ(contractionSplit, 0u)
            << "weight " << w << " makes the lerp fold sensitive to multiply-add contraction";
    }
}

namespace
{
using GameEngine::float32;
using GameEngine::int32;
using GameEngine::uint64;

bool BitsEqual(const void* p, const void* q, std::size_t n) { return std::memcmp(p, q, n) == 0; }

// A blend output is exact when it is byte-identical to one of its inputs
// (Lerp endpoints, Dominant, the non-contributing keep-a copy), to the
// default-constructed value (Skip, and the asset-GUID hatch), or to a lerp that
// did not round — optionally clamped, as the lens-dirt hatch clamps one.
bool BlendsExactly(float32 out, float32 x, float32 y, float w, float32 dflt)
{
    if (BitsEqual(&out, &x, sizeof(out)) || BitsEqual(&out, &y, sizeof(out)) ||
        BitsEqual(&out, &dflt, sizeof(out)))
        return true;
    const double wide = static_cast<double>(x) +
                        (static_cast<double>(y) - static_cast<double>(x)) * static_cast<double>(w);
    const float32 folded = static_cast<float32>(wide);
    if (static_cast<double>(folded) != wide)
        return false;
    if (BitsEqual(&out, &folded, sizeof(out)))
        return true;
    const float32 clamped = std::clamp(folded, 0.0f, 10.0f);
    return BitsEqual(&out, &clamped, sizeof(out));
}

bool BlendsExactly(int32 out, int32 x, int32 y, float, int32 dflt)
{
    return out == x || out == y || out == dflt;
}

bool BlendsExactly(bool out, bool x, bool y, float, bool dflt)
{
    return out == x || out == y || out == dflt;
}

bool BlendsExactly(const uint64 (&out)[2], const uint64 (&x)[2], const uint64 (&y)[2], float,
                   const uint64 (&dflt)[2])
{
    return BitsEqual(out, x, sizeof(out)) || BitsEqual(out, y, sizeof(out)) ||
           BitsEqual(out, dflt, sizeof(out));
}

} // namespace

// The bit-lock is sound only while every locked member's output comes from
// exact arithmetic. A member that folds through libm — a second Log2Lerp
// registration — matches none of the exact forms and surfaces here, naming
// itself, instead of as a fixture that goes red for whoever did not author it.
TEST(PostProcessBlendEquivalence, BitLockedFieldsCarryOnlyExactArithmetic)
{
    using namespace BlendLock;
    const PostProcessSettings dflt{};
    std::map<std::string, std::size_t> inexactCases;
    const std::vector<BlendCase> cases = GoldenCases();
    for (const BlendCase& c : cases)
    {
        PostProcessSettings a = MakeRandomSettings(c.SeedA);
        PostProcessSettings b = MakeRandomSettings(c.SeedB);
        if (c.OverrideExposure)
        {
            a.Exposure = c.ExposureA;
            b.Exposure = c.ExposureB;
        }
        const PostProcessSettings out = RunCase(c);
#define PP_LOCK_EXACTNESS(M) \
    if (!BlendsExactly(out.M, a.M, b.M, c.Weight, dflt.M)) ++inexactCases[#M];
        PP_BLEND_LOCK_EXACT_FIELDS(PP_LOCK_EXACTNESS)
#undef PP_LOCK_EXACTNESS
    }

    for (const auto& [name, count] : inexactCases)
    {
        ADD_FAILURE() << "member '" << name << "' blends to a rounded value in " << count << " of "
                      << cases.size() << " golden cases — it cannot be bit-locked; move it to "
                      << "PP_BLEND_LOCK_TOLERANCE_FIELDS and pin it to a ULP bound";
    }
    EXPECT_TRUE(inexactCases.empty());

    // The partition earns its keep only if the tolerance half is genuinely
    // inexact: Exposure must fail the same check the locked members pass.
    std::size_t exposureInexact = 0;
    for (const BlendCase& c : cases)
    {
        PostProcessSettings a = MakeRandomSettings(c.SeedA);
        PostProcessSettings b = MakeRandomSettings(c.SeedB);
        if (c.OverrideExposure)
        {
            a.Exposure = c.ExposureA;
            b.Exposure = c.ExposureB;
        }
        if (!BlendsExactly(RunCase(c).Exposure, a.Exposure, b.Exposure, c.Weight, dflt.Exposure))
            ++exposureInexact;
    }
    EXPECT_GT(exposureInexact, 0u)
        << "Exposure now blends exactly on every golden case — if its Log2Lerp fold is gone, it "
           "belongs in PP_BLEND_LOCK_EXACT_FIELDS";
}

// The tolerance half is sound only while kExposureUlpBound actually covers the
// freedom a conforming platform has in the Log2Lerp fold. Enumerate it over
// every golden case — log2 and exp2 each faithful to one ULP, the fold
// contracted or not — and require the worst pairwise spread to fit the bound.
// A case added at a wider exposure magnitude surfaces here, naming its spread,
// instead of as a fixture only its author can keep green.
TEST(PostProcessBlendEquivalence, ExposureUlpBoundCoversConformingPlatforms)
{
    using namespace BlendLock;
    // Matches the Log2Lerp clamp in PostProcessSettings.cpp.
    constexpr float32 kLog2LerpFloor = 1e-6f;
    // The bound is a measured worst case, not a free parameter: one far above
    // the spread would stop catching real drift.
    constexpr int64_t kMaxHeadroomFactor = 4;

    const auto stepUlp = [](float32 v, int k)
    {
        const float32 dir = k < 0 ? -std::numeric_limits<float32>::infinity()
                                  : std::numeric_limits<float32>::infinity();
        for (int i = 0; i < (k < 0 ? -k : k); ++i)
            v = std::nextafter(v, dir);
        return v;
    };

    int64_t worstSpread = 0;
    float32 worstA = 0.0f;
    float32 worstB = 0.0f;
    float worstW = 0.0f;
    for (const BlendCase& c : GoldenCases())
    {
        const float32 a = c.OverrideExposure ? c.ExposureA : MakeRandomSettings(c.SeedA).Exposure;
        const float32 b = c.OverrideExposure ? c.ExposureB : MakeRandomSettings(c.SeedB).Exposure;
        const float32 lxNominal = std::log2(std::max(a, kLog2LerpFloor));
        const float32 lyNominal = std::log2(std::max(b, kLog2LerpFloor));

        std::vector<float32> outputs;
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                const float32 lx = stepUlp(lxNominal, dx);
                const float32 ly = stepUlp(lyNominal, dy);
                for (const float32 t : {lx + (ly - lx) * c.Weight, std::fma(ly - lx, c.Weight, lx)})
                {
                    const float32 folded = std::exp2(t);
                    for (int de = -1; de <= 1; ++de)
                        outputs.push_back(stepUlp(folded, de));
                }
            }
        }

        int64_t spread = 0;
        for (std::size_t i = 0; i < outputs.size(); ++i)
            for (std::size_t j = i + 1; j < outputs.size(); ++j)
                spread = std::max(spread, UlpDistance(outputs[i], outputs[j]));
        if (spread > worstSpread)
        {
            worstSpread = spread;
            worstA = a;
            worstB = b;
            worstW = c.Weight;
        }
    }

    EXPECT_LE(worstSpread, kExposureUlpBound)
        << "the Log2Lerp fold spreads " << worstSpread << " ULP across conforming platforms "
        << "(worst case a=" << worstA << " b=" << worstB << " w=" << worstW
        << ") — widen kExposureUlpBound to the measured spread or drop the case that widens it";
    EXPECT_GE(worstSpread * kMaxHeadroomFactor, kExposureUlpBound)
        << "kExposureUlpBound is far wider than the " << worstSpread
        << " ULP the fold actually needs — narrow it back to the measured spread";
}

// ===========================================================================
// PP-ARCH Phase 2 — extraction hooks. Per-effect folds moved from
// RenderExtractionSystem onto the descriptor registrations; these tests pin
// the hook wiring (every effect registers one, the order-sensitive fog pair
// keeps its order) and the fold semantics that are easy to silently regress
// (clamps, enable gating, the shared fog-glow merge, per-effect quirks).
// ===========================================================================

namespace
{

const GameEngine::Rendering::PostProcessEffectDescriptor* FindEffectDescriptor(
    std::string_view componentName)
{
    const GameEngine::Rendering::PostProcessEffectDescriptor* found = nullptr;
    GameEngine::Rendering::PostProcessEffectRegistry::ForEach(
        [&](const GameEngine::Rendering::PostProcessEffectDescriptor& d)
        {
            if (d.ComponentName == componentName)
                found = &d;
        });
    return found;
}

// Mirror of the extraction walk's hook loop: neutralize + extract in
// registration order, null services (headless).
GameEngine::Engine::Renderer::PostProcessExtractedVolume ExtractAllEffects(
    GameEngine::ECS::World& world, GameEngine::ECS::EntityHandle e)
{
    GameEngine::Engine::Renderer::PostProcessExtractedVolume out{};
    GameEngine::Engine::Renderer::PostProcessExtractContext ctx{};
    GameEngine::Rendering::PostProcessEffectRegistry::ForEach(
        [&](const GameEngine::Rendering::PostProcessEffectDescriptor& d)
        {
            if (d.NeutralizeSettings)
                d.NeutralizeSettings(out.Settings);
            if (d.Extract)
                d.Extract(world, e, ctx, out);
        });
    return out;
}

} // namespace

TEST(PostProcessEffectRegistry, LutUsesConciseDisplayName)
{
    const auto* descriptor = FindEffectDescriptor("CubeLutEffect");
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->DisplayName, "LUT");
}

// Adding an effect is one registration: every registered descriptor must carry
// an Extract hook, and the two whose struct defaults are not authored-off
// carry the per-volume neutralize. VolumetricFog must extract before HeightFog
// (the shared fog-glow merge is order-sensitive).
TEST(PostProcessExtractHooks, EveryDescriptorRegistersExtractAndFogOrderHolds)
{
    namespace R = GameEngine::Rendering;
    size_t count = 0;
    int volumetricIndex = -1;
    int heightIndex = -1;
    int index = 0;
    R::PostProcessEffectRegistry::ForEach(
        [&](const R::PostProcessEffectDescriptor& d)
        {
            EXPECT_NE(d.Extract, nullptr) << d.ComponentName;
            if (d.ComponentName == "VolumetricFogEffect")
            {
                volumetricIndex = index;
                EXPECT_NE(d.NeutralizeSettings, nullptr);
            }
            if (d.ComponentName == "AtmosphericCloudLayer")
                EXPECT_NE(d.NeutralizeSettings, nullptr);
            if (d.ComponentName == "HeightFogEffect")
                heightIndex = index;
            ++index;
            ++count;
        });
    EXPECT_EQ(count, 22u);
    ASSERT_GE(volumetricIndex, 0);
    ASSERT_GE(heightIndex, 0);
    EXPECT_LT(volumetricIndex, heightIndex)
        << "volumetric fog must extract before height fog: height fog's shared "
           "fog-glow merge reads volumetric's unconditional write";
}

// The Add Post FX picker creates effects through ComponentFactory. Keep the
// registry and factory in lockstep so selecting an effect can never close the
// picker without adding anything. This specifically covers components the
// header scanner cannot reflect automatically (currently LUT).
TEST(PostProcessEffectRegistry, EveryDescriptorIsFactoryCreatable)
{
    namespace R = GameEngine::Rendering;
    GameEngine::ECS::World world;
    size_t count = 0;

    R::PostProcessEffectRegistry::ForEach(
        [&](const R::PostProcessEffectDescriptor& d)
        {
            ++count;
            EXPECT_TRUE(GameEngine::ECS::ComponentFactory::Has(d.Type))
                << d.ComponentName;

            const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
            ASSERT_TRUE(GameEngine::ECS::ComponentFactory::Create(world, entity, d.Type))
                << d.ComponentName;
            EXPECT_TRUE(world.HasComponent(entity, d.Type)) << d.ComponentName;
        });

    EXPECT_EQ(count, 22u);
}

TEST(PostProcessExtractHooks, BloomFoldClampsAndGatesOnEnabled)
{
    GameEngine::ECS::World world;
    GameEngine::ECS::EntityHandle e = world.CreateEntity();
    GameEngine::Components::BloomEffect bloom{};
    bloom.Enabled = true;
    bloom.Intensity = 2.0f;
    bloom.ScatteringAmount = 1.5f;
    bloom.Radius = 9.0f;   // clamps to 7
    bloom.Octaves = 99;    // clamps to 8
    bloom.Tint[0] = 2.0f;  // clamps to 1
    world.AddComponentImmediate(e, bloom);

    auto out = ExtractAllEffects(world, e);
    EXPECT_FLOAT_EQ(out.Settings.BloomIntensity, 2.0f);
    EXPECT_FLOAT_EQ(out.Settings.BloomScatteringAmount, 1.0f);
    EXPECT_FLOAT_EQ(out.Settings.BloomRadius, 7.0f);
    EXPECT_EQ(out.Settings.BloomOctaves, 8);
    EXPECT_FLOAT_EQ(out.Settings.BloomTintR, 1.0f);

    // Disabled component folds nothing: defaults stay.
    GameEngine::ECS::EntityHandle e2 = world.CreateEntity();
    GameEngine::Components::BloomEffect off = bloom;
    off.Enabled = false;
    world.AddComponentImmediate(e2, off);
    auto out2 = ExtractAllEffects(world, e2);
    EXPECT_FLOAT_EQ(out2.Settings.BloomScatteringAmount, 0.0f);
    EXPECT_FLOAT_EQ(out2.Settings.BloomIntensity, PostProcessSettings{}.BloomIntensity);
}

TEST(PostProcessExtractHooks, AtmosphericCloudPreservesHdrColorAndClampsInvalidChannels)
{
    GameEngine::ECS::World world;
    const GameEngine::ECS::EntityHandle entity = world.CreateEntity();
    GameEngine::Components::AtmosphericCloudLayer cloud{};
    cloud.Enabled = true;
    cloud.CloudColor[0] = 4.0f;
    cloud.CloudColor[1] = -1.0f;
    cloud.CloudColor[2] = 20.0f;
    cloud.Opacity = 12.0f;
    world.AddComponentImmediate(entity, cloud);

    const auto out = ExtractAllEffects(world, entity);
    EXPECT_TRUE(out.HasAtmosphericCloud);
    EXPECT_FLOAT_EQ(out.Settings.AtmosphericCloudColorR, 4.0f);
    EXPECT_FLOAT_EQ(out.Settings.AtmosphericCloudColorG, 0.0f);
    EXPECT_FLOAT_EQ(out.Settings.AtmosphericCloudColorB,
                    GameEngine::Components::kAtmosphericCloudColorMaxIntensity);
    EXPECT_FLOAT_EQ(out.Settings.AtmosphericCloudOpacity, 8.0f);
}

// Both fog effects author the shared glow block; the stronger enabled
// authoring owns it. Volumetric writes unconditionally first; height fog
// merges only when enabled and at least as intense.
TEST(PostProcessExtractHooks, SharedFogGlowMergePrefersStrongerAuthoring)
{
    GameEngine::ECS::World world;

    const auto makeEntity = [&world](float volGlow, float heightGlow)
    {
        GameEngine::ECS::EntityHandle e = world.CreateEntity();
        GameEngine::Components::VolumetricFogEffect vf{};
        vf.Enabled = true;
        vf.FogGlowEnabled = true;
        vf.FogGlowIntensity = volGlow;
        world.AddComponentImmediate(e, vf);
        GameEngine::Components::HeightFogEffect hf{};
        hf.Enabled = true;
        hf.FogGlowEnabled = true;
        hf.FogGlowIntensity = heightGlow;
        world.AddComponentImmediate(e, hf);
        return e;
    };

    const auto weakVolume = ExtractAllEffects(world, makeEntity(0.2f, 0.5f));
    EXPECT_FLOAT_EQ(weakVolume.Settings.FogGlowIntensity, 0.5f)
        << "height fog's stronger glow must win the merge";
    EXPECT_TRUE(weakVolume.HasVolumetricFog);
    EXPECT_TRUE(weakVolume.HasHeightFog);

    const auto strongVolume = ExtractAllEffects(world, makeEntity(0.8f, 0.3f));
    EXPECT_FLOAT_EQ(strongVolume.Settings.FogGlowIntensity, 0.8f)
        << "volumetric's stronger glow must survive height fog's merge";
}

// Deband is present-is-policy: a PRESENT disabled component folds the gate to
// zero explicitly rather than falling through like an absent component.
TEST(PostProcessExtractHooks, DebandPresenceIsPolicyAndClamps)
{
    GameEngine::ECS::World world;
    GameEngine::ECS::EntityHandle e = world.CreateEntity();
    GameEngine::Components::DebandEffect deband{};
    deband.Enabled = false;
    deband.ThresholdLsb = 6.0f;
    world.AddComponentImmediate(e, deband);
    auto out = ExtractAllEffects(world, e);
    EXPECT_FLOAT_EQ(out.Settings.DebandThresholdLsb, 0.0f);

    GameEngine::ECS::EntityHandle e2 = world.CreateEntity();
    deband.Enabled = true;
    deband.ThresholdLsb = 99.0f; // clamps to the GE_DEBAND_THRESHOLD parser range
    world.AddComponentImmediate(e2, deband);
    auto out2 = ExtractAllEffects(world, e2);
    EXPECT_FLOAT_EQ(out2.Settings.DebandThresholdLsb, 16.0f);
}

// The physical lens determines DoF strength; the fold stamps unit intensity
// and the volume Weight supplies the transition blend.
TEST(PostProcessExtractHooks, DofFoldStampsUnitIntensityAndClampsRadius)
{
    GameEngine::ECS::World world;
    GameEngine::ECS::EntityHandle e = world.CreateEntity();
    GameEngine::Components::DepthOfFieldEffect dof{};
    dof.Enabled = true;
    dof.MaxRadius = 10000.0f;
    world.AddComponentImmediate(e, dof);
    auto out = ExtractAllEffects(world, e);
    EXPECT_FLOAT_EQ(out.Settings.DofIntensity, 1.0f);
    EXPECT_FLOAT_EQ(out.Settings.DofMaxRadius,
                    GameEngine::Components::DepthOfFieldEffect::kMaxRadiusMax);
}

// SSSR's whole settings/scene surface is one registration. The node writes its
// push constants by literal name, so none of its members may be shader-named:
// a named entry would silently publish them as rendergraph skipWhen/push
// targets and add them to the ReadableFieldNames on-disk contract.
TEST(PostProcessExtractHooks, SssrRegistersBlendOnlyFieldsAndSceneIoMetadata)
{
    namespace R = GameEngine::Rendering;
    const auto* desc = FindEffectDescriptor("ScreenSpaceReflectionsEffect");
    ASSERT_NE(desc, nullptr) << "SSSR is not registered with the effect registry";
    EXPECT_EQ(desc->DisplayName, "Screen Space Reflections");
    EXPECT_TRUE(desc->RequiredPackage.empty()) << "SSSR ships in the core shader set";
    EXPECT_EQ(desc->NeutralizeSettings, nullptr)
        << "SSSRIntensity defaults to the authored-off value, so no baseline is needed";

    ASSERT_EQ(desc->SettingsFields.size(), 7u);
    for (const R::EffectSettingsField& f : desc->SettingsFields)
        EXPECT_TRUE(f.ShaderName.empty())
            << "SSSR settings must stay blend-only; '" << f.ShaderName << "' is named";

    // The enum keeps its integer on-disk form, and every bounded field carries
    // its load clamp — the hand schema's behavior, expressed as metadata.
    const auto* quality = R::PostProcessEffectRegistry::FindFieldIO(*desc, "SampleQuality");
    ASSERT_NE(quality, nullptr);
    EXPECT_TRUE(quality->EnumAsInt);
    EXPECT_TRUE(quality->HasClamp);
    for (const char* name : {"Intensity", "MaxDistance", "Thickness", "EdgeFade", "MaxSteps"})
    {
        const auto* io = R::PostProcessEffectRegistry::FindFieldIO(*desc, name);
        ASSERT_NE(io, nullptr) << name;
        EXPECT_TRUE(io->HasClamp) << name;
    }
    // Enabled/MultiBounce are plain bools: no metadata, so they must NOT be
    // listed (an entry there would mean a clamp on a bool).
    EXPECT_EQ(R::PostProcessEffectRegistry::FindFieldIO(*desc, "MultiBounce"), nullptr);
}

// Every authored SSSR field is clamped into the range the traversal assumes.
// Reddens if any clamp bound is dropped from the fold or widened.
TEST(PostProcessExtractHooks, SssrFoldClampsEveryAuthoredField)
{
    using SSSR = GameEngine::Components::ScreenSpaceReflectionsEffect;
    GameEngine::ECS::World world;
    GameEngine::ECS::EntityHandle e = world.CreateEntity();
    SSSR sssr{};
    sssr.Enabled = true;
    sssr.Intensity = 99.0f;
    sssr.MaxDistance = -50.0f;
    sssr.Thickness = 0.0f;
    sssr.EdgeFade = 10.0f;
    sssr.MaxSteps = 100000;
    sssr.SampleQuality = static_cast<GameEngine::Components::SssrSampleQuality>(77);
    sssr.MultiBounce = true;
    world.AddComponentImmediate(e, sssr);

    auto out = ExtractAllEffects(world, e);
    EXPECT_FLOAT_EQ(out.Settings.SSSRIntensity, SSSR::kIntensityMax);
    EXPECT_FLOAT_EQ(out.Settings.SSSRMaxDistance, 0.0f);
    EXPECT_FLOAT_EQ(out.Settings.SSSRThickness, SSSR::kMinThickness);
    EXPECT_FLOAT_EQ(out.Settings.SSSREdgeFade, SSSR::kMaxEdgeFade);
    EXPECT_EQ(out.Settings.SSSRMaxSteps, SSSR::kMaxSteps);
    EXPECT_EQ(out.Settings.SSSRSampleQuality, SSSR::kSampleQualityLast);
    EXPECT_EQ(out.Settings.SSSRMultiBounce, 1);

    // The clamped-low arm: a below-range author must land on the minimum, not
    // the maximum the row above would also accept.
    SSSR low{};
    low.Enabled = true;
    low.MaxSteps = 1;
    low.SampleQuality = static_cast<GameEngine::Components::SssrSampleQuality>(-4);
    low.EdgeFade = 0.0f;
    GameEngine::ECS::EntityHandle e2 = world.CreateEntity();
    world.AddComponentImmediate(e2, low);
    auto outLow = ExtractAllEffects(world, e2);
    EXPECT_EQ(outLow.Settings.SSSRMaxSteps, SSSR::kMinSteps);
    EXPECT_EQ(outLow.Settings.SSSRSampleQuality, 0);
    EXPECT_FLOAT_EQ(outLow.Settings.SSSREdgeFade, SSSR::kMinEdgeFade);
}

// A disabled — or absent — effect must leave the settings at the authored-off
// defaults, which is what makes NeutralizeSettings unnecessary for SSSR.
// Reddens if the fold drops its Enabled gate, or if the settings default for
// SSSRIntensity ever becomes non-zero.
TEST(PostProcessExtractHooks, SssrDisabledAndAbsentBothReadInactive)
{
    using SSSR = GameEngine::Components::ScreenSpaceReflectionsEffect;
    GameEngine::ECS::World world;

    GameEngine::ECS::EntityHandle disabled = world.CreateEntity();
    SSSR off{};
    off.Enabled = false;
    off.Intensity = 1.0f; // authored strong, but switched off
    world.AddComponentImmediate(disabled, off);
    auto outDisabled = ExtractAllEffects(world, disabled);
    EXPECT_FLOAT_EQ(outDisabled.Settings.SSSRIntensity, 0.0f);
    EXPECT_FALSE(outDisabled.Settings.IsSSSRActive());

    GameEngine::ECS::EntityHandle bare = world.CreateEntity();
    auto outBare = ExtractAllEffects(world, bare);
    EXPECT_FLOAT_EQ(outBare.Settings.SSSRIntensity, 0.0f);
    EXPECT_FALSE(outBare.Settings.IsSSSRActive());

    // ...and an enabled default-constructed effect DOES read active, so the
    // two rows above cannot pass by the predicate being stuck at false.
    GameEngine::ECS::EntityHandle on = world.CreateEntity();
    world.AddComponentImmediate(on, SSSR{});
    auto outOn = ExtractAllEffects(world, on);
    EXPECT_TRUE(outOn.Settings.IsSSSRActive());
}

// IsSSSRActive gates the six-pass chain AND the world pass's extra MRT slices.
// Each term must independently veto; reddens if any is dropped from the &&.
TEST(PostProcessSettingsWrite, SssrActivePredicateVetoesOnEveryTerm)
{
    PostProcessSettings s{};
    s.SSSRIntensity = 0.75f;
    ASSERT_TRUE(s.IsSSSRActive());

    s.SSSRIntensity = 0.0f;
    EXPECT_FALSE(s.IsSSSRActive()) << "zero intensity contributes nothing";
    s.SSSRIntensity = 0.75f;

    s.SSSRMaxDistance = 0.0f;
    EXPECT_FALSE(s.IsSSSRActive()) << "a zero-length ray can never hit";
    s.SSSRMaxDistance = 100.0f;

    s.SSSRMaxSteps = 0;
    EXPECT_FALSE(s.IsSSSRActive()) << "a zero-step march can never hit";
}

TEST(PostProcessExtractHooks, CrtResolutionDivZeroFallsBackToDefault)
{
    GameEngine::ECS::World world;
    GameEngine::ECS::EntityHandle e = world.CreateEntity();
    GameEngine::Components::CrtEffect crt{};
    crt.Enabled = true;
    crt.Intensity = 1.0f;
    crt.EmulatedResolutionDiv = 0.0f; // authored zero -> shipped 6.0 fallback
    world.AddComponentImmediate(e, crt);
    auto out = ExtractAllEffects(world, e);
    EXPECT_FLOAT_EQ(out.Settings.CrtEmulatedResolutionDiv, 6.0f);
}

// Package-gated effects treat absent services as available (tests, headless
// worlds) — the shipped !rs || IsPackageAvailable(...) contract.
TEST(PostProcessExtractHooks, PackageGatedEffectExtractsWithNullServices)
{
    GameEngine::ECS::World world;
    GameEngine::ECS::EntityHandle e = world.CreateEntity();
    GameEngine::Components::ChromaticAberrationEffect ca{};
    ca.Enabled = true;
    ca.Intensity = 0.5f;
    world.AddComponentImmediate(e, ca);
    auto out = ExtractAllEffects(world, e);
    EXPECT_FLOAT_EQ(out.Settings.ChromaticAberrationIntensity, 0.5f);
}

// The clamp flags gate whether a volume narrows the camera envelope at all —
// an unflagged bound must stay at its no-op identity.
TEST(PostProcessExtractHooks, ExposureAdjustmentRespectsClampFlags)
{
    GameEngine::ECS::World world;
    GameEngine::ECS::EntityHandle e = world.CreateEntity();
    GameEngine::Components::ExposureAdjustmentEffect adj{};
    adj.Enabled = true;
    adj.Compensation = 1.5f;
    adj.ClampMin = true;
    adj.MinEv = 6.0f;
    adj.ClampMax = false;
    adj.MaxEv = 12.0f; // must NOT apply
    world.AddComponentImmediate(e, adj);
    auto out = ExtractAllEffects(world, e);
    EXPECT_FLOAT_EQ(out.Settings.ExposureCompensationEv, 1.5f);
    EXPECT_FLOAT_EQ(out.Settings.ExposureClampMinEv, 6.0f);
    EXPECT_FLOAT_EQ(out.Settings.ExposureClampMaxEv,
                    GameEngine::Engine::Renderer::kExposureClampNoOpMaxEv);
}

// The per-volume neutralize baselines fog/cloud struct defaults for volumes
// that do not carry those effects — a grade-only volume folds as no-fog.
TEST(PostProcessExtractHooks, NeutralizeBaselinesFogAndCloudDefaults)
{
    GameEngine::ECS::World world;
    GameEngine::ECS::EntityHandle e = world.CreateEntity(); // no effects at all
    auto out = ExtractAllEffects(world, e);
    EXPECT_FLOAT_EQ(out.Settings.VolumetricFogIntensity, 0.0f);
    EXPECT_FLOAT_EQ(out.Settings.VolumetricFogDensity, 0.0f);
    EXPECT_EQ(out.Settings.VolumetricFogTemporalEnabled, 0);
    EXPECT_FLOAT_EQ(out.Settings.VolumetricFogTemporalBlend, 0.0f);
    EXPECT_FLOAT_EQ(out.Settings.AtmosphericCloudSkyFill, 0.0f);
    EXPECT_FLOAT_EQ(out.Settings.AtmosphericCloudVaporMass, 0.0f);
    EXPECT_FALSE(FindEffectDescriptor("BloomEffect") == nullptr);
}

TEST(PostProcessBloomControls, AdditiveAndScatteringAmountsAreIndependent)
{
    PostProcessSettings s{};
    EXPECT_FLOAT_EQ(GameEngine::Components::BloomEffect{}.ScatteringAmount, 0);
    s.BloomIntensity = 0.25f;
    EXPECT_TRUE(s.IsBloomActive());
    s.BloomIntensity = 0;
    EXPECT_FALSE(s.IsBloomActive());
    s.BloomScatteringAmount = 0.2f;
    EXPECT_TRUE(s.IsBloomActive());
    s.BloomScatteringAmount = 0;
    s.BloomDepthVeilEnabled = 1;
    EXPECT_TRUE(s.IsBloomActive());
}

TEST(PostProcessBloomControls, ModernRadiusUsesFixedFootprintsAndFadesTheLastOctave)
{
        for (int ceiling : {3, 5, 8})
            for (uint32_t height : {720u, 1080u, 2160u})
            {
                float previous = 0.0f;
                for (int step = 0; step <= 120; ++step)
                {
                    PostProcessSettings s{};
                    s.BloomOctaves = ceiling;
                    s.BloomRadius = 1.0f + step * 0.05f;
                    s.ResolveBloomPyramid(height);
                    EXPECT_GE(s.BloomOctaves, 3);
                    EXPECT_LE(s.BloomOctaves, ceiling);
                    EXPECT_GE(s.BloomOctaveBlend, 0.0f);
                    EXPECT_LE(s.BloomOctaveBlend, 1.0f);
                    EXPECT_GE(s.BloomSampleScale, 0.5f);
                    EXPECT_LE(s.BloomSampleScale, 1.0f);
                    const float extent = s.BloomOctaves - 1.0f + s.BloomOctaveBlend;
                    EXPECT_GE(extent, previous);
                    previous = extent;
                }
            }
    PostProcessSettings s{};
    s.BloomOctaves = 8;
    s.BloomRadius = 2.5f;
    s.ResolveBloomPyramid(1024);
    EXPECT_EQ(s.BloomOctaves, 5);
    EXPECT_NEAR(s.BloomOctaveBlend, (0.5f - 0.001f) / 0.999f, 1e-6f);
    EXPECT_FLOAT_EQ(s.BloomSampleScale, 1.0f);
}
