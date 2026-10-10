#include "Engine/Rendering/PostProcessSettings.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"
#include "Engine/Rendering/VolumetricFogSettings.h"
#include "Engine/Rendering/Exposure.h"
#include "Rendering/Core/NamedPushConstantWriter.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <type_traits>

// Forward-declare GameEngine::Rendering namespace for combined-build compatibility
// (other files in this unity batch use 'using namespace ::GameEngine::Rendering').
namespace GameEngine { namespace Rendering {} }

namespace GameEngine {
namespace Engine {
namespace Renderer {

namespace
{
bool Near(float32 a, float32 b, float32 epsilon)
{
    return std::fabs(a - b) <= epsilon;
}

float32 AsGateValue(bool value)
{
    return value ? 1.0f : 0.0f;
}
} // namespace

bool PostProcessSettings::IsBloomActive(float32 epsilon) const
{
    return IsBloomHighlightsActive(epsilon) || IsBloomScatteringActive(epsilon);
}

bool PostProcessSettings::IsBloomScatteringActive(float32 epsilon) const
{
    return BloomScatteringAmount > epsilon;
}

void PostProcessSettings::DisableBloom()
{
    BloomIntensity = 0.0f;
    BloomDepthVeilEnabled = 0;
    BloomScatteringAmount = 0.0f;
}

bool PostProcessSettings::IsBloomHighlightsActive(float32 epsilon) const
{
    const bool brightPassActive = BloomIntensity > epsilon;
    const bool depthVeilActive = BloomDepthVeilEnabled != 0 &&
                                 BloomDepthVeilIntensity > epsilon &&
                                 BloomDepthVeilEnd > BloomDepthVeilStart + epsilon;
    return brightPassActive || depthVeilActive;
}

void PostProcessSettings::ResolveBloomPyramid(uint32 renderHeight)
{
    // Radius/depth mapping adapted from KinoBloom v2 by Keijiro Takahashi.
    // Copyright (c) 2015-2017 Keijiro Takahashi, MIT License.
    // See ThirdParty/KinoBloom/LICENSE.md and UPSTREAM.md.
    BloomOctaveBlend = 1.0f;
    if (renderHeight == 0)
    {
        BloomOctaves = std::clamp(BloomOctaves, 3, 8);
        BloomSampleScale = 1.0f;
        return;
    }

    const float height = static_cast<float>(renderHeight);
    const float radius = std::clamp(BloomRadius, 1.0f, 7.0f);
    const float logExtent = std::log2(height) + radius - 8.0f;
    // Fade the final octave continuously; do not allocate it for a sub-0.1%
    // fractional contribution. Remapping the fade keeps the cutoff continuous.
    constexpr float kOctaveFadeStart = 0.001f;
    const int32 ceiling = std::clamp(BloomOctaves, 3, 8);
    const float extent = std::clamp(logExtent, 3.0f, static_cast<float>(ceiling));
    const float whole = std::floor(extent);
    const float fraction = extent - whole;
    BloomOctaves = static_cast<int32>(whole);
    if (fraction > kOctaveFadeStart)
    {
        ++BloomOctaves;
        BloomOctaveBlend = (fraction - kOctaveFadeStart) / (1.0f - kOctaveFadeStart);
    }
    BloomSampleScale = std::clamp(std::exp2(logExtent - 3.0f), 0.5f, 1.0f);
}

bool PostProcessSettings::IsBloomLensDirtActive(float32 epsilon) const
{
    // Dirt is a modulation of bloom rather than an independent light source.
    // Keeping this tied to the effective bloom gate prevents allocating its
    // wide pyramid when bloom cannot contribute to the composite.
    return IsBloomHighlightsActive(epsilon) && BloomLensDirtEnabled != 0 && BloomLensDirtIntensity > epsilon;
}

bool PostProcessSettings::IsFogGlowActive(float32 epsilon) const
{
    return FogGlowEnabled != 0 && FogGlowIntensity > epsilon &&
           (IsVolumetricFogActive(epsilon) || IsHeightFogActive(epsilon));
}

bool PostProcessSettings::IsFogGlowFastActive(float32 epsilon) const
{
    return IsFogGlowActive(epsilon) && FogGlowQuality == 0;
}

bool PostProcessSettings::IsFogGlowPyramidActive(float32 epsilon) const
{
    return IsFogGlowActive(epsilon) && FogGlowQuality != 0;
}

void PostProcessSettings::ResolveFogGlowPyramid(uint32 renderHeight)
{
    // The continuous radius/depth mapping is adapted from KinoBloom v2 by
    // Keijiro Takahashi. Copyright (c) 2015-2017, MIT License.
    // See ThirdParty/KinoBloom/LICENSE.md and UPSTREAM.md.
    const int32 quality = std::clamp(FogGlowQuality, 0, 3);
    const int32 qualityCeiling = quality == 1 ? 4 : quality == 2 ? 6 : quality == 3 ? 8 : 3;
    // FogGlowOctaves is derived from quality here (unlike BloomOctaves, whose
    // authored value IS read as its own ceiling). Its blend line is kept only
    // because the field stays reflected + serialized — not because this resolve
    // reads it. FogGlowSampleScale, being neither, is the one that was demoted.
    FogGlowOctaves = qualityCeiling;

    if (renderHeight == 0)
    {
        FogGlowSampleScale = 1.0f;
        return;
    }

    const float height = static_cast<float>(renderHeight);
    const float radius = std::clamp(FogGlowRadius, 1.0f, 7.0f);
    const float logExtent = std::log2(height) + radius - 8.0f;
    const float whole = std::floor(logExtent);
    const int32 requestedOctaves = static_cast<int32>(whole);
    FogGlowOctaves = std::clamp(requestedOctaves, 3, qualityCeiling);
    if (requestedOctaves < 3)
        FogGlowSampleScale = 0.5f;
    else if (requestedOctaves > qualityCeiling)
        FogGlowSampleScale = 1.5f;
    else
        FogGlowSampleScale = 0.5f + (logExtent - whole);
}

void PostProcessSettings::ResolveDerivedForRenderHeight(uint32 renderHeight)
{
    // One entry point for the per-view derived push fields so a generic node
    // never spells out which effects own a render-height-dependent pyramid.
    ResolveBloomPyramid(renderHeight);
    ResolveFogGlowPyramid(renderHeight);
}

bool PostProcessSettings::IsHalationActive(float32 epsilon) const
{
    return HalationIntensity > epsilon && HalationRadius > epsilon;
}

bool PostProcessSettings::IsChromaticAberrationActive(float32 epsilon) const
{
    return ChromaticAberrationIntensity > epsilon || ChromaticAberrationLongitudinal > epsilon ||
           ChromaticAberrationComa > epsilon;
}

bool PostProcessSettings::IsBloomChainActive(float32 epsilon) const
{
    // Film halation has its own highlight extraction and blur chain.
    return IsBloomActive(epsilon);
}

bool PostProcessSettings::IsFilmSimulationGrainActive(float32 epsilon) const
{
    return FilmSimulationGrainIntensity > epsilon && FilmSimulationGrainSize > epsilon &&
           (FilmSimulationGrainMode == 0 || FilmSimulationGrainDensity > epsilon);
}

bool PostProcessSettings::IsFilmSimulationArtifactsActive(float32 epsilon) const
{
    return (FilmSimulationHairEnabled != 0 && FilmSimulationHairAmount > epsilon &&
            FilmSimulationHairIntensity > epsilon) ||
           (FilmSimulationScratchesEnabled != 0 && FilmSimulationScratchAmount > epsilon &&
            FilmSimulationScratchIntensity > epsilon) ||
           (FilmSimulationDustEnabled != 0 && FilmSimulationDustAmount > epsilon &&
            FilmSimulationDustIntensity > epsilon) ||
           (FilmSimulationGateWeaveEnabled != 0 &&
            (FilmSimulationGateWeaveHorizontal > epsilon || FilmSimulationGateWeaveVertical > epsilon ||
             FilmSimulationGateWeaveRotation > epsilon)) ||
           FilmSimulationGateMask != 0;
}

bool PostProcessSettings::IsAOActive(float32 epsilon) const
{
    return AOIntensity > epsilon;
}

// Gates the six-pass chain AND the Forward+ world pass, which pays two extra
// full-res MRT slices only when this is true — so every term is a precondition
// for a ray that could contribute: no strength, no reach, no march budget.
bool PostProcessSettings::IsSSSRActive(float32 epsilon) const
{
    return SSSRIntensity > epsilon && SSSRMaxDistance > epsilon && SSSRMaxSteps > 0;
}

bool PostProcessSettings::IsVhsActive(float32 epsilon) const
{
    return VhsIntensity > epsilon;
}

bool PostProcessSettings::IsColorFilterActive(float32 epsilon) const
{
    if (ColorFilterIntensity <= epsilon)
        return false;

    switch (ColorFilterBlendMode)
    {
    case 0: // Multiply: white is neutral.
        return !Near(ColorFilterR, 1.0f, epsilon)
            || !Near(ColorFilterG, 1.0f, epsilon)
            || !Near(ColorFilterB, 1.0f, epsilon);
    case 1: // Add: black is neutral.
    case 2: // Screen: black is neutral.
        return !Near(ColorFilterR, 0.0f, epsilon)
            || !Near(ColorFilterG, 0.0f, epsilon)
            || !Near(ColorFilterB, 0.0f, epsilon);
    case 3: // SoftLight: middle gray is neutral.
        return !Near(ColorFilterR, 0.5f, epsilon)
            || !Near(ColorFilterG, 0.5f, epsilon)
            || !Near(ColorFilterB, 0.5f, epsilon);
    default:
        return true;
    }
}

bool PostProcessSettings::IsColorGradeActive(float32 epsilon) const
{
    // The log working-space flag alone is not a grade, and neither are the band
    // limits (weights multiply zero offsets) — band offsets, the global
    // contrast/saturation/hue, and white balance activate the pass.
    return !Near(ColorGradeShadowsR, 0.0f, epsilon)
        || !Near(ColorGradeShadowsG, 0.0f, epsilon)
        || !Near(ColorGradeShadowsB, 0.0f, epsilon)
        || !Near(ColorGradeShadowsMaster, 0.0f, epsilon)
        || !Near(ColorGradeMidtonesR, 0.0f, epsilon)
        || !Near(ColorGradeMidtonesG, 0.0f, epsilon)
        || !Near(ColorGradeMidtonesB, 0.0f, epsilon)
        || !Near(ColorGradeMidtonesMaster, 0.0f, epsilon)
        || !Near(ColorGradeHighlightsR, 0.0f, epsilon)
        || !Near(ColorGradeHighlightsG, 0.0f, epsilon)
        || !Near(ColorGradeHighlightsB, 0.0f, epsilon)
        || !Near(ColorGradeHighlightsMaster, 0.0f, epsilon)
        || !Near(ColorGradeContrast, 1.0f, epsilon)
        || !Near(ColorGradeSaturation, 1.0f, epsilon)
        || !Near(ColorGradeHueShift, 0.0f, epsilon)
        || !Near(ColorGradeTemperature, 0.0f, epsilon)
        || !Near(ColorGradeTint, 0.0f, epsilon);
}

bool PostProcessSettings::IsHdrColorFxActive(float32 epsilon) const
{
    return IsColorFilterActive(epsilon) || IsColorGradeActive(epsilon);
}

bool PostProcessSettings::IsCasActive(float32 epsilon) const
{
    return CasStrength > epsilon;
}

bool PostProcessSettings::IsLutActive(float32 epsilon) const
{
    return LutIntensity > epsilon && (LutAssetGuidWords[0] != 0 || LutAssetGuidWords[1] != 0);
}

bool PostProcessSettings::IsVignetteActive(float32 epsilon) const
{
    return VignetteIntensity > epsilon;
}

bool PostProcessSettings::IsLdrStackActive(float32 epsilon) const
{
    return IsCasActive(epsilon) || IsLutActive(epsilon) || IsVignetteActive(epsilon);
}

bool PostProcessSettings::IsCrtActive(float32 epsilon) const
{
    return CrtIntensity > epsilon;
}

bool PostProcessSettings::IsDofActive(float32 epsilon) const
{
    // The debug overlay keeps the pass alive even at zero intensity.
    return (DofIntensity > epsilon && DofMaxRadius > epsilon) || DofDebugMode != 0;
}

bool PostProcessSettings::IsFastBlurActive(float32 epsilon) const
{
    return FastBlurIntensity > epsilon && FastBlurMaxRadius > epsilon && FastBlurFocusRange > epsilon;
}

bool PostProcessSettings::IsHeatDistortionActive(float32 epsilon) const
{
    return HeatDistortionStrength > epsilon
        && HeatDistortionMaskStrength > epsilon
        && HeatDistortionScale > epsilon
        && HeatDistortionDistanceEnd > HeatDistortionDistanceStart + epsilon;
}

bool PostProcessSettings::IsHeightFogActive(float32 epsilon) const
{
    const bool distanceActive = HeightFogDistanceFogEnabled != 0 && HeightFogSmoothLength > epsilon;
    const bool heightActive = HeightFogHeightFogEnabled != 0 && HeightFogTransitionLength > epsilon;
    const bool skyActive = HeightFogSkyEnabled != 0;
    return HeightFogIntensity > epsilon && HeightFogDensity > epsilon && (distanceActive || heightActive || skyActive);
}

bool PostProcessSettings::IsVolumetricFogActive(float32 epsilon) const
{
    return VolumetricFogIntensity > epsilon && VolumetricFogDensity > epsilon;
}

bool PostProcessSettings::IsAtmosphericCloudActive(float32 epsilon) const
{
    return AtmosphericCloudSkyFill > epsilon && AtmosphericCloudVaporMass > epsilon &&
           AtmosphericCloudOpacity > epsilon;
}

bool PostProcessSettings::IsVolumetricCloudsActive(float32 epsilon) const
{
    return CloudsDensityMultiplier > epsilon && CloudsRadius > epsilon &&
           CloudsThickness > epsilon;
}

// Volume-core names (authored on the PostProcessVolume itself) plus the shared
// post-fog glow group (authored by BOTH fog effects — no single owning effect
// registration). Everything an individual effect authors lives in that effect's
// EffectSettingsField table in PostProcessEffectDescriptors.cpp; the write/read
// ladders and ReadableFieldNames() consume registry + this appendix, so the two
// directions cannot drift.
#define PP_CORE_SHARED_FIELDS(X)                \
    X("exposure", Exposure)                     \
    X("tonemapMode", TonemapMode)               \
    X("ditherMode", DitherMode)                 \
    X("ictcpChromaCompression", IctcpChromaCompression) \
    X("fogGlowIntensity", FogGlowIntensity)     \
    X("fogGlowQuality", FogGlowQuality)         \
    X("fogGlowThreshold", FogGlowThreshold)     \
    X("fogGlowKnee", FogGlowKnee)               \
    X("fogGlowAntiFlicker", FogGlowAntiFlicker) \
    X("fogGlowOctaves", FogGlowOctaves)         \
    X("fogGlowScatter", FogGlowScatter)         \
    X("fogGlowFadeStart", FogGlowFadeStart)     \
    X("fogGlowFadeEnd", FogGlowFadeEnd)         \
    X("fogGlowTintR", FogGlowTintR)             \
    X("fogGlowTintG", FogGlowTintG)             \
    X("fogGlowTintB", FogGlowTintB)

namespace
{
// Read one registry-described settings member out of the resolved struct.
// The tables are built with offsetof + a kind derived from the member type
// (PP_SETTING), so the memcpy reads exactly the declared member.
float ReadRegistrySettingsValue(const PostProcessSettings& s,
                                const ::GameEngine::Rendering::EffectSettingsField& field)
{
    const auto* base = reinterpret_cast<const unsigned char*>(&s);
    if (field.Type == ::GameEngine::Rendering::EffectSettingsField::Kind::Int32)
    {
        int32 v = 0;
        std::memcpy(&v, base + field.Offset, sizeof(v));
        return static_cast<float>(v);
    }
    float32 v = 0.0f;
    std::memcpy(&v, base + field.Offset, sizeof(v));
    return v;
}

template <int32 Octave>
bool IsFogGlowOctaveActive(const PostProcessSettings& s)
{
    return s.IsFogGlowPyramidActive() && s.FogGlowOctaves >= Octave;
}

// Gates no single effect registration decides: they combine several effects
// (both fog effects, the HDR and LDR colour stacks) or read the volume core. Every single-effect gate is declared in
// that effect's registration (PostProcessEffectDescriptors.cpp).
using ::GameEngine::Rendering::EffectSettingsGate;
constexpr EffectSettingsGate kCoreGates[]{
    {"fogGlowActive", [](const PostProcessSettings& s) { return s.IsFogGlowActive(); }},
    {"fogGlowFastActive", [](const PostProcessSettings& s) { return s.IsFogGlowFastActive(); }},
    {"fogGlowPyramidActive", [](const PostProcessSettings& s) { return s.IsFogGlowPyramidActive(); }},
    {"fogGlowOctave4Active", &IsFogGlowOctaveActive<4>},
    {"fogGlowOctave5Active", &IsFogGlowOctaveActive<5>},
    {"fogGlowOctave6Active", &IsFogGlowOctaveActive<6>},
    {"fogGlowOctave7Active", &IsFogGlowOctaveActive<7>},
    {"fogGlowOctave8Active", &IsFogGlowOctaveActive<8>},
    // Reads the computed gate; TryWriteField pushes the raw AutoExposureActive
    // flag under the same name.
    {"useAutoExposure", [](const PostProcessSettings& s) { return s.IsAutoExposureActive(); }},
    {"hdrColorFxActive", [](const PostProcessSettings& s) { return s.IsHdrColorFxActive(); }},
    {"ldrStackActive", [](const PostProcessSettings& s) { return s.IsLdrStackActive(); }},
};

const EffectSettingsGate* FindGate(const std::string& name)
{
    for (const EffectSettingsGate& gate : kCoreGates)
    {
        if (gate.ShaderName == name)
            return &gate;
    }
    return ::GameEngine::Rendering::PostProcessEffectRegistry::FindGate(name);
}
} // namespace

bool PostProcessSettings::TryWriteField(
    const std::string& name,
    GameEngine::Rendering::NamedPushConstantWriter& pcw) const
{
    // BloomCombine mixes the scattering pyramid in only when the scattering
    // passes ran, so the pushed amount follows the same predicate as their gates.
    if (name == "bloomScatteringAmount")
        return pcw.Add(name, IsBloomScatteringActive() ? BloomScatteringAmount : 0.0f);

    // Volume-core + shared fog-glow members (source of truth: the appendix
    // macro above).
#define PP_WRITE(jsonName, member) if (name == jsonName) return pcw.Add(name, member);
    PP_CORE_SHARED_FIELDS(PP_WRITE)
#undef PP_WRITE

    // Per-effect simple members (source of truth: the EffectSettingsField
    // tables in PostProcessEffectDescriptors.cpp). Adding a new shared PP
    // parameter: add the field to the struct + one table line in the effect's
    // registration.
    if (const auto* field = ::GameEngine::Rendering::PostProcessEffectRegistry::FindSettingsField(name))
    {
        const auto* base = reinterpret_cast<const unsigned char*>(this);
        if (field->Type == ::GameEngine::Rendering::EffectSettingsField::Kind::Int32)
        {
            int32 v = 0;
            std::memcpy(&v, base + field->Offset, sizeof(v));
            return pcw.Add(name, v);
        }
        float32 v = 0.0f;
        std::memcpy(&v, base + field->Offset, sizeof(v));
        return pcw.Add(name, v);
    }

    // Write-only appendix (no TryReadField counterpart). The two pyramid sample
    // scales are per-view derived outputs (resolved from render height by
    // Resolve{Bloom,FogGlow}Pyramid) — pushed to the shader but never blended or
    // gated on, so they belong here rather than in PP_SHARED_FIELDS. AO.
    // Auto-exposure controls: minExposure/maxExposure invert the EV range to the
    // linear clamp (higher EV -> smaller scale); useAutoExposure flags the
    // tonemap to read the history SSBO. Plus the height-fog authoring group.
    // AutoExposureBiasEv scales the metering target AND both clamp bounds by the same factor —
    // clamp(key*b/avg, lo*b, hi*b) == b*clamp(key/avg, lo, hi) — i.e. exactly a post-clamp
    // exposure bias, without touching the resolve shader's interface. The log-space adaptation
    // smoothing is unaffected (a constant log2 offset on both endpoints).
    if (name == "bloomOctaveBlend")   return pcw.Add(name, BloomOctaveBlend);
    if (name == "bloomSampleScale")   return pcw.Add(name, BloomSampleScale);
    if (name == "fogGlowSampleScale") return pcw.Add(name, FogGlowSampleScale);
    if (name == "aoIntensity")   return pcw.Add(name, AOIntensity);
    if (name == "aoRadius")      return pcw.Add(name, AORadius);
    if (name == "aoThickness")   return pcw.Add(name, AOThickness);
    if (name == "speedUp")       return pcw.Add(name, AutoExposureSpeedUp);
    if (name == "speedDown")     return pcw.Add(name, AutoExposureSpeedDown);
    if (name == "exposureKey")   return pcw.Add(name, ::GameEngine::Rendering::kAutoExposureMiddleGrey * std::exp2(AutoExposureBiasEv));
    if (name == "minExposure")   return pcw.Add(name, ::GameEngine::Rendering::EvToLinearExposure(AutoExposureMaxEv) * std::exp2(AutoExposureBiasEv));
    if (name == "maxExposure")   return pcw.Add(name, ::GameEngine::Rendering::EvToLinearExposure(AutoExposureMinEv) * std::exp2(AutoExposureBiasEv));
    if (name == "useAutoExposure") return pcw.Add(name, AutoExposureActive ? 1 : 0);
    if (name == "heightFogEmissiveR")    return pcw.Add(name, HeightFogEmissiveR);
    if (name == "heightFogEmissiveG")    return pcw.Add(name, HeightFogEmissiveG);
    if (name == "heightFogEmissiveB")    return pcw.Add(name, HeightFogEmissiveB);
    if (name == "heightFogSunDirX")      return pcw.Add(name, HeightFogSunDirX);
    if (name == "heightFogSunDirY")      return pcw.Add(name, HeightFogSunDirY);
    if (name == "heightFogSunDirZ")      return pcw.Add(name, HeightFogSunDirZ);
    if (name == "heightFogSunColorR")    return pcw.Add(name, HeightFogSunColorR);
    if (name == "heightFogSunColorG")    return pcw.Add(name, HeightFogSunColorG);
    if (name == "heightFogSunColorB")    return pcw.Add(name, HeightFogSunColorB);
    if (name == "heightFogPhase")        return pcw.Add(name, HeightFogPhase);
    if (name == "heightFogPhaseWeight0") return pcw.Add(name, HeightFogPhaseWeight0);
    if (name == "heightFogPhaseWeight1") return pcw.Add(name, HeightFogPhaseWeight1);
    if (name == "heightFogSkyPower")     return pcw.Add(name, HeightFogSkyPower);
    if (name == "heightFogSkyFillStart") return pcw.Add(name, HeightFogSkyFillStart);
    if (name == "heightFogSkyFillEnd")   return pcw.Add(name, HeightFogSkyFillEnd);
    return false;
}

bool PostProcessSettings::TryReadField(const std::string& name, float& out) const
{
    // Computed *Active gates (read-only): rendergraph skipWhen follows the
    // actual visual contribution of an effect, not one raw component value.
    if (const EffectSettingsGate* gate = FindGate(name))
    {
        out = AsGateValue(gate->IsActive(*this));
        return true;
    }

    // Volume-core + shared fog-glow members (source of truth: the appendix
    // macro above). Integer enum fields are widened to float — the only
    // consumer is the skipWhen near-zero test.
#define PP_READ(jsonName, member) if (name == jsonName) { out = static_cast<float>(member); return true; }
    PP_CORE_SHARED_FIELDS(PP_READ)
#undef PP_READ

    // Per-effect simple members (source of truth: the EffectSettingsField
    // tables in PostProcessEffectDescriptors.cpp).
    if (const auto* field = ::GameEngine::Rendering::PostProcessEffectRegistry::FindSettingsField(name))
    {
        out = ReadRegistrySettingsValue(*this, *field);
        return true;
    }

    // Read-only aliases (a second name for a shared member).
    if (name == "bloomIntensity")         { out = BloomIntensity; return true; }
    if (name == "volumetricFogIntensity") { out = VolumetricFogIntensity; return true; }
    return false;
}

bool PostProcessSettings::IsGateName(const std::string& name)
{
    return FindGate(name) != nullptr;
}

const std::vector<std::string>& PostProcessSettings::ReadableFieldNames()
{
    static const std::vector<std::string> kNames = []
    {
        std::vector<std::string> names;
        for (const EffectSettingsGate& gate : kCoreGates)
            names.emplace_back(gate.ShaderName);
#define PP_NAME(jsonName, member) names.push_back(jsonName);
        PP_CORE_SHARED_FIELDS(PP_NAME)
#undef PP_NAME
        // Per-effect gates and simple members from the registrations, in registration
        // order (deterministic; the consumer is set-semantics validation).
        ::GameEngine::Rendering::PostProcessEffectRegistry::ForEach(
            [&names](const ::GameEngine::Rendering::PostProcessEffectDescriptor& d)
            {
                for (const auto& gate : d.Gates)
                    names.emplace_back(gate.ShaderName);
                for (const auto& field : d.SettingsFields)
                {
                    if (!field.ShaderName.empty()) // blend-only members have no name
                        names.emplace_back(field.ShaderName);
                }
            });
        names.push_back("bloomIntensity");
        names.push_back("volumetricFogIntensity");
        return names;
    }();
    return kNames;
}

#undef PP_CORE_SHARED_FIELDS

// --- Registry-driven volume blend (PP-ARCH Phase 2) --------------------------
// BlendPostProcessSettings folds over EffectSettingsField tables: each effect's
// registration declares, per member, the blend rule (lerp / dominant / log2 /
// skip) and contributes-group. The core table below carries the volume-core and
// camera-sensor members plus the fog-glow group no single effect owns. The only
// semantics the field vocabulary deliberately does not express are the three
// escape hatches at the end of the blend (bool dominant pick, asset-GUID
// zeroing, the lens-dirt clamp).

namespace
{
using ::GameEngine::Rendering::EffectSettingsField;
using ::GameEngine::Rendering::SettingsBlendGroup;
using ::GameEngine::Rendering::SettingsBlendRule;

// Local twin of the descriptor tables' kind derivation (the tables' macro
// lives with the per-effect registrations in PostProcessEffectDescriptors.cpp).
template <class T>
consteval EffectSettingsField::Kind CoreKindFor()
{
    static_assert(std::is_same_v<T, float32> || std::is_same_v<T, int32>,
                  "blend tables carry only float32/int32 members");
    return std::is_same_v<T, float32> ? EffectSettingsField::Kind::Float32
                                      : EffectSettingsField::Kind::Int32;
}

#define PP_CORE_BLEND(Member, rule, group)                                          \
    EffectSettingsField{{},                                                         \
                        static_cast<std::uint16_t>(offsetof(PostProcessSettings, Member)), \
                        CoreKindFor<decltype(PostProcessSettings::Member)>(),       \
                        SettingsBlendRule::rule, SettingsBlendGroup::group}

constexpr EffectSettingsField kCoreBlendFields[]{
    // Exposure blends in log/stop space (geometric) so a volume crossfade is
    // perceptually even — a fade halfway between 1x and 4x exposure lands at 2x
    // (one stop), not the arithmetic 2.5x.
    PP_CORE_BLEND(Exposure, Log2Lerp, Always),
    PP_CORE_BLEND(TonemapMode, Dominant, Always),
    PP_CORE_BLEND(DitherMode, Dominant, Always),
    PP_CORE_BLEND(IctcpChromaCompression, Lerp, Always),
    PP_CORE_BLEND(AutoExposureMinEv, Lerp, Always),
    PP_CORE_BLEND(AutoExposureMaxEv, Lerp, Always),
    PP_CORE_BLEND(AutoExposureSpeedUp, Lerp, Always),
    PP_CORE_BLEND(AutoExposureSpeedDown, Lerp, Always),
    PP_CORE_BLEND(AutoExposureBiasEv, Lerp, Always),
    // Shared post-fog glow: authored by BOTH fog effects, so it lives here
    // rather than either registration; the group gates on either contributing.
    PP_CORE_BLEND(FogGlowEnabled, Dominant, FogGlow),
    PP_CORE_BLEND(FogGlowQuality, Dominant, FogGlow),
    PP_CORE_BLEND(FogGlowIntensity, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowRadius, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowOctaves, Dominant, FogGlow),
    // Derived pyramid output (ResolveFogGlowPyramid), re-resolved per view:
    // never blended, but it rides the group's keep-a copy.
    PP_CORE_BLEND(FogGlowSampleScale, Skip, FogGlow),
    PP_CORE_BLEND(FogGlowScatter, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowThreshold, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowKnee, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowFadeStart, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowFadeEnd, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowTintR, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowTintG, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowTintB, Lerp, FogGlow),
    PP_CORE_BLEND(FogGlowAntiFlicker, Dominant, FogGlow),
};

#undef PP_CORE_BLEND

bool GroupContributes(SettingsBlendGroup group, SettingsBlendGroupMask contributingGroups)
{
    switch (group)
    {
    case SettingsBlendGroup::FogGlow:
        return contributingGroups.Contains(SettingsBlendGroup::HeightFog) ||
               contributingGroups.Contains(SettingsBlendGroup::VolumetricFog);
    case SettingsBlendGroup::Always:
        return true;
    default:
        return contributingGroups.Contains(group);
    }
}

void ApplyBlendField(const EffectSettingsField& field, const PostProcessSettings& a,
                     const PostProcessSettings& b, float w, bool contributes,
                     PostProcessSettings& out)
{
    static_assert(sizeof(float32) == sizeof(int32));
    constexpr std::size_t kFieldSize = sizeof(float32);
    auto* dst = reinterpret_cast<unsigned char*>(&out) + field.Offset;
    const auto* srcA = reinterpret_cast<const unsigned char*>(&a) + field.Offset;
    const auto* srcB = reinterpret_cast<const unsigned char*>(&b) + field.Offset;

    if (!contributes)
    {
        // A volume that does not author this group keeps a's whole block — an
        // unrelated grading volume must not fade fog toward its defaults.
        std::memcpy(dst, srcA, kFieldSize);
        return;
    }
    switch (field.Blend)
    {
    case SettingsBlendRule::Skip:
        // Derived per view (pyramid scales) or stamped after blending (camera
        // lens): the blend output keeps the default-constructed value.
        return;
    case SettingsBlendRule::Dominant:
        std::memcpy(dst, w >= 0.5f ? srcB : srcA, kFieldSize);
        return;
    case SettingsBlendRule::Log2Lerp:
    {
        assert(field.Type == EffectSettingsField::Kind::Float32);
        float32 x = 0.0f;
        float32 y = 0.0f;
        std::memcpy(&x, srcA, kFieldSize);
        std::memcpy(&y, srcB, kFieldSize);
        const float lx = std::log2(std::max(x, 1e-6f));
        const float ly = std::log2(std::max(y, 1e-6f));
        const float32 v = std::exp2(lx + (ly - lx) * w);
        std::memcpy(dst, &v, kFieldSize);
        return;
    }
    case SettingsBlendRule::Lerp:
    default:
    {
        assert(field.Type == EffectSettingsField::Kind::Float32);
        float32 x = 0.0f;
        float32 y = 0.0f;
        std::memcpy(&x, srcA, kFieldSize);
        std::memcpy(&y, srcB, kFieldSize);
        const float32 v = x + (y - x) * w;
        std::memcpy(dst, &v, kFieldSize);
        return;
    }
    }
}
} // namespace

PostProcessSettings BlendPostProcessSettings(const PostProcessSettings& a,
                                             const PostProcessSettings& b,
                                             float w,
                                             SettingsBlendGroupMask contributingGroups)
{
    PostProcessSettings out;
    const auto apply = [&](const EffectSettingsField& field)
    {
        ApplyBlendField(field, a, b, w, GroupContributes(field.Group, contributingGroups), out);
    };
    for (const EffectSettingsField& field : kCoreBlendFields)
        apply(field);
    for (const EffectSettingsField& field :
         ::GameEngine::Rendering::PostProcessEffectRegistry::AllSettingsFields())
        apply(field);

    // Escape hatches — semantics the per-field vocabulary deliberately does not
    // express (each would be a single-use table feature):
    // 1. The lone bool member: same dominant pick as the int toggles.
    out.AutoExposureActive = (w >= 0.5f) ? b.AutoExposureActive : a.AutoExposureActive;
    // 2. Asset references never blend: extraction re-stamps the dominant
    //    volume's GUID after the weighted fold (a half-faded LUT is still THE
    //    LUT); the blend output itself carries none.
    out.BloomLensDirtAssetGuidWords[0] = 0;
    out.BloomLensDirtAssetGuidWords[1] = 0;
    out.LutAssetGuidWords[0] = 0;
    out.LutAssetGuidWords[1] = 0;
    // 3. The shipped ladder clamped this one lerp output; preserved verbatim.
    out.BloomLensDirtIntensity = std::clamp(out.BloomLensDirtIntensity, 0.0f, 10.0f);
    return out;
}

void CopySettingsGroup(PostProcessSettings& dst, const PostProcessSettings& src,
                       ::GameEngine::Rendering::SettingsBlendGroup group)
{
    const auto copy = [&](const EffectSettingsField& field)
    {
        if (field.Group != group)
            return;
        std::memcpy(reinterpret_cast<unsigned char*>(&dst) + field.Offset,
                    reinterpret_cast<const unsigned char*>(&src) + field.Offset,
                    sizeof(float32));
    };
    for (const EffectSettingsField& field : kCoreBlendFields)
        copy(field);
    for (const EffectSettingsField& field :
         ::GameEngine::Rendering::PostProcessEffectRegistry::AllSettingsFields())
        copy(field);
}

VolumetricFogSettings ToVolumetricFogSettings(const PostProcessSettings& settings)
{
    VolumetricFogSettings fog{};
    fog.enabled = settings.IsVolumetricFogActive(0.0001f);
    fog.isGlobal = settings.VolumetricFogIsGlobal != 0;
    fog.localVolumeValid = settings.VolumetricFogVolumeValid != 0;
    fog.localVolumeShape = settings.VolumetricFogVolumeShape;
    fog.localVolumeBlendDistance = std::max(settings.VolumetricFogVolumeBlendDistance, 0.0f);
    fog.localVolumeCenter[0] = settings.VolumetricFogVolumeCenterX;
    fog.localVolumeCenter[1] = settings.VolumetricFogVolumeCenterY;
    fog.localVolumeCenter[2] = settings.VolumetricFogVolumeCenterZ;
    fog.localVolumeAxisX[0] = settings.VolumetricFogVolumeAxisXX;
    fog.localVolumeAxisX[1] = settings.VolumetricFogVolumeAxisXY;
    fog.localVolumeAxisX[2] = settings.VolumetricFogVolumeAxisXZ;
    fog.localVolumeAxisY[0] = settings.VolumetricFogVolumeAxisYX;
    fog.localVolumeAxisY[1] = settings.VolumetricFogVolumeAxisYY;
    fog.localVolumeAxisY[2] = settings.VolumetricFogVolumeAxisYZ;
    fog.localVolumeAxisZ[0] = settings.VolumetricFogVolumeAxisZX;
    fog.localVolumeAxisZ[1] = settings.VolumetricFogVolumeAxisZY;
    fog.localVolumeAxisZ[2] = settings.VolumetricFogVolumeAxisZZ;
    fog.localVolumeHalfExtents[0] = std::max(settings.VolumetricFogVolumeHalfExtentX, 0.001f);
    fog.localVolumeHalfExtents[1] = std::max(settings.VolumetricFogVolumeHalfExtentY, 0.001f);
    fog.localVolumeHalfExtents[2] = std::max(settings.VolumetricFogVolumeHalfExtentZ, 0.001f);
    fog.maxDistance = std::max(settings.VolumetricFogMaxDistance, 0.01f);
    fog.xyCellSizePixels = static_cast<uint32_t>(std::max(settings.VolumetricFogXYCellSizePixels, 1));
    fog.zSliceCount = static_cast<uint32_t>(std::max(settings.VolumetricFogZSliceCount, 1));
    fog.depthDistribution = std::max(settings.VolumetricFogDepthDistribution, 0.05f);
    fog.density = std::max(settings.VolumetricFogDensity, 0.0f) * std::max(settings.VolumetricFogIntensity, 0.0f);
    fog.baseHeight = settings.VolumetricFogBaseHeight;
    fog.heightFalloff = std::max(settings.VolumetricFogHeightFalloff, 0.01f);
    fog.skyFade = std::clamp(settings.VolumetricFogSkyFade, 0.0f, 1.0f);
    fog.albedo[0] = std::clamp(settings.VolumetricFogAlbedoR, 0.0f, 8.0f);
    fog.albedo[1] = std::clamp(settings.VolumetricFogAlbedoG, 0.0f, 8.0f);
    fog.albedo[2] = std::clamp(settings.VolumetricFogAlbedoB, 0.0f, 8.0f);
    fog.emission[0] = std::max(settings.VolumetricFogEmissionR, 0.0f);
    fog.emission[1] = std::max(settings.VolumetricFogEmissionG, 0.0f);
    fog.emission[2] = std::max(settings.VolumetricFogEmissionB, 0.0f);
    fog.anisotropy = std::clamp(settings.VolumetricFogAnisotropy, -0.95f, 0.95f);
    fog.trackDirectionalLight = settings.VolumetricFogTrackDirectionalLight != 0;
    fog.sunIntensityScale = std::max(settings.VolumetricFogSunIntensityScale, 0.0f);
    fog.sunScatteringTint[0] = std::max(settings.VolumetricFogSunTintR, 0.0f);
    fog.sunScatteringTint[1] = std::max(settings.VolumetricFogSunTintG, 0.0f);
    fog.sunScatteringTint[2] = std::max(settings.VolumetricFogSunTintB, 0.0f);
    fog.ambientScatteringTint[0] = std::max(settings.VolumetricFogAmbientTintR, 0.0f);
    fog.ambientScatteringTint[1] = std::max(settings.VolumetricFogAmbientTintG, 0.0f);
    fog.ambientScatteringTint[2] = std::max(settings.VolumetricFogAmbientTintB, 0.0f);
    fog.noiseEnabled = settings.VolumetricFogNoiseEnabled != 0;
    fog.noiseScale = std::max(settings.VolumetricFogNoiseScale, 0.01f);
    fog.noiseStrength = std::clamp(settings.VolumetricFogNoiseStrength, 0.0f, 1.0f);
    fog.noiseVelocity[0] = settings.VolumetricFogNoiseVelocityX;
    fog.noiseVelocity[1] = settings.VolumetricFogNoiseVelocityY;
    fog.noiseVelocity[2] = settings.VolumetricFogNoiseVelocityZ;
    fog.noiseContrast = std::max(settings.VolumetricFogNoiseContrast, 0.01f);
    fog.noiseChannelWeights[0] = std::max(settings.VolumetricFogNoiseChannelWeightR, 0.0f);
    fog.noiseChannelWeights[1] = std::max(settings.VolumetricFogNoiseChannelWeightG, 0.0f);
    fog.noiseChannelWeights[2] = std::max(settings.VolumetricFogNoiseChannelWeightB, 0.0f);
    fog.noiseChannelWeights[3] = std::max(settings.VolumetricFogNoiseChannelWeightA, 0.0f);
    fog.densityThreshold = std::clamp(settings.VolumetricFogDensityThreshold, 0.0f, 1.0f);
    fog.densityThresholdSoftness = std::max(settings.VolumetricFogDensityThresholdSoftness, 0.0001f);
    fog.temporalEnabled = settings.VolumetricFogTemporalEnabled != 0;
    fog.temporalBlend = std::clamp(settings.VolumetricFogTemporalBlend, 0.0f, 0.99f);
    fog.jitterStrength = std::clamp(settings.VolumetricFogJitterStrength, 0.0f, 1.0f);
    fog.jitterMotion = settings.VolumetricFogJitterMotion != 0;
    fog.compositeDepthBias = settings.VolumetricFogCompositeDepthBias;
    fog.shadowBias = settings.VolumetricFogShadowBias;
    return fog;
}

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
