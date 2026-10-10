#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Per-volume exposure adjustment. Attach to the same entity as a PostProcessVolume.
//
// Volumes never own exposure — the camera is the sensor (see Camera.h). This effect
// only nudges the camera's result in ways that compose without a winner:
//  - Compensation adds ± stops on top of the resolved camera exposure (all modes;
//    folds into the metering key in Auto, mirroring Camera::ExposureCompensation).
//  - The EV clamps intersect the camera's auto-adaptation envelope: a volume can
//    tighten the range, never widen it (Auto mode only). Opt-in per side; setting
//    MinEv == MaxEv pins the adapted EV — manual exposure with the adaptation
//    transition still smoothing entry/exit across the volume boundary.
struct ExposureAdjustmentEffect {
    bool Enabled {true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 Compensation {0.0f}; // ± EV stops
    bool ClampMin {false};
    float32 MinEv {4.0f}; // used when ClampMin; keep in lockstep with Camera::AutoExposureMinEv
    bool ClampMax {false};
    float32 MaxEv {18.0f}; // used when ClampMax; keep in lockstep with Camera::AutoExposureMaxEv
};

} // namespace Components
} // namespace GameEngine
