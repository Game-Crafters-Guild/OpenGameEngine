#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Per-volume screen-space ambient occlusion (GTAO) controls. Attach to the same
// entity as a PostProcessVolume. Drives the AmbientOcclusion render node.
struct AmbientOcclusionEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    float32 Intensity{1.0f}; // pow() strength on visibility; 0 = off, 1 = physical, >1 darker
    float32 Radius{1.5f};    // world-space sampling radius
    float32 Thickness{1.0f}; // occluder thickness for the visibility-bitmask back horizon
};

} // namespace Components
} // namespace GameEngine
