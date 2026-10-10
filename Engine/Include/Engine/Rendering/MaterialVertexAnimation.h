/**
 * @file MaterialVertexAnimation.h
 * @brief The single predicate for "this material moves its own vertices".
 *
 * Either vertex-modifier form displaces the vertex in the vertex stage, from
 * inputs the CPU never sees — a modifier reads the shared animation clock
 * (`inst.deformationTimeSeconds`), so its rasterized depth can differ from
 * last frame's while every instance record is byte-identical.
 *
 * Two consumers read it for two different reasons: the depth class (a
 * deformed surface needs its own depth variant rather than the shared
 * position-only one) and the per-frame content signal (the depth-derived
 * caches must not retain a layer rasterized at an older deformation).
 * Whether a given modifier is actually time-varying is not knowable CPU-side,
 * so both treat any modifier as animated — an honest superset.
 */
#pragma once

#include "Engine/Rendering/Material.h"

#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::Engine::Renderer
{

inline bool MaterialDeformsVertices(const Material& mat)
{
    namespace R = ::GameEngine::Rendering;
    const R::MaterialKeyword keywords = mat.GetVariantKey().materialKeywords;
    return R::HasKeyword(keywords, R::MaterialKeyword::HasVertexMod)
        || R::HasKeyword(keywords, R::MaterialKeyword::HasVertexOutputMod);
}

} // namespace GameEngine::Engine::Renderer
