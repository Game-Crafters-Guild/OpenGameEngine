#version 450

#include "Includes/tonemap_gt7.glsl"
#include "Includes/tonemap_aces2.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uHDRColor;

// Auto-exposure history (AutoExposureNode). exposureScale is the metered+adapted linear multiplier;
// read only when useAutoExposure != 0. Always bound (the node publishes it every frame) so the
// fullscreen stage never skips for a missing buffer.
layout(set = 0, binding = 1, std430) readonly buffer ExposureHistory {
    float exposureScale;
    uint  valid;
} uExposure;

layout(push_constant) uniform TonemapPC {
    float exposure;
    int tonemapMode;  // 0=ACES, 1=Reinhard, 2=AgX, 3=Filmic, 4=Neutral, 5=Linear,
                      // 6=Gran Turismo 7, 7=ACES 2
    int ditherMode;   // 98-99=debug views. This pass owns no dither at all — the terminal
                      // FinalSRGBEncode does — so no value here selects one.
    int outEncoding;  // 1=SDR display-referred linear (undithered — the terminal FinalSRGBEncode
                      // owns the quantization step and both its filters), 4=HDR linear (headroom),
                      // 5=luminance heatmap (debug). The output transfer function
                      // (sRGB/PQ/HLG/scRGB) is applied solely by FinalSRGBEncode.
    float paperWhiteNits;
    float maxOutputNits;
    int preserveAlpha; // 0 = opaque; 1 = pass source .a; 2 = derive RGB coverage for UI
    int useAutoExposure; // 0 = use pc.exposure; 1 = read the metered exposure from uExposure
    float ictcpChromaCompression; // effective HDR10/HDR10+ strength; 0 = exact pass-through
} pc;

// Canonical HDR paper-white floor (framebuffer 1.0 == 80 nits anchor). Mirrors
// Rendering::kHdrPaperWhiteFloorNits and the same const in encode_srgb.frag.
const float kHdrPaperWhiteFloorNits = 80.0;

#include "Includes/ictcp.glsl"

// --- Tonemap operators ---

// ACES filmic tonemap via the RRT+ODT polynomial fit with input/output colour matrices (Stephen
// Hill's reference fit, no input pre-scale). The matrices tone-map in a shared working space and
// restore hue far better than the cheap single-channel fit. They are written COLUMN-major for
// GLSL; each row sums to 1 to within 1e-5 (kAcesOut's third row is 0.999990), so a neutral grey
// stays neutral to well under a code. Reference
// placement: exposed 0.18 lands at display-linear ~0.106. Brightness anchoring is the exposure
// system's job (kDefaultAutoExposureBiasEv), never a scale hidden in the operator.
// Adapted from ACESFitted/RRTAndODTFit in TheRealMJP/BakingLab ACES.hlsl (fit by Stephen Hill,
// @self_shadow): Copyright (c) 2016 MJP, MIT License.
// See ThirdParty/BakingLabAces/LICENSE.txt and UPSTREAM.md.
vec3 TonemapACES(vec3 v)
{
    const mat3 kAcesIn = mat3(
        0.59719, 0.07600, 0.02840,   // column 0
        0.35458, 0.90834, 0.13383,   // column 1
        0.04823, 0.01566, 0.83777);  // column 2
    const mat3 kAcesOut = mat3(
         1.60475, -0.10208, -0.00327,
        -0.53108,  1.10813, -0.07276,
        -0.07367, -0.00605,  1.07602);
    v = kAcesIn * v;
    // Upstream's numerator carries a -0.000090537 constant, which puts the curve BELOW ZERO for
    // working-space values under 0.003253 — so the terminal clamp maps that whole span to a flat
    // black floor, and near-black gradation is destroyed rather than compressed. A channel enters
    // it in order of magnitude, so on a sky-lit shadow red floors out while green and blue are
    // still resolving: the surface loses a channel instead of darkening. Dropping the constant is
    // what removes the floor — the curve then passes exactly through the origin, stays strictly
    // increasing, and is linear in the limit, so the darkest tones keep their ratios. It is the
    // only term that decides the sign at zero and its influence decays as 1/v^2: mid-grey moves
    // +0.25% (0.18 still lands at display-linear ~0.106) and a neutral ramp moves at most 2 codes.
    vec3 num = v * (v + 0.0245786);
    vec3 den = v * (0.983729 * v + 0.432951) + 0.238081;
    v = kAcesOut * (num / den);
    return clamp(v, 0.0, 1.0);
}

vec3 TonemapReinhard(vec3 v)
{
    return v / (v + vec3(1.0));
}

// Exact inverse of the terminal sRGB OETF (encode_srgb.frag LinearToSRGB / a
// hardware-sRGB swapchain). Used to undo a display-referred curve baked into an
// operator's output so it re-enters this pass's display-LINEAR contract.
vec3 SrgbToLinear(vec3 c)
{
    vec3 low = c / 12.92;
    vec3 high = pow((max(c, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(c, vec3(0.04045)));
}

// Jim Hejl / Richard Burgess-Dawson filmic curve. This SPECIFIC rational fit has
// the sRGB OETF baked into its output BY DESIGN — it was published as "no gamma
// correction needed," authored to go straight to an sRGB display. (NOT the
// Uncharted 2 / Hable curve, which is different and returns linear — the
// SrgbToLinear below is valid ONLY for this gamma-baking HBD fit; swapping in a
// linear-output curve without removing it would double-UN-encode.) This pass's
// contract is that every operator returns display-referred LINEAR and the single
// terminal FinalSRGBEncode (or hardware sRGB) applies the OETF once;
// ACES/Reinhard/AgX/Neutral/Linear all honor that. So we undo the baked curve
// here (inverse sRGB) to return linear like the others — otherwise the terminal
// pass encodes a second time and grey 0.18 lands at 189/255 instead of its
// designed 130/255.
vec3 TonemapFilmic(vec3 v)
{
    vec3 x = max(v - vec3(0.004), 0.0);
    vec3 srgbEncoded = (x * (6.2 * x + 0.5)) / (x * (6.2 * x + 1.7) + 0.06);
    return SrgbToLinear(srgbEncoded);
}

// AgX display transform (Troy Sobotka's AgX, github.com/sobotka/AgX).
// sRGB linear → AgX working space, log2 encoding, polynomial sigmoid.
// Pipeline structure, EV range, and the sigmoid fit follow Benjamin Wrensch's "Minimal AgX
// Implementation": Copyright (c) 2024 Missing Deadlines (Benjamin Wrensch), MIT License.
// See ThirdParty/MinimalAgX/LICENSE.txt and UPSTREAM.md — including for how the inset/outset
// matrices below differ from Wrensch's printed values.
vec3 TonemapAgX(vec3 v)
{
    // sRGB-linear → AgX inset working space. The applied matrix, row-major;
    // each row sums to ~1 so the achromatic axis is preserved — a neutral grey
    // stays neutral:
    //   0.842479  0.078434  0.079224
    //   0.042374  0.878468  0.079224
    //   0.042342  0.078434  0.879143
    // Transcribed COLUMN-major for the GLSL mat3 constructor (which takes columns,
    // NOT rows — pasting the reference rows in directly transposes the matrix and
    // breaks the achromatic axis, tinting all AgX content warm).
    const mat3 agxInset = mat3(
        0.842479062253094,  0.0423738568909378, 0.0423421658632190,   // column 0
        0.0784335999206980, 0.878468162822090,  0.0784336006085089,   // column 1
        0.0792237451477643, 0.0792237451477643, 0.879142973793946     // column 2
    );
    vec3 agx = agxInset * max(v, vec3(1e-10));
    // Log2 encoding: map [minEV, maxEV] → [0, 1]
    const float minEV = -12.47393;
    const float maxEV = 4.026069;
    agx = clamp((log2(agx) - minEV) / (maxEV - minEV), 0.0, 1.0);
    // 6th order polynomial sigmoid approximation
    vec3 x2 = agx * agx;
    vec3 x4 = x2 * x2;
    vec3 sig = max(15.5 * x4 * x2 - 40.14 * x4 * agx + 31.96 * x4 - 6.868 * x2 * agx + 0.4298 * x2 + 0.1191 * agx - 0.00232, vec3(0.0));
    // EOTF: outset (inverse of the inset) restores chroma, then ^2.2 returns to scene-linear for the
    // downstream encode. Without the outset the result is the washed-out, desaturated "minimal" AgX.
    // Column-major for GLSL mat3; rows of the applied matrix sum to 1 (achromatic axis preserved).
    const mat3 agxOutset = mat3(
         1.1968800908, -0.0529602234, -0.0529204415,   // column 0
        -0.0980216014,  1.1519157412, -0.0980482752,   // column 1
        -0.0990233529, -0.0990320966,  1.1510759197);  // column 2
    return pow(max(agxOutset * sig, vec3(0.0)), vec3(2.2));
}

// Khronos PBR Neutral display mapping: compresses highlights toward white while preserving hue
// and near-white saturation (a gentle, non-filmic curve that keeps material albedo faithful).
// PBRNeutralToneMapping from KhronosGroup/ToneMapping PBR_Neutral/pbrNeutral.glsl:
// Copyright 2024 The Khronos Group, Inc., Apache-2.0.
// See ThirdParty/KhronosPbrNeutral/LICENSE.txt and UPSTREAM.md.
vec3 TonemapNeutral(vec3 v)
{
    const float startCompression = 0.8 - 0.04;
    const float desaturation = 0.15;
    float x = min(v.r, min(v.g, v.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    v -= offset;
    float peak = max(v.r, max(v.g, v.b));
    if (peak < startCompression)
        return max(v, vec3(0.0));
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    v *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(v, newPeak * vec3(1.0), g);
}

// Identity curve after exposure: no tone mapping at all, so values above 1.0 survive to
// the output-range stage (differs from Neutral, which compresses highlights toward white).
vec3 TonemapLinear(vec3 v)
{
    return max(v, vec3(0.0));
}

float HdrOutputMaxLinearValue()
{
    float paperWhiteNits = max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits);
    float maxOutputNits = max(pc.maxOutputNits, paperWhiteNits);
    return max(maxOutputNits / paperWhiteNits, 1.0);
}

// ACES 2 tier headroom: the display's paper-white-relative peak under an HDR
// encode, exactly 1.0 (the 100-nit tier, t = 0) under SDR encodes.
float Aces2TargetOutputMax()
{
    return (pc.outEncoding >= 2 && pc.outEncoding <= 4) ? HdrOutputMaxLinearValue() : 1.0;
}

vec3 ApplyTonemap(vec3 hdr)
{
    switch (pc.tonemapMode)
    {
        case 0:  return TonemapACES(hdr);
        case 1:  return TonemapReinhard(hdr);
        case 2:  return TonemapAgX(hdr);
        case 3:  return TonemapFilmic(hdr);
        case 4:  return TonemapNeutral(hdr);
        case 5:  return TonemapLinear(hdr);
        case 6:  return TonemapGranTurismo7(hdr);
        case 7:  return TonemapACES2(hdr, Aces2TargetOutputMax());
        default: return TonemapACES(hdr);
    }
}

// The output transfer functions (sRGB / PQ / HLG / scRGB) live solely in the
// terminal FinalSRGBEncode pass (encode_srgb.frag). This pass only tonemaps to
// display-referred LINEAR (SDR clamped, or HDR headroom) and never encodes.

vec3 ApplyHdrDisplayShoulder(vec3 sceneLinear)
{
    vec3 x = max(sceneLinear, vec3(0.0));
    const float shoulderStart = 1.0;
    float paperWhiteNits = max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits);
    float maxOutputNits = max(pc.maxOutputNits, paperWhiteNits);
    float peakSceneWhite = max(maxOutputNits / paperWhiteNits, shoulderStart + 0.001);
    vec3 above = max(x - vec3(shoulderStart), vec3(0.0));
    vec3 rolled = vec3(shoulderStart) + above / (vec3(1.0) + above / (peakSceneWhite - shoulderStart));
    return min(mix(x, rolled, step(vec3(shoulderStart), x)), vec3(peakSceneWhite));
}

// Extends an operator's OWN display-referred output into the display's headroom.
//
// The parameter is what the selected operator produced, never the scene. A tone
// mapper does two jobs — it compresses brightness AND it renders colour
// (desaturates highlights, holds hue, compresses out-of-gamut) — and only the
// operator knows what colour a highlight should be. This applies one positive
// scalar to its result, so chromaticity is preserved exactly and the colour
// rendering cannot be discarded. `sceneLinear` is deliberately NOT a parameter:
// the way to lose that guarantee is to reach for the scene value here.
//
// Below the knee nothing moves. Above it the excess is expanded by the exact
// inverse of a Reinhard shoulder: C1-continuous at the knee (slope 1, no visible
// kink) and mapping operator 1.0 onto `outputMax` exactly. An operator that
// clamps at 1.0 (ACES, GT7) therefore lands on the panel's peak
// luminance, while one that only approaches it (Reinhard, Filmic, Neutral, and
// AgX — whose polynomial sigmoid tops out at 0.9971) lands short. That is each
// curve's own answer, and the right one. Note the slope near the top is
// ((outputMax - knee) / (1 - knee))^2, so a small difference in an operator's
// ceiling is a large difference in peak nits: AgX's 0.3% shortfall costs it 14%.
//
// The knee is the lowest 0.05-grid value that keeps every operator's rendering
// of a neutral scene 1.0 at or below paper white, so diffuse white stays
// anchored near BT.2408 reference white and the headroom goes to speculars and
// emitters. Neutral binds it: it renders scene 1.0 at 0.869, landing at 0.966
// here (196 nits at a 203-nit paper white); a 0.70 knee would put that at 214.
vec3 ApplyHdrHighlightExtension(vec3 displayReferred, float outputMax)
{
    const float kExtensionKnee = 0.75;
    const float kOperatorHeadroom = 1.0 - kExtensionKnee;

    vec3 v = max(displayReferred, vec3(0.0));
    float peak = max(max(v.r, v.g), v.b);
    if (peak <= kExtensionKnee)
        return v;

    // c maps an excess of (1.0 - knee) onto (outputMax - knee). Clamping the
    // excess keeps the denominator positive for any input, including an operator
    // that overshoots 1.0.
    float excess = min(peak - kExtensionKnee, kOperatorHeadroom);
    float c = 1.0 / kOperatorHeadroom - 1.0 / (outputMax - kExtensionKnee);
    float extendedPeak = kExtensionKnee + excess / (1.0 - c * excess);
    return v * (extendedPeak / peak);
}

vec3 ApplyOutputRangeTonemap(vec3 sceneLinear, vec3 sdrTonemapped)
{
    if (pc.outEncoding < 2 || pc.outEncoding > 4)
        return sdrTonemapped;

    // Linear is the one mode with no colour rendering to preserve — it applies no
    // curve at all — so it rolls the raw scene value into the headroom instead.
    if (pc.tonemapMode == 5)
        return ApplyHdrDisplayShoulder(sceneLinear);

    float outputMax = HdrOutputMaxLinearValue();
    if (outputMax <= 1.001)
        return clamp(sdrTonemapped, vec3(0.0), vec3(1.0));

    // ACES 2 is peak-parameterised: ApplyTonemap already produced
    // display-referred output spanning [0, outputMax] from its tier ladder, so
    // like Linear it bypasses the highlight extension — extending it would
    // apply headroom twice. Clamp only.
    if (pc.tonemapMode == 7)
        return min(sdrTonemapped, vec3(outputMax));

    // Every other mode, present and future: extend what IT produced. A new
    // operator needs no edit here, which is why this is not a switch.
    return min(ApplyHdrHighlightExtension(sdrTonemapped, outputMax), vec3(outputMax));
}

vec3 ApplyIctcpHighlightChromaCompression(vec3 rec709Linear)
{
    // Exact pass-through at zero strength: no clamp, no matrix round trip.
    if (pc.ictcpChromaCompression <= 0.0)
        return rec709Linear;

    const vec3 compressed =
        GE_IctcpCompressHighlightChroma(rec709Linear,
                                        pc.ictcpChromaCompression,
                                        max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits),
                                        pc.maxOutputNits);
    return min(compressed, vec3(HdrOutputMaxLinearValue()));
}

vec3 LuminanceHeatmap(vec3 sceneLinear)
{
    const vec3 lumaWeights = vec3(0.2126, 0.7152, 0.0722);
    float nits = dot(max(sceneLinear, vec3(0.0)), lumaWeights) * max(pc.paperWhiteNits, kHdrPaperWhiteFloorNits);
    float t = clamp(log2(max(nits, 0.01) / 100.0) / 6.0 + 0.5, 0.0, 1.0);
    return mix(vec3(0.0, 0.1, 0.8), mix(vec3(0.0, 0.9, 0.2), vec3(1.0, 0.1, 0.0), smoothstep(0.45, 1.0, t)), smoothstep(0.0, 0.55, t));
}

void main()
{
    vec2 uv = vec2(vUV.x, vUV.y);

    // Auto-exposure (eye adaptation) overrides the static exposure when active; otherwise the
    // Fixed/Manual/Physical scalar from the push constant is used (byte-identical when off).
    // Guarded like bloom_threshold: metering can decline (shaderpkg race, missing HDR source),
    // leaving the history buffer unwritten — a non-positive/NaN/absurd scale falls back to the
    // static exposure instead of a persistent black or blown-out frame.
    float autoExposure = uExposure.exposureScale;
    float exposure = (pc.useAutoExposure != 0 && autoExposure > 0.0 && autoExposure < 1e9)
                         ? autoExposure
                         : pc.exposure;

    // Debug modes
    if (pc.ditherMode == 99) { oColor = vec4(1.0, 0.0, 1.0, 1.0); return; }
    if (pc.ditherMode == 98) { vec3 raw = texture(uHDRColor, uv).rgb * exposure; oColor = vec4(raw, 1.0); return; }

    vec3 hdr = max(texture(uHDRColor, uv).rgb * exposure, vec3(0.0));
    vec3 lin = ApplyTonemap(hdr);
    lin = ApplyOutputRangeTonemap(hdr, lin);
    lin = ApplyIctcpHighlightChromaCompression(lin);
    // Default to opaque (1.0) so the main view is unchanged. Lens-flare thumbnails
    // use mode 2 because their RGB atlas is luminous data on black rather than a
    // conventional alpha texture. Normalize the straight RGB representation before
    // the HDR UI pass premultiplies it, preserving flare color and brightness while
    // still exposing a useful coverage alpha.
    float outA = 1.0;
    if (pc.preserveAlpha == 2)
    {
        outA = clamp(max(lin.r, max(lin.g, lin.b)), 0.0, 1.0);
        lin = outA > 1e-5 ? lin / outA : vec3(0.0);
    }
    else if (pc.preserveAlpha != 0)
    {
        outA = texture(uHDRColor, uv).a;
    }
    // HDR: emit display-referred LINEAR (headroom-tonemapped); FinalSRGBEncode
    // applies PQ/HLG/scRGB.
    if (pc.outEncoding == 4) { oColor = vec4(lin, outA); return; }
    if (pc.outEncoding == 5) { oColor = vec4(LuminanceHeatmap(hdr), 1.0); return; }

    // SDR: display-referred LINEAR, UNDITHERED, whatever the swapchain is. The
    // terminal FinalSRGBEncode owns the quantization step and therefore owns
    // both filters, sized to the real destination — a step this pass cannot see.
    // Unconditional on purpose: a blueprint still carrying the retired
    // outEncoding 0 lands here rather than on a hole.
    oColor = vec4(lin, outA);
}
