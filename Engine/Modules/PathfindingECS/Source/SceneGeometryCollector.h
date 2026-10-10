#pragma once

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <vector>

namespace GameEngine
{
namespace ECS
{
class World;
}
} // namespace GameEngine

namespace GameEngine::PathfindingECS
{

// Aggregated world-space mesh geometry for NavMesh baking.
struct CollectedGeometry
{
    std::vector<float32> Vertices; // [VertexCount * 3] XYZ interleaved, world space
    std::vector<uint32> Indices;   // [TriangleCount * 3]
    uint32 VertexCount = 0;
    uint32 TriangleCount = 0;
};

// Collect renderable mesh geometry from all entities that have both
// MeshRenderer (with a valid modelAssetGuid) and WorldTransform.
// Vertex positions are transformed into world space.
CollectedGeometry CollectSceneGeometry(ECS::World& world);

// Collect geometry from a set of ModelAsset GUIDs.
// Only sourceNodeTransform is applied (model space); no world transform.
CollectedGeometry CollectGeometryFromAssets(const std::vector<GUID>& assetGuids);

} // namespace GameEngine::PathfindingECS
