#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Contrast Adaptive Sharpening (CAS)-style post effect.
// Attach to the same entity as a PostProcessVolume.
struct ContrastAdaptiveSharpenEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // Relative execution order among reorderable LDR post FX.
    int32 StackOrder{1};
    // 0 = bypass, 1 = strongest sharpening.
    float32 Strength{0.35f};
};

} // namespace Components
} // namespace GameEngine
