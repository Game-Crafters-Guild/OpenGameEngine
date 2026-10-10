#pragma once

#include "Types/Types.h"

namespace GameEngine::TerrainGrass
{

// How a view's grass draws resolve their alpha this frame. Blades from every terrain share one
// draw per LOD band, so the mode is never per terrain.
//
// It is resolved per view, but only `colorSamples` is a per-view fact. The other two inputs are
// reduced once per params UPLOAD, across every ACTIVE terrain in the world: the params array is
// not frustum-filtered and every view reads the same published slot. So a terrain no view can see
// still decides them, and two views can differ only where their sample counts do.
//
// Opaque and Blend are the two pipeline classes; Dither and DitherA2C are the two implementations
// of the depth-correct alpha path, split by the view's color sample count. Alpha-to-coverage needs
// real MSAA samples — at one sample the hardware degenerates it to a hard 0.5 cutoff — so
// single-sample views take the screen-door discard instead.
enum class GrassDrawMode : uint32
{
    Blend = 0,
    Opaque = 1,
    Dither = 2,
    DitherA2C = 3,
};
constexpr uint32 kGrassDrawModeCount = 4u;

// The per-view draw mode, from the two facts the upload-time reduction carries plus the view's
// color sample count.
//
// `anyAlphaNeeded` gates everything, and gates it FIRST: it is the "only if the texture needs it"
// rule. With no soft alpha anywhere in the view there is nothing for an alpha path to resolve, so
// the authored mode has nothing to say and the draw takes the opaque fast path — no alpha-to-coverage,
// no discard AT ALL, depth write and test. That is the whole geometric-ribbon case (which binds no
// texture) and the card case whose texture probes uniformly opaque. It overrides Blend deliberately:
// blending an alpha-1.0 blade only buys back the ordering artefact Blend documents, where the far
// LOD band's placement order reads as an occlusion order.
//
// Opaque removes the discard INSTRUCTION, not just the branch. Its material composes the surface
// under the GRASS_OPAQUE user keyword, which compiles the card cutout out, so the fragment module
// carries no OpKill and the hardware can early-Z it. That is what the mode is for: a kill costs
// early-Z wherever it is compiled in, reachable or not, so branching around one buys nothing back.
// The saving is therefore depth-test scheduling as well as the raster state and the dither
// evaluation. ShippedSurfaceCompose.TerrainGrassOpaqueSurfaceCompilesNoFragmentKill asserts that
// absence against the real compiled SPIR-V, with the keyword-free variant as its positive control.
//
// The cost of the removal: the cutout is no longer there to rescue a WRONG probe. A texture that
// carries soft alpha but is classified opaque used to have its holes punched anyway by the
// runtime-gated cutout; under GRASS_OPAQUE it renders solid instead. The misclassification was
// always a defect — this changes how it presents, from a subtle mode difference to a visible one.
//
// `allBlend` is authored and requires EVERY active grass row to ask for it, because one draw cannot
// serve two pipeline classes; mixed authoring resolves to the depth-correct dither instead.
constexpr GrassDrawMode ResolveGrassDrawMode(bool allBlend, bool anyAlphaNeeded,
                                             uint32 colorSamples)
{
    if (!anyAlphaNeeded)
        return GrassDrawMode::Opaque;
    if (allBlend)
        return GrassDrawMode::Blend;
    return colorSamples > 1u ? GrassDrawMode::DitherA2C : GrassDrawMode::Dither;
}

// The camera-prepass head a mode's draws carry: the same blades through the same vertex stage,
// drawn into the depth prepass so its readers (GTAO, the screen-space shadows, the light-cull depth
// bounds) see the grass and the world pass can attach that depth read-only. The head must cover
// exactly the samples the colour draw keeps: more, and the ground behind a discarded texel fails
// the depth test against a blade that is not there; fewer, and the colour draw tests against the
// ground behind it.
enum class GrassPrepassHead : uint32
{
    // Opaque: no discard anywhere, so the head has no fragment stage and keeps early-Z.
    DepthOnly = 0,
    // Dither: the head runs the colour draw's own screen-door discard (the same surface, the same
    // per-pixel threshold), in a fragment stage that writes no colour.
    Discard = 1,
    // DitherA2C: the head's fragment stage writes the colour draw's alpha at location 0 and its
    // pipeline enables alpha-to-coverage, so the hardware keeps the same samples with no colour
    // target attached (the surface's GE_SURFACE_ALPHA_TO_COVERAGE).
    AlphaToCoverage = 2,
    // Blend: no head. Blended blades write no depth anywhere.
    None = 3,
};

constexpr GrassPrepassHead ResolveGrassPrepassHead(GrassDrawMode mode)
{
    switch (mode)
    {
    case GrassDrawMode::Opaque:
        return GrassPrepassHead::DepthOnly;
    case GrassDrawMode::Dither:
        return GrassPrepassHead::Discard;
    case GrassDrawMode::DitherA2C:
        return GrassPrepassHead::AlphaToCoverage;
    case GrassDrawMode::Blend:
        break;
    }
    return GrassPrepassHead::None;
}

} // namespace GameEngine::TerrainGrass
