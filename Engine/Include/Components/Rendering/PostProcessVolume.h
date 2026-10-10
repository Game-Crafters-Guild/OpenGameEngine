#pragma once

#include "Types/Types.h"
#include "Components/Rendering/TonemapMode.h"

namespace GameEngine {
namespace Components {

enum class PostProcessVolumeShape : int32 {
    Box      = 0,
    Sphere   = 1,
    Capsule  = 2,
    Cylinder = 3,
};

// How the scene exposure is driven. Fixed = the legacy linear multiplier (Exposure). Manual =
// an absolute photographic EV100 (ManualExposureEV) anchored to 203-nit reference white. Physical =
// the EV100 is computed from camera Aperture/ShutterTime/Iso (sunny-16 = f/16, 1/100 s, ISO 100).
// Auto (metered) is reserved for a later phase.
enum class ExposureMode : int32 {
    Fixed    = 0,
    Manual   = 1,
    Physical = 2,
    Auto     = 3, // histogram metering + eye adaptation (AutoExposureNode)
};

// The EV100 a manual exposure falls back to when nothing has authored one.
// Chosen so an authored base colour reads back close to what the colour picker showed: swept
// against pure R/G/B under the default sky and scored as CIE76 dE against the authored sRGB, the
// error bottoms at 15.4. Tied to the default Khronos PBR Neutral tonemap — under an identity curve
// the optimum is 15.0.
inline constexpr float32 kDefaultManualExposureEv = 15.4f;

struct PostProcessVolume {
    float32 Weight {1.0f};
    int32 Priority {0};

    // Bitmask of which cameras this volume affects. A camera's
    // Components::Camera::PostProcessMask is ANDed against this; the volume
    // contributes only when the result is non-zero. Default ~0u = all cameras.
    uint32 PostProcessMask {0xFFFFFFFFu};

    // Spatial extent. When IsGlobal is true the volume affects the whole world
    // regardless of camera position. When false, the volume is a unit shape
    // (cube or sphere of diameter 1) transformed by the entity's
    // WorldTransform — the transform's scale defines the dimensions and
    // rotation orients it. Non-uniform scale on a Sphere shape produces an
    // oriented ellipsoid. BlendDistance is the soft fade region around the
    // shape, in world units.
    bool IsGlobal {true};
    PostProcessVolumeShape Shape {PostProcessVolumeShape::Box};
    float32 BlendDistance {1.0f};

    // Exposure is NOT a volume property. It lives on Components::Camera (the sensor) and, for views
    // without a camera (the editor Scene View), on the world default. The ExposureMode enum above is
    // shared by Camera.h / Exposure.h. A volume entity may carry an ExposureAdjustmentEffect —
    // composable modifiers (± stop compensation, narrow-only adaptation clamps) that stack on the
    // camera's result, never replace it.

    // Tonemapping
    TonemapMode Tonemap {TonemapMode::Neutral};
    int32 DitherMode {0}; // 0=Bayer, 1=BlueNoise
    // HDR10/HDR10+ only: perceptually compress highlight chroma in ICtCp after
    // tonemapping and before UI composition / the terminal PQ encode. Zero is
    // an exact pass-through and keeps the feature opt-in.
    float32 IctcpChromaCompression {0.0f};

};

} // namespace Components
} // namespace GameEngine
