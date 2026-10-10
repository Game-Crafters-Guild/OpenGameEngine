#pragma once

#include "Rendering/Geometry/VertexAttributeFlags.h"
#include "Rendering/Materials/MaterialDocument.h"
#include "Rendering/Materials/ShaderVariantKey.h"

#include <span>
#include <vector>

namespace GameEngine::Components
{
struct ParticleRenderer;
}

namespace GameEngine::Particles
{
/// The material an emitter's particles draw with: `source` (the renderer's material asset) or the
/// procedural smoke, with the particle shaders, blending and the renderer's texture sheet, lighting,
/// six-way maps and emission applied.
MaterialDocument BuildParticleRenderMaterialDocument(const Components::ParticleRenderer& renderer,
                                                     const MaterialDocument* source);

/// One document for every shader program BuildParticleRenderMaterialDocument can ask for: each
/// lighting mode and six-way layout, with and without a texture sheet and an emission texture. The
/// values, textures and source material an emitter brings change no program, so the variant cook
/// cooks these and a runtime without a shader compiler draws every emitter.
std::vector<MaterialDocument> ParticleRenderMaterialShapes();

/// The pass keywords every particle draw command carries: the sprite and the assigned meshes are
/// mesh-backed instanced draws. The late transparent pass adds its own lighting keywords.
inline constexpr Rendering::MaterialKeyword kParticleDrawKeywords = Rendering::MaterialKeyword::Instanced;

/// One shader variant the renderer asks a particle render material for: the pass keywords and the
/// vertex layout of the mesh it binds.
struct ParticleRenderVariant
{
    const char* Name;
    Rendering::MaterialKeyword PassKeywords;
    Rendering::VertexAttributeFlags VertexFlags;
};

/// Every variant the renderer draws a ParticleRenderMaterialShapes() document with, so the variant cook
/// cooks these and nothing else: the keyword-less base registration builds before the material can draw,
/// and the late transparent pass of the shipped pipelines (ForwardPlus, Shadows, with and without IBL)
/// over kParticleDrawKeywords, on the sprite's layout (with tangents) and a mesh particle's (with or
/// without). Declared, not derived: a pipeline whose transmissive pass carries another keyword set, or
/// a mesh particle with another layout, asks for a variant no cook produced, and a runtime without a
/// shader compiler refuses that draw by name.
std::span<const ParticleRenderVariant> ParticleRenderVariants();
} // namespace GameEngine::Particles
