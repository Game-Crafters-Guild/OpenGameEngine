#pragma once

#include <span>

#include "Types/Types.h"

namespace GameEngine::SceneBvh
{

// Non-owning view of one mesh's geometry in its LOCAL space, as consumed by
// ThreadedBvhBuilder. Parallel arrays rather than an interleaved vertex struct:
// callers already hold mesh data in whatever layout the asset pipeline produced,
// and the builder interleaves once into the GPU vertex record itself.
//
// The optional spans may be empty; each has a defined fallback so a mesh
// without normals or UVs still produces a valid tree.
struct MeshGeometryView
{
    // 3 floats per vertex. Required; its length defines the vertex count.
    std::span<const float32> Positions;

    // 3 floats per vertex. Empty -> zero normals, which the traversal shader
    // reads as "no shading normal, use the geometric one".
    std::span<const float32> Normals;

    // 2 floats per vertex. Empty -> zero texcoords.
    std::span<const float32> TexCoords;

    // 3 indices per triangle. Empty -> vertices are consumed sequentially
    // (triangle t uses vertices 3t, 3t+1, 3t+2).
    std::span<const uint32> Indices;

    // 1 uber-material index per triangle. Empty -> every triangle uses 0.
    std::span<const uint32> TriangleMaterials;

    uint32 VertexCount() const { return static_cast<uint32>(Positions.size() / 3u); }

    uint32 TriangleCount() const
    {
        return Indices.empty() ? VertexCount() / 3u
                               : static_cast<uint32>(Indices.size() / 3u);
    }
};

} // namespace GameEngine::SceneBvh
