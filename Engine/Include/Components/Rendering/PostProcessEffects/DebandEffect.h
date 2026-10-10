#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// Per-volume control over the terminal encode's gradient-aware deband
// (SRGBEncodePass). Attach to the same entity as a PostProcessVolume.
// The deband is OPT-IN: volume-less worlds and volumes WITHOUT this component
// run with the filter off. Attaching it makes the volume state an explicit
// policy — Enabled at the default threshold engages the reviewed 6-LSB gate
// wherever the volume wins; Enabled=false pins it off explicitly. GE_DEBAND /
// GE_DEBAND_THRESHOLD env overrides beat any volume (debug/A-B instruments;
// see SRGBEncodePass).
struct DebandEffect {
    bool    Enabled     {true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;
    // Weber gate threshold in output LSBs: tap averages are adopted only while
    // every channel stays within this distance of the original center. 6 covers
    // the measured 8-bit-authored gradient band class while real edges stay
    // shut; larger gates risk eating texture detail. 0 = filter off.
    float32 ThresholdLsb{6.0f};
};

} // namespace Components
} // namespace GameEngine
