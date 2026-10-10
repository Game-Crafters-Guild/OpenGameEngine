#pragma once

#include "Types/Types.h"

namespace GameEngine {
namespace Components {

// HDR color grading (before tonemap). Attach next to PostProcessVolume.
//
// Unified three-way corrector graded in log space (ACEScct-shaped curve),
// applied post-exposure so the tonal bands track a stable mid-grey anchor. Each
// band carries an RGB color offset (the trackball puck) plus a master lightness;
// the offsets are zero-centered so an all-default component is an exact no-op.
// Global Contrast and Saturation act on the whole image; GradeInLog selects the
// working curve (log by default, linear only for parity debugging).
struct ColorGradeEffect {
    bool Enabled{true};
    // Off pins the effect off wherever its volume applies: a value the volume blend reads.
    static constexpr bool KeepsOwnEnabledField = true;

    // Per-band offsets: RGB chroma offset (0 = neutral) + master lightness (0 = neutral).
    float32 ShadowsColor[3]{0.0f, 0.0f, 0.0f};
    float32 ShadowsLightness{0.0f};
    float32 MidtonesColor[3]{0.0f, 0.0f, 0.0f};
    float32 MidtonesLightness{0.0f};
    float32 HighlightsColor[3]{0.0f, 0.0f, 0.0f};
    float32 HighlightsLightness{0.0f};

    // Global adjustments. Contrast pivots on encoded mid-grey; Saturation lerps
    // toward luma (1 = unchanged). HueShift rotates hue in degrees [-180, 180]
    // (HSV rotation on the graded color, applied after the corrector — URP's
    // ColorAdjustments.hueShift semantics).
    float32 Contrast{1.0f};
    float32 Saturation{1.0f};
    float32 HueShift{0.0f};

    // White balance, URP ranges: Temperature warms (+) / cools (-), Tint pushes
    // magenta (+) / green (-), both [-100, 100] with 0 = neutral. Resolved on the
    // CPU to von Kries LMS gains (URP's ColorBalanceToLMSCoeffs math) and applied
    // in linear before the corrector, so a converted Unity WhiteBalance matches.
    float32 Temperature{0.0f};
    float32 Tint{0.0f};

    // Band partition on the encoded-log axis [0,1] (exposed mid-grey sits at
    // ~0.41): shadow weight fades out over ShadowsStart..ShadowsEnd, highlight
    // weight fades in over HighlightsStart..HighlightsEnd, midtones take the
    // remainder. Defaults mirror kColorGrade*Default in ColorGradeParamsUBO.h;
    // a reversed pair is sanitized (end raised to start) before upload.
    float32 ShadowsStart{0.0f};
    float32 ShadowsEnd{0.45f};
    float32 HighlightsStart{0.45f};
    float32 HighlightsEnd{0.95f};

    // Grade working space: true = log (ACEScct-shaped), false = linear (debug parity).
    bool GradeInLog{true};
};

} // namespace Components
} // namespace GameEngine
