#include "Engine/Rendering/ColorGradeParamsUBO.h"

#include "Engine/Rendering/PostProcessSettings.h"

#include <algorithm>

namespace GameEngine::Engine::Renderer {

namespace {

// Band-limit sanitizer (the single choke point — schema, converter, blend and
// IPC writes all funnel through here before the GPU sees a value): start clamps
// to [0,1]; end clamps to [0,1] then raises to start + kColorGradeMinBandWidth,
// so a degenerate or reversed authored pair yields the narrowest valid ramp at
// the authored start instead of an undefined smoothstep.
void SanitizeBandRange(float32 start, float32 end, float& outStart, float& outEnd)
{
    outStart = std::clamp(start, 0.0f, 1.0f);
    outEnd = std::max(std::clamp(end, 0.0f, 1.0f), outStart + kColorGradeMinBandWidth);
}

// URP-parity white balance (ColorUtils.ColorBalanceToLMSCoeffs): temperature/tint
// steer the target white point's CIE xy around D65, then the coefficients are the
// von Kries gains D65 / target in URP's CAT02 LMS. Mirrored verbatim (including
// URP's asymmetric temperature slope) so a converted Unity WhiteBalance renders
// identically; a full Bradford CAT differs only in the sharpening matrix —
// second-order over this +-100 range — and would break converter parity.
void ColorBalanceToLmsCoeffs(float temperature, float tint, float outCoeffs[3])
{
    const float t1 = temperature / 65.0f;
    const float t2 = tint / 65.0f;

    // Target white's chromaticity: D65 x (0.31271) skewed by temperature, y on
    // the standard-illuminant curve (2.87x - 3x^2 - 0.27509507) plus the tint push.
    const float x = 0.31271f - t1 * (t1 < 0.0f ? 0.1f : 0.05f);
    const float y = 2.87f * x - 3.0f * x * x - 0.27509507f + t2 * 0.05f;

    // CIE xyY (Y = 1) -> XYZ -> CAT02 LMS of the target white.
    const float bigX = x / y;
    const float bigZ = (1.0f - x - y) / y;
    const float lmsL = 0.7328f * bigX + 0.4296f - 0.1624f * bigZ;
    const float lmsM = -0.7036f * bigX + 1.6975f + 0.0061f * bigZ;
    const float lmsS = 0.0030f * bigX + 0.0136f + 0.9834f * bigZ;

    // D65 white in the same LMS.
    outCoeffs[0] = 0.949237f / lmsL;
    outCoeffs[1] = 1.03542f / lmsM;
    outCoeffs[2] = 1.08728f / lmsS;
}

} // namespace

void FillColorGradeParamsUBO(const PostProcessSettings& settings, ColorGradeParamsUBO& out)
{
    out.shadowsR = settings.ColorGradeShadowsR;
    out.shadowsG = settings.ColorGradeShadowsG;
    out.shadowsB = settings.ColorGradeShadowsB;
    out.shadowsMaster = settings.ColorGradeShadowsMaster;
    out.midtonesR = settings.ColorGradeMidtonesR;
    out.midtonesG = settings.ColorGradeMidtonesG;
    out.midtonesB = settings.ColorGradeMidtonesB;
    out.midtonesMaster = settings.ColorGradeMidtonesMaster;
    out.highlightsR = settings.ColorGradeHighlightsR;
    out.highlightsG = settings.ColorGradeHighlightsG;
    out.highlightsB = settings.ColorGradeHighlightsB;
    out.highlightsMaster = settings.ColorGradeHighlightsMaster;

    // Delta encoding so a zero-filled placeholder reads as an identity grade.
    out.contrastMinusOne = settings.ColorGradeContrast - 1.0f;
    out.saturationMinusOne = settings.ColorGradeSaturation - 1.0f;
    out.gradeInLinear = (settings.ColorGradeInLog != 0) ? 0.0f : 1.0f;

    // Band limits: sanitize, then delta-encode against the shader-side defaults
    // (zero-filled placeholder == default partition).
    float shadowsStart = 0.0f, shadowsEnd = 0.0f, highlightsStart = 0.0f, highlightsEnd = 0.0f;
    SanitizeBandRange(settings.ColorGradeShadowsStart, settings.ColorGradeShadowsEnd,
                      shadowsStart, shadowsEnd);
    SanitizeBandRange(settings.ColorGradeHighlightsStart, settings.ColorGradeHighlightsEnd,
                      highlightsStart, highlightsEnd);
    out.shadowsStartDelta = shadowsStart - kColorGradeShadowsStartDefault;
    out.shadowsEndDelta = shadowsEnd - kColorGradeShadowsEndDefault;
    out.highlightsStartDelta = highlightsStart - kColorGradeHighlightsStartDefault;
    out.highlightsEndDelta = highlightsEnd - kColorGradeHighlightsEndDefault;

    // White balance: gated on authored non-zero so neutral stays EXACTLY identity
    // (the D65/D65 ratio is ~1 +- 1 ulp, which would defeat the shader's
    // wbActive gate and perturb every graded frame by a matrix round-trip).
    const float temperature = std::clamp(settings.ColorGradeTemperature,
                                         -kColorGradeTemperatureTintRange,
                                         kColorGradeTemperatureTintRange);
    const float tint = std::clamp(settings.ColorGradeTint, -kColorGradeTemperatureTintRange,
                                  kColorGradeTemperatureTintRange);
    if (temperature != 0.0f || tint != 0.0f)
    {
        float coeffs[3];
        ColorBalanceToLmsCoeffs(temperature, tint, coeffs);
        out.whiteBalanceLMinusOne = coeffs[0] - 1.0f;
        out.whiteBalanceMMinusOne = coeffs[1] - 1.0f;
        out.whiteBalanceSMinusOne = coeffs[2] - 1.0f;
    }

    out.hueShiftTurns = std::clamp(settings.ColorGradeHueShift, -kColorGradeHueShiftRangeDegrees,
                                   kColorGradeHueShiftRangeDegrees) /
                        360.0f;
}

} // namespace GameEngine::Engine::Renderer
