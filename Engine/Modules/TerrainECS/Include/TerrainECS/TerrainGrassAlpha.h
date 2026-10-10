#pragma once

#include "Components/Terrain/TerrainGrass.h"
#include "Engine/Rendering/RenderServices.h"
#include "Engine/Rendering/TextureService.h"

namespace GameEngine::TerrainECS
{

// Whether a grass row's blade texture actually carries soft alpha for an alpha path to resolve —
// the "only if the texture needs it" rule, in one place because two callers must never disagree
// about it: extraction sets the GPU flag that picks the draw mode from this, and the inspector
// shows the alpha controls from this. A row whose controls are visible but inert, or inert but
// hidden, is the drift that keeping one predicate prevents.
//
// Card grass only — geometric ribbon blades sample no texture, so their alpha is 1.0 by
// construction and nothing here can soften it.
//
// An albedo that cannot be probed (unreadable, undecodable, or a decode deferred by the probe's
// concurrency cap) reports as NEEDING alpha for both callers — AlphaIsUniformlyOpaque only says
// true on a definite answer, so an uncertain texture keeps its alpha path and keeps its control.
//
// `answerWithoutServices` covers the narrower case of having no renderer to ask at all, where the
// two callers want opposite directions. Extraction passes false: with no services nothing resolved
// a bindless index either, so the shader samples no texture and the blade really is alpha 1.0. The
// inspector passes true: it is describing what the component WOULD do once a renderer exists, and
// hiding a live control is worse than showing an inert one.
inline bool TerrainGrassNeedsAlpha(Engine::Renderer::RenderServices* renderServices,
                                   const Components::TerrainGrass& grass,
                                   bool answerWithoutServices)
{
    if (!grass.TextureGrass)
        return false;
    // A separate alpha map is taken at its word: it is sampled through RED, and the probe measures
    // a texture's ALPHA channel, so it cannot answer for that map at all.
    if (!grass.AlphaTextureAssetGuid.ToGuid().IsNull())
        return true;
    const GUID albedo = grass.AlbedoTextureAssetGuid.ToGuid();
    if (albedo.IsNull())
        return false;
    if (!renderServices)
        return answerWithoutServices;
    return !renderServices->Textures().AlphaIsUniformlyOpaque(albedo);
}

} // namespace GameEngine::TerrainECS
