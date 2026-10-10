#pragma once

// ModelMaterialBridge: converts embedded model material data (ModelAsset::Material)
// into Material v2 documents (MaterialDocument) suitable for shader compilation.
//
// Responsibilities:
//   - Map glTF PBR material properties to MaterialDocument properties/textures
//   - Derive stable GUIDs for embedded materials (from model GUID + material index)
//   - Set appropriate lighting model, alpha mode, and double-sided flags
//   - Select the correct built-in surface shader path
//
// This is a pure data-conversion utility. It does NOT compile shaders, create GPU
// resources, or touch the device. Output MaterialDocuments are consumed by the
// existing MaterialCompiler / MaterialBuildService pipeline.
//
// Ownership:
//   - Stateless utility. No persistent state, no GPU dependencies.
//   - Called by ModelAsset loading, ModelEntityFactory and ModelRenderSetup.

#include "AssetCore/GUID.h"
#include "Rendering/Materials/MaterialDocument.h"

#include <cstdint>
#include <string>
#include <vector>

namespace GameEngine
{
struct ImportedMaterialData; // Forward: ModelAsset.h import-time material struct

namespace Engine::Renderer
{

// Result of converting a single embedded model material.
// Render state (alphaMode, doubleSided) lives on the MaterialDocument.
struct ConvertedModelMaterial
{
    MaterialDocument document;

    GUID derivedGuid;

    uint32_t materialIndex = 0;
};

class ModelMaterialBridge
{
  public:
    // Convert a single ModelAsset::Material to a MaterialDocument.
    //
    // `modelGuid` is the asset GUID of the parent model.
    // `materialIndex` is the index within the model's material array.
    // `material` is the source material data from ModelAsset.
    //
    // The derived GUID is computed as GUID::Derive(modelGuid, "material/<index>").
    static ConvertedModelMaterial Convert(const GUID& modelGuid,
                                          uint32_t materialIndex,
                                          const ImportedMaterialData& material);

    // Convert all materials from a model asset at once.
    static std::vector<ConvertedModelMaterial> ConvertAll(const GUID& modelGuid,
                                                          const std::vector<ImportedMaterialData>& materials);

    // Derive a stable GUID for an embedded material.
    static GUID DeriveMaterialGuid(const GUID& modelGuid, uint32_t materialIndex);
};

} // namespace Engine::Renderer
} // namespace GameEngine
