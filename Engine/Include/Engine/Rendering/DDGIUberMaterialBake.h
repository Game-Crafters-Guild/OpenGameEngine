#pragma once

#include "SceneBvh/UberMaterial.h"

namespace GameEngine::Engine::Renderer
{
class DDGIMaterialMapAtlas;
class Material;

// Bakes the CPU-side shading record the DDGI software kernel reads
// (Shaders/ddgi_trace_sw.comp) out of a material's packed parameter block.
//
// The parameters are found BY NAME: a surface that declares its properties packs
// them in declaration order, so nothing sits at a fixed lane — the shipped
// waterfall_fx puts panSpeed where a fixed reader would look for base colour.
// Surfaces without declarations answer from the shared legacy lane map instead.
//
// A null material, or one whose parameter cache has not been packed yet, bakes
// the neutral record: opaque white, non-metal, non-emissive — the sweep over a
// scene's materials never fails on one.
SceneBvh::UberMaterial BakeUberMaterial(const Material* material,
                                        const DDGIMaterialMapAtlas* atlas);

} // namespace GameEngine::Engine::Renderer
