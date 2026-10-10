/**
 * @file MaterialDepthClassify.h
 * @brief The single depth-class predicate: Material -> MaterialDepthClass.
 *
 * Mirrors (and inverts) the useSharedDepth PSO-selection predicate in
 * DepthDrawRecorder: a material is shared-depth eligible only when it draws
 * through the material-independent depth pipeline — in the shadow families
 * (where eligible casters also collapse into class-sentinel ranges) and in
 * the camera prepass (PSO substitution only; prepass ranges stay keyed on
 * colorClassId). Everything that needs its own depth variant — alpha-test
 * (Mask), vertex-modified, or transmissive — is material-dependent and keeps
 * its (material, mesh) shadow batch. Non-opaque (Blend) is also dependent:
 * blend casters are filtered out of the depth pass entirely, so they must
 * never fold into a class sentinel that a sibling opaque caster would draw.
 *
 * RenderServices caches the result per materialIndex; extraction, the shadow
 * table build, and the depth consumer all read that one array so the scatter
 * routing bit, the GPU table, and the draw lookup can never disagree.
 */
#pragma once

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MaterialVertexAnimation.h"

#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::Engine::Renderer
{

inline ::GameEngine::Rendering::MaterialDepthClass ClassifyMaterialDepthClass(const Material& mat)
{
    namespace R = ::GameEngine::Rendering;
    const R::MaterialKeyword keywords = mat.GetVariantKey().materialKeywords;
    const bool dependent =
        mat.GetAlphaMode() != ::GameEngine::MaterialAlphaMode::Opaque // Mask or Blend
        || MaterialDeformsVertices(mat)
        || R::HasKeyword(keywords, R::MaterialKeyword::Transmission);
    if (dependent)
        return R::MaterialDepthClass::MaterialDependent;
    return mat.IsDoubleSided() ? R::MaterialDepthClass::EligibleDoubleSided
                               : R::MaterialDepthClass::EligibleSingleSided;
}

} // namespace GameEngine::Engine::Renderer
