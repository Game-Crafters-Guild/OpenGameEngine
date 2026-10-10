#pragma once

#include "Types/Types.h"
#include "Components/Rendering/PostProcessVolume.h" // ExposureMode (exposure is a camera/sensor property)

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace Components {

// Film gate / sensor format presets for the physical camera. Each maps to a
// vertical gate/sensor height in millimeters (the dimension FovY spans), which
// drives both the derived focal length and the DoF circle-of-confusion scale:
// small gates (8/16 mm) give deep focus, large ones (65 mm, IMAX, medium
// format) melt backgrounds at the same field of view.
enum class CameraSensorPreset : uint32 {
    Custom = 0,
    Standard8,        // Regular 8 mm: 4.8 x 3.5
    Super8,           // Super 8: 5.79 x 4.01
    Film16,           // 16 mm: 10.26 x 7.49
    Super16,          // Super 16: 12.52 x 7.41
    Film35TwoPerf,    // 35 mm 2-perf (Techniscope): 21.95 x 9.47
    Film35ThreePerf,  // 35 mm 3-perf: 24.89 x 13.87
    Film35Academy,    // 35 mm Academy 4-perf: 21.95 x 16.0
    Super35,          // Super 35 4-perf: 24.89 x 18.66
    Film65FivePerf,   // 65 mm 5-perf: 52.48 x 23.01
    Imax15Perf,       // IMAX 15/70: 70.41 x 52.63
    MicroFourThirds,  // 17.3 x 13.0
    ApsC,             // 23.6 x 15.6
    FullFrame,        // 36 x 24
    MediumFormat4433, // digital medium format (Fujifilm GFX / Hasselblad X): 43.8 x 32.9
    MediumFormat5440, // large digital medium format (Phase One XF): 53.7 x 40.2
    ArriAlexa35,      // ARRI Alexa 35 Super 35 open gate: 27.99 x 19.22
    ArriAlexaLF,      // ARRI Alexa LF / Mini LF open gate: 36.70 x 25.54
    ArriAlexa65,      // ARRI Alexa 65 open gate: 54.12 x 25.58
};

constexpr float32 CameraSensorPresetHeightMm(CameraSensorPreset preset)
{
    switch (preset) {
    case CameraSensorPreset::Standard8:        return 3.5f;
    case CameraSensorPreset::Super8:           return 4.01f;
    case CameraSensorPreset::Film16:           return 7.49f;
    case CameraSensorPreset::Super16:          return 7.41f;
    case CameraSensorPreset::Film35TwoPerf:    return 9.47f;
    case CameraSensorPreset::Film35ThreePerf:  return 13.87f;
    case CameraSensorPreset::Film35Academy:    return 16.0f;
    case CameraSensorPreset::Super35:          return 18.66f;
    case CameraSensorPreset::Film65FivePerf:   return 23.01f;
    case CameraSensorPreset::Imax15Perf:       return 52.63f;
    case CameraSensorPreset::MicroFourThirds:  return 13.0f;
    case CameraSensorPreset::ApsC:             return 15.6f;
    case CameraSensorPreset::FullFrame:        return 24.0f;
    case CameraSensorPreset::MediumFormat4433: return 32.9f;
    case CameraSensorPreset::MediumFormat5440: return 40.2f;
    case CameraSensorPreset::ArriAlexa35:      return 19.22f;
    case CameraSensorPreset::ArriAlexaLF:      return 25.54f;
    case CameraSensorPreset::ArriAlexa65:      return 25.58f;
    case CameraSensorPreset::Custom:           break;
    }
    return 24.0f;
}

inline float32 CameraFocalLengthMmFromVerticalFov(float32 fovYDegrees,
                                                   float32 sensorHeightMm)
{
    constexpr float32 kDegreesToRadians = 0.01745329251994329577f;
    const float32 safeFov = std::clamp(fovYDegrees, 0.1f, 179.0f);
    const float32 safeSensorHeight = std::max(sensorHeightMm, 1.0f);
    return (0.5f * safeSensorHeight) /
           std::tan(0.5f * safeFov * kDegreesToRadians);
}

inline float32 CameraVerticalFovFromFocalLengthMm(float32 focalLengthMm,
                                                  float32 sensorHeightMm)
{
    constexpr float32 kRadiansToDegrees = 57.29577951308232088f;
    const float32 safeFocalLength = std::max(focalLengthMm, 1.0f);
    const float32 safeSensorHeight = std::max(sensorHeightMm, 1.0f);
    return 2.0f * std::atan(0.5f * safeSensorHeight / safeFocalLength) *
           kRadiansToDegrees;
}

struct Camera {
    static constexpr float32 kApertureMin = 0.95f;
    static constexpr float32 kFocusDistanceMin = 0.01f;
    static constexpr uint32 kApertureBladeCountMin = 3u;
    static constexpr uint32 kApertureBladeCountMax = 16u;

    bool Perspective {true};
    float32 FovY {60.0f};
    // Vertical world-space extent used when Perspective is false.
    float32 OrthographicSize {10.0f};
    float32 NearZ {0.01f};
    float32 FarZ {1000.0f};
    uint32 CullingMask {0xFFFFFFFFu};
    uint32 PostProcessProfileId {0};
    // 0 = Default (uses scene view MSAA from RenderServices), 1 = Off, 2/4/8 = explicit.
    uint32 MSAASamples {0};
    // Anti-aliasing mode override: 0 = Default (engine AA mode), else
    // AntiAliasingMode + 1 — 1 = Off, 2 = MSAA (sample count from
    // MSAASamples), 3 = TAA, 4 = FXAA, 5 = SMAA, 6 = TemporalFXAA. Resolved by
    // RenderServices::ResolveAntiAliasing; an explicit MSAASamples of 2/4/8
    // with mode Default keeps forcing MSAA (legacy contract).
    uint32 AntiAliasing {0};
    // Per-camera render scale override: 0 = inherit the engine default,
    // including Dynamic resolution. An explicit value pins this camera's view
    // to a FIXED scale in [kMinRenderScale, kMaxRenderScale] (above 1.0
    // supersamples — SSAA), opting it out of the engine-wide dynamic
    // controller. Applied as the view's per-view render-scale override.
    float32 RenderScale {0.0f};

    // Bitmask of post-process volume layers this camera samples. ANDed against
    // PostProcessVolume::PostProcessMask; volume contributes when non-zero.
    uint32 PostProcessMask {0xFFFFFFFFu};

    // Exposure (the camera's sensor/eye) — the single source of truth for exposure. When this camera
    // drives a view, its exposure sets that view's exposure (PostProcessVolumes contribute only grading
    // / tonemap / bloom / fog, not exposure). Views without a Camera (e.g. the editor Scene View) expose
    // from the world default (Auto). Fields mirror the photographic controls: Fixed linear, Manual EV100,
    // Physical camera, or Auto (histogram metering + eye adaptation). See Engine/Rendering/Exposure.h.
    ExposureMode ExposureControl {ExposureMode::Auto};
    float32 Exposure {1.0f};              // Fixed mode: linear scene multiplier (1 = identity)
    float32 ManualExposureEV {kDefaultManualExposureEv}; // Manual mode: absolute EV100
    float32 ExposureCompensation {0.0f};  // +/- stops, applied in every mode (+ = brighter)
    float32 Aperture {16.0f};             // Lens f-number; clamped to kApertureMin
    float32 ShutterTime {0.01f};          // Physical mode: seconds (1/100 s)
    float32 Iso {100.0f};                 // Physical mode: sensor sensitivity
    // Lens focus-plane distance in world units. Like exposure, focus is a property of
    // the camera (the lens), not of a spatial volume: a DepthOfFieldEffect on a
    // PostProcessVolume enables the blur and reads this together with Aperture, with
    // focal length derived from FovY against the sensor height below.
    float32 FocusDistance {10.0f};
    // Per-camera focus-plane visualization. The effect volume enables physical
    // DoF, while the rendered camera owns how its lens is diagnosed.
    int32 FocusDebugMode {0};
    float32 FocusDebugAlpha {0.5f};
    // Iris geometry used by physical DoF bokeh. Roundness blends the regular
    // polygon made by the blades toward a circular aperture (0 = straight
    // blades, 1 = circular), while retaining the same f-number/CoC radius.
    uint32 ApertureBladeCount {7};
    float32 ApertureRoundness {1.0f};
    float32 ApertureRotation {0.0f}; // Iris orientation in degrees.
    // Horizontal optical squeeze. Values above 1 widen the captured horizontal
    // field of view and create vertically elongated anamorphic bokeh after
    // display-space de-squeeze. 1 is a conventional spherical lens.
    float32 AnamorphicSqueeze {1.0f};
    // Film gate / sensor format. SensorHeightMm is the single runtime source of
    // truth (vertical gate height, mm); SensorPreset records which preset wrote
    // it — the inspector flips it to Custom when the height is edited by hand.
    CameraSensorPreset SensorPreset {CameraSensorPreset::FullFrame};
    float32 SensorHeightMm {24.0f};
    // Auto: the darkest scene EV100 the meter may fully expose (the max-gain clamp). +4 lets a
    // moonlit night meter near-correct while a starlit scene stays several stops under — night
    // reads as night. Lower it per-camera for deliberate day-for-night brightening (-4 ≈ 3250x gain).
    float32 AutoExposureMinEv {4.0f};
    float32 AutoExposureMaxEv {18.0f};
    float32 AutoExposureSpeedUp {1.0f};
    float32 AutoExposureSpeedDown {3.0f};

    // Aspect ratio preset for projection and letterboxing. 0 = match viewport (native).
    uint32 AspectPreset {0};
    // Custom aspect ratio numerator/denominator used when AspectPreset is Custom.
    float32 CustomAspectWidth {16.0f};
    float32 CustomAspectHeight {9.0f};

    // Pixel-perfect orthographic mode. Only applies when !Perspective.
    // When enabled, the orthographic size is computed (ignoring OrthographicSize) so
    // the reference resolution upscales by an integer factor that fits the viewport,
    // yielding integer screen-pixels-per-source-texel for crisp pixel art.
    bool PixelPerfect {false};
    // Reference pixel size: source texels per world unit (Pixels Per Unit).
    uint32 PixelPerfectPixelsPerUnit {32};
    // Reference resolution the integer upscale is computed against.
    uint32 PixelPerfectReferenceWidth {320};
    uint32 PixelPerfectReferenceHeight {180};
    // Snap the camera's world X/Y translation to the pixel grid to avoid shimmer.
    bool PixelPerfectPixelSnap {true};
    // Crop mode for non-matching screen aspects. false = letterbox: show exactly
    // the reference resolution and black-bar the rest. true = expand: reveal a
    // little more world so the image fills the screen (still integer pixels).
    bool PixelPerfectExpand {false};
    // Optional jitter/TAA params added by systems, not persisted here
};

} // namespace Components
} // namespace GameEngine
