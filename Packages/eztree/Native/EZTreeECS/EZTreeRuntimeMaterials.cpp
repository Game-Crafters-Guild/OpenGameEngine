#include "EZTreeECS/EZTreeRuntimeMaterials.h"

#include <vector>

namespace GameEngine::EZTreeECS
{

MaterialDocument MakeBarkRuntimeMaterialShape()
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "Tree Generator Bark";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    doc.doubleSided = true;
    doc.textureFilter = MaterialTextureFilter::Trilinear;
    doc.properties["roughness"] = 0.82f;
    doc.properties["metallic"] = 0.0f;
    doc.properties["emissive"] = std::vector<float>{0.0f, 0.0f, 0.0f};
    doc.properties["emissionLuminance"] = 203.0f;
    doc.properties["ao"] = 1.0f;
    doc.properties["opacity"] = 1.0f;
    return doc;
}

MaterialDocument MakeLeafRuntimeMaterialShape()
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "Tree Generator Leaves";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/ez_tree_leaves.glsl";
    doc.vertexModifier = "VertexModifiers/ez_tree_wind.glsl";
    doc.alphaMode = MaterialAlphaMode::Mask;
    doc.doubleSided = true;
    doc.textureFilter = MaterialTextureFilter::Trilinear;
    doc.properties["roughness"] = 0.9f;
    doc.properties["metallic"] = 0.0f;
    doc.properties["emissive"] = std::vector<float>{0.0f, 0.0f, 0.0f};
    doc.properties["emissionLuminance"] = 203.0f;
    doc.properties["ao"] = 1.0f;
    doc.properties["opacity"] = 1.0f;
    doc.textures["normalMap"] = std::string{};
    doc.textures["metallicRoughnessMap"] = std::string{};
    doc.textures["aoMap"] = std::string{};
    return doc;
}

MaterialDocument MakeTrellisRuntimeMaterialShape()
{
    MaterialDocument doc{};
    doc.schemaVersion = 3;
    doc.materialName = "Tree Generator Trellis";
    doc.lightingModel = "StandardPBR";
    doc.surfaceShader = "Surfaces/standard_pbr.glsl";
    doc.alphaMode = MaterialAlphaMode::Opaque;
    doc.doubleSided = true;
    doc.textureFilter = MaterialTextureFilter::Trilinear;
    doc.properties["roughness"] = 0.76f;
    doc.properties["metallic"] = 0.0f;
    doc.properties["emissive"] = std::vector<float>{0.0f, 0.0f, 0.0f};
    doc.properties["emissionLuminance"] = 203.0f;
    doc.properties["ao"] = 1.0f;
    doc.properties["opacity"] = 1.0f;
    return doc;
}

} // namespace GameEngine::EZTreeECS
