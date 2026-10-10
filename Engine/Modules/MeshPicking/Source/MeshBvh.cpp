#include "MeshPicking/MeshBvh.h"

#include "SceneBvh/BinnedSahSplit.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstring>
// Per-node 8-child SIMD slab test path selection.
//
// AVX2 (x86_64): one 256-bit lane covers all 8 children in a single pass.
// NEON (aarch64 / Apple Silicon / Windows-ARM64): two 128-bit passes of
//      4 children each; aarch64 implies NEON unconditionally so no
//      runtime check needed.
// Scalar fallback: any architecture without one of the above.
//
// The three implementations share the same control flow and produce
// bit-identical (hitBits, tMinArr) outputs for the downstream sort and
// phase-1/2 traversal.
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#  define GE_MESHBVH_USE_AVX2 1
#  include <immintrin.h>
#else
#  define GE_MESHBVH_USE_AVX2 0
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
#  define GE_MESHBVH_USE_NEON 1
#  include <arm_neon.h>
#else
#  define GE_MESHBVH_USE_NEON 0
#endif
#include <limits>

#include "Core/CpuProfiler.h"

namespace GameEngine::MeshPicking
{

namespace
{

using Mathematics::Vector3;

constexpr float32 kRayTriangleEps = 1e-7f;

inline Vector3 ReadPosition(const MeshView& mesh, uint32 vertexIndex)
{
    const auto* base = reinterpret_cast<const std::byte*>(mesh.Positions);
    const auto* p    = reinterpret_cast<const Vector3*>(base + size_t(vertexIndex) * size_t(mesh.VertexStride));
    return *p;
}

inline void ExpandBounds(Vector3& mn, Vector3& mx, const Vector3& p)
{
    mn.x = std::min(mn.x, p.x); mn.y = std::min(mn.y, p.y); mn.z = std::min(mn.z, p.z);
    mx.x = std::max(mx.x, p.x); mx.y = std::max(mx.y, p.y); mx.z = std::max(mx.z, p.z);
}

// Sentinels for accumulator initialization. The names match their *role*
// not their value: SentinelHighForMin starts a running-min accumulator
// at +inf so any sample lowers it; SentinelLowForMax starts a running-max
// accumulator at -inf so any sample raises it. Old names InfMin/InfMax
// were inverse-suggestive — kept as deprecated aliases below for any
// stale call sites the audit missed.
inline Vector3 SentinelHighForMin() { return {std::numeric_limits<float32>::max(),  std::numeric_limits<float32>::max(),  std::numeric_limits<float32>::max()};  }
inline Vector3 SentinelLowForMax()  { return {-std::numeric_limits<float32>::max(), -std::numeric_limits<float32>::max(), -std::numeric_limits<float32>::max()}; }
inline Vector3 InfMin() { return SentinelHighForMin(); }
inline Vector3 InfMax() { return SentinelLowForMax(); }

inline float32 FullSurfaceArea(const Vector3& mn, const Vector3& mx)
{
    const float32 dx = std::max(0.0f, mx.x - mn.x);
    const float32 dy = std::max(0.0f, mx.y - mn.y);
    const float32 dz = std::max(0.0f, mx.z - mn.z);
    // Full SA, matching the units of BinnedSahCandidate::Cost.
    return 2.0f * (dx * dy + dy * dz + dz * dx);
}

inline Vector3 TriangleCentroid(const Vector3& v0, const Vector3& v1, const Vector3& v2)
{
    return Vector3((v0.x + v1.x + v2.x) * (1.0f / 3.0f),
                   (v0.y + v1.y + v2.y) * (1.0f / 3.0f),
                   (v0.z + v1.z + v2.z) * (1.0f / 3.0f));
}


}

bool RayTriangle(const Vector3& origin,
                 const Vector3& direction,
                 const Vector3& v0,
                 const Vector3& v1,
                 const Vector3& v2,
                 float32& outT,
                 float32& outU,
                 float32& outV)
{
    const Vector3 e1  = v1 - v0;
    const Vector3 e2  = v2 - v0;
    const Vector3 p   = Vector3::Cross(direction, e2);
    const float32 det = Vector3::Dot(e1, p);
    if (det > -kRayTriangleEps && det < kRayTriangleEps)
        return false;

    const float32 invDet = 1.0f / det;
    const Vector3 s      = origin - v0;
    const float32 u      = Vector3::Dot(s, p) * invDet;
    if (u < 0.0f || u > 1.0f)
        return false;

    const Vector3 q = Vector3::Cross(s, e1);
    const float32 v = Vector3::Dot(direction, q) * invDet;
    if (v < 0.0f || u + v > 1.0f)
        return false;

    const float32 t = Vector3::Dot(e2, q) * invDet;
    if (t <= kRayTriangleEps)
        return false;

    outT = t;
    outU = u;
    outV = v;
    return true;
}

bool RayMesh(const MeshView& mesh,
             const Vector3& origin,
             const Vector3& direction,
             PickHit& outHit)
{
    if (!mesh.IsValid())
        return false;

    bool    hit       = false;
    float32 bestT     = std::numeric_limits<float32>::max();
    uint32  bestTri   = ~0u;
    float32 bestU     = 0.0f;
    float32 bestV     = 0.0f;

    const uint32 triCount = mesh.TriangleCount();
    for (uint32 i = 0; i < triCount; ++i)
    {
        const uint32 i0 = mesh.Indices[i * 3 + 0];
        const uint32 i1 = mesh.Indices[i * 3 + 1];
        const uint32 i2 = mesh.Indices[i * 3 + 2];
        if (i0 >= mesh.VertexCount || i1 >= mesh.VertexCount || i2 >= mesh.VertexCount)
            continue;

        const Vector3 v0 = ReadPosition(mesh, i0);
        const Vector3 v1 = ReadPosition(mesh, i1);
        const Vector3 v2 = ReadPosition(mesh, i2);

        float32 t = 0.0f, u = 0.0f, v = 0.0f;
        if (RayTriangle(origin, direction, v0, v1, v2, t, u, v) && t < bestT)
        {
            bestT   = t;
            bestU   = u;
            bestV   = v;
            bestTri = i;
            hit     = true;
        }
    }

    if (hit)
    {
        outHit.Distance      = bestT;
        outHit.TriangleIndex = bestTri;
        outHit.Barycentrics  = Vector3(1.0f - bestU - bestV, bestU, bestV);
    }
    return hit;
}

MeshBvh MeshBvh::Build(const MeshView& mesh)
{
    GE_CPU_PROFILE_SCOPE("MeshBvh.Build");
    MeshBvh bvh;
    if (!mesh.IsValid())
        return bvh;

    const uint32 triCount = mesh.TriangleCount();
    bvh.m_Triangles.reserve(triCount);

    for (uint32 i = 0; i < triCount; ++i)
    {
        const uint32 i0 = mesh.Indices[i * 3 + 0];
        const uint32 i1 = mesh.Indices[i * 3 + 1];
        const uint32 i2 = mesh.Indices[i * 3 + 2];
        if (i0 >= mesh.VertexCount || i1 >= mesh.VertexCount || i2 >= mesh.VertexCount)
            continue;

        Triangle tri;
        tri.V0            = ReadPosition(mesh, i0);
        tri.V1            = ReadPosition(mesh, i1);
        tri.V2            = ReadPosition(mesh, i2);
        tri.OriginalIndex = i;
        bvh.m_Triangles.push_back(tri);
    }

    if (bvh.m_Triangles.empty())
        return bvh;

    bvh.m_TriIndices.resize(bvh.m_Triangles.size());
    for (uint32 i = 0; i < bvh.m_TriIndices.size(); ++i)
        bvh.m_TriIndices[i] = i;

    // Phase D: build a binary SAH tree as a temporary, then collapse it
    // into the CWBVH8 storage. The binary intermediate is discarded after
    // collapse; only m_Nodes8 + m_Triangles + m_TriIndices persist.
    //
    // Per-triangle centroids and bounds are computed ONCE here for the shared
    // binned-SAH kernel; the previous build recomputed both per node per axis.
    // The centroid is the vertex mean — this builder's binning definition,
    // distinct from ThreadedBvhBuilder's AABB midpoint (see BinnedSahRange).
    std::vector<BinaryNode> bins;
    bins.reserve(bvh.m_Triangles.size() * 2u);
    {
        const size_t n = bvh.m_Triangles.size();
        bvh.m_BuildCentroids.resize(n * 3u);
        bvh.m_BuildTriBounds.resize(n * 6u);
        for (size_t t = 0; t < n; ++t)
        {
            const Triangle& tri = bvh.m_Triangles[t];
            const Vector3 c = TriangleCentroid(tri.V0, tri.V1, tri.V2);
            bvh.m_BuildCentroids[t * 3u + 0] = c.x;
            bvh.m_BuildCentroids[t * 3u + 1] = c.y;
            bvh.m_BuildCentroids[t * 3u + 2] = c.z;
            Vector3 mn = InfMin();
            Vector3 mx = InfMax();
            ExpandBounds(mn, mx, tri.V0);
            ExpandBounds(mn, mx, tri.V1);
            ExpandBounds(mn, mx, tri.V2);
            bvh.m_BuildTriBounds[t * 6u + 0] = mn.x;
            bvh.m_BuildTriBounds[t * 6u + 1] = mn.y;
            bvh.m_BuildTriBounds[t * 6u + 2] = mn.z;
            bvh.m_BuildTriBounds[t * 6u + 3] = mx.x;
            bvh.m_BuildTriBounds[t * 6u + 4] = mx.y;
            bvh.m_BuildTriBounds[t * 6u + 5] = mx.z;
        }
    }
    bvh.BuildBinary(bins, 0u, static_cast<uint32>(bvh.m_Triangles.size()));
    bvh.m_BuildCentroids.clear();
    bvh.m_BuildCentroids.shrink_to_fit();
    bvh.m_BuildTriBounds.clear();
    bvh.m_BuildTriBounds.shrink_to_fit();

    if (!bins.empty())
    {
        bvh.m_RootBounds.min = bins[0].BoundsMin;
        bvh.m_RootBounds.max = bins[0].BoundsMax;
        // Collapse: 8 binary descendants per BVH8 node => roughly N/8
        // BVH8 nodes for an N-leaf binary tree.
        bvh.m_Nodes8.reserve(bins.size() / 4u + 1u);
        bvh.CollapseFromBinary(bins, 0u);
    }
    return bvh;
}

namespace
{

}

uint32 MeshBvh::BuildBinary(std::vector<BinaryNode>& bins, uint32 first, uint32 count)
{
    const uint32 nodeIdx = static_cast<uint32>(bins.size());
    bins.push_back({});
    bins[nodeIdx].FirstTri = first;
    bins[nodeIdx].TriCount = 0u;

    // Combined triangle AABB and centroid AABB for the range, off the arrays
    // Build() precomputed (AABB-of-AABBs equals AABB-of-vertices exactly:
    // min/max only).
    Vector3 mn  = InfMin();
    Vector3 mx  = InfMax();
    Vector3 cmn = InfMin();
    Vector3 cmx = InfMax();
    for (uint32 i = 0; i < count; ++i)
    {
        const size_t tri = m_TriIndices[first + i];
        const float32* tb = &m_BuildTriBounds[tri * 6u];
        ExpandBounds(mn, mx, Vector3(tb[0], tb[1], tb[2]));
        ExpandBounds(mn, mx, Vector3(tb[3], tb[4], tb[5]));
        const float32* tc = &m_BuildCentroids[tri * 3u];
        ExpandBounds(cmn, cmx, Vector3(tc[0], tc[1], tc[2]));
    }
    bins[nodeIdx].BoundsMin = mn;
    bins[nodeIdx].BoundsMax = mx;

    if (count <= kLeafSize)
    {
        bins[nodeIdx].TriCount = count;
        return nodeIdx;
    }

    // Split scoring is the shared binned-SAH kernel; this builder's policy
    // around it is unchanged: it always splits above kLeafSize (no leaf-beats-
    // split comparison — leaf quality is CollapseFromBinary's concern), and
    // halves by index when the range is degenerate.
    SceneBvh::BinnedSahRange range;
    range.Order = std::span<const uint32>(m_TriIndices).subspan(first, count);
    range.Centroids = m_BuildCentroids;
    range.TriangleBounds = m_BuildTriBounds;
    range.CentroidBounds.min = cmn;
    range.CentroidBounds.max = cmx;
    const SceneBvh::BinnedSahCandidate candidate = SceneBvh::FindBestBinnedSahSplit(range);

    const float32 leafCostNoSplit = static_cast<float32>(count) * FullSurfaceArea(mn, mx);
    if (!candidate.Found || candidate.Cost >= leafCostNoSplit * 1e10f)
    {
        const uint32 leftCountFallback = count / 2u;
        const uint32 leftIdx  = BuildBinary(bins, first, leftCountFallback);
        const uint32 rightIdx = BuildBinary(bins, first + leftCountFallback, count - leftCountFallback);
        bins[nodeIdx].Left  = leftIdx;
        bins[nodeIdx].Right = rightIdx;
        return nodeIdx;
    }

    // Partition the tri-index range in-place around the chosen split. A
    // triangle goes left if its centroid bin <= candidate.Bin on the chosen
    // axis; the predicate assigns bins exactly as the kernel scored them.
    auto leftOfSplit = [&](uint32 triIdx) {
        return SceneBvh::BinnedSahBinOf(m_BuildCentroids[static_cast<size_t>(triIdx) * 3u + candidate.Axis],
                                        candidate.AxisMin, candidate.Scale) <= candidate.Bin;
    };

    uint32 lo = first;
    uint32 hi = first + count - 1u;
    while (lo <= hi)
    {
        if (leftOfSplit(m_TriIndices[lo]))
        {
            ++lo;
        }
        else
        {
            std::swap(m_TriIndices[lo], m_TriIndices[hi]);
            if (hi == 0u) break;
            --hi;
        }
    }
    uint32 leftCountFinal = lo - first;
    if (leftCountFinal == 0u || leftCountFinal == count)
        leftCountFinal = count / 2u;

    const uint32 leftIdx  = BuildBinary(bins, first, leftCountFinal);
    const uint32 rightIdx = BuildBinary(bins, first + leftCountFinal, count - leftCountFinal);
    bins[nodeIdx].Left  = leftIdx;
    bins[nodeIdx].Right = rightIdx;
    return nodeIdx;
}

namespace
{

// Conservative quantization helpers. Min floors so the dequantized value
// is at most the input; Max ceils so the dequantized value is at least
// the input. Result: dequantized AABB always encloses the true child
// AABB. False-positive ray hits at boundaries are filtered by the leaf
// triangle test; false-negatives (missed real hits) are impossible.
inline uint8 QuantFloor(float32 t)
{
    if (t <= 0.0f) return 0u;
    if (t >= 255.0f) return 255u;
    return static_cast<uint8>(std::floor(t));
}
inline uint8 QuantCeil(float32 t)
{
    if (t <= 0.0f) return 0u;
    if (t >= 255.0f) return 255u;
    return static_cast<uint8>(std::ceil(t));
}

}

uint32 MeshBvh::CollapseFromBinary(const std::vector<BinaryNode>& bins, uint32 binIdx)
{
    // Reserve our slot now so child recursion's m_Nodes8.push_back can't
    // invalidate references. Don't hold a reference into m_Nodes8 across
    // recursive calls — only access via index.
    const uint32 outIdx = static_cast<uint32>(m_Nodes8.size());
    m_Nodes8.emplace_back();

    // Greedy collapse: start with [binIdx], then while we have <8 slots
    // and at least one slot is internal, replace the largest-surface-area
    // internal slot with its 2 binary children (net +1 slot per expansion).
    // Stops when all slots are leaves OR adding 1 more would overflow 8.
    uint32 slotBin[8];
    int    slotCount = 1;
    slotBin[0] = binIdx;

    while (slotCount < 8)
    {
        int   bestSlot  = -1;
        float bestSA    = -1.0f;
        for (int i = 0; i < slotCount; ++i)
        {
            const auto& n = bins[slotBin[i]];
            if (n.TriCount > 0u) continue;  // leaf can't be expanded
            // Argmax key only, so the full-vs-half surface-area factor is moot.
            const float32 sa = FullSurfaceArea(n.BoundsMin, n.BoundsMax);
            if (sa > bestSA)
            {
                bestSA   = sa;
                bestSlot = i;
            }
        }
        if (bestSlot < 0) break;  // all slots are leaves

        // Expand: replace slotBin[bestSlot] with its Left + Right children.
        const auto& parent = bins[slotBin[bestSlot]];
        // Shift slots after bestSlot right by 1 to make room for Right.
        for (int i = slotCount; i > bestSlot + 1; --i)
            slotBin[i] = slotBin[i - 1];
        slotBin[bestSlot]     = parent.Left;
        slotBin[bestSlot + 1] = parent.Right;
        slotCount++;
    }

    // Compute the parent (this BVH8 node) AABB as the union of all slot
    // children's bounds. This is the full-precision bound against which
    // each child gets quantized.
    Vector3 pMin = InfMin();
    Vector3 pMax = InfMax();
    for (int i = 0; i < slotCount; ++i)
    {
        const auto& n = bins[slotBin[i]];
        ExpandBounds(pMin, pMax, n.BoundsMin);
        ExpandBounds(pMin, pMax, n.BoundsMax);
    }

    // Per-axis quantization step. Guard against zero extent (degenerate
    // axis) by treating the step as 1.0 — all qMin = 0, qMax = 0, which
    // dequantizes back to a zero-extent slab on that axis. Correct.
    const float32 invStepX = (pMax.x > pMin.x) ? 255.0f / (pMax.x - pMin.x) : 0.0f;
    const float32 invStepY = (pMax.y > pMin.y) ? 255.0f / (pMax.y - pMin.y) : 0.0f;
    const float32 invStepZ = (pMax.z > pMin.z) ? 255.0f / (pMax.z - pMin.z) : 0.0f;

    // Write parent AABB + valid mask into the slot (without holding a
    // reference; recursion below pushes new BvhNode8 entries).
    m_Nodes8[outIdx].ParentMin = pMin;
    m_Nodes8[outIdx].ParentMax = pMax;
    m_Nodes8[outIdx].ValidMask = 0u;
    for (int i = slotCount; i < 8; ++i)
    {
        // Defense in depth: encode unused slots as INVERTED quantized
        // slabs (qMin=255, qMax=0). A degenerate-but-zero slab (qMin=0,
        // qMax=0) is technically a point that the AVX2 SLAB test can
        // report as a "hit" if a ray passes through that exact corner —
        // and `ValidMask & hitBits` would then need to mask them out.
        // Inverting the slab makes hit IMPOSSIBLE (tMin > tMax always),
        // so even if a future code path drops the ValidMask gate the
        // unused lanes can't produce a phantom hit.
        m_Nodes8[outIdx].ChildMinX[i] = 255u;
        m_Nodes8[outIdx].ChildMinY[i] = 255u;
        m_Nodes8[outIdx].ChildMinZ[i] = 255u;
        m_Nodes8[outIdx].ChildMaxX[i] = 0u;
        m_Nodes8[outIdx].ChildMaxY[i] = 0u;
        m_Nodes8[outIdx].ChildMaxZ[i] = 0u;
        m_Nodes8[outIdx].ChildIdx[i]      = 0u;
        m_Nodes8[outIdx].ChildTriCount[i] = 0u;
    }

    // Quantize each slot's AABB and recurse for internal slots. Cache the
    // slot data into locals first — child recursion will append to
    // m_Nodes8 and may reallocate.
    for (int i = 0; i < slotCount; ++i)
    {
        const auto& n = bins[slotBin[i]];
        const uint8 qMinX = QuantFloor((n.BoundsMin.x - pMin.x) * invStepX);
        const uint8 qMinY = QuantFloor((n.BoundsMin.y - pMin.y) * invStepY);
        const uint8 qMinZ = QuantFloor((n.BoundsMin.z - pMin.z) * invStepZ);
        const uint8 qMaxX = QuantCeil ((n.BoundsMax.x - pMin.x) * invStepX);
        const uint8 qMaxY = QuantCeil ((n.BoundsMax.y - pMin.y) * invStepY);
        const uint8 qMaxZ = QuantCeil ((n.BoundsMax.z - pMin.z) * invStepZ);

        m_Nodes8[outIdx].ChildMinX[i] = qMinX;
        m_Nodes8[outIdx].ChildMinY[i] = qMinY;
        m_Nodes8[outIdx].ChildMinZ[i] = qMinZ;
        m_Nodes8[outIdx].ChildMaxX[i] = qMaxX;
        m_Nodes8[outIdx].ChildMaxY[i] = qMaxY;
        m_Nodes8[outIdx].ChildMaxZ[i] = qMaxZ;

        if (n.TriCount > 0u)
        {
            // Leaf slot — direct triangle range.
            m_Nodes8[outIdx].ChildIdx[i]      = n.FirstTri;
            m_Nodes8[outIdx].ChildTriCount[i] = static_cast<uint16>(n.TriCount);
        }
        else
        {
            // Internal slot — recurse to build sub-tree's BVH8.
            const uint32 childOut = CollapseFromBinary(bins, slotBin[i]);
            m_Nodes8[outIdx].ChildIdx[i]      = childOut;
            m_Nodes8[outIdx].ChildTriCount[i] = 0u;
        }
        m_Nodes8[outIdx].ValidMask |= static_cast<uint8>(1u << i);
    }

    return outIdx;
}

bool MeshBvh::Raycast(const Vector3& origin,
                      const Vector3& direction,
                      PickHit& outHit) const
{
    GE_CPU_PROFILE_SCOPE("MeshBvh.Raycast");
    if (m_Nodes8.empty())
        return false;

    bool    hit     = false;
    float32 bestT   = std::numeric_limits<float32>::max();
    uint32  bestTri = ~0u;
    float32 bestU   = 0.0f;
    float32 bestV   = 0.0f;

    // Zero-safe direction. If any component is exactly 0 (or denormal-
    // close-to-0) then `1/dir` is ±inf, and `(slabMin - origin) * inf`
    // is NaN exactly when origin sits on the slab face. NaN poisoning
    // then propagates through min/max in subtle ways. Clamping |dir|
    // away from zero gives a finite (very large) invDir instead of
    // ±inf, preserving "ray misses everything" semantics for axis-
    // aligned rays without relying on NaN-propagation rules.
    //
    // NOT a defense against NaN-direction inputs: std::abs(NaN) == NaN
    // and (NaN < kEps) == false, so this lambda passes NaN through
    // unchanged. NaN inputs naturally produce zero hits anyway because
    // _CMP_*_OQ compares with NaN return false → hitMask all-zero →
    // continue (no traversal). Don't add an isnan check; the implicit
    // path is already correct and adding a branch would slow the hot
    // path for no benefit.
    auto safeDir = [](float32 d) {
        constexpr float32 kEps = 1e-30f;
        if (std::abs(d) < kEps) return (d < 0.0f) ? -kEps : kEps;
        return d;
    };
    const float32 invDirX_s = 1.0f / safeDir(direction.x);
    const float32 invDirY_s = 1.0f / safeDir(direction.y);
    const float32 invDirZ_s = 1.0f / safeDir(direction.z);
#if GE_MESHBVH_USE_AVX2
    const __m256 rOrigX  = _mm256_set1_ps(origin.x);
    const __m256 rOrigY  = _mm256_set1_ps(origin.y);
    const __m256 rOrigZ  = _mm256_set1_ps(origin.z);
    const __m256 rInvDirX = _mm256_set1_ps(invDirX_s);
    const __m256 rInvDirY = _mm256_set1_ps(invDirY_s);
    const __m256 rInvDirZ = _mm256_set1_ps(invDirZ_s);
    const __m256 zero     = _mm256_setzero_ps();
    const __m256 q255inv  = _mm256_set1_ps(1.0f / 255.0f);
#elif GE_MESHBVH_USE_NEON
    const float32x4_t rOrigX_v   = vdupq_n_f32(origin.x);
    const float32x4_t rOrigY_v   = vdupq_n_f32(origin.y);
    const float32x4_t rOrigZ_v   = vdupq_n_f32(origin.z);
    const float32x4_t rInvDirX_v = vdupq_n_f32(invDirX_s);
    const float32x4_t rInvDirY_v = vdupq_n_f32(invDirY_s);
    const float32x4_t rInvDirZ_v = vdupq_n_f32(invDirZ_s);
    const float32x4_t zero_v     = vdupq_n_f32(0.0f);
    const float32x4_t q255inv_v  = vdupq_n_f32(1.0f / 255.0f);
#endif

    // Iterative traversal. Depth is bounded by ~log_8(triCount) but the
    // greedy collapse can produce shallower-than-optimal nodes on
    // unbalanced inputs; 128 is generous headroom (every BVH8 node
    // descent below this depth would represent at most 8^128 triangles,
    // which is astronomical). Debug-asserts on overflow to catch any
    // pathology that pushes past the bound.
    static constexpr int kStackSize = 128;
    std::array<uint32, kStackSize> stack;
    int sp = 0;
    stack[sp++] = 0u;  // root

    while (sp > 0)
    {
        const uint32 nodeIdx = stack[--sp];
        const BvhNode8& node = m_Nodes8[nodeIdx];

        // Decode the 8 child AABBs against the parent's full-precision
        // bounds and ray-AABB slab test all 8. Produces hitBits (mask of
        // children that pass tMin<=tMax AND tMax>=0 AND tMin<bestT, ANDed
        // with node.ValidMask) and tMinArr[i] = slab tEnter for slot i.
        alignas(32) float32 tMinArr[8];
        int hitBits = 0;
#if GE_MESHBVH_USE_AVX2
        // AVX2 path: process all 8 children in one 256-bit lane.
        const __m256 stepX = _mm256_set1_ps((node.ParentMax.x - node.ParentMin.x));
        const __m256 stepY = _mm256_set1_ps((node.ParentMax.y - node.ParentMin.y));
        const __m256 stepZ = _mm256_set1_ps((node.ParentMax.z - node.ParentMin.z));
        const __m256 pMinX = _mm256_set1_ps(node.ParentMin.x);
        const __m256 pMinY = _mm256_set1_ps(node.ParentMin.y);
        const __m256 pMinZ = _mm256_set1_ps(node.ParentMin.z);

        auto decode = [&](const uint8* q, const __m256& step, const __m256& pMin) -> __m256 {
            const __m128i qi8  = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(q));
            const __m256i qi32 = _mm256_cvtepu8_epi32(qi8);
            const __m256  qf   = _mm256_cvtepi32_ps(qi32);
            // qf in [0, 255] -> normalized [0, 1] -> scaled to step -> + pMin.
            const __m256  norm = _mm256_mul_ps(qf, q255inv);
            return _mm256_fmadd_ps(norm, step, pMin);
        };

        const __m256 minX = decode(node.ChildMinX, stepX, pMinX);
        const __m256 minY = decode(node.ChildMinY, stepY, pMinY);
        const __m256 minZ = decode(node.ChildMinZ, stepZ, pMinZ);
        const __m256 maxX = decode(node.ChildMaxX, stepX, pMinX);
        const __m256 maxY = decode(node.ChildMaxY, stepY, pMinY);
        const __m256 maxZ = decode(node.ChildMaxZ, stepZ, pMinZ);

        // Per-axis SLAB test. With |dir| ~ 0 components, invDir is ±inf
        // and (mn-orig)*inf is ±inf; min/max picks correctly.
        const __m256 t1x = _mm256_mul_ps(_mm256_sub_ps(minX, rOrigX), rInvDirX);
        const __m256 t2x = _mm256_mul_ps(_mm256_sub_ps(maxX, rOrigX), rInvDirX);
        const __m256 t1y = _mm256_mul_ps(_mm256_sub_ps(minY, rOrigY), rInvDirY);
        const __m256 t2y = _mm256_mul_ps(_mm256_sub_ps(maxY, rOrigY), rInvDirY);
        const __m256 t1z = _mm256_mul_ps(_mm256_sub_ps(minZ, rOrigZ), rInvDirZ);
        const __m256 t2z = _mm256_mul_ps(_mm256_sub_ps(maxZ, rOrigZ), rInvDirZ);

        const __m256 tMinX = _mm256_min_ps(t1x, t2x);
        const __m256 tMaxX = _mm256_max_ps(t1x, t2x);
        const __m256 tMinY = _mm256_min_ps(t1y, t2y);
        const __m256 tMaxY = _mm256_max_ps(t1y, t2y);
        const __m256 tMinZ = _mm256_min_ps(t1z, t2z);
        const __m256 tMaxZ = _mm256_max_ps(t1z, t2z);

        const __m256 tMin = _mm256_max_ps(tMinX, _mm256_max_ps(tMinY, tMinZ));
        const __m256 tMax = _mm256_min_ps(tMaxX, _mm256_min_ps(tMaxY, tMaxZ));

        // Hit when tMin <= tMax AND tMax >= 0 AND tMin < bestT.
        const __m256 bestTv = _mm256_set1_ps(bestT);
        const __m256 hit1   = _mm256_cmp_ps(tMin, tMax, _CMP_LE_OQ);
        const __m256 hit2   = _mm256_cmp_ps(tMax, zero, _CMP_GE_OQ);
        const __m256 hit3   = _mm256_cmp_ps(tMin, bestTv, _CMP_LT_OQ);
        const __m256 hitMask = _mm256_and_ps(_mm256_and_ps(hit1, hit2), hit3);
        const int    rawBits = _mm256_movemask_ps(hitMask);
        hitBits = rawBits & static_cast<int>(node.ValidMask);
        _mm256_store_ps(tMinArr, tMin);
#elif GE_MESHBVH_USE_NEON
        // NEON path (aarch64): one 128-bit lane covers 4 children, run
        // twice (slots 0-3 then 4-7). Identical algorithm and bit-output
        // semantics to the AVX2 branch — same dequantize formula
        // (q/255 * step + parentMin via FMA), same slab math, same hit
        // predicate (tMin<=tMax AND tMax>=0 AND tMin<bestT) ANDed with
        // node.ValidMask.
        const float32x4_t stepX_v  = vdupq_n_f32(node.ParentMax.x - node.ParentMin.x);
        const float32x4_t stepY_v  = vdupq_n_f32(node.ParentMax.y - node.ParentMin.y);
        const float32x4_t stepZ_v  = vdupq_n_f32(node.ParentMax.z - node.ParentMin.z);
        const float32x4_t pMinX_v  = vdupq_n_f32(node.ParentMin.x);
        const float32x4_t pMinY_v  = vdupq_n_f32(node.ParentMin.y);
        const float32x4_t pMinZ_v  = vdupq_n_f32(node.ParentMin.z);
        const float32x4_t bestT_v  = vdupq_n_f32(bestT);

        // Decode all 8 quantized child bytes per axis into low/high
        // float32x4 halves. ChildMinX/Y/Z and ChildMaxX/Y/Z are uint8[8],
        // so a single vld1_u8 covers the whole node.
        auto decode8 = [&](const uint8* q,
                           const float32x4_t& step,
                           const float32x4_t& pMin,
                           float32x4_t& outLo,
                           float32x4_t& outHi)
        {
            const uint8x8_t   qi8   = vld1_u8(reinterpret_cast<const uint8_t*>(q));
            const uint16x8_t  qi16  = vmovl_u8(qi8);
            const uint32x4_t  qi32lo = vmovl_u16(vget_low_u16(qi16));
            const uint32x4_t  qi32hi = vmovl_u16(vget_high_u16(qi16));
            const float32x4_t qfLo   = vcvtq_f32_u32(qi32lo);
            const float32x4_t qfHi   = vcvtq_f32_u32(qi32hi);
            const float32x4_t normLo = vmulq_f32(qfLo, q255inv_v);
            const float32x4_t normHi = vmulq_f32(qfHi, q255inv_v);
            // pMin + norm * step (FMA).
            outLo = vfmaq_f32(pMin, normLo, step);
            outHi = vfmaq_f32(pMin, normHi, step);
        };

        float32x4_t minX_lo, minX_hi, minY_lo, minY_hi, minZ_lo, minZ_hi;
        float32x4_t maxX_lo, maxX_hi, maxY_lo, maxY_hi, maxZ_lo, maxZ_hi;
        decode8(node.ChildMinX, stepX_v, pMinX_v, minX_lo, minX_hi);
        decode8(node.ChildMinY, stepY_v, pMinY_v, minY_lo, minY_hi);
        decode8(node.ChildMinZ, stepZ_v, pMinZ_v, minZ_lo, minZ_hi);
        decode8(node.ChildMaxX, stepX_v, pMinX_v, maxX_lo, maxX_hi);
        decode8(node.ChildMaxY, stepY_v, pMinY_v, maxY_lo, maxY_hi);
        decode8(node.ChildMaxZ, stepZ_v, pMinZ_v, maxZ_lo, maxZ_hi);

        // 4-wide slab test. Returns the 4-bit hit mask (lanes 0..3 → bits 0..3)
        // and writes tMin into outTmin.
        auto slab4 = [&](float32x4_t mnX, float32x4_t mxX,
                         float32x4_t mnY, float32x4_t mxY,
                         float32x4_t mnZ, float32x4_t mxZ,
                         float32* outTmin) -> uint32
        {
            const float32x4_t t1x = vmulq_f32(vsubq_f32(mnX, rOrigX_v), rInvDirX_v);
            const float32x4_t t2x = vmulq_f32(vsubq_f32(mxX, rOrigX_v), rInvDirX_v);
            const float32x4_t t1y = vmulq_f32(vsubq_f32(mnY, rOrigY_v), rInvDirY_v);
            const float32x4_t t2y = vmulq_f32(vsubq_f32(mxY, rOrigY_v), rInvDirY_v);
            const float32x4_t t1z = vmulq_f32(vsubq_f32(mnZ, rOrigZ_v), rInvDirZ_v);
            const float32x4_t t2z = vmulq_f32(vsubq_f32(mxZ, rOrigZ_v), rInvDirZ_v);

            const float32x4_t tMin = vmaxq_f32(vminq_f32(t1x, t2x),
                                               vmaxq_f32(vminq_f32(t1y, t2y),
                                                         vminq_f32(t1z, t2z)));
            const float32x4_t tMax = vminq_f32(vmaxq_f32(t1x, t2x),
                                               vminq_f32(vmaxq_f32(t1y, t2y),
                                                         vmaxq_f32(t1z, t2z)));

            const uint32x4_t hit1 = vcleq_f32(tMin, tMax);
            const uint32x4_t hit2 = vcgeq_f32(tMax, zero_v);
            const uint32x4_t hit3 = vcltq_f32(tMin, bestT_v);
            const uint32x4_t mask = vandq_u32(vandq_u32(hit1, hit2), hit3);

            // movemask-equivalent: each lane is 0 or 0xFFFFFFFF; AND with
            // bit-position weights, horizontal-add to pack into 4 bits.
            // vaddvq_u32 is ARMv8-A; aarch64 always has it.
            alignas(16) static const uint32_t kWeights[4] = {1u, 2u, 4u, 8u};
            const uint32x4_t bits = vandq_u32(mask, vld1q_u32(kWeights));
            const uint32_t   bm   = vaddvq_u32(bits);

            vst1q_f32(outTmin, tMin);
            return bm;
        };

        const uint32_t loBits = slab4(minX_lo, maxX_lo, minY_lo, maxY_lo,
                                      minZ_lo, maxZ_lo, tMinArr + 0);
        const uint32_t hiBits = slab4(minX_hi, maxX_hi, minY_hi, maxY_hi,
                                      minZ_hi, maxZ_hi, tMinArr + 4);
        hitBits = static_cast<int>((loBits | (hiBits << 4)) &
                                   static_cast<uint32_t>(node.ValidMask));
#else
        // Scalar fallback (any non-x86, non-aarch64 target): same per-
        // slot decode + slab test as the AVX2 path, just unrolled across
        // the 8 children.
        const float stepX = node.ParentMax.x - node.ParentMin.x;
        const float stepY = node.ParentMax.y - node.ParentMin.y;
        const float stepZ = node.ParentMax.z - node.ParentMin.z;
        const float pMinX = node.ParentMin.x;
        const float pMinY = node.ParentMin.y;
        const float pMinZ = node.ParentMin.z;
        constexpr float q255inv = 1.0f / 255.0f;

        for (int s = 0; s < 8; ++s)
        {
            tMinArr[s] = std::numeric_limits<float32>::max();
            if (!(node.ValidMask & (1u << s)))
                continue;

            const float minX = static_cast<float>(node.ChildMinX[s]) * q255inv * stepX + pMinX;
            const float minY = static_cast<float>(node.ChildMinY[s]) * q255inv * stepY + pMinY;
            const float minZ = static_cast<float>(node.ChildMinZ[s]) * q255inv * stepZ + pMinZ;
            const float maxX = static_cast<float>(node.ChildMaxX[s]) * q255inv * stepX + pMinX;
            const float maxY = static_cast<float>(node.ChildMaxY[s]) * q255inv * stepY + pMinY;
            const float maxZ = static_cast<float>(node.ChildMaxZ[s]) * q255inv * stepZ + pMinZ;

            const float t1x = (minX - origin.x) * invDirX_s;
            const float t2x = (maxX - origin.x) * invDirX_s;
            const float t1y = (minY - origin.y) * invDirY_s;
            const float t2y = (maxY - origin.y) * invDirY_s;
            const float t1z = (minZ - origin.z) * invDirZ_s;
            const float t2z = (maxZ - origin.z) * invDirZ_s;

            const float tnx = std::min(t1x, t2x);
            const float txx = std::max(t1x, t2x);
            const float tny = std::min(t1y, t2y);
            const float txy = std::max(t1y, t2y);
            const float tnz = std::min(t1z, t2z);
            const float txz = std::max(t1z, t2z);

            const float tn = std::max(tnx, std::max(tny, tnz));
            const float tx = std::min(txx, std::min(txy, txz));

            tMinArr[s] = tn;
            if (tn <= tx && tx >= 0.0f && tn < bestT)
                hitBits |= (1 << s);
        }
#endif
        if (hitBits == 0) continue;

        // Collect hit slot indices, sorted ascending by tMin (near-first).
        // Up to 8 entries; tiny insertion sort is faster than a SIMD sort
        // on this size.
        struct HitEntry { float32 tEnter; uint8 slot; };
        HitEntry hits[8];
        int hitCount = 0;
        for (int i = 0; i < 8; ++i)
        {
            if (hitBits & (1 << i))
            {
                HitEntry e{tMinArr[i], static_cast<uint8>(i)};
                int j = hitCount;
                while (j > 0 && hits[j - 1].tEnter > e.tEnter)
                {
                    hits[j] = hits[j - 1];
                    --j;
                }
                hits[j] = e;
                ++hitCount;
            }
        }

        // Phase 1: process all leaf hits in near-first order so bestT
        // tightens before we push internal far-children. The SIMD AABB
        // test already filtered against bestT, but tightening here gives
        // the next pop's test more pruning power.
        for (int i = 0; i < hitCount; ++i)
        {
            const uint8 slot     = hits[i].slot;
            const uint16 triCnt  = node.ChildTriCount[slot];
            if (triCnt == 0u) continue;  // internal — handled in phase 2

            const uint32 firstTri = node.ChildIdx[slot];
            for (uint32 t = 0; t < triCnt; ++t)
            {
                const Triangle& tri = m_Triangles[m_TriIndices[firstTri + t]];
                float32 tHit = 0.0f, u = 0.0f, v = 0.0f;
                if (RayTriangle(origin, direction, tri.V0, tri.V1, tri.V2, tHit, u, v)
                    && tHit < bestT)
                {
                    bestT   = tHit;
                    bestU   = u;
                    bestV   = v;
                    bestTri = tri.OriginalIndex;
                    hit     = true;
                }
            }
        }

        // Phase 2: push internal hit children. Reverse-near-first order so
        // the nearest sits on top of the LIFO stack and gets popped next.
        // Re-check tEnter < bestT in case leaf hits in phase 1 tightened
        // it past a queued internal node.
        for (int i = hitCount - 1; i >= 0; --i)
        {
            const uint8 slot = hits[i].slot;
            if (node.ChildTriCount[slot] != 0u) continue;  // leaf — phase 1
            if (hits[i].tEnter >= bestT) continue;
            // Stack overflow at this depth means a pathologically
            // unbalanced tree — silently dropping the child would cause
            // a missed-hit with no diagnostic. Assert in debug; in
            // release, we still drop the child (kStackSize is generous
            // enough that this should never fire on real meshes).
            assert(sp < static_cast<int>(stack.size())
                   && "MeshBvh: traversal stack exceeded kStackSize. "
                      "Tree is pathologically deep — increase kStackSize.");
            if (sp < static_cast<int>(stack.size()))
                stack[sp++] = node.ChildIdx[slot];
        }
    }

    if (hit)
    {
        outHit.Distance      = bestT;
        outHit.TriangleIndex = bestTri;
        outHit.Barycentrics  = Vector3(1.0f - bestU - bestV, bestU, bestV);
    }
    return hit;
}

namespace
{

// Append `n` bytes of `src` to `dst`. Used to build the serialised buffer
// without locking ourselves to a specific endianness (we don't claim
// cross-platform stability — same layout as live struct on x86_64 Windows).
void AppendBytes(std::vector<uint8>& dst, const void* src, size_t n)
{
    const auto* p = static_cast<const uint8*>(src);
    dst.insert(dst.end(), p, p + n);
}

// Read `n` bytes starting at `offset`. Advances `offset`. Returns nullptr on
// out-of-range, signalling a malformed buffer to the caller.
const uint8* ReadBytes(const std::vector<uint8>& src, size_t& offset, size_t n)
{
    if (offset + n > src.size())
        return nullptr;
    const uint8* p = src.data() + offset;
    offset += n;
    return p;
}

// Layout of the serialised payload after magic/version/counts.
struct SerializedHeader
{
    uint32 Magic;
    uint32 Version;
    uint32 TriangleCount;
    uint32 TriIndexCount;
    uint32 NodeCount;
    Vector3 RootMin;
    Vector3 RootMax;
};

// On-disk layout depends on these struct sizes. If padding shifts (e.g. a
// new field added to SerializedHeader) every cached file is silently
// corrupt — the assert forces a build break instead. The BvhNode8 size
// invariant is asserted inside MeshBvh::Serialize where the private type
// is in scope.
static_assert(sizeof(SerializedHeader) == 44,
              "SerializedHeader layout changed; bump kSerializedVersion to invalidate caches");
static_assert(sizeof(uint32) == 4,
              "Per-triangle OriginalIndex is u32 on disk");

}

void MeshBvh::Serialize(std::vector<uint8>& outBytes) const
{
    // BvhNode8 is bit-for-bit copied to disk. Layout drift = silent cache
    // corruption; this assert forces a build break.
    static_assert(sizeof(BvhNode8) == 128,
                  "MeshBvh::BvhNode8 layout changed; bump kSerializedVersion in MeshBvh.h");

    outBytes.clear();
    outBytes.reserve(sizeof(SerializedHeader)
                     + m_Triangles.size() * sizeof(uint32)
                     + m_TriIndices.size() * sizeof(uint32)
                     + m_Nodes8.size() * sizeof(BvhNode8));

    SerializedHeader hdr{};
    hdr.Magic         = kSerializedMagic;
    hdr.Version       = kSerializedVersion;
    hdr.TriangleCount = static_cast<uint32>(m_Triangles.size());
    hdr.TriIndexCount = static_cast<uint32>(m_TriIndices.size());
    hdr.NodeCount     = static_cast<uint32>(m_Nodes8.size());
    hdr.RootMin       = m_RootBounds.min;
    hdr.RootMax       = m_RootBounds.max;
    AppendBytes(outBytes, &hdr, sizeof(hdr));

    // Per-triangle source-triangle index (Triangle::OriginalIndex). Vertex
    // positions are reconstructed on load from the source MeshView.
    for (const auto& tri : m_Triangles)
        AppendBytes(outBytes, &tri.OriginalIndex, sizeof(uint32));

    // Tri-indices array (BVH internal triangle ordering after build).
    if (!m_TriIndices.empty())
        AppendBytes(outBytes, m_TriIndices.data(), m_TriIndices.size() * sizeof(uint32));

    // Node array: bit-for-bit copy of m_Nodes8 (POD CWBVH8, x86_64 layout).
    if (!m_Nodes8.empty())
        AppendBytes(outBytes, m_Nodes8.data(), m_Nodes8.size() * sizeof(BvhNode8));
}

bool MeshBvh::Deserialize(const std::vector<uint8>& bytes, const MeshView& source)
{
    // Mirror Serialize's invariant — if BvhNode8 layout drifts the bytes
    // we memcpy below would silently mis-align.
    static_assert(sizeof(BvhNode8) == 128,
                  "MeshBvh::BvhNode8 layout changed; bump kSerializedVersion in MeshBvh.h");

    m_Triangles.clear();
    m_TriIndices.clear();
    m_Nodes8.clear();
    m_RootBounds = {};

    if (bytes.size() < sizeof(SerializedHeader))
        return false;

    size_t offset = 0;
    const uint8* hdrBytes = ReadBytes(bytes, offset, sizeof(SerializedHeader));
    if (!hdrBytes)
        return false;
    SerializedHeader hdr{};
    std::memcpy(&hdr, hdrBytes, sizeof(SerializedHeader));

    if (hdr.Magic != kSerializedMagic || hdr.Version != kSerializedVersion)
        return false;

    if (!source.IsValid() && (hdr.TriangleCount > 0 || hdr.NodeCount > 0))
        return false;

    // Triangles: read OriginalIndex per triangle, rebuild V0/V1/V2 from source.
    m_Triangles.reserve(hdr.TriangleCount);
    for (uint32 i = 0; i < hdr.TriangleCount; ++i)
    {
        const uint8* p = ReadBytes(bytes, offset, sizeof(uint32));
        if (!p)
            return false;
        uint32 origIdx = 0;
        std::memcpy(&origIdx, p, sizeof(uint32));

        const uint32 base = origIdx * 3u;
        if (base + 2u >= source.IndexCount)
            return false;
        const uint32 i0 = source.Indices[base + 0];
        const uint32 i1 = source.Indices[base + 1];
        const uint32 i2 = source.Indices[base + 2];
        if (i0 >= source.VertexCount || i1 >= source.VertexCount || i2 >= source.VertexCount)
            return false;

        Triangle tri;
        tri.V0            = ReadPosition(source, i0);
        tri.V1            = ReadPosition(source, i1);
        tri.V2            = ReadPosition(source, i2);
        tri.OriginalIndex = origIdx;
        m_Triangles.push_back(tri);
    }

    // Tri-indices.
    m_TriIndices.resize(hdr.TriIndexCount);
    if (hdr.TriIndexCount > 0)
    {
        const uint8* p = ReadBytes(bytes, offset, hdr.TriIndexCount * sizeof(uint32));
        if (!p)
            return false;
        std::memcpy(m_TriIndices.data(), p, hdr.TriIndexCount * sizeof(uint32));
        // Validate: each tri-index must be < TriangleCount.
        for (uint32 v : m_TriIndices)
        {
            if (v >= hdr.TriangleCount)
                return false;
        }
    }

    // BVH8 nodes: bit-for-bit memcpy then validate child references.
    m_Nodes8.resize(hdr.NodeCount);
    if (hdr.NodeCount > 0)
    {
        const uint8* p = ReadBytes(bytes, offset, hdr.NodeCount * sizeof(BvhNode8));
        if (!p)
            return false;
        std::memcpy(m_Nodes8.data(), p, hdr.NodeCount * sizeof(BvhNode8));
        // Validate every populated slot's child reference. Leaves point
        // into m_TriIndices; internals point at another BvhNode8. Empty
        // slots (ValidMask bit clear) are not validated.
        //
        // Forward-only invariant: CollapseFromBinary builds bottom-up by
        // emplacing the parent BVH8 node FIRST, then recursing for each
        // internal child slot — child indices are always strictly greater
        // than the parent's index. A corrupted file pointing back to an
        // earlier node would create a traversal cycle and infinite-loop
        // (or hit the kStackSize debug-assert in Raycast). Reject here.
        for (uint32 nodeIdx = 0; nodeIdx < hdr.NodeCount; ++nodeIdx)
        {
            const auto& node = m_Nodes8[nodeIdx];
            for (int i = 0; i < 8; ++i)
            {
                if ((node.ValidMask & (1u << i)) == 0u) continue;
                if (node.ChildTriCount[i] > 0u)
                {
                    const uint64 firstTri = node.ChildIdx[i];
                    if (firstTri + node.ChildTriCount[i] > hdr.TriIndexCount)
                        return false;
                }
                else
                {
                    // Internal child must point forward in the array.
                    if (node.ChildIdx[i] <= nodeIdx ||
                        node.ChildIdx[i] >= hdr.NodeCount)
                        return false;
                }
            }
        }
    }

    m_RootBounds.min = hdr.RootMin;
    m_RootBounds.max = hdr.RootMax;

    // Defense-in-depth: recompute the root AABB from the rebuilt triangles
    // (their vertex positions came from the *current* source mesh) and
    // reject if it diverges from the stamped header bounds. The upstream
    // fingerprint + ContentSignature checks should have caught a stale
    // source already; this is a final tripwire for the case where a sample-
    // hash collision lets a mutated mesh past the gate.
    if (!m_Triangles.empty())
    {
        Vector3 mn = InfMin();
        Vector3 mx = InfMax();
        for (const auto& tri : m_Triangles)
        {
            ExpandBounds(mn, mx, tri.V0);
            ExpandBounds(mn, mx, tri.V1);
            ExpandBounds(mn, mx, tri.V2);
        }
        const float32 eps = 1e-3f;
        auto diff = [eps](float32 a, float32 b) { return std::abs(a - b) > eps; };
        if (diff(mn.x, hdr.RootMin.x) || diff(mn.y, hdr.RootMin.y) || diff(mn.z, hdr.RootMin.z) ||
            diff(mx.x, hdr.RootMax.x) || diff(mx.y, hdr.RootMax.y) || diff(mx.z, hdr.RootMax.z))
        {
            m_Triangles.clear();
            m_TriIndices.clear();
            m_Nodes8.clear();
            m_RootBounds = {};
            return false;
        }
    }
    return true;
}

}
