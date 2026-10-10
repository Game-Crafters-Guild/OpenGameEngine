#pragma once

#include "Mathematics/Types.h"
#include "Types/Types.h"

namespace GameEngine::MeshPicking
{

// Non-owning view of a CPU triangle mesh. The picking module is agnostic to
// where the data came from (ModelAsset, primitive generator, custom buffer):
// callers pass positions and indices and the module does the math.
//
// Positions are read at the given stride so callers can point directly into
// interleaved Vertex structs without copying.
struct MeshView
{
    const Mathematics::Vector3* Positions    = nullptr;
    uint32                      VertexStride = sizeof(Mathematics::Vector3);
    uint32                      VertexCount  = 0;
    const uint32*               Indices      = nullptr;
    uint32                      IndexCount   = 0;

    uint32 TriangleCount() const { return IndexCount / 3u; }
    bool   IsValid()       const { return Positions && Indices && IndexCount >= 3u; }
};

// Result of a successful ray-mesh hit test. All values are in the same space
// as the ray that was tested (world if the ray was world-space, local if
// local-space). Callers transform back as needed.
struct PickHit
{
    uint32               TriangleIndex = ~0u;
    Mathematics::Vector3 Barycentrics{};   // (1-u-v, u, v) per Moller-Trumbore convention
    float32              Distance      = 0.0f;
};

}
