#pragma once

#include "Types/Types.h"

#include <cmath>
#include <cstddef>

namespace GameEngine::Engine::Renderer {

struct PostProcessSettings;

// std140 grade block shared by hdr_color_fx.frag and ColorGradeParamsUploadNode.
//
// Working space: the render's Rec.709-LINEAR RGB (the same primaries the scene is
// lit and tonemapped in). A future AP1/ACEScg advanced option would gamut-map in
// and out around this block; not implemented yet (see hdr_color_fx.frag hook).
//
// Zero == identity by construction: band offsets are zero-centered, Contrast /
// Saturation are stored as deltas from 1.0, and the band limits are stored as
// deltas from the kColorGrade*Default constants below. A zero-filled (unbound /
// placeholder) buffer therefore reads as a pass-through grade at the default band
// partition, so pipelines that run hdr_color_fx without an upload node still
// render correctly (colour filter only).
struct ColorGradeParamsUBO
{
    // vec4 per band: rgb chroma offset in .xyz (0 = neutral), master lightness in .w.
    float shadowsR = 0.0f, shadowsG = 0.0f, shadowsB = 0.0f, shadowsMaster = 0.0f;
    float midtonesR = 0.0f, midtonesG = 0.0f, midtonesB = 0.0f, midtonesMaster = 0.0f;
    float highlightsR = 0.0f, highlightsG = 0.0f, highlightsB = 0.0f, highlightsMaster = 0.0f;
    // vec4 globals: contrast-1, saturation-1, gradeInLinear (0 = log), pad.
    float contrastMinusOne = 0.0f;
    float saturationMinusOne = 0.0f;
    float gradeInLinear = 0.0f;
    float _pad0 = 0.0f;
    // vec4 band limits, encoded-log domain, stored as sanitized-value MINUS the
    // matching kColorGrade*Default (so zero-filled == defaults). Sanitized by
    // FillColorGradeParamsUBO: start clamped to [0,1], end >= start + minimum
    // width — the shader consumes them without further ordering checks.
    float shadowsStartDelta = 0.0f;
    float shadowsEndDelta = 0.0f;
    float highlightsStartDelta = 0.0f;
    float highlightsEndDelta = 0.0f;
    // vec4 white balance + hue: von Kries LMS gains minus 1 (URP's
    // ColorBalanceToLMSCoeffs, computed CPU-side from Temperature/Tint; exactly
    // zero at neutral so the shader's gate and the zero-filled placeholder both
    // read identity), hue rotation in turns (degrees / 360) in .w.
    float whiteBalanceLMinusOne = 0.0f;
    float whiteBalanceMMinusOne = 0.0f;
    float whiteBalanceSMinusOne = 0.0f;
    float hueShiftTurns = 0.0f;
};

static_assert(sizeof(ColorGradeParamsUBO) == 96, "ColorGradeParamsUBO must be 96 bytes (6 std140 vec4)");
static_assert(offsetof(ColorGradeParamsUBO, midtonesR) == 16, "midtones vec4 must start at offset 16");
static_assert(offsetof(ColorGradeParamsUBO, highlightsR) == 32, "highlights vec4 must start at offset 32");
static_assert(offsetof(ColorGradeParamsUBO, contrastMinusOne) == 48, "globals vec4 must start at offset 48");
static_assert(offsetof(ColorGradeParamsUBO, shadowsStartDelta) == 64, "band-limits vec4 must start at offset 64");
static_assert(offsetof(ColorGradeParamsUBO, whiteBalanceLMinusOne) == 80, "white-balance vec4 must start at offset 80");

// Default band partition on the encoded-log axis (EncodeGradeLog domain, exposed
// mid-grey = 0.4136). Shadows fade out over [start, end]; highlights fade in over
// [start, end]; midtones take the remainder. Mirrored verbatim as the kDefault*
// constants in hdr_color_fx.frag (a zero-filled UBO decodes to exactly these) —
// keep the two in lockstep. Components::ColorGradeEffect and PostProcessSettings
// default-initialize their band limits to the same values.
inline constexpr float kColorGradeShadowsStartDefault = 0.0f;
inline constexpr float kColorGradeShadowsEndDefault = 0.45f;
inline constexpr float kColorGradeHighlightsStartDefault = 0.45f;
inline constexpr float kColorGradeHighlightsEndDefault = 0.95f;

// Narrowest sanitized band ramp: FillColorGradeParamsUBO raises end to at least
// start + this, so the shader's smoothstep never sees degenerate (undefined)
// edge0 >= edge1.
inline constexpr float kColorGradeMinBandWidth = 1e-3f;

// Authoring ranges shared by the descriptor clamps, extraction and the inspector.
// Temperature/Tint mirror URP WhiteBalance (+-100 around neutral 0); HueShift is
// a +-half-turn in degrees like URP ColorAdjustments.hueShift.
inline constexpr float kColorGradeTemperatureTintRange = 100.0f;
inline constexpr float kColorGradeHueShiftRangeDegrees = 180.0f;

// GradeLog: ACEScct-shaped curve (linear toe below the breakpoint, log2 body above),
// mapped to ~[0,1] around exposed mid-grey. A plain log2 would blow up on scene
// blacks (log2(0) = -inf); the toe keeps 0 finite and gradeable. Mirrored verbatim
// in hdr_color_fx.frag — keep the two in lockstep.
inline float EncodeGradeLog(float x)
{
    return (x <= 0.0078125f) ? (x * 10.5402f + 0.0729f)
                             : ((std::log2(x) + 9.72f) / 17.52f);
}

inline float DecodeGradeLog(float y)
{
    return (y <= 0.1554f) ? ((y - 0.0729f) / 10.5402f)
                          : std::exp2(y * 17.52f - 9.72f);
}

void FillColorGradeParamsUBO(const PostProcessSettings& settings, ColorGradeParamsUBO& out);

} // namespace GameEngine::Engine::Renderer
