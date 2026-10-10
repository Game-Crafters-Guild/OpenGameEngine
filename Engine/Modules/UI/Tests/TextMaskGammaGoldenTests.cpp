// CPU golden tests for the bidirectional text coverage retarget
// (Shaders/UI/text_mask_gamma.glsl), P4 of the #767 ENCODE-FIRST series.
//
// Tautology guard: the shader maths is mirrored here in float, but the
// asserted property is never mirror-vs-mirror. It is the blend-model
// DEFINITION, forward-simulated in double precision: blend the corrected
// coverage in the attachment's ACTUAL space and require the displayed result
// to land where a blend in the TARGET space would have put the boosted
// coverage. A wrong closed form, span, guard, or space in the shader (and its
// mirror) fails that simulation; it cannot agree with itself.
//
// Tails are asserted AND printed — the v1 parity failure hid +16.8/-6.7
// levels behind a mean of +2, so every sweep here reports its max, never only
// a mean.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{

// ---------------------------------------------------------------------------
// Float mirror of Shaders/UI/text_mask_gamma.glsl, kept in the shader's own
// shapes. If the .glsl changes, this mirror must change with it — and the
// definition-based properties below are what keep the pair honest.
// ---------------------------------------------------------------------------
namespace shader
{
constexpr float kTextBlendGammaLinearTarget = 0.0f;
constexpr float kTextSpanFadeEnd = 1.0f / 128.0f;
constexpr float kTextSpanFadeEndEncoded = 1.0f / 128.0f;

float Clamp(float x, float lo, float hi)
{
    return std::min(std::max(x, lo), hi);
}

float Mix(float a, float b, float t)
{
    return a + (b - a) * t;
}

float Smoothstep(float e0, float e1, float x)
{
    const float t = Clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

float SrgbEncodeScalar(float linear)
{
    const float c = Clamp(linear, 0.0f, 1.0f);
    return (c <= 0.0031308f) ? (c * 12.92f) : (1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f);
}

float SrgbDecodeScalar(float encoded)
{
    const float c = Clamp(encoded, 0.0f, 1.0f);
    return (c <= 0.04045f) ? (c / 12.92f) : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

float ApplyTextContrast(float srca, float adjustedContrast)
{
    return srca + (1.0f - srca) * adjustedContrast * srca;
}

float TextTargetBlendLinear(float a, float src, float dst, float srcLin, float dstLin,
                            float blendGamma)
{
    if (blendGamma <= kTextBlendGammaLinearTarget)
        return Mix(dstLin, srcLin, a);
    const float srcG = std::pow(src, blendGamma);
    const float dstG = std::pow(dst, blendGamma);
    return SrgbDecodeScalar(std::pow(Mix(dstG, srcG, a), 1.0f / blendGamma));
}

float CorrectTextCoverage(float coverage, float glyphLuminanceLinear, float contrast,
                          float blendGamma, int blendSpaceEncoded)
{
    // The vec3 -> Rec.709 luminance dot of the .glsl is outside this mirror;
    // tests key on the luminance directly.
    const float srcLin = Clamp(glyphLuminanceLinear, 0.0f, 1.0f);
    const float src = SrgbEncodeScalar(srcLin);
    const float dst = 1.0f - src;
    const float dstLin = SrgbDecodeScalar(dst);

    const float a = ApplyTextContrast(coverage, contrast * dstLin);

    if (blendSpaceEncoded == 0)
    {
        if (blendGamma <= kTextBlendGammaLinearTarget)
            return a;
        const float wantedLin = TextTargetBlendLinear(a, src, dst, srcLin, dstLin, blendGamma);
        const float span = srcLin - dstLin;
        // Denominator guard, mirroring the .glsl: 0/0 at the exact inverse
        // poisons the mix even at weight zero.
        const float safeSpan = (span == 0.0f) ? 1.0f : span;
        return Mix(a, Clamp((wantedLin - dstLin) / safeSpan, 0.0f, 1.0f),
                   Smoothstep(0.0f, kTextSpanFadeEnd, std::abs(span)));
    }

    const float wantedLin = TextTargetBlendLinear(a, src, dst, srcLin, dstLin, blendGamma);
    const float spanEnc = src - dst;
    const float safeSpanEnc = (spanEnc == 0.0f) ? 1.0f : spanEnc;
    return Mix(a, Clamp((SrgbEncodeScalar(wantedLin) - dst) / safeSpanEnc, 0.0f, 1.0f),
               Smoothstep(0.0f, kTextSpanFadeEndEncoded, std::abs(spanEnc)));
}
} // namespace shader

// ---------------------------------------------------------------------------
// Double-precision blend-model reference: the definition the shader must meet.
// ---------------------------------------------------------------------------
namespace model
{
double Encode(double linear)
{
    const double c = std::min(std::max(linear, 0.0), 1.0);
    return (c <= 0.0031308) ? (c * 12.92) : (1.055 * std::pow(c, 1.0 / 2.4) - 0.055);
}

double Decode(double encoded)
{
    const double c = std::min(std::max(encoded, 0.0), 1.0);
    return (c <= 0.04045) ? (c / 12.92) : std::pow((c + 0.055) / 1.055, 2.4);
}

double Mix(double a, double b, double t)
{
    return a + (b - a) * t;
}

double Boost(double coverage, double adjustedContrast)
{
    return coverage + (1.0 - coverage) * adjustedContrast * coverage;
}

// Where a blend of coverage `a` between encoded endpoints src/dst lands in
// linear light, blending linearly in the space of exponent g over encoded
// values (g = 0 sentinel: linear light itself).
double TargetBlendLinear(double a, double srcEnc, double dstEnc, double g)
{
    if (g <= 0.0)
        return Mix(Decode(dstEnc), Decode(srcEnc), a);
    return Decode(std::pow(Mix(std::pow(dstEnc, g), std::pow(srcEnc, g), a), 1.0 / g));
}

// The two ACTUAL blends an attachment can perform on corrected coverage.
double DisplayedLinear_LinearBlend(double correctedCoverage, double srcEnc, double dstEnc)
{
    return Mix(Decode(dstEnc), Decode(srcEnc), correctedCoverage);
}

double DisplayedLinear_EncodedBlend(double correctedCoverage, double srcEnc, double dstEnc)
{
    return Decode(Mix(dstEnc, srcEnc, correctedCoverage));
}
} // namespace model

constexpr float kCoverages[] = {0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f,
                                0.45f, 0.50f, 0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f,
                                0.85f, 0.90f, 0.95f};

// Float mirror + double simulation: formula errors show up at 1e-1..1e-3
// linear; float noise through the guarded divisions stays under ~2e-5.
constexpr double kSimTolLinear = 5.0e-4;

} // namespace

// The shipped default path must be the boost and nothing else, exactly — this
// is the byte-identity half of the dark landing at the model level (the other
// half is the resolver pin: no constructible target space selects the encoded
// arm).
TEST(TextMaskGammaGolden, LinearArmDefaultTargetIsExactlyTheBoost)
{
    for (int sl = 0; sl <= 20; ++sl)
    {
        const float srcLin = static_cast<float>(sl) / 20.0f;
        const float dstLin = shader::SrgbDecodeScalar(1.0f - shader::SrgbEncodeScalar(srcLin));
        for (const float c : kCoverages)
        {
            const float expected = shader::ApplyTextContrast(c, 1.0f * dstLin);
            const float got = shader::CorrectTextCoverage(c, srcLin, 1.0f, 0.0f,
                                                          /*blendSpaceEncoded=*/0);
            EXPECT_EQ(expected, got) << "srcLin=" << srcLin << " coverage=" << c;
        }
    }
}

// Definition property, linear arm: a LINEAR blend of the corrected coverage
// must land where a blendGamma-space blend of the boosted coverage would have,
// under the correction's own dst guess. Swept over fractional and >1 targets —
// the knob must be sound at every expressible value, not only at 1.0.
TEST(TextMaskGammaGolden, LinearArmLandsOnTheTargetBlend)
{
    double tail = 0.0;
    for (const float g : {0.5f, 1.0f, 2.2f, 4.0f})
    {
        for (int sl = 0; sl <= 100; ++sl)
        {
            const double srcLin = sl / 100.0;
            const double srcEnc = model::Encode(srcLin);
            const double dstEnc = 1.0 - srcEnc;
            const double dstLin = model::Decode(dstEnc);
            // The property holds where the fade is fully off (the guard is
            // pinned separately).
            if (std::abs(srcLin - dstLin) < shader::kTextSpanFadeEnd * 1.01)
                continue;
            for (const float c : kCoverages)
            {
                const double a = model::Boost(c, 1.0 * dstLin);
                const float corrected = shader::CorrectTextCoverage(
                    c, static_cast<float>(srcLin), 1.0f, g, /*blendSpaceEncoded=*/0);
                const double displayed =
                    model::DisplayedLinear_LinearBlend(corrected, srcEnc, dstEnc);
                const double wanted = model::TargetBlendLinear(a, srcEnc, dstEnc, g);
                tail = std::max(tail, std::abs(displayed - wanted));
            }
        }
    }
    std::printf("[golden] linear arm tail: max |displayed-wanted| = %.3e linear\n", tail);
    EXPECT_LT(tail, kSimTolLinear);
}

// Definition property, encoded arm — the #767 flip configuration at
// blendGamma 0 (area-exact target) plus the same fractional sweep: an ENCODED
// source-over of the corrected coverage must land on the target blend.
TEST(TextMaskGammaGolden, EncodedArmLandsOnTheTargetBlend)
{
    double tail = 0.0;
    for (const float g : {0.0f, 0.5f, 1.0f, 2.2f, 4.0f})
    {
        for (int sl = 0; sl <= 100; ++sl)
        {
            const double srcLin = sl / 100.0;
            const double srcEnc = model::Encode(srcLin);
            const double dstEnc = 1.0 - srcEnc;
            const double dstLin = model::Decode(dstEnc);
            if (std::abs(srcEnc - dstEnc) < shader::kTextSpanFadeEndEncoded * 1.01)
                continue;
            for (const float c : kCoverages)
            {
                const double a = model::Boost(c, 1.0 * dstLin);
                const float corrected = shader::CorrectTextCoverage(
                    c, static_cast<float>(srcLin), 1.0f, g, /*blendSpaceEncoded=*/1);
                const double displayed =
                    model::DisplayedLinear_EncodedBlend(corrected, srcEnc, dstEnc);
                const double wanted = model::TargetBlendLinear(a, srcEnc, dstEnc, g);
                tail = std::max(tail, std::abs(displayed - wanted));
            }
        }
    }
    std::printf("[golden] encoded arm tail: max |displayed-wanted| = %.3e linear\n", tail);
    EXPECT_LT(tail, kSimTolLinear);
}

// Each arm's identity sits at its own blend space: the linear arm at the 0
// sentinel (pinned bit-exactly above), the encoded arm at blendGamma 1.0 —
// coverage that should blend linearly in the encoded space, blended by an
// encoded-space ROP, needs no correction. This is what replaces the deleted
// resolver gate ("encoded attachment drops the retarget"): the drop was the
// special case of this fixed point, hand-forced because the one-directional
// shader could not express it.
TEST(TextMaskGammaGolden, EncodedArmAtEncodedTargetIsTheIdentity)
{
    double tail = 0.0;
    for (int sl = 0; sl <= 100; ++sl)
    {
        const double srcLin = sl / 100.0;
        const double srcEnc = model::Encode(srcLin);
        const double dstLin = model::Decode(1.0 - srcEnc);
        if (std::abs(srcEnc - (1.0 - srcEnc)) < shader::kTextSpanFadeEndEncoded * 1.01)
            continue;
        for (const float c : kCoverages)
        {
            const double a = model::Boost(c, 1.0 * dstLin);
            const float corrected = shader::CorrectTextCoverage(
                c, static_cast<float>(srcLin), 1.0f, 1.0f, /*blendSpaceEncoded=*/1);
            tail = std::max(tail, std::abs(corrected - a));
        }
    }
    std::printf("[golden] encoded-arm identity tail: max |a'-a| = %.3e\n", tail);
    EXPECT_LT(tail, kSimTolLinear);
}

// Both arms are unstable at the same glyph colour — mid-grey against its own
// guessed inverse (src = 0.5 encoded) — and both must fade to the boosted
// coverage there instead of dividing by nothing. The float encode/decode
// round-trip leaves the span at ~2 ulp rather than 0, so the fade weight is
// ~1e-9, not exactly 0 — hence NEAR at 1e-6, which still fails on any real
// guard regression (a missing fade lands whole clamp-widths away).
TEST(TextMaskGammaGolden, BothArmsFadeToTheBoostAtMidGrey)
{
    const float srcLin = shader::SrgbDecodeScalar(0.5f);
    const float dstLin = shader::SrgbDecodeScalar(1.0f - shader::SrgbEncodeScalar(srcLin));
    for (const float c : kCoverages)
    {
        const float boosted = shader::ApplyTextContrast(c, 1.0f * dstLin);
        EXPECT_NEAR(boosted,
                    shader::CorrectTextCoverage(c, srcLin, 1.0f, 1.0f, /*blendSpaceEncoded=*/0),
                    1e-6f);
        EXPECT_NEAR(boosted,
                    shader::CorrectTextCoverage(c, srcLin, 1.0f, 0.0f, /*blendSpaceEncoded=*/1),
                    1e-6f);
    }
}

// ---------------------------------------------------------------------------
// The dst-aware sweep: glyph luminance x TRUE backdrop, 8x8 encoded levels
// including the four measured pairings from the #767 record. The correction
// guesses dst = 1 - src (Skia's approximation, which Chrome ships too), so
// against the TRUE backdrop the residual is the guess error, not a formula
// error. This sweep characterizes it — max per cell over coverage, in encoded
// 8-bit levels — and pins its shape so a silent change to the guess cannot
// hide.
//
// Provenance of the ceilings: computed from this model at P4 time (double
// reference). They are characterization pins of the inherited approximation,
// NOT acceptance bounds for the flip — the flip's acceptance (section-5 m3 of
// the design of record) is measured against the rendered reference, where
// both stacks share this guess and its error cancels. On the theme pairing
// the guess residual (+2.8% ink, 4.98 peak levels) is SMALLER than Chrome's
// own background-blind error (+5.1% mask, byte-identical thm/wob output).
// ---------------------------------------------------------------------------
TEST(TextMaskGammaGolden, EncodedArmDstAwareSweepTailIsCharacterized)
{
    constexpr int kLevels[] = {0, 39, 73, 110, 146, 183, 229, 255};

    double gridTail = 0.0;
    int gridTailGlyph = -1, gridTailBg = -1;
    double cellErr[8][8] = {};

    for (int gi = 0; gi < 8; ++gi)
    {
        for (int bi = 0; bi < 8; ++bi)
        {
            const double srcEnc = kLevels[gi] / 255.0;
            const double srcLin = model::Decode(srcEnc);
            const double trueDstEnc = kLevels[bi] / 255.0;
            const double trueDstLin = model::Decode(trueDstEnc);
            double worst = 0.0;
            for (const float c : kCoverages)
            {
                // Shader path: guessed dst, encoded arm, linear target.
                const float corrected = shader::CorrectTextCoverage(
                    c, static_cast<float>(srcLin), 1.0f, 0.0f, /*blendSpaceEncoded=*/1);
                const double displayedLin =
                    model::DisplayedLinear_EncodedBlend(corrected, srcEnc, trueDstEnc);
                // Perfect-knowledge ideal: boost and target blend keyed on the
                // TRUE backdrop.
                const double aTrue = model::Boost(c, 1.0 * trueDstLin);
                const double idealLin = model::Mix(trueDstLin, srcLin, aTrue);
                const double errLevels =
                    std::abs(model::Encode(displayedLin) - model::Encode(idealLin)) * 255.0;
                worst = std::max(worst, errLevels);
            }
            cellErr[gi][bi] = worst;
            if (worst > gridTail)
            {
                gridTail = worst;
                gridTailGlyph = kLevels[gi];
                gridTailBg = kLevels[bi];
            }
        }
    }

    std::printf("[golden] dst-aware sweep, encoded arm, contrast 1.0 — max |displayed-ideal| "
                "in encoded levels (rows: glyph; cols: backdrop)\n        ");
    for (const int b : kLevels)
        std::printf("%7d", b);
    std::printf("\n");
    for (int gi = 0; gi < 8; ++gi)
    {
        std::printf("  %3d: ", kLevels[gi]);
        for (int bi = 0; bi < 8; ++bi)
            std::printf("%7.2f", cellErr[gi][bi]);
        std::printf("\n");
    }
    std::printf("[golden] grid tail %.2f levels at glyph=%d bg=%d\n", gridTail, gridTailGlyph,
                gridTailBg);

    // The four measured pairings. Indices into kLevels: 255->7, 0->0, 229->6,
    // 39->1.
    const double wob = cellErr[7][0];   // #FFF on #000
    const double thm = cellErr[6][1];   // #E5E5E5 on #272727
    const double bow = cellErr[0][7];   // #000 on #FFF
    const double thmInv = cellErr[1][6]; // #272727 on #E5E5E5
    std::printf("[golden] pairings: wob %.3f thm %.3f bow %.3f thm-inv %.3f levels\n", wob, thm,
                bow, thmInv);

    // Where the guess is exact (a backdrop that IS the paint's inverse), the
    // correction must be exact — these two pairings are the flip's headline
    // cases and they carry no approximation at all.
    EXPECT_LE(wob, 0.02);
    EXPECT_LE(bow, 0.02);
    // Theme pairings carry the guess residual; ceilings sit just above the
    // computed values (4.98 / 4.62).
    EXPECT_LE(thm, 5.5);
    EXPECT_LE(thmInv, 5.1);
    // Whole-grid tail (worst cell: mid-grey glyph on black, where the guess is
    // maximally wrong; computed 29.96).
    EXPECT_LE(gridTail, 31.0);
}

// The normalized polarity metric from the record: ink attained over ink
// wanted, dark-background pairing against its light-background mirror. The
// guess makes the theme ratio 1.05 (Chrome's background-blind class — its own
// mask runs +5.1% hot on thm); a dst-aware correction would put it at 1.0.
// Pinned so the asymmetry's size is visible and cannot drift silently; wob/bow
// must be exactly symmetric (the guess is exact for both).
TEST(TextMaskGammaGolden, EncodedArmNormalizedPolarityRatioIsCharacterized)
{
    const auto inkRatio = [](int glyphLevel, int bgLevel) {
        const double srcEnc = glyphLevel / 255.0;
        const double srcLin = model::Decode(srcEnc);
        const double trueDstEnc = bgLevel / 255.0;
        const double trueDstLin = model::Decode(trueDstEnc);
        double got = 0.0, want = 0.0;
        for (const float c : kCoverages)
        {
            const float corrected = shader::CorrectTextCoverage(
                c, static_cast<float>(srcLin), 1.0f, 0.0f, /*blendSpaceEncoded=*/1);
            const double displayedLin =
                model::DisplayedLinear_EncodedBlend(corrected, srcEnc, trueDstEnc);
            const double aTrue = model::Boost(c, 1.0 * trueDstLin);
            const double idealLin = model::Mix(trueDstLin, srcLin, aTrue);
            got += std::abs(displayedLin - trueDstLin);
            want += std::abs(idealLin - trueDstLin);
        }
        return got / want;
    };

    const double wob = inkRatio(255, 0);
    const double bow = inkRatio(0, 255);
    const double thm = inkRatio(229, 39);
    const double thmInv = inkRatio(39, 229);
    std::printf("[golden] normalized ink: wob %.4f bow %.4f (ratio %.4f), thm %.4f thm-inv %.4f "
                "(ratio %.4f)\n",
                wob, bow, wob / bow, thm, thmInv, thm / thmInv);

    EXPECT_NEAR(wob / bow, 1.0, 0.001);
    // Computed 1.0500; the ceiling pins the guess residual's class. Closing
    // this to 1.0 needs a dst-aware correction (a per-primitive backdrop
    // channel), which is a named follow-up, not a constants-level change.
    EXPECT_NEAR(thm / thmInv, 1.05, 0.02);
}

// The measured stakes, demonstrated in this pixel model (direction and class;
// the record's numbers are ppem-integrated over real glyphs): a naive flip —
// mask unchanged into an encoded blend — collapses light-on-dark ink, and a
// boosted-but-unretargeted flip runs dark-on-light heavy. The record measured
// +34.9% thin (theme block) and 1.16-1.34x heavy respectively; this model
// puts the same failures at 0.60x and 1.23x. The retargeted arm holds both at
// 1.00 — which is the reason P4 exists.
TEST(TextMaskGammaGolden, TheMeasuredFailureModesAndTheirCorrection)
{
    // White on black, naive: corrected coverage == raw mask (no boost for
    // white text, no retarget).
    {
        double naive = 0.0, corrected = 0.0, ideal = 0.0;
        for (const float c : kCoverages)
        {
            naive += model::DisplayedLinear_EncodedBlend(c, 1.0, 0.0);
            const float a = shader::CorrectTextCoverage(c, 1.0f, 1.0f, 0.0f,
                                                        /*blendSpaceEncoded=*/1);
            corrected += model::DisplayedLinear_EncodedBlend(a, 1.0, 0.0);
            ideal += model::Mix(0.0, 1.0, c);
        }
        std::printf("[golden] wob ink vs area-exact: naive %.3f, retargeted %.3f\n",
                    naive / ideal, corrected / ideal);
        EXPECT_LT(naive / ideal, 0.75); // the +34.9%-thin class
        EXPECT_NEAR(corrected / ideal, 1.0, 0.01);
    }
    // Black on white, boosted but unretargeted vs retargeted.
    {
        double heavy = 0.0, corrected = 0.0, ideal = 0.0;
        for (const float c : kCoverages)
        {
            const double boosted = model::Boost(c, 1.0);
            heavy += 1.0 - model::DisplayedLinear_EncodedBlend(boosted, 0.0, 1.0);
            const float a = shader::CorrectTextCoverage(c, 0.0f, 1.0f, 0.0f,
                                                        /*blendSpaceEncoded=*/1);
            corrected += 1.0 - model::DisplayedLinear_EncodedBlend(a, 0.0, 1.0);
            ideal += 1.0 - model::Mix(1.0, 0.0, boosted);
        }
        std::printf("[golden] bow ink vs target: unretargeted %.3fx, retargeted %.3fx\n",
                    heavy / ideal, corrected / ideal);
        EXPECT_GT(heavy / ideal, 1.10); // the record's 1.16-1.34x class
        EXPECT_LT(heavy / ideal, 1.35);
        EXPECT_NEAR(corrected / ideal, 1.0, 0.01);
    }
}
