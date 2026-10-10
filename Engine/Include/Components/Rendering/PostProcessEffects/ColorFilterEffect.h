#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

enum class ColorFilterBlendMode : int32 {
    Multiply = 0,
    Add      = 1,
    Screen   = 2,
    SoftLight = 3,
};

constexpr int32 kColorFilterBlendModeCount = 4;

// Per-volume color filter effect. Attach to the same entity as a
// PostProcessVolume to tint scene color within that volume's spatial extent.
struct ColorFilterEffect {
    bool                 Enabled  {true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // Relative execution order among reorderable LDR post FX.
    int32                StackOrder{0};
    ColorFilterBlendMode BlendMode{ColorFilterBlendMode::Multiply};
    float32              Color[3] {1.0f, 1.0f, 1.0f}; // linear RGB tint multiplier
    float32              Intensity{1.0f};             // 0 = pass-through, 1 = full tint
};

} // namespace Components
} // namespace GameEngine
