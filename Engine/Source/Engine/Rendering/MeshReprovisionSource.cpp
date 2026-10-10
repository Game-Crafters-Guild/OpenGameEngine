// MeshReprovisionSource implementation: asset-backed geometry first, then
// regeneration for engine built-ins.

#include "Engine/Rendering/MeshReprovisionSource.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Core/Engine.h"
#include "Engine/Rendering/PrimitiveGenerator.h"

namespace GameEngine
{
namespace Engine::Renderer
{

Rendering::MeshGPUCpuSource MeshReprovisionSource::operator()(const Rendering::MeshGPUKey& key)
{
    // A bare harness (unit tests, tools) can rebuild the device before a full
    // engine init has constructed the AssetManager; skip the asset path so such
    // entries resolve by regeneration, a CPU mirror, or a tombstone — never a
    // null manager deref.
    if (AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager())
    {
        SharedPtr<Asset> asset = assetManager->GetAsset(key.assetGuid);
        if (const auto* model = dynamic_cast<const ModelAsset*>(asset.get()))
        {
            if (key.submeshIndex < model->GetMeshCount())
                return {&model->GetMesh(key.submeshIndex), Rendering::MeshGPUSourceOrigin::Asset};
        }
    }

    // Built-in primitives have no asset and no retained picking mirror. A lookup
    // that only consulted the AssetManager tombstoned every one of them, so any
    // scene built from primitives lost all of its geometry for the life of the
    // process. Their GUIDs are deterministic, so regenerate rather than look up.
    //
    // Whether the GUID names a primitive is decided by GenerateByGuid returning
    // geometry, not by a separate predicate: a membership test built on its own GUID
    // list could reject an identity the table can actually generate, which is the
    // registered-but-unrecoverable hole this path exists to close.
    if (key.submeshIndex == 0)
    {
        auto it = m_Regenerated.find(key.assetGuid);
        if (it == m_Regenerated.end())
        {
            Mesh mesh = PrimitiveGenerator::GenerateByGuid(key.assetGuid);
            if (mesh.Indices.empty())
                return {};
            it = m_Regenerated.emplace(key.assetGuid, std::move(mesh)).first;
        }
        return {&it->second, Rendering::MeshGPUSourceOrigin::Generated};
    }

    return {};
}

} // namespace Engine::Renderer
} // namespace GameEngine
