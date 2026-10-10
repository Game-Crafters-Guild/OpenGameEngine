#pragma once

// Anti-aliasing mode selection + the temporal-AA jitter primitives. The mode
// enum is the single seam future AA paths (DLSS/FSR-class temporal upscalers)
// extend: they reuse the TAA jitter + motion-vector infrastructure and slot in
// as additional enum values, so consumers switch on mode instead of growing
// parallel boolean knobs.

#include "Types/Types.h"

namespace GameEngine::Engine::Renderer
{

// Engine-wide anti-aliasing mode. MSAA is single-pass hardware multisampling;
// every other mode renders single-sample, and the MSAA sample count is honored
// only in MSAA mode.
//
// Two of them jitter the projection and resolve over frames: TAA accumulates an
// exponential history (many frames); TemporalFXAA is the Decima-2017 two-frame
// technique — spatial FXAA plus a fixed 50/50 blend with the previous frame's
// sharpened FXAA render, never an accumulated output, so it converges in two
// frames with no long ghosting trail. FXAA and SMAA are purely spatial and
// UNJITTERED: one frame in, one frame out, bit-stable on static content.
enum class AntiAliasingMode : uint8
{
    Off = 0,
    MSAA = 1,
    TAA = 2,
    // Classic single-frame FXAA: the spatial pass alone (Fast or Quality).
    FXAA = 3,
    // SMAA 1x: edge classification + analytic revectorization.
    SMAA = 4,
    // Two-frame FXAA: the spatial pass under a two-phase axis-alternating jitter, resolved
    // 50/50 against the previous frame's sharpened render.
    TemporalFXAA = 5,
};

// FXAA spatial-pass quality. Fast is the compact console reconstruction
// (5 luma taps, diagonal blend); Quality is the FXAA 3.11-style edge-walk
// search — better gradients on long, shallow edges for a few extra taps on
// edge pixels only. Both feed the same two-frame temporal resolve.
enum class FxaaQuality : uint8
{
    Fast = 0,
    Quality = 1,
};

// Per-camera override encoding (Components::Camera::AntiAliasing).
// 0 = inherit the engine default, 1..6 = AntiAliasingMode + 1.
inline constexpr uint32 kCameraAAInherit = 0u;

// Modes that carry per-view AA state on the ViewRegistry: the jitter phase for
// the temporal modes, and — for the spatial FXAA and SMAA, which stay
// unjittered — the mode itself, which is what their nodes gate on.
inline constexpr bool UsesPerViewAntiAliasingState(AntiAliasingMode mode)
{
    return mode == AntiAliasingMode::TAA || mode == AntiAliasingMode::TemporalFXAA ||
           mode == AntiAliasingMode::FXAA || mode == AntiAliasingMode::SMAA;
}

// Modes that jitter the projection. FXAA, SMAA and MSAA rasterize at the pixel
// centre; only TAA and TemporalFXAA offset it (each with its own generator
// below).
inline constexpr bool IsJitteredAntiAliasingMode(AntiAliasingMode mode)
{
    return mode == AntiAliasingMode::TAA || mode == AntiAliasingMode::TemporalFXAA;
}

// An AA mode paired with the sample count that belongs with it; the count is 1
// outside MSAA mode. Two things produce one: the per-view resolve
// (RenderServices::ResolveAntiAliasing, where SampleCount is what that view's
// color target must be created with) and the capability default below.
struct ResolvedAntiAliasing
{
    AntiAliasingMode Mode = AntiAliasingMode::Off;
    uint32 SampleCount = 1;
};

// The MSAA sample count nothing has chosen against: what a project naming no
// count gets, and the top rung of the ladder below. Higher counts remain
// selectable, they are simply not the default —
// RenderServices::SetDefaultMSAASampleCount halves this down to whatever the
// device can actually create targets with.
//
// 4x heads the ladder as a product decision taken with its cost on the table:
// on a dense grass scene 4x costs +10.3% to +15.9% of the multisampled world
// pass over 2x (Release, per-pass GPU timings, measured 2026-08-31 for the
// AA-chain cost slice; the report itself is not in the tree).
inline constexpr uint32 kDefaultMsaaSampleCount = 4u;

// The device inputs the anti-aliasing default reads (Rendering::Capabilities).
// A default-constructed value means "no device answered": MaxMsaaSamples 0 is a
// count no live backend reports, since Vulkan and Metal both floor it at 1.
struct AntiAliasingDeviceCaps
{
    uint32 MaxMsaaSamples = 0u;
    bool PrefersNoDefaultMsaa = false;
};

// 4x MSAA, else 2x, else TAA — the first rung this device can run, and the AA
// every host takes when nothing has chosen one: the engine-wide default at
// RenderServices::Initialize, the value the editor writes into a project on
// first open (MaterializeDefaultAntiAliasing), and what a packaged game applies
// for a game.config that names no mode.
//
// A rung is "supported" when MaxMsaaSamples reaches it, which reads that cap as
// "every power of two up to this one works" — the same reading
// SetDefaultMSAASampleCount's halving clamp already relies on.
//
// PrefersNoDefaultMsaa skips the MSAA rungs outright: the flag marks drivers
// whose MSAA paths are fragile (Device.h), which is exactly "the hardware does
// not support it" for a default the user never asked for. It deliberately does
// NOT reach kDefaultMsaaSampleCount, which answers a count for a mode the user
// did choose.
//
// The ladder's final -> off tail is NOT decided here and deliberately so: TAA
// applicability is per-view, not per-device. A view that cannot run TAA
// (orthographic, 2D, fixed-orientation, letterboxed, or already multisampled)
// drops it in the host's per-frame view resolve and renders unantialiased.
// Resolving TAA here therefore means "TAA where it applies, otherwise off".
constexpr ResolvedAntiAliasing ResolveDefaultAntiAliasing(AntiAliasingDeviceCaps caps)
{
    if (!caps.PrefersNoDefaultMsaa)
    {
        for (uint32 samples = kDefaultMsaaSampleCount; samples >= 2u; samples /= 2u)
        {
            if (caps.MaxMsaaSamples >= samples)
                return {AntiAliasingMode::MSAA, samples};
        }
    }
    return {AntiAliasingMode::TAA, 1u};
}

// The engine-wide AA defaults, as one value. Per-camera Camera::AntiAliasing /
// Camera::MSAASamples overrides blend against these
// (RenderServices::ResolveAntiAliasing); the render scale is deliberately NOT
// here — it is shared with dynamic resolution, which owns it.
struct AntiAliasingSettings
{
    // Off pending the TAA quality verdict. Project rendering settings,
    // Camera::AntiAliasing, GE_AA_MODE, and (for MSAA) a legacy
    // GE_MSAA_SAMPLES > 1 all raise it.
    AntiAliasingMode Mode = AntiAliasingMode::Off;
    // A concrete count, honored only in MSAA mode; every other mode renders
    // single-sample. RenderServices::Initialize replaces this no-device value
    // with the rung the device can run.
    uint32 MsaaSamples = ResolveDefaultAntiAliasing({}).SampleCount;
    FxaaQuality Fxaa = FxaaQuality::Quality;
    // TAA Halton cycle length (8 or 16). TemporalFXAA forces its own 2-phase pair.
    uint32 TaaSequenceLength = 8;
};

// Radical-inverse Halton sequence in the given base. index is 1-based: the
// 0th radical inverse is exactly 0.0, which after the -0.5 recenter would pin
// the first jitter sample to the pixel corner instead of spreading it.
inline float HaltonSequence(uint32 index, uint32 base)
{
    float result = 0.0f;
    float f = 1.0f;
    while (index > 0u)
    {
        f /= static_cast<float>(base);
        result += f * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

// Sub-pixel TAA jitter for frame `phase` of a `sequenceLength` cycle, as the
// standard Halton(2,3) pair. Returned offsets are in texels, in [-0.5, 0.5).
inline void TemporalJitterOffset(uint32 phase, uint32 sequenceLength, float& outX, float& outY)
{
    const uint32 length = sequenceLength > 0u ? sequenceLength : 1u;
    const uint32 index = (phase % length) + 1u;
    outX = HaltonSequence(index, 2u) - 0.5f;
    outY = HaltonSequence(index, 3u) - 0.5f;
}

// FXAA-cycle length: two phases, one per axis (see below).
inline constexpr uint32 kFxaaJitterSequenceLength = 2u;

// Two-frame FXAA edge jitter, Decima 2017 (SIGGRAPH "Advances in Lighting and
// AA", slides 41-43): never render at the pixel centres. Odd frames offset
// HORIZONTALLY between the centres, even frames VERTICALLY, by half a pixel.
// Each frame then holds two edge samples per pixel (left+right, or top+bottom),
// and the two frames together the four edge midpoints — a FLIPQUAD-like
// diamond, slightly inset. Offsets are in texels, matching
// TemporalJitterOffset's convention; the sign is immaterial, since a +0.5
// offset puts a sample on every pixel's right edge, which is its neighbour's
// left edge.
//
// TWO phases is what makes the two-frame resolve converge: it blends the last
// two frames 50/50 and never accumulates, so its output is a function of the
// last two positions only. With exactly two, every consecutive pair is the
// same pair and a static scene resolves to the same image every frame. A
// four-position rotation (an earlier misreading of the pattern) cycled through
// four different blends and oscillated at every edge.
inline constexpr float kFxaaEdgeJitterTexels = 0.5f;
inline void TemporalFxaaJitterOffset(uint32 phase, float& outX, float& outY)
{
    const bool odd = (phase & 1u) != 0u;
    outX = odd ? kFxaaEdgeJitterTexels : 0.0f;
    outY = odd ? 0.0f : kFxaaEdgeJitterTexels;
}

// Add a constant NDC-space offset to a column-major clip-space matrix
// (proj, viewProj, or viewProjRel; float[16], m[c][r] at m[c*4+r]).
// Row operation row0 += ndcX * row3 / row1 += ndcY * row3 — i.e. clip-space
// x += ndcX * w, y += ndcY * w. Because the LH reverse-Z projection sets
// w_clip = z_view exactly (MakePerspectiveLH_ZO_ReverseZ, m[2][3] = 1), this
// is an EXACT constant NDC translation at every depth: for the bare proj it
// reduces to writing the free zeros m[2][0]/m[2][1], and being a row (left)
// operation it distributes over composition, so applying it to viewProj
// equals composing the jittered proj with view. Depth rows are untouched —
// reverse-Z precision, depth compares, and depth-derived keys are unaffected
// by construction.
inline void ApplyNdcJitter(float m[16], float ndcOffsetX, float ndcOffsetY)
{
    for (int c = 0; c < 4; ++c)
    {
        m[c * 4 + 0] += ndcOffsetX * m[c * 4 + 3];
        m[c * 4 + 1] += ndcOffsetY * m[c * 4 + 3];
    }
}

} // namespace GameEngine::Engine::Renderer
