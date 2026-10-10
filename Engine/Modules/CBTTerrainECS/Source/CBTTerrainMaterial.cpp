#include "CBTTerrainECS/CBTTerrainMaterial.h"

namespace GameEngine::CBTTerrainECS
{

using Rendering::MaterialKeyword;

const GUID& CBTTerrainMaterialGuid()
{
    static const GUID kGuid = GUID::Derive(GUID{}, "cbt_terrain/default_material");
    return kGuid;
}

MaterialDocument BuildCBTTerrainMaterialDocument()
{
    MaterialDocument doc{};
    doc.materialName = "CBTTerrain/Default";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "CBT/cbt_surface.glsl";
    doc.vertexModifier = "CBT/cbt_vertex_modifier.glsl";
    // Fully-procedural geometry: no vertex buffer. The clamp turns this into
    // CustomVertexShader | HasVertexOutputMod and forces vertexFlags = None.
    doc.customVertexShader = true;
    // Double-sided so the LEB triangles survive the negative-viewport-Y CCW->CW
    // flip regardless of the emitted corner order (plan §8 C3 winding acceptance):
    // cullMode = None (MaterialSystem sets it from IsDoubleSided). C4 can tighten
    // to single-sided once the winding is pinned.
    doc.doubleSided = true;
    doc.alphaMode = MaterialAlphaMode::Opaque;
    return doc;
}

MaterialKeyword CBTTerrainMaterialKeywords()
{
    // Lit Forward+ (matches the world pass set-0 layout).
    return MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows | MaterialKeyword::IBL;
}

} // namespace GameEngine::CBTTerrainECS
