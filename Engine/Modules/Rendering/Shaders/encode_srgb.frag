#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uLinearColor;

// Both quantization filters below cover the WHOLE image. Which pixels they may
// touch is decided by where this pass is declared, not by anything in this
// block: a world view finalizing at its own resolve holds world pixels and
// nothing else, and a composite of already-finalized content arrives with
// ditherLsb and debandThreshold at 0 (FinalizeQuantizer::None), so a pass that
// would otherwise grain flat chrome runs neither filter.
layout(push_constant) uniform OutputEncodePC {
    int outEncoding;       // 1=sRGB, 2=HDR10 PQ, 3=HLG, 4=scRGB/extended linear;
                           // 5/6 = input ALREADY sRGB-encoded (#767 P6b Finalize):
                           // 5 requantizes the raw bytes (UNORM dst), 6 writes the
                           // sRGB decode D(c) so an _SRGB dst's ROP re-encode
                           // round-trips byte-exact
    float paperWhiteNits;  // SDR white level in HDR output modes
    float ditherLsb;       // dither step: ONE LSB of the destination's quantizer
                           // (1/255 8-bit, 1/1023 10-bit); 0 disables
    float scRGBRefWhiteNits; // luminance of framebuffer 1.0 in scRGB mode (80 = Windows convention)
    float debandThreshold; // deband gate in OUTPUT-ENCODED units (N output LSBs); <= 0 disables
    float debandRadius;    // first-iteration deband tap radius in pixels
    float ditherPhase;     // temporal dither phase in [0,1): a wrapped shift of the
                           // dither's noise sample, advancing per movie/render frame.
                           // 0 = the screen-space-static pattern (every shipped path
                           // unless a temporal-dither toggle is on). Applies to the
                           // TPDF dither ONLY — never to the deband, whose taps must
                           // not move per frame (see TriangularDither below)
} pc;

// Canonical HDR paper-white floor: the scRGB anchor where framebuffer 1.0 == 80
// nits. Mirrors Rendering::kHdrPaperWhiteFloorNits and the same const in
// tonemap.frag / ui_sdf_common.glsl.
const float kHdrPaperWhiteFloorNits = 80.0;

vec3 LinearToSRGB(vec3 c)
{
    vec3 low = 12.92 * c;
    vec3 high = 1.055 * pow(max(c, vec3(1e-6)), vec3(1.0/2.4)) - 0.055;
    return mix(high, low, lessThanEqual(c, vec3(0.0031308)));
}

// Exact sRGB EOTF (the inverse of LinearToSRGB), for the outEncoding 6 arm:
// the shader hands the _SRGB destination D(c) and the ROP's fixed-function
// re-encode E(D(c)) must reproduce the input byte for all 256 levels —
// pinned by SrgbEncodeRampRoundTripTests (#767 §5 measurement 5).
vec3 SRGBToLinear(vec3 c)
{
    vec3 low = c / 12.92;
    vec3 high = pow((max(c, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(c, vec3(0.04045)));
}

vec3 Rec709ToBT2020(vec3 c)
{
    mat3 m = mat3(
        0.6274040, 0.0690970, 0.0163916,
        0.3292820, 0.9195400, 0.0880132,
        0.0433136, 0.0113612, 0.8955950);
    return max(m * c, vec3(0.0));
}

float LinearNitsToPQ(float nits)
{
    nits = clamp(nits / 10000.0, 0.0, 1.0);
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 32.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 128.0;
    const float c3 = 2392.0 / 128.0;
    float p = pow(nits, m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

vec3 LinearSceneToHDR10PQ(vec3 sceneLinear)
{
    vec3 bt2020 = Rec709ToBT2020(sceneLinear);
    float paperWhite = max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits);
    return vec3(
        LinearNitsToPQ(bt2020.r * paperWhite),
        LinearNitsToPQ(bt2020.g * paperWhite),
        LinearNitsToPQ(bt2020.b * paperWhite));
}

float LinearToHLG(float x)
{
    x = max(x, 0.0);
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    return x <= (1.0 / 12.0) ? sqrt(3.0 * x) : a * log(12.0 * x - b) + c;
}

vec3 LinearSceneToHLG(vec3 sceneLinear)
{
    const float hlgReferencePeakNits = 1000.0;
    float paperWhiteNits = max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits);
    vec3 bt2020 = Rec709ToBT2020(max(sceneLinear, vec3(0.0)) * (paperWhiteNits / hlgReferencePeakNits));
    return clamp(vec3(LinearToHLG(bt2020.r), LinearToHLG(bt2020.g), LinearToHLG(bt2020.b)), 0.0, 1.0);
}

// Interleaved gradient noise — screen-space, temporally stable, low visual
// artefacting. Returns a value in [0, 1).
float InterleavedGradientNoise(vec2 screenPos, float offset)
{
    vec2 p = screenPos + vec2(offset, offset * 0.7);
    return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y));
}

// The quantizing transfer curve for the active output (sRGB / PQ / HLG).
// scRGB (outEncoding 4) never reaches this — it is a linear passthrough with
// no quantizer at this stage, so it neither dithers nor debands. Encoded
// inputs (5/6) pass through raw: the source already holds output-encoded
// values, which is exactly the domain deband/dither operate in.
vec3 EncodeForOutput(vec3 src)
{
    if (pc.outEncoding == 2)
        return LinearSceneToHDR10PQ(src);
    if (pc.outEncoding == 3)
        return LinearSceneToHLG(src);
    if (pc.outEncoding >= 5)
        return src;
    return LinearToSRGB(src);
}

vec3 SampleEncoded(vec2 uv)
{
    return EncodeForOutput(max(texture(uLinearColor, uv).rgb, vec3(0.0)));
}

// ── Gradient-aware deband (pre-dither) ──────────────────────────────────────
// The banding this removes is born UPSTREAM of this pass: 8-bit-authored
// albedo gradients (1-count steps, clean-quantized, undithered) stretched by
// scene exposure gain into 4-7-output-LSB staircases with plateaus tens of
// pixels wide. The 1-LSB TPDF below cannot break steps several LSB tall, so
// shallow staircases are detected and smoothed here first — in OUTPUT-ENCODED
// space, the domain that step class was measured in and the domain the
// threshold (pc.debandThreshold, N output LSBs) is expressed in.
//
// Weber-style gate, one radius class per iteration: average four taps on a
// per-pixel rotated cross whose radius grows with the iteration (covers
// plateau widths that scale with camera distance); if EVERY channel of
// |avg - center| is under the threshold this is a shallow staircase and the
// result moves to that average (largest passing radius wins). A real edge or
// texture detail exceeds the threshold in at least one channel and passes
// through untouched — the all-channel gate also keeps hue stable (no
// per-channel partial blends).
//
// Taps clamp to this image's own edge, and because the image is one view's
// world colour that edge is the viewport's — a tap can no longer average a
// sample from across a viewport border into a pixel just inside it.
//
// Every iteration gates against the ORIGINAL center, never the evolving
// result: chaining sub-threshold steps would let a pixel drift up to
// iterations x threshold near edge-adjacent gradients (measured as 17-LSB
// halos in an earlier build of this filter). Gating on the original bounds
// total displacement to ONE threshold by construction — the strongest
// edge-safety guarantee available — while a staircase needs moves of only
// s/2 < threshold for full smoothing, so no band-breaking power is lost.
// Taps always sample the source texture, so the filter is a pure function of
// the original image. Residual sub-threshold structure is then broken by the
// TPDF dither.
const int kDebandIterations = 3;

vec3 DebandEncoded(vec3 encCenter, vec2 uv)
{
    vec2 texel = 1.0 / vec2(textureSize(uLinearColor, 0));
    // Two decorrelated per-pixel uniforms drive tap distance and rotation;
    // golden-ratio increments decorrelate the iterations from each other.
    float u1 = InterleavedGradientNoise(gl_FragCoord.xy, 5.0);
    float u2 = InterleavedGradientNoise(gl_FragCoord.xy, 11.0);
    vec3 res = encCenter;
    for (int i = 1; i <= kDebandIterations; ++i)
    {
        float r1 = fract(u1 + float(i) * 0.61803399);
        float r2 = fract(u2 + float(i) * 0.61803399);
        float dist = pc.debandRadius * float(i) * (0.25 + 0.75 * r1);
        float angle = 6.2831853 * r2;
        vec2 o = dist * vec2(cos(angle), sin(angle)) * texel;
        vec3 avg = 0.25 * (SampleEncoded(uv + o)
                         + SampleEncoded(uv + vec2(-o.y, o.x))
                         + SampleEncoded(uv - o)
                         + SampleEncoded(uv + vec2(o.y, -o.x)));
        vec3 diff = abs(avg - encCenter);
        if (all(lessThan(diff, vec3(pc.debandThreshold))))
            res = avg;
    }
    return res;
}

// Triangular-PDF dither from ONE uniform sample: remap IGN's [0, 1) output to
// a triangular distribution in (-1, 1). Differencing two shifted IGN
// evaluations does NOT produce a TPDF — IGN is a linear-gradient hash, so
// IGN(p) - IGN(p + delta) collapses to a near-constant offset, i.e. no
// band-breaking power at all. The delta that shipped here was 1.3247 (the
// plastic number, not the golden ratio): the collapse magnitude is how close
// 52.9829189 * dot(vec2(0.06711056, 0.00583715), vec2(delta, 1.0 - delta))
// lands to an integer, and for 1.3247 that product is 4.99704 — 0.003 off, so
// the difference degenerates to a ~0.003 spread.
//
// pc.ditherPhase advances the realisation per frame (Rendering::Passes::
// TemporalDither owns the mapping). It shifts the UNIFORM sample and wraps,
// which leaves the distribution — and therefore the amplitude — exactly
// triangular, and keeps the phase out of InterleavedGradientNoise entirely so
// it can never reach DebandEncoded's tap geometry: a smoothing decision that
// flickered per frame is the artefact temporal dither is trying not to trade
// for. A phase of 0 is bitwise the unphased pattern, since fract() of a value
// already in [0,1) returns it unchanged.
float TriangularDither(vec2 screenPos, float phase)
{
    float u = fract(InterleavedGradientNoise(screenPos, 0.0) + phase);
    float o = 2.0 * u - 1.0;
    return sign(o) * (1.0 - sqrt(max(0.0, 1.0 - abs(o))));
}

void main()
{
    vec4 lin = texture(uLinearColor, vUV);
    vec3 c = max(lin.rgb, vec3(0.0));

    if (pc.outEncoding == 4)
    {
        // scRGB extended-linear: lift the SDR anchor from the framebuffer's
        // reference white to the configured paper-white. Windows anchors
        // framebuffer 1.0 at 80 nits (scale = paperWhite/80); macOS extended
        // linear anchors 1.0 at the reference white itself (scale = 1, the OS
        // applies the lift). Highlights above 1.0 carry through as headroom.
        // Target is FP16, so no dithering needed.
        float refWhite = max(pc.scRGBRefWhiteNits, kHdrPaperWhiteFloorNits);
        float scale = max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits) / refWhite;
        oColor = vec4(c * scale, lin.a);
        return;
    }

    // Quantizing outputs (sRGB / HDR10 PQ / HLG): encode, deband shallow
    // staircases in encoded space, then break the residual sub-LSB structure
    // with a TPDF dither at the output's 1-LSB step (pc.ditherLsb, sized to the
    // destination format in SRGBEncodePass.cpp). This pass owns the SDR dither
    // on the manual path; Tonemap emits undithered linear there.
    //
    // Dither AFTER the transfer function and immediately before the store is
    // the only correct position: the quantizer this dissolves is the ROP's
    // encoded-domain rounding, so the noise must be shaped in that domain at
    // that step. Encoding a dithered linear value instead would scale the
    // noise by the curve's local slope and mis-size it everywhere but one
    // luminance. Arm 6's decode below is the single exception, and it is
    // applied after the dither for exactly this reason.
    vec3 enc = EncodeForOutput(c);
    if (pc.debandThreshold > 0.0)
        enc = DebandEncoded(enc, vUV);
    if (pc.ditherLsb > 0.0)
        enc += vec3(TriangularDither(gl_FragCoord.xy, pc.ditherPhase)) * pc.ditherLsb;

    // Finalize arm 6 (#767 P6b): the _SRGB destination's ROP applies E() on
    // write, so hand it D(enc) and the round-trip E(D(enc)) reproduces the
    // encoded byte. The decode comes LAST — deband and dither operate at the
    // output's 1-LSB step, which exists only in the encoded domain. Clamped
    // because dither can push past the endpoints and D() is defined on [0,1].
    if (pc.outEncoding == 6)
        enc = SRGBToLinear(clamp(enc, vec3(0.0), vec3(1.0)));
    oColor = vec4(enc, lin.a);
}
