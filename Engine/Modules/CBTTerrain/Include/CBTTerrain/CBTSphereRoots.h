#pragma once

// CBTSphereRoots.h — the cube-sphere base mesh for the spherical CBT domain (plan §8
// C7). Generates the 24 root bisectors of a cube (6 faces x 4 pie-slice triangles) with
// their HeapIDs, neighbor links, and integer cube-corner positions.
//
// Topology (THE hard part — cross-face conformity): each root is one pie slice of a
// cube face, (v0 = a face corner, v1 = the face centroid = apex, v2 = the next face
// corner). Its SPLIT edge (v0, v2) is a cube EDGE, and its twin is the pie slice on the
// ADJACENT face sharing that cube edge — so cross-face adjacency rides the twin link,
// and the CBT compatibility chain (which conforms twins) conforms across faces from
// depth 1. The two within-face legs (v0,v1)/(v1,v2) are the neighbor0/neighbor1 links to
// the sibling pie slices of the same face. The result is a CLOSED manifold: every root
// has all three neighbors valid (no boundary), unlike the planar unit-square base whose
// legs are domain boundaries. This matches the reference (AnisB/large_cbt) general-mesh
// setup our kernels were ported from — prev/next within a face, twin across it.
//
// The GLSL decode (cbt_domain.glsl CBT_SphereRootCorners) computes the SAME per-root
// corners from the SAME face table, so geometry and topology sit on one mesh. The
// CBTSphereRootsTests assert reciprocity, twin-across-cube-edge, outward winding, and
// (via the real-device cross-face conformity oracle) that refinement stays conforming.

#include <algorithm>
#include <array>
#include <cstdint>

#include "CBTTerrain/CBTLayout.h"

namespace GameEngine::CBTTerrain
{

// 6 faces x 4 pie slices. baseDepth = ceil(log2(24)) = 5 (2^5 = 32 >= 24); roots at
// heapID 32 + rootIndex, all at depth 5 (uniform). Slots for heapIDs 56..63 stay unused
// (no root maps there; CBT_RootOf still recovers each root exactly).
inline constexpr uint32_t kCubeFaceCount = 6u;
inline constexpr uint32_t kSlicesPerFace = 4u; // a face quad split into 4 pie slices
static_assert(kSlicesPerFace == 4u, "the (idx & 3u) edge-wrap below assumes 4 slices per face");
inline constexpr uint32_t kSphereRootCount = kCubeFaceCount * kSlicesPerFace; // 24
static_assert(kCubeFaceCount == kCubeFaceCount6,
              "CBTLayout kCubeFaceCount6 (sphere sculpt atlas band count) must equal the cube face count");
inline constexpr uint32_t kSphereBaseDepth = 5u;
static_assert((1u << kSphereBaseDepth) >= kSphereRootCount,
              "2^kSphereBaseDepth must cover kSphereRootCount so each root sits at a distinct "
              "heapID at uniform depth = kSphereBaseDepth");
static_assert(kSphereRootCount <= kDefaultBisectorPoolSize, "roots must fit the bisector pool");

// 6 faces, 4 cube-corner indices each, wound CCW as seen from OUTSIDE (outward normal by
// the right-hand rule). MUST match CBT_CUBE_FACES in cbt_domain.glsl exactly. Corner
// index bits: bit0 = +x, bit1 = +y, bit2 = +z (corner i at ((i&1)?+1:-1, ...)).
inline constexpr int kCubeFaces[24] = {
    1, 3, 7, 5, // +X
    0, 4, 6, 2, // -X
    2, 6, 7, 3, // +Y
    0, 1, 5, 4, // -Y
    4, 5, 7, 6, // +Z
    0, 2, 3, 1  // -Z
};

// One root bisector of the cube-sphere base mesh.
struct CBTSphereRoot
{
    uint64_t HeapID = 0u;
    CBTNeighbors Neighbors{};
    // Integer cube corners (components in {-1, 0, +1}; the centroid of an axis-aligned
    // cube face is integer). v1 is the face centroid (apex), v0/v2 the split-edge corners.
    // Exact integers so the cross-face conformity oracle can compare vertices bit-exactly.
    std::array<int32_t, 3> V0{};
    std::array<int32_t, 3> V1{};
    std::array<int32_t, 3> V2{};
};

inline std::array<int32_t, 3> CBTCubeCorner(int idx)
{
    return {(idx & 1) ? 1 : -1, (idx & 2) ? 1 : -1, (idx & 4) ? 1 : -1};
}

// Builds all 24 roots: HeapIDs, neighbor links (n0/n1 within-face legs, twin across the
// cube edge), and cube corners. Pure function of kCubeFaces — no device, unit-testable.
inline std::array<CBTSphereRoot, kSphereRootCount> BuildSphereRoots()
{
    std::array<CBTSphereRoot, kSphereRootCount> roots{};
    const uint64_t baseHeapID = uint64_t(1) << kSphereBaseDepth; // 32

    for (uint32_t f = 0; f < kCubeFaceCount; ++f)
    {
        std::array<int32_t, 3> centroid{0, 0, 0};
        for (uint32_t k = 0; k < kSlicesPerFace; ++k)
        {
            const std::array<int32_t, 3> c = CBTCubeCorner(kCubeFaces[f * 4u + k]);
            centroid[0] += c[0];
            centroid[1] += c[1];
            centroid[2] += c[2];
        }
        centroid[0] /= 4;
        centroid[1] /= 4;
        centroid[2] /= 4; // axis-aligned face -> exact integer face centre

        for (uint32_t e = 0; e < kSlicesPerFace; ++e)
        {
            const uint32_t r = f * kSlicesPerFace + e;
            CBTSphereRoot& root = roots[r];
            root.HeapID = baseHeapID + r;
            root.V0 = CBTCubeCorner(kCubeFaces[f * kSlicesPerFace + e]);
            root.V1 = centroid;
            root.V2 = CBTCubeCorner(kCubeFaces[f * kSlicesPerFace + ((e + 1u) & 3u)]);
            // Within-face legs: n0 across (v1,v2) leg -> the next pie slice; n1 across
            // (v0,v1) leg -> the previous pie slice. Reciprocal (slice e's n0 == e+1, and
            // slice e+1's n1 == e). Twin filled below. This exact n0/n1 DIRECTION (which
            // leg is n0 vs n1, and the twin's swapped split-edge orientation) is what makes
            // the closed manifold conform; the convention is pinned by the device-gated
            // CBTSphereDomainTests cross-face conformity oracles (which GTEST_SKIP without a
            // Vulkan device) — a swapped assignment would show non-conforming edges there.
            root.Neighbors.Neighbor0 = f * kSlicesPerFace + ((e + 1u) & 3u);
            root.Neighbors.Neighbor1 = f * kSlicesPerFace + ((e + 3u) & 3u);
            root.Neighbors.Twin = kInvalidPointer;
            root.Neighbors.Unused = 0u;
        }
    }

    // Twin = the pie slice on the OTHER face whose split edge is the same cube edge
    // (unordered corner-index pair). Each of the 12 cube edges is the split edge of
    // exactly two pie slices (one per adjacent face), so the match is unique + mutual.
    for (uint32_t r = 0; r < kSphereRootCount; ++r)
    {
        const uint32_t f = r / kSlicesPerFace;
        const uint32_t e = r % kSlicesPerFace;
        const int a = kCubeFaces[f * kSlicesPerFace + e];
        const int b = kCubeFaces[f * kSlicesPerFace + ((e + 1u) & 3u)];
        const int lo = std::min(a, b);
        const int hi = std::max(a, b);
        for (uint32_t r2 = 0; r2 < kSphereRootCount; ++r2)
        {
            if (r2 == r)
                continue;
            const uint32_t f2 = r2 / kSlicesPerFace;
            const uint32_t e2 = r2 % kSlicesPerFace;
            const int a2 = kCubeFaces[f2 * kSlicesPerFace + e2];
            const int b2 = kCubeFaces[f2 * kSlicesPerFace + ((e2 + 1u) & 3u)];
            if (std::min(a2, b2) == lo && std::max(a2, b2) == hi)
            {
                roots[r].Neighbors.Twin = r2;
                break;
            }
        }
    }
    return roots;
}

} // namespace GameEngine::CBTTerrain
