#ifndef GE_PERCEPTUAL_LUMA_GLSL
#define GE_PERCEPTUAL_LUMA_GLSL

// Perceptual luma for the post-AA edge detectors (FXAA, SMAA).
//
// Their thresholds and gradient tests are perceptual judgements, and both
// techniques specify NON-LINEAR input for them. This engine's LDR chain does
// not provide that: the tonemap emits display-referred LINEAR, because exactly
// one terminal pass owns the output transfer function
// (Rendering/Passes/FinalizeContract.h). Detecting on raw linear RGB misses
// shadow edges and over-filters highlights, so encode here instead.
//
// The curve is gamma 2.0 up to paper white, log-continued above it:
//
//   Y'(l) = sqrt(l)              l <= 1
//         = 1 + 0.5 * ln(l)      l >  1
//
// Below paper white this is the standard gamma-2.0 approximation of sRGB — the
// transfer FXAA's own integration note recommends.
//
// Above it matters because HDR output is a live configuration: with an HDR
// swapchain the tonemap emits its headroom arm, so this pass reads values up to
// 20x paper white (GetHdrOutputMaxLinearValue clamps there) and the AA nodes run
// on them unconditionally. Plain gamma 2.0 keeps growing there and holds edge
// sensitivity flat with brightness, whereas PQ — what an HDR display actually
// applies — becomes steadily less sensitive as luminance rises. The log
// continuation reproduces that trend to within ~10% of a PQ reference, against
// ~130% for gamma 2.0 alone. A BOUNDED continuation is not an option: once the
// encode saturates, bright edges have no encoded range left and the gate stops
// firing on them entirely.
//
// The join is C1 — both sides give value 1.0 and slope 0.5 at l == 1 — so an
// SDR frame that overshoots 1.0 slightly (post-chain sharpen, bloom) sees no
// kink, and SDR content below paper white is bit-identical to plain gamma 2.0.
//
// Weights are applied to the ENCODED channels, which is what Rec.601 luma
// (Y' over R'G'B') means; applying them to linear RGB would be luminance, a
// different quantity. Detection only: callers blend linear colour, which is
// what a coverage reconstruction should average in.
vec3 GE_PerceptualEncode(vec3 c)
{
    c = max(c, vec3(0.0));
    // max() inside log keeps its argument >= 1 so the unselected branch cannot
    // produce -inf for a black texel.
    return mix(sqrt(c), vec3(1.0) + 0.5 * log(max(c, vec3(1.0))), greaterThan(c, vec3(1.0)));
}

float GE_PerceptualLuma(vec3 c)
{
    return dot(GE_PerceptualEncode(c), vec3(0.299, 0.587, 0.114));
}

#endif // GE_PERCEPTUAL_LUMA_GLSL
