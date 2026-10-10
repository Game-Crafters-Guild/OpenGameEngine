#pragma once

#include "Types/Types.h"

namespace GameEngine::Components {

enum class DofSamplingQuality : int32 {
    Performance = 0,
    Balanced = 1,
    Quality = 2,
};

// Physical depth of field. The lens lives on the Camera (FocusDistance and
// Aperture, with focal length derived from FovY) — this component enables the
// blur on a PostProcessVolume and caps it artistically. The circle of
// confusion follows the thin-lens model, so f-number, focal length, and focus
// distance behave like a real camera: wider aperture or longer lens = shallower
// depth of field.
struct DepthOfFieldEffect {
    static constexpr float32 kMaxRadiusMax = 512.0f;

    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // Artistic clamp on the blur radius in pixels.
    float32 MaxRadius{16.0f};
    // Caps the gather budget used for large blur radii.
    DofSamplingQuality SamplingQuality{DofSamplingQuality::Balanced};
    // Legacy scene-load fields. Focus visualization is owned by Camera;
    // retaining these keeps older serialized volumes readable.
    int32 DebugMode{0};
    float32 DebugAlpha{0.5f};
};

} // namespace GameEngine::Components
