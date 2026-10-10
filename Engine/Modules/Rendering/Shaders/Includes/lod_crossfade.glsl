// Packed LOD-crossfade channel: the bit layout of the per-record indirection
// word plus the dither that decodes it — shared by the producer
// (draw_command_scatter.comp), the vertex-stage unpack (instance_io.glsl) and
// BOTH fragment consumers, the depth-only and the colour form of
// adapter_forward.glsl.
//
// The word is otherwise a bare GPUScene instance index. The crossfade rides in
// the high byte, which is ZERO on every non-fading record — so a stream
// produced with the feature off is byte-identical to one produced before the
// channel existed.
//
//   bits  0..23  GPUScene instance index. A 24-bit domain, the same cap the
//                batch table asserts for its mesh axis (kMeshParityBit).
//   bits 24..30  w7 — the dither threshold in 1/127ths. 0 means NOT FADING,
//                which is why a fading record's weight is clamped to 1..127
//                and a fully-faded transition drops back to one record.
//   bit  31      phase — which side of the complementary dither test this
//                record keeps. 0 = incoming level, 1 = outgoing level. Both
//                records of a pair carry the SAME weight, so the two tests
//                partition the pixels exactly.
//
// C++ mirror: GPUDrawStreamBuilder::kLodFade* (pinned by BatchScatterComputeTests).

#ifndef GE_LOD_CROSSFADE_GLSL
#define GE_LOD_CROSSFADE_GLSL

// Unconditional: every reader of the indirection word must strip the channel,
// whether or not it decodes it.
const uint kLodFadeIndexMask = 0x00FFFFFFu;

// The rest is needed only by the producer — which defines GE_LOD_FADE_PACKING
// ahead of this include, having no variant keyword of its own — and by the
// keyword-enabled consumers. Guarded so a variant that merely masks the index
// does not carry unused constants into its SPIR-V.
#if defined(GE_LOD_FADE_PACKING) || defined(GE_LOD_CROSSFADE)

const uint  kLodFadeWeightShift = 24u;
const uint  kLodFadeWeightMask  = 0x7Fu;
const uint  kLodFadeMaxWeight   = 127u;
const uint  kLodFadePhaseBit    = 0x80000000u;
const float kLodFadeWeightScale = 1.0 / 127.0;

// Fade state packing (draw_command_scatter.comp binding 11, uvec2.x). Four bits
// per level covers kMaxMeshLODs with room to spare; .y is the start time.
const uint kLodFadeLevelMask    = 0xFu;
const uint kLodFadeToLevelShift = 4u;

#endif

#ifdef GE_LOD_CROSSFADE

// Screen-space ordered dither for the LOD crossfade. A transitioning instance
// draws twice — incoming level with phase 0, outgoing with phase 1, both
// carrying the same weight — and the two tests below keep exactly complementary
// pixel sets, so the pair composes to full coverage with no double-shading and
// no holes. Bayer 4x4 (16 levels) is chosen for determinism: the same pose
// produces the same mask, which is what makes an A/B capture comparable.
// A code of 0 means "not fading" and keeps every pixel.
//
// PHASE PARITY. This is the ONLY definition in the shader tree, and the depth
// prepass and the colour pass both call it on the same record's code at the same
// gl_FragCoord, so early-Z admits exactly the fragments the colour pass keeps.
// Forking it — a second copy, a different ordered-dither basis, a screen
// position derived from anything but gl_FragCoord — silently reintroduces the
// mutual z-kill the tail split exists to remove, because the two passes would
// then partition the silhouette differently.
// Pinned by LodCrossfadeShaderContractTests.
bool GE_LodCrossfadeKeep(uint fadeCode, vec2 fragCoord)
{
    if (fadeCode == 0u)
        return true;
    const uint kBayer4x4[16] = uint[16](0u, 8u, 2u, 10u,
                                        12u, 4u, 14u, 6u,
                                        3u, 11u, 1u, 9u,
                                        15u, 7u, 13u, 5u);
    ivec2 p = ivec2(fragCoord) & 3;
    float d = float(kBayer4x4[p.y * 4 + p.x]) * (1.0 / 16.0);
    float weight = float((fadeCode >> kLodFadeWeightShift) & kLodFadeWeightMask)
                 * kLodFadeWeightScale;
    return (fadeCode & kLodFadePhaseBit) == 0u ? (d < weight) : (d >= weight);
}

#endif // GE_LOD_CROSSFADE

#endif // GE_LOD_CROSSFADE_GLSL
