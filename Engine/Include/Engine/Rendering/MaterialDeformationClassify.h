/**
 * @file MaterialDeformationClassify.h
 * @brief The single predicate for deforming-motion membership: Material -> bool.
 *
 * A material is in the deformer lane when its surface moves between two frames
 * for a reason the per-instance mover lane cannot reproduce — a vertex modifier
 * evaluated at two deformation clocks — AND the composed motion variant can
 * actually be built for it. The three refusals below mirror the adapter's own,
 * by name rather than by coincidence:
 *
 *  - the simple `vec3 ModifyVertex(vec3, InstanceData)` form only. The extended
 *    output form produces world positions and never applies the instance
 *    transform, so its previous endpoint has no prevTransform to pair with;
 *    adapter_vertex.glsl refuses it with a #error.
 *  - procedural geometry (CBT/LEB) always pairs with the extended form and has
 *    no vertex buffer at all.
 *  - Blend. A blended surface writes no prepass depth, so the motion it would
 *    write describes a surface the depth buffer never admitted, and both
 *    consumers index this target by the opaque surface at that pixel.
 *
 * Two readers, one input. MaterialSystem caches the verdict per materialIndex
 * beside the depth class for the mover exclusion, which has only an index;
 * WorldDrawBuilder evaluates the predicate on the material itself when it
 * derives a view's batch keys. Both read the variant key and the alpha mode,
 * which change only on the register edge that refreshes the cache, so the two
 * readers cannot disagree between edges.
 *
 * LIMIT: the verdict reads the variant KEY. A materialized shader graph can
 * reach HAS_VERTEX_MODIFIER through its @sg-variant tag without the keyword bit
 * (ShaderComposer derives the define from the merged list), and such a material
 * is therefore not in the deformer lane — it keeps today's behaviour, which is
 * the mover lane or nothing. Conservative by construction: a material outside
 * the lane is never excluded from the mover lane either.
 */
#pragma once

#include "Engine/Rendering/Material.h"

#include "Rendering/Materials/ShaderVariantKey.h"

namespace GameEngine::Engine::Renderer
{

inline bool SupportsDeformationMotion(const Material& mat)
{
    namespace R = ::GameEngine::Rendering;
    const R::MaterialKeyword keywords = mat.GetVariantKey().materialKeywords;
    if (!R::HasKeyword(keywords, R::MaterialKeyword::HasVertexMod))
        return false;
    if (R::HasKeyword(keywords, R::MaterialKeyword::HasVertexOutputMod))
        return false;
    if (R::HasKeyword(keywords, R::MaterialKeyword::CustomVertexShader))
        return false;
    return mat.GetAlphaMode() != ::GameEngine::MaterialAlphaMode::Blend;
}

} // namespace GameEngine::Engine::Renderer
