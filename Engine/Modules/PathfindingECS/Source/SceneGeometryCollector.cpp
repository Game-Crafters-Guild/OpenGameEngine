#include "SceneGeometryCollector.h"

#include "Assets/AssetManager.h"
#include "Assets/ModelAsset.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Transform.h"
#include "Core/Engine.h"
#include "ECS/Query.h"
#include "Logger/Logger.h"

#include <cstring>

namespace GameEngine::PathfindingECS
{

namespace
{

// Transform a point by a 4x4 column-major matrix (affine).
void TransformPoint(const float32 m[16], const float32 in[3], float32 out[3])
{
    out[0] = m[0] * in[0] + m[4] * in[1] + m[8]  * in[2] + m[12];
    out[1] = m[1] * in[0] + m[5] * in[1] + m[9]  * in[2] + m[13];
    out[2] = m[2] * in[0] + m[6] * in[1] + m[10] * in[2] + m[14];
}

// Multiply two 4x4 column-major matrices: out = a * b.
void MultiplyMatrices(const float32 a[16], const float32 b[16], float32 out[16])
{
    for (int col = 0; col < 4; ++col)
    {
        for (int row = 0; row < 4; ++row)
        {
            out[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

// Append all submeshes from a ModelAsset into the output buffers, transforming
// each vertex by the combined matrix (sourceNodeTransform * extraTransform).
// When extraTransform is nullptr only sourceNodeTransform is applied.
void AppendModelGeometry(const ModelAsset& model,
                         const float32* extraTransform,
                         CollectedGeometry& out)
{
    for (uint32 mi = 0; mi < model.GetMeshCount(); ++mi)
    {
        const Mesh& mesh = model.GetMesh(mi);
        if (mesh.Vertices.empty() || mesh.Indices.empty())
            continue;

        // Build the combined transform for this submesh.
        float32 combined[16];
        if (extraTransform)
        {
            MultiplyMatrices(extraTransform, mesh.SourceNodeTransform, combined);
        }
        else
        {
            std::memcpy(combined, mesh.SourceNodeTransform, sizeof(combined));
        }

        const uint32 baseVertex = out.VertexCount;

        // Transform and append vertices.
        for (const auto& vertex : mesh.Vertices)
        {
            float32 worldPos[3];
            TransformPoint(combined, vertex.Position, worldPos);
            out.Vertices.push_back(worldPos[0]);
            out.Vertices.push_back(worldPos[1]);
            out.Vertices.push_back(worldPos[2]);
        }
        out.VertexCount += static_cast<uint32>(mesh.Vertices.size());

        // Append indices offset by the base vertex.
        for (uint32 idx : mesh.Indices)
        {
            out.Indices.push_back(baseVertex + idx);
        }
        out.TriangleCount += static_cast<uint32>(mesh.Indices.size()) / 3;
    }
}

} // anonymous namespace

CollectedGeometry CollectSceneGeometry(ECS::World& world)
{
    CollectedGeometry result;

    // A host with no AssetManager (navigation driven without a full
    // EngineCore::Initialize) can resolve no model, so the scene contributes no
    // geometry and the warning below reports it.
    if (AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager())
    {
        world.Query<ECS::Read<Components::MeshRenderer>,
                    ECS::Read<Components::WorldTransform>>()
            .Each([&](ECS::EntityHandle /*e*/,
                      const Components::MeshRenderer& renderer,
                      const Components::WorldTransform& wt)
            {
                if (renderer.modelAssetGuid.IsNull())
                    return;

                const GUID assetGuid = renderer.modelAssetGuid.ToGuid();

                auto asset = assetManager->GetAsset(assetGuid);
                if (!asset || !asset->IsLoaded() || asset->GetType() != AssetType::Model)
                {
                    return;
                }

                auto* model = static_cast<ModelAsset*>(asset.get());
                AppendModelGeometry(*model, wt.matrix, result);
            });
    }

    if (result.VertexCount == 0)
    {
        Logger::Log::Warning("[PathfindingECS] CollectSceneGeometry found no renderable geometry");
    }

    return result;
}

CollectedGeometry CollectGeometryFromAssets(const std::vector<GUID>& assetGuids)
{
    CollectedGeometry result;
    AssetManager* assetManager = EngineCore::GetInstance().TryGetAssetManager();
    if (!assetManager)
    {
        Logger::Log::Warning("[PathfindingECS] CollectGeometryFromAssets: no asset manager, nothing to resolve");
        return result;
    }

    for (const auto& guid : assetGuids)
    {
        auto asset = assetManager->GetAsset(guid);
        if (!asset || !asset->IsLoaded() || asset->GetType() != AssetType::Model)
        {
            Logger::Log::Warning("[PathfindingECS] CollectGeometryFromAssets: asset not loaded or not a model");
            continue;
        }

        auto* model = static_cast<ModelAsset*>(asset.get());
        AppendModelGeometry(*model, nullptr, result);
    }

    return result;
}

} // namespace GameEngine::PathfindingECS
