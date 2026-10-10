#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Lightweight depth-of-field blur. Attach to the same entity as a PostProcessVolume.
struct FastBlurEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // 0 = bypass, 1 = full contribution.
    float32 Intensity{0.0f};
    // View-space focus distance in world units.
    float32 FocusDistance{12.0f};
    // Distance around the focus plane that remains mostly sharp.
    float32 FocusRange{6.0f};
    // Maximum blur radius in pixels. Keep small for the single-pass fast path.
    float32 MaxRadius{6.0f};
    // Far blur is always enabled; this controls whether near foreground also blurs.
    bool NearBlur{true};
};

} // namespace Components
} // namespace GameEngine
