#pragma once

// Which draws write a parallax material's relief depth, which read it, and how (parallax design, D7 to
// D9). The depth recorder, the world pass, the colour variant cache and the warm-up all decide through
// these, so the variants warmed are the ones drawn.
//
// The camera prepass owns the relief's depth: it marches and writes the hit. A colour pass that can read
// that depth rebuilds the hit from it instead of marching again, so both agree by construction.

#include "Rendering/Materials/MaterialAlphaMode.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <cstdint>

namespace GameEngine::Engine::Renderer
{
// Parallax materials write their relief's depth in the camera prepass and the world pass
// (GE_PARALLAX_DEPTH_OFFSET=0 disables it, for triage and for timing it against the polygon's depth).
bool ParallaxDepthOffsetEnabled();

// Whether a material whose own keywords are `materialKeywords` writes its relief's depth.
inline bool WritesReliefDepth(Rendering::MaterialKeyword materialKeywords)
{
    return ParallaxDepthOffsetEnabled() && Rendering::HasKeyword(materialKeywords, Rendering::MaterialKeyword::Parallax);
}

// Where a world pass's colour draws find the relief depth.
enum class ReliefDepthSource : uint8_t
{
    NoPrepass,                  // no prepass wrote this view's depth: the colour pass writes it
    PrepassUnreadable,          // a prepass wrote it, but the pass attaches depth writable (a forward
                                // draw that writes its own depth, ForwardDrawDepth::ColourPass, such as
                                // terrain or grass before its head's pipeline is built), so it cannot
                                // also sample it: the colour pass marches and tests with a tolerance,
                                // writing nothing
    PrepassReadable,            // a prepass wrote it and the pass attaches it read-only, single-sampled
    PrepassReadableMultisample, // the same, multisampled
};

// The source for a world pass: whether the prepass wrote this depth, whether the pass attaches it
// read-only, and its sample count.
inline ReliefDepthSource WorldPassReliefDepthSource(bool depthPrepassWroteThisDepth, bool depthAttachedReadOnly,
                                                    uint32_t depthSampleCount)
{
    if (!depthPrepassWroteThisDepth)
        return ReliefDepthSource::NoPrepass;
    if (!depthAttachedReadOnly)
        return ReliefDepthSource::PrepassUnreadable;
    return depthSampleCount > 1u ? ReliefDepthSource::PrepassReadableMultisample : ReliefDepthSource::PrepassReadable;
}

// The relief-depth pass keywords the world pass adds to one material's colour draw. Opaque and Mask
// parallax materials only: a Blend or a transmissive one writes no depth, and a depth-exporting variant
// would give up early depth testing for nothing.
inline Rendering::MaterialKeyword WorldPassReliefDepthKeywords(Rendering::MaterialKeyword materialKeywords,
                                                               MaterialAlphaMode alphaMode, ReliefDepthSource source)
{
    using Rendering::MaterialKeyword;
    if (!WritesReliefDepth(materialKeywords) || alphaMode == MaterialAlphaMode::Blend ||
        Rendering::HasKeyword(materialKeywords, MaterialKeyword::Transmission))
        return MaterialKeyword::None;
    switch (source)
    {
    case ReliefDepthSource::PrepassReadable:
        return MaterialKeyword::ParallaxDepthFromPrepass;
    case ReliefDepthSource::PrepassReadableMultisample:
        return MaterialKeyword::ParallaxDepthFromPrepass | MaterialKeyword::ParallaxPrepassDepthMultisample;
    case ReliefDepthSource::PrepassUnreadable:
        return MaterialKeyword::ParallaxDepthOffset | MaterialKeyword::ParallaxDepthTolerance;
    case ReliefDepthSource::NoPrepass:
        break;
    }
    return MaterialKeyword::ParallaxDepthOffset;
}

// Whether a colour variant's pipeline writes depth: the material's own setting, off for a variant that
// reads the prepass's relief depth or tests against it with a tolerance, either of which must leave it in
// the buffer exactly as the prepass wrote it.
inline bool ColorVariantWritesDepth(bool materialWritesDepth, Rendering::MaterialKeyword passKeywords)
{
    return materialWritesDepth &&
           !Rendering::HasKeyword(passKeywords, Rendering::MaterialKeyword::ParallaxDepthFromPrepass) &&
           !Rendering::HasKeyword(passKeywords, Rendering::MaterialKeyword::ParallaxDepthTolerance);
}
} // namespace GameEngine::Engine::Renderer
