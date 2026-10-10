#include "Ocean/OceanSurfaceMaterial.h"

namespace GameEngine::Ocean
{

const GUID& OceanSurfaceMaterialGuid()
{
    static const GUID guid = GUID::Derive(GUID{}, "ocean/default_material");
    return guid;
}

MaterialDocument BuildOceanSurfaceMaterialDocument()
{
    MaterialDocument doc{};
    doc.materialName = "Ocean/Default";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Ocean/ocean_surface.glsl";
    doc.vertexModifier = "Ocean/ocean_vertex_modifier.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    return doc;
}

Rendering::MaterialKeyword OceanSurfaceMaterialKeywords(bool compatProfile)
{
    using Rendering::MaterialKeyword;
    // HasVertexMod mirrors MaterialRegistry::Register, which stamps it for any
    // document with a vertexModifier (the build service later adds
    // HasVertexOutputMod from capability detection — the runtime key carries
    // BOTH). The offline cook keys with these exact keywords, so leaving it out
    // cooks a variant the runtime never addresses.
    MaterialKeyword keywords = MaterialKeyword::ForwardPlus | MaterialKeyword::Shadows |
                               MaterialKeyword::Instanced |
                               MaterialKeyword::ProceduralVertexOutput |
                               MaterialKeyword::HasVertexMod;
    if (!compatProfile)
        keywords = keywords | MaterialKeyword::IBL;
    return keywords;
}

} // namespace GameEngine::Ocean
