#pragma once

#include "Types/Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine { namespace Rendering {
class NamedPushConstantWriter;
enum class SettingsBlendGroup : std::uint8_t;
} }

namespace GameEngine {
namespace Engine {
namespace Renderer {

struct VolumetricFogSettings;

// The contributes-groups (GameEngine::Rendering::SettingsBlendGroup) an incoming
// volume authors when BlendPostProcessSettings folds it over the base. A group
// the volume does not author keeps the base's value for every member of the
// group; SettingsBlendGroup::Always members always blend, and the shared FogGlow
// block blends when either fog group does.
class SettingsBlendGroupMask
{
  public:
    static constexpr SettingsBlendGroupMask All() { return SettingsBlendGroupMask{~0u}; }
    static constexpr SettingsBlendGroupMask None() { return SettingsBlendGroupMask{0u}; }

    constexpr void Set(GameEngine::Rendering::SettingsBlendGroup group, bool contributes)
    {
        if (contributes)
            m_Bits |= Bit(group);
        else
            m_Bits &= ~Bit(group);
    }
    constexpr bool Contains(GameEngine::Rendering::SettingsBlendGroup group) const
    {
        return (m_Bits & Bit(group)) != 0u;
    }

  private:
    explicit constexpr SettingsBlendGroupMask(std::uint32_t bits) : m_Bits(bits) {}
    static constexpr std::uint32_t Bit(GameEngine::Rendering::SettingsBlendGroup group)
    {
        return 1u << static_cast<std::uint32_t>(group);
    }

    std::uint32_t m_Bits;
};

// "No narrowing" identity endpoints for the volume exposure clamps, at the edges of the
// authorable EV100 range (physical scenes span roughly -5..20): intersecting with them
// never narrows a camera envelope, and a partially-weighted volume's clamp lerps from
// here toward its authored value, engaging proportionally within authorable EVs.
inline constexpr float32 kExposureClampNoOpMinEv = -5.0f;
inline constexpr float32 kExposureClampNoOpMaxEv = 20.0f;

// Default auto-exposure bias, in stops. The engine's display-brightness anchor for auto-metered
// views: the tonemap operators are spec-reference (the ACES Hill fit places exposed 0.18 at
// display-linear ~0.106, darker than a photographic print), so the default look is anchored HERE,
// as an explicit exposure lever, never as a scale hidden inside an operator. 0.0 ships the
// reference-photographic metering (an 18% card meters to exposed 0.18, tonemapped by the untouched
// reference curve); +0.85 ~ log2(1.8) brightens auto-metered views by a factor of 1.8 before the
// tonemap. The default is 0.0; choosing the shipped value is tracked in #2935.
// Applies to Auto metering only — Manual/Physical/Fixed are absolute photographic exposures and take no
// hidden trim.
inline constexpr float32 kDefaultAutoExposureBiasEv = 0.0f;

// Resolved per-frame post-process state after blending all active PostProcessVolume entities.
// TonemapMode stored as int32 to avoid cross-namespace include in this header;
// values match Components::TonemapMode enum (0=ACES, 1=Reinhard, 2=AgX, 3=Filmic,
// 4=Neutral, 5=Linear, 6=Gran Turismo 7, 7=ACES 2).
struct PostProcessSettings
{
    float32 Exposure       = 1.0f;
    float32 BloomThreshold = 1.0f;
    float32 BloomKnee      = 0.1f;
    int32   BloomAntiFlicker = 1;
    // Off until a PostProcessVolume stack (BloomEffect on the same entity) sets non-zero values.
    float32 BloomIntensity = 0.0f;
    float32 BloomScatteringAmount = 0.0f;
    float32 BloomTintR     = 1.0f;
    float32 BloomTintG     = 1.0f;
    float32 BloomTintB     = 1.0f;
    float32 BloomRadius    = 2.5f;
    int32   BloomOctaves   = 3;
    // Per-view reconstruction scale derived from render height and Radius.
    float32 BloomSampleScale = 1.0f;
    float32 BloomOctaveBlend = 1.0f; // contribution of the last active octave
    // Bias between narrow and broad independently filtered pyramid levels.
    float32 BloomScatter   = 0.5f;
    int32 BloomDepthVeilEnabled = 0;
    float32 BloomDepthVeilIntensity = 1.0f;
    float32 BloomDepthVeilStart = 25.0f;
    float32 BloomDepthVeilEnd = 500.0f;
    float32 BloomDepthVeilTintR = 1.0f;
    float32 BloomDepthVeilTintG = 1.0f;
    float32 BloomDepthVeilTintB = 1.0f;
    int32 BloomLensDirtEnabled = 0;
    int32 BloomLensDirtVignette = 0;
    float32 BloomLensDirtVignetteIntensity = 1.0f;
    float32 BloomLensDirtVignetteRadius = 0.25f;
    float32 BloomLensDirtVignetteSmoothness = 0.2f;
    int32 BloomLensDirtVignetteRounded = 0;
    float32 BloomLensDirtVignetteColorR = 1.0f;
    float32 BloomLensDirtVignetteColorG = 1.0f;
    float32 BloomLensDirtVignetteColorB = 1.0f;
    float32 BloomLensDirtIntensity = 0.0f;
    float32 BloomLensDirtScatter = 0.5f;
    uint64 BloomLensDirtAssetGuidWords[2]{0, 0};
    float32 HalationIntensity = 0.0f;
    float32 HalationRadius = 1.0f;
    float32 HalationTintR = 1.0f;
    float32 HalationTintG = 0.12f;
    float32 HalationTintB = 0.025f;
    // Default to Khronos PBR Neutral: tonemapping is a baseline of the pipeline,
    // so volume-less worlds are tonemapped by default. A PostProcessVolume
    // overrides this. The always-present Tonemap pass's skipWhen still skips it
    // only when something EXPLICITLY selects Linear (mode 5) + exposure 1 +
    // dither 0.
    int32   TonemapMode    = 4; // Components::TonemapMode as int32 (4 = Neutral)
    int32   DitherMode     = 0;
    // HDR10/HDR10+ highlight-chroma compression in ICtCp. The tonemap node
    // forces this to zero for SDR, HLG, and scRGB. Zero is exact pass-through.
    float32 IctcpChromaCompression = 0.0f;

    // Radial red/blue separation applied in the LDR post-process stack.
    float32 ChromaticAberrationIntensity = 0.0f;
    float32 ChromaticAberrationStartOffset = 0.25f;
    float32 ChromaticAberrationSaturation = 1.0f;

    float32 FilmSimulationFrameRate = 24.0f;
    int32 FilmSimulationGrainMode = 0;
    float32 FilmSimulationGrainIntensity = 0.0f;
    float32 FilmSimulationGrainSize = 1.0f;
    int32 FilmSimulationGrainSmooth = 0;
    float32 FilmSimulationGrainDensity = 1.0f;
    float32 FilmSimulationGrainShadowResponse = 1.0f;
    float32 FilmSimulationGrainMidtoneResponse = 1.0f;
    float32 FilmSimulationGrainHighlightResponse = 0.35f;
    int32 FilmSimulationGrainColored = 0;

    // Mechanical and surface artifacts applied as a final LDR film pass.
    int32 FilmSimulationHairEnabled = 0;
    float32 FilmSimulationHairAmount = 0.2f;
    float32 FilmSimulationHairIntensity = 0.3f;
    float32 FilmSimulationHairWidth = 1.25f;
    float32 FilmSimulationHairLength = 220.0f;
    float32 FilmSimulationHairRandomSize = 1.0f;
    float32 FilmSimulationHairCurl = 0.25f;
    float32 FilmSimulationHairCurlRandomness = 1.0f;
    int32 FilmSimulationScratchesEnabled = 0;
    float32 FilmSimulationScratchAmount = 0.2f;
    float32 FilmSimulationScratchIntensity = 0.35f;
    float32 FilmSimulationScratchWidth = 0.8f;
    float32 FilmSimulationScratchLength = 0.65f;
    float32 ChromaticAberrationLongitudinal = 0.0f;
    float32 ChromaticAberrationComa = 0.0f;

    // Physical depth of field. Intensity/MaxRadius come from the volume's
    // DepthOfFieldEffect; FocusDistance/FocalLengthMm/Aperture are stamped
    // per-view from the camera after volume blending (the lens, like exposure,
    // is a camera property — see applyCameraExposure).
    float32 DofIntensity = 0.0f;
    float32 DofMaxRadius = 16.0f;
    int32 DofSamplingQuality = 1;
    int32 DofDebugMode = 0;
    float32 DofDebugAlpha = 0.5f;
    float32 DofFocusDistance = 10.0f;
    float32 DofFocalLengthMm = 20.78f;
    float32 DofAperture = 16.0f;
    float32 DofSensorHeightMm = 24.0f;
    int32 DofApertureBladeCount = 7;
    float32 DofApertureRoundness = 1.0f;
    float32 DofApertureRotation = 0.0f;
    float32 DofAnamorphicSqueeze = 1.0f;
    int32 FilmSimulationDustEnabled = 0;
    float32 FilmSimulationDustAmount = 0.3f;
    float32 FilmSimulationDustIntensity = 0.25f;
    float32 FilmSimulationDustSize = 3.0f;
    float32 FilmSimulationDustRandomSize = 1.0f;
    int32 FilmSimulationGateWeaveEnabled = 0;
    float32 FilmSimulationGateWeaveHorizontal = 0.75f;
    float32 FilmSimulationGateWeaveVertical = 0.5f;
    float32 FilmSimulationGateWeaveRotation = 0.08f;
    int32 FilmSimulationGateMask = 0;
    float32 FilmSimulationGateMaskFeather = 3.0f;
    float32 FilmSimulationGateMaskRoundness = 0.07f;

    // Terminal-encode deband gate in output LSBs; 0 disables the filter. The
    // deband is OPT-IN: volume-less worlds (and volumes without a DebandEffect)
    // run with it off — enabling is an explicit per-volume choice via
    // DebandEffect (whose default carries the reviewed 6-LSB gate). Keep in
    // lockstep with kOutputDebandBaselineThresholdLsb (FinalizeContract.h).
    // GE_DEBAND / GE_DEBAND_THRESHOLD env overrides beat any volume (decided
    // in ResolveOutputDebandThresholdLsb).
    float32 DebandThresholdLsb = 0.0f;

    // Color filter (per-channel multiplier mixed by ColorFilterIntensity).
    // Intensity == 0 means pass-through (effect disabled in shader).
    float32 ColorFilterR         = 1.0f;
    float32 ColorFilterG         = 1.0f;
    float32 ColorFilterB         = 1.0f;
    float32 ColorFilterIntensity = 0.0f;
    int32   ColorFilterBlendMode = 0; // Components::ColorFilterBlendMode as int32
    int32   ColorFilterStackOrder = 0;
    float32 CasStrength          = 0.0f;
    int32   CasStackOrder        = 1;

    // .cube LUT (LDR stack; intensity 0 disables sampling in shader).
    float32 LutIntensity     = 0.0f;
    int32   LutStackOrder    = 0;
    int32   LutInputEncoding = 0;
    int32   LutTextureFormat = 0; // Components::CubeLutTextureFormat as int32
    // Dominant volume's LUT asset GUID stored as raw bytes.
    uint64  LutAssetGuidWords[2]{0, 0};

    // Optical vignette (LDR stack; intensity 0 = pass-through in shader).
    float32 VignetteIntensity  = 0.0f;
    float32 VignetteSmoothness = 0.2f;
    int32   VignetteRounded    = 0;
    float32 VignetteColorR     = 0.0f;
    float32 VignetteColorG     = 0.0f;
    float32 VignetteColorB     = 0.0f;
    int32   VignetteStackOrder = 0;

    // Animated analog-tape distortion (LDR; intensity 0 = pass-through).
    float32 VhsIntensity = 0.0f;
    float32 VhsWobble = 0.45f;
    float32 VhsTracking = 0.45f;
    float32 VhsSignalGlitches = 0.0f;
    float32 VhsGlitchOffsets = 0.0f;
    float32 VhsInterference = 0.0f;
    float32 VhsFrameFeedback = 0.0f;
    float32 VhsFeedbackDecay = 0.70f;
    float32 VhsFeedbackMotionThreshold = 0.06f;
    float32 VhsFeedbackTrailLength = 3.0f;
    float32 VhsCompositeSignalMode = 0.0f;
    float32 VhsDotCrawl = 0.20f;
    float32 VhsColorBleed = 1.0f;
    float32 VhsColorBleedOffset = 4.0f;
    float32 VhsTapeNoise = 0.18f;
    float32 VhsChromaStreaks = 0.08f;
    float32 VhsPreFilterChromaStreaks = 0.0f;
    float32 VhsDropouts = 0.05f;
    float32 VhsRfDropouts = 0.0f;
    float32 VhsScanlines = 0.08f;
    float32 VhsSpeed = 1.0f;
    float32 VhsOverlayEnabled = 1.0f;
    float32 VhsOverlayColorR = 0.35f;
    float32 VhsOverlayColorG = 1.0f;
    float32 VhsOverlayColorB = 0.35f;
    float32 VhsOverlayOpacity = 0.85f;
    float32 VhsOverlaySize = 2.0f;
    float32 VhsOverlayFont = 0.0f;
    float32 VhsOverlayPositionX = 0.01957f;
    float32 VhsOverlayPositionY = 0.0f;
    float32 VhsOverlayTextLength = 3.0f;
    float32 VhsOverlayText0 = 82.0f;
    float32 VhsOverlayText1 = 69.0f;
    float32 VhsOverlayText2 = 67.0f;
    float32 VhsOverlayText3 = 0.0f;
    float32 VhsOverlayText4 = 0.0f;
    float32 VhsOverlayText5 = 0.0f;
    float32 VhsOverlayText6 = 0.0f;
    float32 VhsOverlayText7 = 0.0f;
    float32 VhsDateBurnEnabled = 0.0f;
    float32 VhsDateBurnColorR = 1.0f;
    float32 VhsDateBurnColorG = 0.95f;
    float32 VhsDateBurnColorB = 0.78f;
    float32 VhsDateBurnSize = 1.5f;
    float32 VhsDateBurnPositionX = 0.031875f;
    float32 VhsDateBurnPositionY = 0.072773f;
    float32 VhsDateBurnYear = 2018.0f;
    float32 VhsDateBurnMonth = 5.0f;
    float32 VhsDateBurnDay = 16.0f;
    float32 VhsDateBurnHour = 10.0f;
    float32 VhsDateBurnMinute = 44.0f;
    float32 VhsTransportMode = 0.0f;
    float32 VhsTransportStrength = 0.65f;

    // CRT-like post (shader pass-through when crtIntensity == 0).
    float32 CrtIntensity    = 0.0f;
    float32 CrtCurvature    = 0.0f;
    float32 CrtScanlines    = 0.12f;
    float32 CrtVignette     = 0.25f;
    float32 CrtAberration   = 0.0025f;
    float32 CrtSoftness     = 0.0f;
    // 0 = off, 1 = auto scanline-based exposure compensation.
    float32 CrtExposureCompensation = 0.0f;
    // Lottes: emulated framebuffer is textureSize.xy / divisor. Matches pixel-perfect 2D
    // when divisor == pixel-perfect scale (aligned scan rows to logical artwork pixels).
    float32 CrtEmulatedResolutionDiv = 6.0f;

    // Fast single-pass depth-of-field blur. Intensity == 0 means disabled.
    float32 FastBlurIntensity = 0.0f;
    float32 FastBlurFocusDistance = 12.0f;
    float32 FastBlurFocusRange = 6.0f;
    float32 FastBlurMaxRadius = 6.0f;
    int32 FastBlurNearBlur = 1;

    // Screen-space heat-wave refraction. Strength == 0 means disabled.
    float32 HeatDistortionStrength = 0.0f;
    float32 HeatDistortionSpeed = 1.15f;
    float32 HeatDistortionScale = 11.0f;
    float32 HeatDistortionMaskStrength = 1.0f;
    float32 HeatDistortionDistanceStart = 0.02f;
    float32 HeatDistortionDistanceEnd = 0.14f;
    float32 HeatDistortionDirectionalFalloff = 4.0f;
    int32 HeatDistortionUseAbsoluteY = 1;
    float32 HeatDistortionSoftness = 1.0f;

    // Analytical screen-space height fog. Intensity == 0 means disabled.
    float32 HeightFogIntensity = 0.0f;
    float32 HeightFogDensity = 0.0f;
    float32 HeightFogMaxOpacity = 0.85f;
    float32 HeightFogMinDistance = 0.0f;
    float32 HeightFogSmoothLength = 1.0f;
    float32 HeightFogBaseHeight = 0.0f;
    float32 HeightFogTransitionLength = 120.0f;
    float32 HeightFogEmissiveR = 0.48f;
    float32 HeightFogEmissiveG = 0.54f;
    float32 HeightFogEmissiveB = 0.60f;
    float32 HeightFogSunDirX = 0.35f;
    float32 HeightFogSunDirY = -0.65f;
    float32 HeightFogSunDirZ = 0.68f;
    float32 HeightFogSunColorR = 1.0f;
    float32 HeightFogSunColorG = 0.82f;
    float32 HeightFogSunColorB = 0.58f;
    float32 HeightFogSunIntensity = 1.0f;
    float32 HeightFogPhase = -0.5f;
    float32 HeightFogPhaseWeight0 = 1.0f;
    float32 HeightFogPhaseWeight1 = 0.0f;
    int32 HeightFogSkyEnabled = 1;
    float32 HeightFogSkyPower = 1.0f;
    float32 HeightFogSkyFillStart = 0.0f;
    float32 HeightFogSkyFillEnd = 1.0f;
    int32 HeightFogDistanceFogEnabled = 1;
    int32 HeightFogHeightFogEnabled = 1;
    float32 HeightFogMaxDistance = 1000.0f;
    int32 HeightFogLayerMode = 0;
    float32 HeightFogHorizonHeightOffset = 0.0f;
    float32 HeightFogHorizonHeightBlendStart = 0.0f;
    float32 HeightFogHorizonHeightBlendEnd = 0.0f;
    int32 HeightFogAxisMode = 0;
    float32 HeightFogAxisX = 0.0f;
    float32 HeightFogAxisY = 1.0f;
    float32 HeightFogAxisZ = 0.0f;
    int32 HeightFogGradientMode = 0;
    float32 HeightFogGradientStrength = 0.0f;
    float32 HeightFogGradientLowR = 0.48f;
    float32 HeightFogGradientLowG = 0.54f;
    float32 HeightFogGradientLowB = 0.60f;
    float32 HeightFogGradientHighR = 0.72f;
    float32 HeightFogGradientHighG = 0.78f;
    float32 HeightFogGradientHighB = 0.85f;
    int32 HeightFogTrackDirectionalLight = 1;
    float32 HeightFogSunIntensityScale = 1.0f;
    int32 HeightFogNoiseEnabled = 0;
    float32 HeightFogNoiseScale = 80.0f;
    float32 HeightFogNoiseStrength = 0.28f;
    float32 HeightFogNoiseContrast = 1.25f;
    float32 HeightFogNoiseVelX = 0.0f;
    float32 HeightFogNoiseVelY = 0.0f;
    float32 HeightFogNoiseVelZ = 0.0f;
    float32 HeightFogNoiseMin = 0.0f;
    float32 HeightFogNoiseMax = 1.0f;
    float32 HeightFogNoiseFadeStart = 0.0f;
    float32 HeightFogNoiseFadeEnd = 0.0f;
    int32 HeightFogUseTimeOfDay = 0;
    float32 HeightFogSkyHorizonOffset = 0.0f;
    float32 HeightFogSkyBottomStrength = 0.0f;

    // HDR color grade (unified three-way corrector in hdr_color_fx.frag), before
    // tonemap. Band offsets are zero-centered (0 = neutral); Contrast/Saturation
    // are 1 = unchanged; ColorGradeInLog selects the log working curve. Resolved
    // into ColorGradeParamsUBO for the shader; see ColorGradeParamsUBO.h.
    float32 ColorGradeShadowsR = 0.0f;
    float32 ColorGradeShadowsG = 0.0f;
    float32 ColorGradeShadowsB = 0.0f;
    float32 ColorGradeShadowsMaster = 0.0f;
    float32 ColorGradeMidtonesR = 0.0f;
    float32 ColorGradeMidtonesG = 0.0f;
    float32 ColorGradeMidtonesB = 0.0f;
    float32 ColorGradeMidtonesMaster = 0.0f;
    float32 ColorGradeHighlightsR = 0.0f;
    float32 ColorGradeHighlightsG = 0.0f;
    float32 ColorGradeHighlightsB = 0.0f;
    float32 ColorGradeHighlightsMaster = 0.0f;
    float32 ColorGradeContrast     = 1.0f;
    float32 ColorGradeSaturation   = 1.0f;
    // HSV hue rotation in degrees [-180, 180], applied after the corrector.
    float32 ColorGradeHueShift     = 0.0f;
    // White balance temperature/tint, URP ranges [-100, 100] around 0 = neutral.
    // Resolved to LMS gains in FillColorGradeParamsUBO (after volume blending, so
    // blended temp/tint interpolate the authored values like Unity volumes do).
    float32 ColorGradeTemperature  = 0.0f;
    float32 ColorGradeTint         = 0.0f;
    int32   ColorGradeInLog        = 1;
    // Band partition on the encoded-log axis: shadows fade out over start..end,
    // highlights fade in over start..end, midtones take the remainder. Defaults
    // mirror kColorGrade*Default in ColorGradeParamsUBO.h; FillColorGradeParamsUBO
    // sanitizes ordering before upload.
    float32 ColorGradeShadowsStart    = 0.0f;
    float32 ColorGradeShadowsEnd      = 0.45f;
    float32 ColorGradeHighlightsStart = 0.45f;
    float32 ColorGradeHighlightsEnd   = 0.95f;

    // Volumetric fog resolved from PostProcessVolume.
    float32 VolumetricFogIntensity = 0.0f;
    int32 VolumetricFogIsGlobal = 1;
    int32 VolumetricFogVolumeShape = 0;
    int32 VolumetricFogVolumeValid = 0;
    float32 VolumetricFogVolumeBlendDistance = 0.0f;
    float32 VolumetricFogVolumeCenterX = 0.0f;
    float32 VolumetricFogVolumeCenterY = 0.0f;
    float32 VolumetricFogVolumeCenterZ = 0.0f;
    float32 VolumetricFogVolumeAxisXX = 1.0f;
    float32 VolumetricFogVolumeAxisXY = 0.0f;
    float32 VolumetricFogVolumeAxisXZ = 0.0f;
    float32 VolumetricFogVolumeAxisYX = 0.0f;
    float32 VolumetricFogVolumeAxisYY = 1.0f;
    float32 VolumetricFogVolumeAxisYZ = 0.0f;
    float32 VolumetricFogVolumeAxisZX = 0.0f;
    float32 VolumetricFogVolumeAxisZY = 0.0f;
    float32 VolumetricFogVolumeAxisZZ = 1.0f;
    float32 VolumetricFogVolumeHalfExtentX = 0.5f;
    float32 VolumetricFogVolumeHalfExtentY = 0.5f;
    float32 VolumetricFogVolumeHalfExtentZ = 0.5f;
    float32 VolumetricFogMaxDistance = 160.0f;
    int32 VolumetricFogXYCellSizePixels = 8;
    int32 VolumetricFogZSliceCount = 128;
    float32 VolumetricFogDepthDistribution = 1.6f;
    float32 VolumetricFogDensity = 0.02f;
    float32 VolumetricFogBaseHeight = 0.0f;
    float32 VolumetricFogHeightFalloff = 30.0f;
    float32 VolumetricFogSkyFade = 0.5f;
    float32 VolumetricFogAlbedoR = 0.82f;
    float32 VolumetricFogAlbedoG = 0.78f;
    float32 VolumetricFogAlbedoB = 0.72f;
    float32 VolumetricFogEmissionR = 0.0f;
    float32 VolumetricFogEmissionG = 0.0f;
    float32 VolumetricFogEmissionB = 0.0f;
    float32 VolumetricFogAnisotropy = 0.35f;
    int32 VolumetricFogTrackDirectionalLight = 1;
    float32 VolumetricFogSunIntensityScale = 1.0f;
    float32 VolumetricFogSunTintR = 1.0f;
    float32 VolumetricFogSunTintG = 0.78f;
    float32 VolumetricFogSunTintB = 0.52f;
    float32 VolumetricFogAmbientTintR = 0.32f;
    float32 VolumetricFogAmbientTintG = 0.38f;
    float32 VolumetricFogAmbientTintB = 0.48f;
    int32 VolumetricFogNoiseEnabled = 1;
    float32 VolumetricFogNoiseScale = 65.0f;
    float32 VolumetricFogNoiseStrength = 0.35f;
    float32 VolumetricFogNoiseVelocityX = 0.0f;
    float32 VolumetricFogNoiseVelocityY = 0.0f;
    float32 VolumetricFogNoiseVelocityZ = 0.0f;
    float32 VolumetricFogNoiseContrast = 1.25f;
    float32 VolumetricFogNoiseChannelWeightR = 1.0f;
    float32 VolumetricFogNoiseChannelWeightG = 0.0f;
    float32 VolumetricFogNoiseChannelWeightB = 0.0f;
    float32 VolumetricFogNoiseChannelWeightA = 0.0f;
    float32 VolumetricFogDensityThreshold = 0.0f;
    float32 VolumetricFogDensityThresholdSoftness = 0.15f;
    int32 VolumetricFogTemporalEnabled = 1;
    float32 VolumetricFogTemporalBlend = 0.92f;
    float32 VolumetricFogJitterStrength = 0.45f;
    int32 VolumetricFogJitterMotion = 0;
    float32 VolumetricFogCompositeDepthBias = 0.0f;
    float32 VolumetricFogShadowBias = 0.0f;

    // Shared post-fog multiple-scattering approximation. Both height and
    // volumetric fog author these values; the renderer runs one common chain.
    int32 FogGlowEnabled = 0;
    int32 FogGlowQuality = 2;
    float32 FogGlowIntensity = 0.0f;
    float32 FogGlowRadius = 3.0f;
    int32 FogGlowOctaves = 6;
    float32 FogGlowSampleScale = 1.0f;
    float32 FogGlowScatter = 0.7f;
    float32 FogGlowThreshold = 0.0f;
    float32 FogGlowKnee = 0.5f;
    float32 FogGlowFadeStart = 0.05f;
    float32 FogGlowFadeEnd = 0.8f;
    float32 FogGlowTintR = 1.0f;
    float32 FogGlowTintG = 1.0f;
    float32 FogGlowTintB = 1.0f;
    int32 FogGlowAntiFlicker = 1;

    // Screen-space atmospheric cloud layer resolved from PostProcessVolume.
    float32 AtmosphericCloudSkyFill = 0.0f;
    float32 AtmosphericCloudVaporMass = 0.0f;
    float32 AtmosphericCloudColorR = 1.0f;
    float32 AtmosphericCloudColorG = 1.0f;
    float32 AtmosphericCloudColorB = 1.0f;
    float32 AtmosphericCloudOpacity = 1.0f;
    float32 AtmosphericCloudFloorHeight = 900.0f;
    float32 AtmosphericCloudLayerDepth = 1800.0f;
    float32 AtmosphericCloudBodyFrequency = 0.72f;
    float32 AtmosphericCloudEdgeFrequency = 3.2f;
    float32 AtmosphericCloudEdgeBreakup = 0.58f;
    float32 AtmosphericCloudDriftAngle = 24.0f;
    float32 AtmosphericCloudDriftRate = 0.08f;
    float32 AtmosphericCloudSunFade = 0.55f;
    float32 AtmosphericCloudSkyBounce = 0.32f;
    float32 AtmosphericCloudRimBoost = 0.42f;
    float32 AtmosphericCloudOcclusion = 0.55f;
    float32 AtmosphericCloudHistoryWeight = 0.90f;
    float32 AtmosphericCloudPixelScale = 0.75f;

    // Raymarched volumetric clouds (VolumetricClouds module, SebLague/Clouds
    // port). The volume is a viewer-centered disc: Radius wide, spanning
    // [Altitude, Altitude + Thickness]. DensityMultiplier 0 is the authored-off
    // state (NeutralizeSettings zeroes it for volumes without the effect).
    float32 CloudsRadius = 0.0f;
    float32 CloudsAltitude = 900.0f;
    float32 CloudsThickness = 0.0f;
    int32   CloudsNumStepsLight = 8;
    float32 CloudsStepSize = 11.0f;
    float32 CloudsRayOffsetStrength = 10.0f;
    float32 CloudsScale = 1.0f;
    float32 CloudsDensityMultiplier = 0.0f;
    float32 CloudsDensityOffset = 0.0f;
    float32 CloudsShapeOffsetX = 0.0f;
    float32 CloudsShapeOffsetY = 0.0f;
    float32 CloudsShapeOffsetZ = 0.0f;
    float32 CloudsShapeWeightR = 1.0f;
    float32 CloudsShapeWeightG = 0.48f;
    float32 CloudsShapeWeightB = 0.15f;
    float32 CloudsShapeWeightA = 0.0f;
    float32 CloudsDetailScale = 10.0f;
    float32 CloudsDetailWeight = 0.1f;
    float32 CloudsDetailWeightR = 1.0f;
    float32 CloudsDetailWeightG = 0.5f;
    float32 CloudsDetailWeightB = 0.25f;
    float32 CloudsDetailOffsetX = 0.0f;
    float32 CloudsDetailOffsetY = 0.0f;
    float32 CloudsDetailOffsetZ = 0.0f;
    float32 CloudsAbsorptionThroughCloud = 1.0f;
    float32 CloudsAbsorptionTowardSun = 1.0f;
    float32 CloudsDarknessThreshold = 0.2f;
    float32 CloudsPhaseForward = 0.83f;
    float32 CloudsPhaseBack = 0.3f;
    float32 CloudsPhaseBase = 0.8f;
    float32 CloudsPhaseFactor = 0.15f;
    float32 CloudsTimeScale = 1.0f;
    float32 CloudsBaseSpeed = 1.0f;
    float32 CloudsDetailSpeed = 2.0f;
    float32 CloudsHistoryWeight = 0.85f;

    // Screen-space GTAO (AmbientOcclusion node). Intensity == 0 disables the
    // effect (the node skips its dispatch and the world pass sees the no-op fallback).
    float32 AOIntensity = 0.0f;
    float32 AORadius    = 1.5f;
    float32 AOThickness = 1.0f;

    // Stochastic screen-space reflections (ScreenSpaceReflectionsNode). Surface
    // roughness comes from the Forward+ material MRT, so no roughness member
    // lives here; EdgeFade governs only screen-boundary confidence. Intensity
    // 0 is the authored-off state, which is why no NeutralizeSettings hook is
    // registered: a volume without the effect leaves these at their defaults
    // and reads as inactive.
    float32 SSSRIntensity = 0.0f;
    float32 SSSRMaxDistance = 100.0f;
    float32 SSSRThickness = 0.2f;
    float32 SSSREdgeFade = 0.08f;
    int32 SSSRMaxSteps = 48;
    int32 SSSRSampleQuality = 1; // Components::SssrSampleQuality: 0=Low, 1=Medium, 2=High
    int32 SSSRMultiBounce = 0;   // 1 = hits sample the previous frame's composited color

    // Histogram auto-exposure (AutoExposureNode). Active only when the resolved exposure mode is
    // Auto. The EV range becomes the linear-exposure clamp; speeds drive the log-space adaptation;
    // the metered average is keyed to Rendering::kAutoExposureMiddleGrey times 2^AutoExposureBiasEv.
    bool    AutoExposureActive    = false;
    float32 AutoExposureMinEv     = 4.0f; // keep in lockstep with Components::Camera::AutoExposureMinEv
    float32 AutoExposureMaxEv     = 18.0f; // keep in lockstep with PostProcessVolume::AutoExposureMaxEv
    float32 AutoExposureSpeedUp   = 1.0f; // dark adaptation (exposure rises) is slow...
    float32 AutoExposureSpeedDown = 3.0f; // ...light adaptation (exposure falls) is fast
    // Total metering bias in stops, applied OUTSIDE the adaptation envelope (UE semantics: the
    // envelope stays the meter's absolute-EV authority; bias trims the resolved exposure on top,
    // so it stays effective even when adaptation is pinned at MinEv/MaxEv). The extraction resolve
    // stamps kDefaultAutoExposureBiasEv + camera ExposureCompensation + volume compensation here;
    // TryWriteField folds it into the resolve shader's key AND clamp bounds (mathematically
    // identical to a post-clamp multiply), keeping the shader interface unchanged.
    float32 AutoExposureBiasEv    = kDefaultAutoExposureBiasEv;

    // Volume exposure modifiers (ExposureAdjustmentEffect), applied on top of the camera-sensor
    // resolve — never owning it. Compensation is ± stops; the clamps intersect (narrow) the
    // camera's auto-adaptation envelope. The clamp defaults sit outside any authorable EV100
    // range so volume-less worlds change nothing.
    float32 ExposureCompensationEv = 0.0f;
    float32 ExposureClampMinEv     = kExposureClampNoOpMinEv;
    float32 ExposureClampMaxEv     = kExposureClampNoOpMaxEv;

    bool IsBloomActive(float32 epsilon = 1.0f / 1024.0f) const;
    void ResolveBloomPyramid(uint32 renderHeight);
    // Switches bloom off as a whole: additive highlights, depth veil and
    // scattering (lens dirt follows the highlights), so no bloom pass runs.
    void DisableBloom();
    bool IsBloomLensDirtActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsHalationActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsBloomHighlightsActive(float32 epsilon = 1.0f / 1024.0f) const;
    // The one scattering predicate: the scattering passes run, BloomCombine
    // samples their pyramid, and the pushed bloomScatteringAmount is non-zero
    // exactly when this is true.
    bool IsBloomScatteringActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsChromaticAberrationActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsBloomChainActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsFogGlowActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsFogGlowFastActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsFogGlowPyramidActive(float32 epsilon = 1.0f / 1024.0f) const;
    void ResolveFogGlowPyramid(uint32 renderHeight);
    // Sequences every per-view derived-field resolve (bloom + fog-glow pyramid
    // scales) so render-graph nodes stay effect-agnostic — a node that runs the
    // post-process chain calls this without naming individual effects.
    void ResolveDerivedForRenderHeight(uint32 renderHeight);
    bool IsFilmSimulationGrainActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsFilmSimulationArtifactsActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsColorFilterActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsColorGradeActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsHdrColorFxActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsCasActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsLutActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsVignetteActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsLdrStackActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsVhsActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsCrtActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsFastBlurActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsDofActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsHeatDistortionActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsHeightFogActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsVolumetricFogActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsAtmosphericCloudActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsVolumetricCloudsActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsAOActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsSSSRActive(float32 epsilon = 1.0f / 1024.0f) const;
    bool IsAutoExposureActive() const { return AutoExposureActive; }

    // Try to write the field matching `name` to the push constant writer.
    // Returns true if a matching field was found. Called by FullscreenShaderNode
    // for each reflected push constant member in the shader — the shader is the
    // source of truth for which settings it needs.
    bool TryWriteField(const std::string& name,
                       GameEngine::Rendering::NamedPushConstantWriter& pcw) const;

    // Read a numeric field by shader-side name into `out`. Mirrors TryWriteField
    // for the gating direction: FullscreenShaderNode uses this to evaluate
    // "is this effect a no-op for the current frame?" via the skipWhen JSON.
    bool TryReadField(const std::string& name, float& out) const;

    // The exact set of names TryReadField resolves (shared simple members +
    // computed *Active gates + read-only aliases). The pipeline compiler
    // validates a pass's "skipWhen" keys against this; kept in lockstep with
    // TryReadField because both consume the same sources — the core X-macro in
    // the .cpp plus the per-effect EffectSettingsField registrations
    // (PostProcessEffectDescriptors.cpp).
    static const std::vector<std::string>& ReadableFieldNames();

    // True when `name` is a computed *Active gate (TryReadField answers it with
    // 1.0 or 0.0), as opposed to a stored value field. The pipeline compiler
    // accepts only gates as "inputGates" values.
    static bool IsGateName(const std::string& name);
};

// Blend two settings by weight, folding over the per-effect EffectSettingsField
// tables (lerp for floats, highest-priority-wins for discrete fields, log2 for
// exposure — each member's rule lives in its effect's registration).
// contributingGroups names the groups b authors: a group outside it keeps a's
// values, so a blended-in volume without ColorGradeEffect does not reset grading
// back to identity.
PostProcessSettings BlendPostProcessSettings(const PostProcessSettings& a,
                                             const PostProcessSettings& b,
                                             float weight,
                                             SettingsBlendGroupMask contributingGroups);

// Copy every member of one contributes-group from src to dst (group membership
// lives on the EffectSettingsField tables). Extraction's fog-only blend paths
// use this instead of hand-kept per-field copy lists.
void CopySettingsGroup(PostProcessSettings& dst, const PostProcessSettings& src,
                       GameEngine::Rendering::SettingsBlendGroup group);

VolumetricFogSettings ToVolumetricFogSettings(const PostProcessSettings& settings);

} // namespace Renderer
} // namespace Engine
} // namespace GameEngine
