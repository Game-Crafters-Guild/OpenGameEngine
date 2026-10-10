// ModelMaterialBridge implementation: converts ModelAsset::Material to MaterialDocument.

#include "Engine/Rendering/ModelMaterialBridge.h"
#include "AssetCore/SubassetDeriveKeys.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/LightPhotometry.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace Engine::Renderer
{
namespace
{
bool IsIdentityTransform(const std::array<float, 8>& st)
{
    return st[0] == 1.0f && st[1] == 0.0f && st[2] == 0.0f &&
           st[4] == 0.0f && st[5] == 1.0f && st[6] == 0.0f;
}

// An emission component as a file stores it, made usable: negative, NaN and infinite values read 0.
float CleanEmissionComponent(float value)
{
    return std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

// The imported emission color as the material's emission: the color normalized to a [0, 1] color in
// `emissive` and its brightness in nits in `emissionLuminance`, a peak of 1 being reference white.
// The importer has already folded the source's emission factor and strength into the color; the
// surface multiplies it with the emissive texture. Black writes nothing, which the defaults read as
// no emission (luminance 0), a bound texture included. An imported emission is physical: showing it
// whatever the view's exposure is the author's choice in the material.
void AddEmission(MaterialDocument& doc, const ImportedMaterialData& material)
{
    const float color[3] = {CleanEmissionComponent(material.EmissiveColor[0]),
                            CleanEmissionComponent(material.EmissiveColor[1]),
                            CleanEmissionComponent(material.EmissiveColor[2])};
    const float peak = std::max({color[0], color[1], color[2]});
    if (peak <= 0.0f)
        return;
    doc.properties["emissive"] = std::vector<float>{color[0] / peak, color[1] / peak, color[2] / peak};
    doc.properties["emissionLuminance"] = Components::kReferenceWhiteNits * peak;
    doc.properties["emissiveExposureWeight"] = kDefaultEmissiveExposureWeight;
}

void AddTextureTransform(MaterialDocument& doc,
                         const char* textureName,
                         const std::array<float, 8>& st)
{
    if (!IsIdentityTransform(st))
        doc.textureTransforms[textureName] = st;
}
} // namespace

GUID ModelMaterialBridge::DeriveMaterialGuid(const GUID& modelGuid, uint32_t materialIndex)
{
    // Key layout lives in AssetCore/SubassetDeriveKeys.h; the journal persists
    // the keys so container renames can cascade redirects for these GUIDs.
    return GUID::Derive(modelGuid, ModelMaterialDeriveKey(materialIndex));
}

ConvertedModelMaterial ModelMaterialBridge::Convert(const GUID& modelGuid,
                                                    uint32_t materialIndex,
                                                    const ImportedMaterialData& material)
{
    ConvertedModelMaterial result{};
    result.materialIndex = materialIndex;
    result.derivedGuid = DeriveMaterialGuid(modelGuid, materialIndex);

    // --- MaterialDocument ---

    MaterialDocument& doc = result.document;
    doc.schemaVersion = 3;
    doc.materialName = material.Name.empty()
                           ? ("EmbeddedMaterial_" + std::to_string(materialIndex))
                           : std::string(material.Name.c_str());

    // Lighting model: glTF materials are PBR by default.
    doc.lightingModel = "StandardPBR";

    // The standard surface samples emissive and AO maps as well; only separate
    // roughness and metallic maps need the extended surface, which has no
    // combined metallic-roughness slot.
    const bool hasExtendedTextures =
        !material.RoughnessTexture.empty()
        || !material.MetallicTexture.empty();

    // Surface shader: use the extended surface only when imported material
    // data needs the extra sampler slots.
    doc.surfaceShader = hasExtendedTextures
        ? "Surfaces/standard_pbr_extended.glsl"
        : "Surfaces/standard_pbr.glsl";

    // --- Properties ---

    // Base color (diffuseColor in ModelAsset is float[4]).
    doc.properties["baseColor"] = std::vector<float>{
        material.DiffuseColor[0],
        material.DiffuseColor[1],
        material.DiffuseColor[2],
        material.DiffuseColor[3]};

    // Metallic-roughness.
    doc.properties["metallic"] = material.Metallic;
    doc.properties["roughness"] = material.Roughness;

    AddEmission(doc, material);

    // Alpha cutoff (relevant when AlphaMode == Mask).
    doc.properties["alphaCutoff"] = material.AlphaCutoff;

    // --- Textures ---

    if (!material.DiffuseTexture.empty())
    {
        doc.textures["albedoMap"] = std::string(material.DiffuseTexture.c_str());
        AddTextureTransform(doc, "albedoMap", material.DiffuseTextureTransform);
    }

    if (!material.NormalTexture.empty())
    {
        doc.textures["normalMap"] = std::string(material.NormalTexture.c_str());
        AddTextureTransform(doc, "normalMap", material.NormalTextureTransform);
    }

    if (!material.SpecularTexture.empty() && !hasExtendedTextures)
    {
        doc.textures["metallicRoughnessMap"] = std::string(material.SpecularTexture.c_str());
        AddTextureTransform(doc, "metallicRoughnessMap", material.SpecularTextureTransform);
    }

    if (!material.EmissiveTexture.empty())
    {
        doc.textures["emissiveMap"] = std::string(material.EmissiveTexture.c_str());
        AddTextureTransform(doc, "emissiveMap", material.EmissiveTextureTransform);
    }

    if (!material.OcclusionTexture.empty())
    {
        doc.textures["aoMap"] = std::string(material.OcclusionTexture.c_str());
        AddTextureTransform(doc, "aoMap", material.OcclusionTextureTransform);
    }

    if (!material.RoughnessTexture.empty())
    {
        doc.textures["roughnessMap"] = std::string(material.RoughnessTexture.c_str());
        AddTextureTransform(doc, "roughnessMap", material.RoughnessTextureTransform);
    }

    if (!material.MetallicTexture.empty())
    {
        doc.textures["metallicMap"] = std::string(material.MetallicTexture.c_str());
        AddTextureTransform(doc, "metallicMap", material.MetallicTextureTransform);
    }

    // --- Render state ---

    doc.doubleSided = material.DoubleSided;
    doc.ignoreVertexColor = material.IgnoresVertexColor;

    switch (material.AlphaMode)
    {
    case AlphaMode::Mask:
        doc.alphaMode = MaterialAlphaMode::Mask;
        break;
    case AlphaMode::Blend:
        doc.alphaMode = MaterialAlphaMode::Blend;
        break;
    default:
        doc.alphaMode = MaterialAlphaMode::Opaque;
        break;
    }

    return result;
}

std::vector<ConvertedModelMaterial> ModelMaterialBridge::ConvertAll(
    const GUID& modelGuid,
    const std::vector<ImportedMaterialData>& materials)
{
    std::vector<ConvertedModelMaterial> results;
    results.reserve(materials.size());

    for (uint32_t i = 0; i < static_cast<uint32_t>(materials.size()); ++i)
    {
        results.push_back(Convert(modelGuid, i, materials[i]));
    }

    return results;
}

} // namespace Engine::Renderer
} // namespace GameEngine
