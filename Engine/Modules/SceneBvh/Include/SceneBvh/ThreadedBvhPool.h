#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "SceneBvh/ThreadedBvh.h"

namespace GameEngine::SceneBvh
{

// Node range one pooled mesh occupies inside PooledThreadedBvh::Nodes, in exactly
// the half-open [root, end) form TlasInstance::BlasRoot/BlasEnd
// wants. Empty (End <= Begin) marks a mesh that contributed nothing — an empty
// input tree, or one the pool refused because it would have overflowed the
// node/triangle ceilings; a traversal seeded with an empty range terminates on
// its first loop test, so no caller needs a special case.
struct PooledBvhRange
{
    uint32_t NodeBegin = 0;
    uint32_t NodeEnd   = 0;

    bool IsEmpty() const { return NodeEnd <= NodeBegin; }
};

// Several per-mesh ThreadedBvhs concatenated into one set of shared flat
// arrays, each mapping 1:1 onto an SSBO. ThreadedBvh.h emits BLAS-LOCAL node
// indices, miss links and leaf triangle offsets (its POOLING note); this is
// the rebasing that turns N independent trees into one buffer set a single
// traversal shader can address.
struct PooledThreadedBvh
{
    std::vector<uint32_t> Nodes;
    std::vector<uint32_t> TriangleIndices;
    std::vector<uint32_t> TriangleMaterials;
    std::vector<float> VertexData;

    // One entry per input mesh, in input order.
    std::vector<PooledBvhRange> Ranges;
};

// Rebases and concatenates `meshes` into one PooledThreadedBvh.
//
// Per mesh, against running node/triangle/vertex bases:
//   * every node's miss link (word +6) gains the node base — universal to
//     interior AND leaf nodes, since the miss link is the escape pointer for
//     both;
//   * a leaf word (+7) is re-encoded with its triangle offset shifted by the
//     TRIANGLE-granular base (a leaf addresses TriangleIndices[3t..3t+2] per
//     triangle t, so the base accumulates TriangleCount, not word count); the
//     interior sentinel passes through untouched;
//   * AABB words (+0..+5) are unchanged — they are local-space bounds, and the
//     instance transform is applied at the TLAS level, not here;
//   * every TriangleIndices entry gains the vertex base (they are vertex
//     indices);
//   * VertexData is appended verbatim (local-space floats).
//
// TriangleMaterials is NOT copied from the input. Each mesh's triangles are
// all stamped with `meshMaterialSlots[i]`, that mesh's index into the caller's
// global uber-material table — the one-material-per-submesh v1 rule, which is
// also why the builder is fed an empty MeshGeometryView::TriangleMaterials.
//
// Returns an empty pool (and logs) when the two spans disagree in size. A mesh
// that would push the pooled node count past kThreadedBvhMaxNodes or the
// pooled triangle count past the leaf word's 24-bit offset field is skipped
// with an empty range, and so is every mesh after it.
PooledThreadedBvh PoolThreadedBvhs(std::span<const ThreadedBvh> meshes,
                                std::span<const uint32_t> meshMaterialSlots);

}  // namespace GameEngine::SceneBvh
