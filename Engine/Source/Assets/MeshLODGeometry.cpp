#include "Assets/MeshLODGeometry.h"
#include "Assets/MeshLODGenerator.h"
#include "Assets/ModelAsset.h"

#include <algorithm>
#include <cmath>

namespace GameEngine {
namespace {
bool Finite(const Mathematics::Vector3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool Finite(const Mathematics::BoundingBox& box) {
    return Finite(box.center) && Finite(box.halfExtents) &&
           Finite(box.center - box.halfExtents) &&
           Finite(box.center + box.halfExtents) && std::isfinite(box.Radius());
}
} // namespace

MeshLODGeometry ResolveMeshLODGeometry(const Mesh& mesh) {
    MeshLODGeometry result;
    if (mesh.Vertices.empty() || mesh.Indices.empty()) return result;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(mesh.MinBounds[axis]) || !std::isfinite(mesh.MaxBounds[axis]) ||
            mesh.MinBounds[axis] > mesh.MaxBounds[axis]) {
            result.Issue = MeshLODGeometryIssue::InvalidBaseBounds;
            return result;
        }
    }
    result.ReferenceBounds = Mathematics::BoundingBox::FromMinMax(
        {mesh.MinBounds[0], mesh.MinBounds[1], mesh.MinBounds[2]},
        {mesh.MaxBounds[0], mesh.MaxBounds[1], mesh.MaxBounds[2]});
    if (!Finite(result.ReferenceBounds)) {
        result.Issue = MeshLODGeometryIssue::InvalidBaseBounds;
        return result;
    }
    result.Bounds = result.ReferenceBounds;
    result.LevelCount = 1;
    const uint32 declared = std::min(mesh.LODCount(), MeshLODConfig::kMaxLODs);
    // Resolve the drawable prefix before examining blocks; unused blocks must
    // not expand bounds or reject an otherwise valid chain.
    while (result.LevelCount < declared && !mesh.LODIndices(result.LevelCount).empty())
        ++result.LevelCount;
    if (result.LevelCount < declared)
        result.Issue = MeshLODGeometryIssue::EmptyLevel;

    bool own = false;
    for (uint32 level = 1; level < result.LevelCount; ++level)
        own |= level <= mesh.ExtraLODVertices.size() && !mesh.ExtraLODVertices[level - 1].empty();
    // Own-vertex levels carry the Vertex-interleaved attributes plus an aligned
    // per-level RGBA block. The remaining parallel streams address only LOD0's
    // vertex block; per-level support for one of them must land here and in
    // upload together, which is why both ask the same predicate.
    if (own && !mesh.OwnVertexLODStreamsSupported()) {
        result.LevelCount = 1;
        result.Issue = MeshLODGeometryIssue::UnsupportedStreams;
        return result;
    }

    for (uint32 level = 1; level < result.LevelCount; ++level) {
        const auto& vertices = level <= mesh.ExtraLODVertices.size() &&
            !mesh.ExtraLODVertices[level - 1].empty()
            ? mesh.ExtraLODVertices[level - 1] : mesh.Vertices;
        bool valid = true;
        for (uint32 index : mesh.LODIndices(level))
            valid &= index < vertices.size();
        if (&vertices != &mesh.Vertices) {
            for (const Vertex& vertex : vertices) {
                const Mathematics::Vector3 p{vertex.Position[0], vertex.Position[1], vertex.Position[2]};
                if (!Finite(p)) { valid = false; break; }
                const auto delta = p - result.Bounds.center;
                auto& extent = result.Bounds.halfExtents;
                extent.x = std::max(extent.x, std::abs(delta.x));
                extent.y = std::max(extent.y, std::abs(delta.y));
                extent.z = std::max(extent.z, std::abs(delta.z));
            }
        }
        if (!valid || !Finite(result.Bounds)) {
            result.Bounds = result.ReferenceBounds;
            result.LevelCount = 1;
            result.Issue = MeshLODGeometryIssue::InvalidLowerGeometry;
            return result;
        }
    }
    return result;
}
} // namespace GameEngine
