#pragma once

#include "SceneBvh/ThreadedBvh.h"
#include "Types/Types.h"

namespace GameEngine::SceneBvh
{

// Pass to Step() to run the whole refit in one call.
inline constexpr uint32 kUnboundedRefitBudget = 0xFFFFFFFFu;

enum class ThreadedBvhRefitStatus : uint8
{
    // Every node in the tree now holds exact bounds for the current vertices.
    Complete,

    // The budget ran out mid-walk. Node bounds are partially updated and the
    // tree must NOT be traced until a later Step() returns Complete.
    Budgeted,

    // A threaded-layout invariant is broken, so no valid bounds can be
    // produced from this node array. The caller must discard the tree and
    // rebuild it with ThreadedBvhBuilder; retrying the refit will never help.
    RebuildRequired,
};

// Re-tightens an existing tree's node bounds after its vertices moved, without
// re-partitioning. The caller writes deformed positions into
// ThreadedBvh::VertexData first (topology, and therefore TriangleIndices, must
// be unchanged — that is what makes the partition still valid), then steps this
// until it reports Complete.
//
// Walking the node array in REVERSE pre-order visits every child before its
// parent, because pre-order guarantees a child's index is greater than its
// parent's. Leaves recompute exact bounds from their triangles; interiors union
// their two children (left = i + 1, right = the left child's miss link). Bounds
// are therefore exact after every refit and never accumulate slack; only the
// PARTITION quality is frozen at build-time topology, which for bounded
// deformations costs a little traversal efficiency and never correctness.
//
// Reverse pre-order is what makes the walk resumable: any completed suffix
// stays valid across a yield, so a large mesh's refit can be spread over
// several frames instead of spiking one.
class ThreadedBvhRefit
{
public:
    // Binds to `bvh` for the lifetime of the refit; the tree must outlive it.
    explicit ThreadedBvhRefit(ThreadedBvh& bvh);

    // Consumes at most `workBudget` units, where one unit is either a triangle
    // folded into a leaf's bounds or a node whose bounds were finalized. A leaf
    // that runs out of budget mid-way keeps its partial accumulator and resumes
    // on the next call.
    ThreadedBvhRefitStatus Step(uint32 workBudget);

    uint32 ProcessedNodes() const { return m_ProcessedNodes; }
    uint32 ProcessedTriangles() const { return m_ProcessedTriangles; }

private:
    ThreadedBvhRefitStatus Fail();

    ThreadedBvh* m_Bvh = nullptr;
    uint32 m_NodeCount = 0;

    // Reverse walk cursor. int64 so the "walked past node 0" termination test
    // needs no unsigned-underflow special case.
    int64 m_Cursor = -1;

    // Partial leaf accumulator, live only while a leaf is mid-budget.
    bool m_LeafActive = false;
    uint32 m_LeafNextTriangle = 0;
    uint32 m_LeafEndTriangle = 0;
    Mathematics::AABB m_LeafBounds{};

    bool m_RebuildRequired = false;
    uint32 m_ProcessedNodes = 0;
    uint32 m_ProcessedTriangles = 0;
};

} // namespace GameEngine::SceneBvh
