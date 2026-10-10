#include "TlasInternal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>

namespace GameEngine::Scene::Internal
{

namespace
{

// Slab test for ray-vs-AABB. Returns true on hit; tEnter/tExit in ray param.
inline bool RayAabb(const Mathematics::Ray3D& ray, const AABB& box,
                    float32 tMax, float32& tEnter, float32& tExit)
{
    float32 tmin = 0.0f;
    float32 tmax = tMax;

    for (int axis = 0; axis < 3; ++axis)
    {
        const float32 dir = axis == 0 ? ray.direction.x
                          : axis == 1 ? ray.direction.y : ray.direction.z;
        const float32 ori = axis == 0 ? ray.origin.x
                          : axis == 1 ? ray.origin.y : ray.origin.z;
        const float32 mn  = axis == 0 ? box.min.x : axis == 1 ? box.min.y : box.min.z;
        const float32 mx  = axis == 0 ? box.max.x : axis == 1 ? box.max.y : box.max.z;

        if (std::abs(dir) < 1e-7f)
        {
            if (ori < mn || ori > mx) return false;
        }
        else
        {
            float32 invD = 1.0f / dir;
            float32 t0 = (mn - ori) * invD;
            float32 t1 = (mx - ori) * invD;
            if (t0 > t1) std::swap(t0, t1);
            tmin = std::max(tmin, t0);
            tmax = std::min(tmax, t1);
            if (tmin > tmax) return false;
        }
    }
    tEnter = tmin;
    tExit  = tmax;
    return true;
}

inline bool SphereAabb(const Vector3& c, float32 r, const AABB& b)
{
    auto sq = [](float32 v) { return v * v; };
    float32 d2 = 0.0f;
    if (c.x < b.min.x) d2 += sq(c.x - b.min.x);
    else if (c.x > b.max.x) d2 += sq(c.x - b.max.x);
    if (c.y < b.min.y) d2 += sq(c.y - b.min.y);
    else if (c.y > b.max.y) d2 += sq(c.y - b.max.y);
    if (c.z < b.min.z) d2 += sq(c.z - b.min.z);
    else if (c.z > b.max.z) d2 += sq(c.z - b.max.z);
    return d2 <= sq(r);
}

inline bool FrustumAabb(std::span<const Mathematics::Plane> planes, const AABB& b)
{
    // Per-plane test: AABB rejected if entirely on the negative side of
    // any plane. Plane has inward-pointing normal; positive halfspace
    // means inside the frustum.
    for (const auto& p : planes)
    {
        // Find the corner of the AABB most aligned with -plane.normal
        // (the "positive vertex"); if even that corner is outside, reject.
        Vector3 pv{
            p.normal.x >= 0.0f ? b.max.x : b.min.x,
            p.normal.y >= 0.0f ? b.max.y : b.min.y,
            p.normal.z >= 0.0f ? b.max.z : b.min.z,
        };
        const float32 dot = p.normal.x * pv.x + p.normal.y * pv.y + p.normal.z * pv.z + p.d;
        if (dot < 0.0f) return false;
    }
    return true;
}

inline bool LeafPassesFilter(const TlasLeaf& leaf, uint32 layerMask,
                             std::span<const GameEngine::ECS::EntityHandle> ignore)
{
    if ((leaf.LayerMask & layerMask) == 0u) return false;
    for (const auto& e : ignore)
        if (e.id == leaf.Entity.id) return false;
    return true;
}

inline TlasInstance ToInstance(const TlasLeaf& leaf)
{
    TlasInstance inst;
    inst.Entity       = leaf.Entity;
    inst.WorldBounds  = leaf.WorldBounds;
    inst.LayerMask    = leaf.LayerMask;
    inst.InstanceMask = leaf.InstanceMask;
    return inst;
}

}  // anon namespace

uint32 TraverseRay(const SectorState& sector,
                   const Mathematics::Ray3D& ray,
                   const TraverseRayOptions& options,
                   FunctionRef<bool(const TlasInstance&,
                                    float32 tEnter,
                                    float32 tExit)> visit)
{
    if (sector.Nodes.empty()) return 0u;

    // Collect candidate (leaf-index, tEnter, tExit) tuples in entry-t
    // order, then dispatch callbacks AFTER releasing the read lock. The
    // caller's shared_lock owns the descent; we don't release-per-leaf
    // here because that would let a writer slip in mid-walk and mutate
    // the very tree we're reading.
    //
    // For tiny snapshots (ray hits 0-100 leaves typical) this is cheap.
    // BLAS dispatch outside the lock keeps writers responsive.

    struct Candidate {
        uint32  leafIdx;
        float32 tEnter;
        float32 tExit;
    };
    std::array<uint32, kMaxStackDepth> stack;
    int sp = 0;
    stack[sp++] = 0u;  // root

    // Reused scratch (function-local; small).
    std::vector<Candidate> hits;
    hits.reserve(64);

    uint32 visited = 0u;
    while (sp > 0)
    {
        const uint32 nodeIdx = stack[--sp];
        const auto& node = sector.Nodes[nodeIdx];

        float32 te, tx;
        if (!RayAabb(ray, node.Bounds, options.MaxDistance, te, tx))
            continue;

        if (node.LeafCount > 0u)
        {
            for (uint32 i = 0; i < node.LeafCount; ++i)
            {
                const uint32 leafIdx = node.FirstLeaf + i;
                const auto& leaf = sector.Leaves[leafIdx];
                if (!LeafPassesFilter(leaf, options.LayerMask, options.IgnoreEntities))
                    continue;
                float32 lte, ltx;
                if (!RayAabb(ray, leaf.WorldBounds, options.MaxDistance, lte, ltx))
                    continue;
                hits.push_back({leafIdx, lte, ltx});
                ++visited;
            }
        }
        else
        {
            if (sp + 2 <= static_cast<int>(stack.size()))
            {
                stack[sp++] = node.Right;
                stack[sp++] = node.Left;
            }
        }
    }

    std::sort(hits.begin(), hits.end(),
              [](const Candidate& a, const Candidate& b) { return a.tEnter < b.tEnter; });

    for (const auto& h : hits)
    {
        const TlasInstance inst = ToInstance(sector.Leaves[h.leafIdx]);
        if (!visit(inst, h.tEnter, h.tExit))
            break;
    }
    return visited;
}

uint32 TraverseSphere(const SectorState& sector,
                      const Mathematics::Vector3& center,
                      float32 radius,
                      const TraverseSphereOptions& options,
                      FunctionRef<void(const TlasInstance&)> visit)
{
    if (sector.Nodes.empty()) return 0u;

    std::array<uint32, kMaxStackDepth> stack;
    int sp = 0;
    stack[sp++] = 0u;

    std::vector<uint32> hits;
    hits.reserve(64);

    while (sp > 0)
    {
        const uint32 nodeIdx = stack[--sp];
        const auto& node = sector.Nodes[nodeIdx];

        if (!SphereAabb(center, radius, node.Bounds))
            continue;

        if (node.LeafCount > 0u)
        {
            for (uint32 i = 0; i < node.LeafCount; ++i)
            {
                const uint32 leafIdx = node.FirstLeaf + i;
                const auto& leaf = sector.Leaves[leafIdx];
                if ((leaf.LayerMask & options.LayerMask) == 0u) continue;
                if (!SphereAabb(center, radius, leaf.WorldBounds)) continue;
                hits.push_back(leafIdx);
            }
        }
        else
        {
            if (sp + 2 <= static_cast<int>(stack.size()))
            {
                stack[sp++] = node.Right;
                stack[sp++] = node.Left;
            }
        }
    }

    for (uint32 idx : hits)
        visit(ToInstance(sector.Leaves[idx]));
    return static_cast<uint32>(hits.size());
}

uint32 TraverseFrustum(const SectorState& sector,
                       std::span<const Mathematics::Plane> planes,
                       const TraverseFrustumOptions& options,
                       FunctionRef<void(const TlasInstance&)> visit)
{
    if (sector.Nodes.empty() || planes.empty()) return 0u;

    std::array<uint32, kMaxStackDepth> stack;
    int sp = 0;
    stack[sp++] = 0u;

    std::vector<uint32> hits;
    hits.reserve(64);

    while (sp > 0)
    {
        const uint32 nodeIdx = stack[--sp];
        const auto& node = sector.Nodes[nodeIdx];

        if (!FrustumAabb(planes, node.Bounds))
            continue;

        if (node.LeafCount > 0u)
        {
            for (uint32 i = 0; i < node.LeafCount; ++i)
            {
                const uint32 leafIdx = node.FirstLeaf + i;
                const auto& leaf = sector.Leaves[leafIdx];
                if ((leaf.LayerMask & options.LayerMask) == 0u) continue;
                if (!FrustumAabb(planes, leaf.WorldBounds)) continue;
                hits.push_back(leafIdx);
            }
        }
        else
        {
            if (sp + 2 <= static_cast<int>(stack.size()))
            {
                stack[sp++] = node.Right;
                stack[sp++] = node.Left;
            }
        }
    }

    for (uint32 idx : hits)
        visit(ToInstance(sector.Leaves[idx]));
    return static_cast<uint32>(hits.size());
}

// ---- Cross-sector traversal entry points ---------------------------------
// E.5.1 walked every populated sector linearly. E.5.3 replaces that with
// 3D-DDA grid-stepping for rays (Amanatides-Woo) and per-sector AABB
// pre-culling for sphere/frustum, so empty sectors and sectors outside
// the query volume cost a single hash miss / cheap reject test.
//
// The dedup set is stack-local so re-entrant traversals (a user callback
// firing a secondary ray) each get their own independent dedup state.

namespace
{

// 3D-DDA ray walk over a uniform sector grid. For each sector cell the
// ray actually enters (in t-order along the ray), invoke `visit` with
// the SectorKey and the [tEnter, tExit] range within that cell.
//
// Standard Amanatides-Woo with one defensive twist: an axis with very
// small |dir.axis| is encoded as tDelta = +inf, so the algorithm never
// advances on that axis — correct for axis-aligned rays. tMax is also
// clamped at +inf in that case to keep the per-axis step ordering safe.
template <class Visit>
void WalkRayThroughSectorGrid(const Mathematics::Ray3D& ray,
                              float32 sectorSize,
                              float32 maxDistance,
                              Visit&& visit)
{
    const float32 inf = std::numeric_limits<float32>::infinity();

    // Direction reciprocals; signed step direction per axis.
    auto ddaInit = [&](float32 origin, float32 dir, int32& step,
                       float32& tMax, float32& tDelta, int32& cell) {
        cell = static_cast<int32>(std::floor(origin / sectorSize));
        if (std::abs(dir) < 1e-30f)
        {
            step    = 0;
            tMax    = inf;
            tDelta  = inf;
            return;
        }
        if (dir > 0.0f)
        {
            step           = 1;
            const float32 next = static_cast<float32>(cell + 1) * sectorSize;
            tMax           = (next - origin) / dir;
            tDelta         = sectorSize / dir;
        }
        else
        {
            step           = -1;
            const float32 next = static_cast<float32>(cell) * sectorSize;
            tMax           = (next - origin) / dir;
            tDelta         = -sectorSize / dir;  // dir<0 so this is positive
        }
    };

    int32 cellX, cellY, cellZ;
    int32 stepX, stepY, stepZ;
    float32 tMaxX, tMaxY, tMaxZ;
    float32 tDeltaX, tDeltaY, tDeltaZ;
    ddaInit(ray.origin.x, ray.direction.x, stepX, tMaxX, tDeltaX, cellX);
    ddaInit(ray.origin.y, ray.direction.y, stepY, tMaxY, tDeltaY, cellY);
    ddaInit(ray.origin.z, ray.direction.z, stepZ, tMaxZ, tDeltaZ, cellZ);

    float32 tEnter = 0.0f;
    for (uint32 step = 0; step < kMaxRayDdaSteps; ++step)
    {
        // tExit = nearest grid plane crossing.
        const float32 tExit = std::min(tMaxX, std::min(tMaxY, tMaxZ));
        const float32 tCellExit = std::min(tExit, maxDistance);

        // Ray segment within this cell: [tEnter, tCellExit].
        if (tEnter > maxDistance)
            return;

        const SectorKey key{cellX, cellY, cellZ};
        const bool keepGoing = visit(key, tEnter, tCellExit);
        if (!keepGoing)
            return;

        if (tCellExit >= maxDistance)
            return;

        // Advance to next cell on the axis with smallest tMax.
        if (tMaxX <= tMaxY && tMaxX <= tMaxZ)
        {
            cellX += stepX;
            tEnter = tMaxX;
            tMaxX += tDeltaX;
            if (stepX == 0) return;  // axis-aligned and reached cell boundary; nothing more on this axis means done
        }
        else if (tMaxY <= tMaxZ)
        {
            cellY += stepY;
            tEnter = tMaxY;
            tMaxY += tDeltaY;
            if (stepY == 0) return;
        }
        else
        {
            cellZ += stepZ;
            tEnter = tMaxZ;
            tMaxZ += tDeltaZ;
            if (stepZ == 0) return;
        }
    }
}

}  // anon namespace

uint32 TraverseRayAcrossSectors(const SceneTlasImpl& impl,
                                const Mathematics::Ray3D& ray,
                                const TraverseRayOptions& options,
                                FunctionRef<bool(const TlasInstance&,
                                                 float32 tEnter,
                                                 float32 tExit)> visit)
{
    // Collect candidates from sectors the ray actually passes through.
    // 3D-DDA gives us cell-by-cell iteration in t-order; the per-sector
    // BVH then narrows further. We re-sort globally by tEnter before
    // dispatching callbacks because a single sector's BVH may emit hits
    // out of strict order across replicated leaves.
    struct Candidate { TlasInstance Inst; float32 TEnter; float32 TExit; };
    std::vector<Candidate> hits;
    hits.reserve(64);

    const float32 sectorSize = impl.SectorSize.load(std::memory_order_relaxed);

    auto collectFromSector = [&](const SectorState& sector)
    {
        TraverseRay(sector, ray, options,
            [&](const TlasInstance& inst, float32 te, float32 tx) -> bool {
                hits.push_back({inst, te, tx});
                return true;
            });
    };

    collectFromSector(impl.PrimarySector);

    if (!impl.Sectors.empty())
    {
        WalkRayThroughSectorGrid(ray, sectorSize, options.MaxDistance,
            [&](const SectorKey& key, float32 /*tEnter*/, float32 /*tExit*/) {
                if (key.X == 0 && key.Y == 0 && key.Z == 0)
                    return true;  // already collected from PrimarySector
                auto it = impl.Sectors.find(key);
                if (it == impl.Sectors.end())
                    return true;  // empty sector — skip with one hash miss
                collectFromSector(*it->second);
                return true;
            });
    }

    std::sort(hits.begin(), hits.end(),
              [](const Candidate& a, const Candidate& b) { return a.TEnter < b.TEnter; });

    std::unordered_set<GameEngine::ECS::EntityId> dedup;
    dedup.reserve(hits.size());
    uint32 visited = 0u;
    for (const auto& h : hits)
    {
        if (!dedup.insert(h.Inst.Entity.id).second)
            continue;  // replicated entity already reported
        ++visited;
        if (!visit(h.Inst, h.TEnter, h.TExit))
            break;
    }
    return visited;
}

namespace
{

// Sphere-vs-AABB overlap (squared-distance-to-box).
inline bool SphereAabbOverlap(const Mathematics::Vector3& center,
                              float32 radius, const AABB& b)
{
    auto sq = [](float32 v) { return v * v; };
    float32 d2 = 0.0f;
    if (center.x < b.min.x) d2 += sq(center.x - b.min.x);
    else if (center.x > b.max.x) d2 += sq(center.x - b.max.x);
    if (center.y < b.min.y) d2 += sq(center.y - b.min.y);
    else if (center.y > b.max.y) d2 += sq(center.y - b.max.y);
    if (center.z < b.min.z) d2 += sq(center.z - b.min.z);
    else if (center.z > b.max.z) d2 += sq(center.z - b.max.z);
    return d2 <= sq(radius);
}

// Frustum-vs-AABB cheap reject: AABB is outside if any plane reports it
// strictly outside (positive vertex test).
inline bool FrustumAabbOverlap(std::span<const Mathematics::Plane> planes, const AABB& b)
{
    for (const auto& p : planes)
    {
        Mathematics::Vector3 pv{
            p.normal.x >= 0.0f ? b.max.x : b.min.x,
            p.normal.y >= 0.0f ? b.max.y : b.min.y,
            p.normal.z >= 0.0f ? b.max.z : b.min.z,
        };
        const float32 dot = p.normal.x * pv.x + p.normal.y * pv.y + p.normal.z * pv.z + p.d;
        if (dot < 0.0f) return false;
    }
    return true;
}

}  // anon namespace

uint32 TraverseSphereAcrossSectors(const SceneTlasImpl& impl,
                                   const Mathematics::Vector3& center,
                                   float32 radius,
                                   const TraverseSphereOptions& options,
                                   FunctionRef<void(const TlasInstance&)> visit)
{
    std::unordered_set<GameEngine::ECS::EntityId> dedup;
    uint32 visited = 0u;

    auto dispatch = [&](const SectorState& sector)
    {
        TraverseSphere(sector, center, radius, options,
            [&](const TlasInstance& inst) {
                if (!dedup.insert(inst.Entity.id).second)
                    return;
                ++visited;
                visit(inst);
            });
    };

    dispatch(impl.PrimarySector);

    // Sector-level AABB pre-cull: skip sectors that don't overlap the
    // sphere with a single SphereAabb test before descending into the
    // sector's BVH.
    const float32 sectorSize = impl.SectorSize.load(std::memory_order_relaxed);
    for (const auto& kv : impl.Sectors)
    {
        const AABB volume = SectorVolumeForKey(kv.first, sectorSize);
        if (!SphereAabbOverlap(center, radius, volume))
            continue;
        dispatch(*kv.second);
    }

    return visited;
}

uint32 TraverseFrustumAcrossSectors(const SceneTlasImpl& impl,
                                    std::span<const Mathematics::Plane> planes,
                                    const TraverseFrustumOptions& options,
                                    FunctionRef<void(const TlasInstance&)> visit)
{
    std::unordered_set<GameEngine::ECS::EntityId> dedup;
    uint32 visited = 0u;

    auto dispatch = [&](const SectorState& sector)
    {
        TraverseFrustum(sector, planes, options,
            [&](const TlasInstance& inst) {
                if (!dedup.insert(inst.Entity.id).second)
                    return;
                ++visited;
                visit(inst);
            });
    };

    dispatch(impl.PrimarySector);

    const float32 sectorSize = impl.SectorSize.load(std::memory_order_relaxed);
    for (const auto& kv : impl.Sectors)
    {
        const AABB volume = SectorVolumeForKey(kv.first, sectorSize);
        if (!FrustumAabbOverlap(planes, volume))
            continue;
        dispatch(*kv.second);
    }

    return visited;
}

}  // namespace GameEngine::Scene::Internal
