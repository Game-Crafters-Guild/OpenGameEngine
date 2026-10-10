/**
 * @file MaterialColorClassify.h
 * @brief The color-class predicate: Material -> merge eligibility + a
 *        collision-free PSO signature (P2 cross-material batch merge).
 *
 * Two opaque draws that differ ONLY in materialIndex can share one indirect
 * batch iff they resolve to the same color PSO. This header answers "which
 * materials may merge" and "which of them share a class":
 *
 *  - IsColorMergeEligible == shared-depth eligibility (ClassifyMaterialDepthClass
 *    != MaterialDependent). This is deliberately identical to the depth-class
 *    gate, and it is what keeps the merge SHADOW-SAFE: the color/depth-prepass
 *    key set is shared with the shadow-cascade consumer (same view id), which
 *    dedups eligible casters by (mesh, side). Merging only eligible materials
 *    means the shadow consumer's (mesh, side) dedup absorbs the collapsed keys
 *    with no per-material shadow draw lost. Material-dependent casters (Mask,
 *    vertex-modified, transmissive, blend) keep their (material, mesh) identity
 *    exactly as R1.5 requires, so shadow output is untouched.
 *
 *  - ColorClassSignature captures EVERY PSO-affecting input as exact values
 *    (no lossy hash of the discriminant): the full materialKeywords mask, the
 *    surface / vertex-modifier / lighting-model compile-spec fields, and the
 *    fixed-function alphaMode / doubleSided / ignoreVertexColor. Two materials
 *    equal on all of these compile to a byte-identical GraphicsPipelineDesc, so
 *    one may bind the PSO for the whole merged batch. vertexFlags is
 *    intentionally EXCLUDED -- the draw-time PSO uses effectiveFlags, and the
 *    merge is per-mesh, so instances in a (class, mesh) batch share the
 *    mesh-derived bits. ignoreVertexColor is the one MATERIAL-driven input to
 *    effectiveFlags (it strips HasColor), which is exactly why it must ride the
 *    signature: without it, same-mesh materials differing only in the flag
 *    would merge and bind the representative's vertex-color variant.
 *
 * RenderServices maps each distinct signature to a downward-allocated
 * colorClassId (see RenderServices::EnsureMaterialColorClassMap) and caches the
 * per-materialIndex result in m_MaterialColorClass -- the single source the
 * color table build, the scatter routing SSBO, and the consumer lookup all read.
 */
#pragma once

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialColorClassSignature.h"
#include "Engine/Rendering/MaterialDepthClassify.h"

namespace GameEngine::Engine::Renderer
{

// A material may merge into a color class only when it draws through the
// material-independent depth path -- the same predicate the shadow cascades
// use to collapse casters. Keeping the two gates identical is what makes the
// shared-key merge shadow-safe (see the file header).
inline bool IsColorMergeEligible(const Material& mat)
{
    return ClassifyMaterialDepthClass(mat)
           != ::GameEngine::Rendering::MaterialDepthClass::MaterialDependent;
}

// Collision-free PSO signature for grouping eligible materials into classes.
inline ColorClassSignature ComputeColorClassSignature(const Material& mat)
{
    const MaterialCompileSpec& spec = mat.GetCompileSpec();
    ColorClassSignature sig{};
    sig.SurfaceShaderPath  = spec.surfaceShaderPath;
    sig.VertexModifierPath = spec.vertexModifierPath;
    sig.LightingModel      = spec.lightingModel;
    sig.MaterialKeywords =
        static_cast<uint64_t>(mat.GetVariantKey().materialKeywords);
    sig.AlphaMode         = static_cast<uint8_t>(mat.GetAlphaMode());
    sig.DoubleSided       = mat.IsDoubleSided();
    sig.IgnoreVertexColor = mat.IgnoresVertexColor();
    return sig;
}

} // namespace GameEngine::Engine::Renderer
