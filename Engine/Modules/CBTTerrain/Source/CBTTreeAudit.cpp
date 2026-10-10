#include "CBTTerrain/CBTTreeAudit.h"

#include "CBTTerrain/CBTDeepDecode.h"
#include "CBTTerrain/CBTInstance.h"
#include "CBTTerrain/CBTSphereRoots.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <type_traits>
#include <unordered_set>

namespace GameEngine::CBTTerrain
{

namespace
{
constexpr uint32_t kHeapWordsPerSlot = 2u;
constexpr uint32_t kNeighborWordsPerSlot = sizeof(CBTNeighbors) / 4u;
constexpr uint32_t kBisectorWordsPerSlot = sizeof(CBTBisectorData) / 4u;
constexpr uint32_t kVertexWordsPerSlot = sizeof(CBTVertexData) / 4u;
constexpr size_t kSampleBadLimit = 16u;
// WalkBary is exact to this many subdivisions (Scale < 2^63); a deeper leaf is corrupt on every
// arm (the GPU decode caps lower still) and is counted malformed rather than decoded.
constexpr uint32_t kAuditMaxSubdiv = 52u;
// A cached corner UV within this of the decode is the same dyadic point: the finest live facet
// edge at kMaxDecodeSubdiv is 2^-20 in UV, four decades above this.
constexpr float kCornerUvTolerance = 2.5e-7f;

uint32_t HeapDepth(uint64_t h)
{
    uint32_t d = 0;
    while (h > 1u)
    {
        h >>= 1;
        ++d;
    }
    return d;
}

using Point = std::array<int64_t, 3>;

// An undirected edge between two exact lattice points; endpoints ordered so the key is canonical.
struct EdgeKey
{
    Point A;
    Point B;
    bool operator<(const EdgeKey& o) const { return A != o.A ? A < o.A : B < o.B; }
    bool operator==(const EdgeKey& o) const { return A == o.A && B == o.B; }
};

EdgeKey MakeEdge(const Point& a, const Point& b)
{
    return (b < a) ? EdgeKey{b, a} : EdgeKey{a, b};
}

// The two unit-square roots as integer corners (the table CBT_PlanarBaryToUV in cbt_domain.glsl
// folds into its scalar sums).
void PlanarRootCorners(uint32_t rootIndex, std::array<int64_t, 2>& c0, std::array<int64_t, 2>& c1,
                       std::array<int64_t, 2>& c2)
{
    if ((rootIndex & 1u) == 0u)
    {
        c0 = {1, 0};
        c1 = {0, 0};
        c2 = {0, 1};
    }
    else
    {
        c0 = {0, 1};
        c1 = {1, 1};
        c2 = {1, 0};
    }
}

struct DecodedLeaf
{
    uint32_t Depth = 0;
    uint32_t NumSubdiv = 0;
    uint32_t RootIndex = 0;
    DeepDecode::Bary Weights{};
};

bool DecodeLeaf(uint64_t h, uint32_t baseDepth, uint32_t rootCount, DecodedLeaf& out)
{
    out.Depth = HeapDepth(h);
    if (out.Depth < baseDepth)
        return false;
    out.NumSubdiv = out.Depth - baseDepth;
    if (out.NumSubdiv > kAuditMaxSubdiv)
        return false;
    out.RootIndex = static_cast<uint32_t>(h >> out.NumSubdiv) - (1u << baseDepth);
    if (out.RootIndex >= rootCount)
        return false;
    out.Weights = DeepDecode::WalkBary(h, baseDepth);
    return true;
}

const int64_t* CornerWeights(const DeepDecode::Bary& bary, uint32_t corner)
{
    return corner == 0u ? bary.B0 : corner == 1u ? bary.B1 : bary.B2;
}

// One corner as an exact lattice point at the common scale 2^commonSubdiv (planar: unit-square
// UV numerators, z = 0; spherical: cube numerators, the integer both faces of a cube edge share).
Point CornerPoint(const DecodedLeaf& leaf, uint32_t corner, uint32_t domainMode,
                  uint32_t commonSubdiv, const std::array<CBTSphereRoot, kSphereRootCount>& roots)
{
    const int64_t* b = CornerWeights(leaf.Weights, corner);
    const uint32_t shift = commonSubdiv - leaf.NumSubdiv;
    Point p{0, 0, 0};
    if (domainMode == kDomainSpherical)
    {
        const CBTSphereRoot& root = roots[leaf.RootIndex];
        for (int c = 0; c < 3; ++c)
            p[static_cast<size_t>(c)] =
                (b[0] * root.V0[static_cast<size_t>(c)] + b[1] * root.V1[static_cast<size_t>(c)] +
                 b[2] * root.V2[static_cast<size_t>(c)])
                << shift;
        return p;
    }
    std::array<int64_t, 2> rc0, rc1, rc2;
    PlanarRootCorners(leaf.RootIndex, rc0, rc1, rc2);
    p[0] = (b[0] * rc0[0] + b[1] * rc1[0] + b[2] * rc2[0]) << shift;
    p[1] = (b[0] * rc0[1] + b[1] * rc1[1] + b[2] * rc2[1]) << shift;
    return p;
}

// A planar edge on the terrain boundary has both endpoints on ONE side of the unit square; it is
// legitimately owned by a single leaf. The sphere is closed: no edge is.
bool IsBoundaryEdge(const EdgeKey& e, uint32_t domainMode, int64_t extent)
{
    if (domainMode == kDomainSpherical)
        return false;
    return (e.A[0] == 0 && e.B[0] == 0) || (e.A[0] == extent && e.B[0] == extent) ||
           (e.A[1] == 0 && e.B[1] == 0) || (e.A[1] == extent && e.B[1] == extent);
}

void SampleBad(CBTTreeAuditResult& r, uint64_t heapId)
{
    if (r.SampleBadHeapIds.size() < kSampleBadLimit)
        r.SampleBadHeapIds.push_back(heapId);
}

// Kernel_VertexEval's planar corner UV for one corner: the exact numerator cast once to float,
// scaled by the exact power-of-two reciprocal.
std::array<float, 2> PlanarCornerUv(const DecodedLeaf& leaf, uint32_t corner)
{
    const int64_t* b = CornerWeights(leaf.Weights, corner);
    std::array<int64_t, 2> rc0, rc1, rc2;
    PlanarRootCorners(leaf.RootIndex, rc0, rc1, rc2);
    const int64_t nu = b[0] * rc0[0] + b[1] * rc1[0] + b[2] * rc2[0];
    const int64_t nv = b[0] * rc0[1] + b[1] * rc1[1] + b[2] * rc2[1];
    const float invScale = 1.0f / static_cast<float>(leaf.Weights.Scale);
    return {static_cast<float>(nu) * invScale, static_cast<float>(nv) * invScale};
}

bool IsLive(const CBTTreeSnapshot& s, uint32_t slot)
{
    return slot < s.PoolSize && s.HeapIds[slot] != kFreeSlotHeapID;
}

bool RecordPointsAt(const CBTNeighbors& n, uint32_t slot)
{
    return n.Neighbor0 == slot || n.Neighbor1 == slot || n.Twin == slot;
}

uint32_t NeighborAt(const CBTNeighbors& n, uint32_t i)
{
    return i == 0u ? n.Neighbor0 : i == 1u ? n.Neighbor1 : n.Twin;
}

// Kernel_Validate's reciprocity rule on one record: any live neighbour must point back, and a
// same-depth twin must point back through ITS twin link (equal-depth twins share their split
// edge). Planar-boundary links (INVALID) and free targets are unverifiable by design.
bool LinksReciprocal(const CBTTreeSnapshot& s, uint32_t slot, uint32_t myDepth)
{
    const CBTNeighbors& n = s.Neighbors[slot];
    for (uint32_t i = 0; i < 3u; ++i)
    {
        const uint32_t nb = NeighborAt(n, i);
        if (nb == kInvalidPointer || !IsLive(s, nb))
            continue;
        const CBTNeighbors& nn = s.Neighbors[nb];
        if (!RecordPointsAt(nn, slot))
            return false;
        if (i == 2u && HeapDepth(s.HeapIds[nb]) == myDepth && nn.Twin != slot)
            return false;
    }
    return true;
}

// IndicesX[0..count) must be exactly the live slots satisfying `member`, each once.
uint32_t AuditStream(const CBTTreeSnapshot& s, const std::vector<uint32_t>& stream, uint32_t count,
                     uint32_t expectedCount, std::vector<uint8_t>& visited,
                     bool (*member)(const CBTTreeSnapshot&, uint32_t))
{
    uint32_t errors = 0;
    std::fill(visited.begin(), visited.end(), uint8_t{0});
    if (count != expectedCount || stream.size() < count)
        ++errors;
    const uint32_t n = std::min<uint32_t>(count, static_cast<uint32_t>(stream.size()));
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint32_t slot = stream[i];
        if (!member(s, slot) || visited[slot] != 0u)
        {
            ++errors;
            continue;
        }
        visited[slot] = 1u;
    }
    return errors;
}

bool LiveMember(const CBTTreeSnapshot& s, uint32_t slot)
{
    return IsLive(s, slot);
}

bool VisibleMember(const CBTTreeSnapshot& s, uint32_t slot)
{
    return IsLive(s, slot) && (s.Bisectors[slot].Flags & kFlagVisible) != 0u;
}

template <typename T>
void UnpackRecords(const std::vector<uint32_t>& words, uint32_t wordsPerSlot, uint32_t poolSize,
                   std::vector<T>& out)
{
    static_assert(std::is_trivially_copyable_v<T>);
    out.resize(poolSize);
    const size_t available = words.size() / wordsPerSlot;
    const size_t n = std::min<size_t>(available, poolSize);
    for (size_t i = 0; i < n; ++i)
        std::memcpy(&out[i], &words[i * wordsPerSlot], sizeof(T));
}
} // namespace

CBTTreeSnapshot ReadTreeSnapshot(CBTInstance& instance, bool withCorners)
{
    CBTTreeSnapshot s;
    s.PoolSize = instance.GetPoolSize();
    s.BaseDepth = instance.GetBaseDepth();
    s.RootCount = instance.GetRootCount();
    s.DomainMode = instance.GetDomainMode();
    if (s.PoolSize == 0u)
        return s;

    const std::vector<uint32_t> heap =
        instance.DebugReadWords(CBTBinding::HeapID, s.PoolSize * kHeapWordsPerSlot);
    s.HeapIds.assign(s.PoolSize, kFreeSlotHeapID);
    for (uint32_t i = 0; i < s.PoolSize && i * 2u + 1u < heap.size(); ++i)
        s.HeapIds[i] = static_cast<uint64_t>(heap[i * 2u]) |
                       (static_cast<uint64_t>(heap[i * 2u + 1u]) << 32);

    // RecordUpdate flips the parity at its tail, so the buffer it now reads as CURRENT is the
    // settled NEXT of the last update — the record every consumer dereferences next frame.
    const CBTBinding current =
        instance.GetNeighborsReadIsA() ? CBTBinding::NeighborsA : CBTBinding::NeighborsB;
    UnpackRecords(instance.DebugReadWords(current, s.PoolSize * kNeighborWordsPerSlot),
                  kNeighborWordsPerSlot, s.PoolSize, s.Neighbors);
    UnpackRecords(instance.DebugReadWords(CBTBinding::BisectorData, s.PoolSize * kBisectorWordsPerSlot),
                  kBisectorWordsPerSlot, s.PoolSize, s.Bisectors);
    s.Bitfield = instance.DebugReadWords(CBTBinding::Bitfield, CBTBitfieldWords(s.PoolSize));
    s.Bitfield.resize(CBTBitfieldWords(s.PoolSize), 0u);
    if (withCorners)
        UnpackRecords(instance.DebugReadWords(CBTBinding::CurrentVertex, s.PoolSize * kVertexWordsPerSlot),
                      kVertexWordsPerSlot, s.PoolSize, s.Vertices);

    const std::vector<uint32_t> draw = instance.DebugReadWords(CBTBinding::IndirectDraw, kIndirectDrawWords);
    auto streamCount = [&](uint32_t stream) -> uint32_t {
        const uint32_t w = DrawStreamWordOffset(stream) + kDrawIndexCountField;
        return w < draw.size() ? std::min(draw[w] / 3u, s.PoolSize) : 0u;
    };
    s.AllCount = streamCount(kDrawStreamAll);
    s.VisibleCount = streamCount(kDrawStreamVisible);
    s.ModifiedCount = streamCount(kDrawStreamModified);
    s.IndicesAll = instance.DebugReadWords(CBTBinding::IndicesAll, s.AllCount);
    s.IndicesVisible = instance.DebugReadWords(CBTBinding::IndicesVisible, s.VisibleCount);
    return s;
}

CBTTreeAuditResult AuditTree(const CBTTreeSnapshot& s)
{
    CBTTreeAuditResult r;
    if (s.PoolSize == 0u || s.HeapIds.size() < s.PoolSize || s.Neighbors.size() < s.PoolSize ||
        s.Bisectors.size() < s.PoolSize)
        return r;
    const std::array<CBTSphereRoot, kSphereRootCount> sphereRoots = BuildSphereRoots();

    // Pass 1: decode every live leaf; the common scale is the deepest live leaf's.
    std::vector<uint32_t> liveSlots;
    std::vector<DecodedLeaf> leaves;
    liveSlots.reserve(s.AllCount);
    leaves.reserve(s.AllCount);
    std::unordered_set<uint64_t> liveIds;
    liveIds.reserve(s.AllCount * 2u);
    uint32_t commonSubdiv = 0;
    uint32_t visibleLive = 0;
    for (uint32_t slot = 0; slot < s.PoolSize; ++slot)
    {
        const uint64_t h = s.HeapIds[slot];
        const bool live = h != kFreeSlotHeapID;
        const bool bit = ((s.Bitfield[slot / 32u] >> (slot % 32u)) & 1u) != 0u;
        if (bit != live)
            ++r.Zombies;
        if (!live)
            continue;
        ++r.LiveCount;
        if ((s.Bisectors[slot].Flags & kFlagVisible) != 0u)
            ++visibleLive;
        DecodedLeaf leaf;
        if (!DecodeLeaf(h, s.BaseDepth, s.RootCount, leaf))
        {
            ++r.MalformedLeaves;
            SampleBad(r, h);
            continue;
        }
        r.MaxDepth = std::max(r.MaxDepth, leaf.Depth);
        commonSubdiv = std::max(commonSubdiv, leaf.NumSubdiv);
        liveSlots.push_back(slot);
        leaves.push_back(leaf);
        liveIds.insert(h);
    }

    // Pass 2: the topology — overlaps, per-root coverage, and the edge multiset.
    std::vector<uint64_t> rootCoverage(s.RootCount, 0u);
    const uint64_t rootFull = uint64_t(1) << commonSubdiv;
    std::vector<EdgeKey> edges;
    edges.reserve(leaves.size() * 3u);
    const uint32_t rootBand = 1u << s.BaseDepth;
    for (size_t i = 0; i < leaves.size(); ++i)
    {
        const DecodedLeaf& leaf = leaves[i];
        const uint64_t h = s.HeapIds[liveSlots[i]];
        bool overlapping = false;
        for (uint64_t a = h >> 1; a >= rootBand; a >>= 1)
        {
            if (liveIds.count(a) != 0u)
            {
                overlapping = true;
                break;
            }
        }
        if (overlapping)
        {
            ++r.OverlappingLeaves;
            SampleBad(r, h);
        }
        uint64_t& cover = rootCoverage[leaf.RootIndex];
        if (cover <= rootFull)
            cover += uint64_t(1) << (commonSubdiv - leaf.NumSubdiv);

        const Point p0 = CornerPoint(leaf, 0u, s.DomainMode, commonSubdiv, sphereRoots);
        const Point p1 = CornerPoint(leaf, 1u, s.DomainMode, commonSubdiv, sphereRoots);
        const Point p2 = CornerPoint(leaf, 2u, s.DomainMode, commonSubdiv, sphereRoots);
        edges.push_back(MakeEdge(p0, p1));
        edges.push_back(MakeEdge(p1, p2));
        edges.push_back(MakeEdge(p2, p0));
    }
    for (uint64_t cover : rootCoverage)
        if (cover != rootFull)
            ++r.IncompleteRoots;

    std::sort(edges.begin(), edges.end());
    const int64_t extent = static_cast<int64_t>(rootFull);
    for (size_t i = 0; i < edges.size();)
    {
        size_t j = i + 1;
        while (j < edges.size() && edges[j] == edges[i])
            ++j;
        const size_t count = j - i;
        const size_t expected = IsBoundaryEdge(edges[i], s.DomainMode, extent) ? 1u : 2u;
        if (count != expected)
            ++r.NonConformingEdges;
        i = j;
    }

    // Pass 3: the caches against that topology.
    for (size_t i = 0; i < leaves.size(); ++i)
    {
        const uint32_t slot = liveSlots[i];
        const DecodedLeaf& leaf = leaves[i];
        if (!LinksReciprocal(s, slot, leaf.Depth))
        {
            ++r.NonReciprocalLinks;
            SampleBad(r, s.HeapIds[slot]);
        }
        if (s.DomainMode == kDomainPlanar && s.Vertices.size() >= s.PoolSize)
        {
            const CBTVertexData& v = s.Vertices[slot];
            const float* cornerW[3] = {v.Corner0, v.Corner1, v.Corner2};
            bool stale = false;
            for (uint32_t k = 0; k < 3u && !stale; ++k)
            {
                const std::array<float, 2> uv = PlanarCornerUv(leaf, k);
                stale = std::fabs(cornerW[k][3] - uv[0]) > kCornerUvTolerance ||
                        std::fabs(v.Meta[k] - uv[1]) > kCornerUvTolerance;
            }
            if (stale)
            {
                ++r.StaleCornerUVs;
                SampleBad(r, s.HeapIds[slot]);
            }
        }
    }

    std::vector<uint8_t> visited(s.PoolSize, 0u);
    r.StreamAllErrors = AuditStream(s, s.IndicesAll, s.AllCount, r.LiveCount, visited, LiveMember);
    r.StreamVisibleErrors =
        AuditStream(s, s.IndicesVisible, s.VisibleCount, visibleLive, visited, VisibleMember);
    return r;
}

uint64_t TreeSignature(const CBTTreeSnapshot& s)
{
    std::vector<uint64_t> ids;
    ids.reserve(s.AllCount);
    for (uint64_t h : s.HeapIds)
        if (h != kFreeSlotHeapID)
            ids.push_back(h);
    std::sort(ids.begin(), ids.end());
    uint64_t hash = 1469598103934665603ull; // FNV-1a
    for (uint64_t h : ids)
    {
        for (uint32_t byte = 0; byte < 8u; ++byte)
        {
            hash ^= (h >> (byte * 8u)) & 0xFFull;
            hash *= 1099511628211ull;
        }
    }
    hash ^= ids.size();
    return hash;
}

} // namespace GameEngine::CBTTerrain
