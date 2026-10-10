#include "SceneBvh/TlasPacker.h"

#include "Logger/Logger.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace GameEngine::SceneBvh
{

namespace
{

using Mathematics::AABB;
using Mathematics::Vector3;

uint32 WidestAxis(const AABB& bounds)
{
    const float32 dx = bounds.max.x - bounds.min.x;
    const float32 dy = bounds.max.y - bounds.min.y;
    const float32 dz = bounds.max.z - bounds.min.z;
    if (dx >= dy && dx >= dz)
        return 0u;
    return dy >= dz ? 1u : 2u;
}

// World AABB of a local AABB under an affine transform: transform the centre
// and sum the absolute per-axis contributions of the half-extents.
AABB TransformBounds(const AABB& local, const glm::mat4& worldFromLocal)
{
    const Vector3 centre{(local.min.x + local.max.x) * 0.5f,
                         (local.min.y + local.max.y) * 0.5f,
                         (local.min.z + local.max.z) * 0.5f};
    const Vector3 extent{(local.max.x - local.min.x) * 0.5f,
                         (local.max.y - local.min.y) * 0.5f,
                         (local.max.z - local.min.z) * 0.5f};

    AABB result;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float32 worldCentre = worldFromLocal[0][axis] * centre.x +
                                    worldFromLocal[1][axis] * centre.y +
                                    worldFromLocal[2][axis] * centre.z +
                                    worldFromLocal[3][axis];
        const float32 worldExtent = std::abs(worldFromLocal[0][axis]) * extent.x +
                                    std::abs(worldFromLocal[1][axis]) * extent.y +
                                    std::abs(worldFromLocal[2][axis]) * extent.z;
        result.min[axis] = worldCentre - worldExtent;
        result.max[axis] = worldCentre + worldExtent;
    }
    return result;
}

// A TLAS node before serialization. Kept separate from the packed floats so the
// median partition can run without touching the output buffer's stride.
struct TlasNodeRecord
{
    AABB Bounds{};
    uint32 Miss = 0;
    uint32 InstanceOffset = 0;
    uint32 InstanceCount = 0;
};

} // namespace

PackedTlas TlasPacker::Pack(std::span<const UberMaterial> materials,
                            std::span<const TlasInstance> instances)
{
    PackedTlas packed;
    if (instances.empty())
        return packed;

    if (instances.size() >= kMaxTlasInstances)
    {
        Logger::Log::Error("TlasPacker: {} instances exceeds the TLAS leaf payload ceiling ({})",
                           instances.size(), kMaxTlasInstances);
        return packed;
    }

    const uint32 instanceCount = static_cast<uint32>(instances.size());
    const uint32 materialCount = static_cast<uint32>(materials.size());

    // Per-instance world AABBs and centroids drive the partition; the packed
    // records are written afterwards, in the resulting slot order.
    std::vector<AABB> worldBounds(instanceCount);
    std::vector<Vector3> centroids(instanceCount);
    AABB sceneBounds = AABB::Empty();
    for (uint32 i = 0; i < instanceCount; ++i)
    {
        worldBounds[i] = TransformBounds(instances[i].LocalBounds,
                                         instances[i].WorldFromLocal.GetGLM());
        centroids[i] = Vector3((worldBounds[i].min.x + worldBounds[i].max.x) * 0.5f,
                               (worldBounds[i].min.y + worldBounds[i].max.y) * 0.5f,
                               (worldBounds[i].min.z + worldBounds[i].max.z) * 0.5f);
        sceneBounds.Expand(worldBounds[i]);
    }

    std::vector<uint32> order(instanceCount);
    std::iota(order.begin(), order.end(), 0u);

    // Median split on the widest axis of the node's own AABB, emitted threaded
    // in one pass: push the node, recurse, then patch its miss link. The
    // partition is frozen after this build — moving instances only rewrite
    // records and refit bounds, they never re-sort.
    std::vector<TlasNodeRecord> records;
    struct WorkItem
    {
        uint32 Lo = 0;
        uint32 Hi = 0;
        uint32 NodeIndex = 0;
        bool Finalize = false;
    };
    std::vector<WorkItem> work;
    work.push_back(WorkItem{0u, instanceCount, 0u, false});

    while (!work.empty())
    {
        const WorkItem item = work.back();
        work.pop_back();

        if (item.Finalize)
        {
            records[item.NodeIndex].Miss = static_cast<uint32>(records.size());
            continue;
        }

        const uint32 index = static_cast<uint32>(records.size());
        records.push_back(TlasNodeRecord{});
        TlasNodeRecord& record = records.back();
        record.Bounds = AABB::Empty();
        for (uint32 slot = item.Lo; slot < item.Hi; ++slot)
            record.Bounds.Expand(worldBounds[order[slot]]);

        if (item.Hi - item.Lo <= kTlasLeafInstances)
        {
            record.InstanceOffset = item.Lo;
            record.InstanceCount = item.Hi - item.Lo;
            record.Miss = index + 1u; // escape = next slot
            continue;
        }

        const uint32 axis = WidestAxis(record.Bounds);
        const uint32 mid = item.Lo + (item.Hi - item.Lo) / 2u;
        std::nth_element(order.begin() + item.Lo, order.begin() + mid, order.begin() + item.Hi,
                         [&](uint32 lhs, uint32 rhs) {
                             const float32 a = centroids[lhs][static_cast<int>(axis)];
                             const float32 b = centroids[rhs][static_cast<int>(axis)];
                             return a != b ? a < b : lhs < rhs;
                         });

        work.push_back(WorkItem{0u, 0u, index, true});
        work.push_back(WorkItem{mid, item.Hi, 0u, false});
        work.push_back(WorkItem{item.Lo, mid, 0u, false});
    }

    const uint32 tlasNodeCount = static_cast<uint32>(records.size());
    packed.MaterialCount = materialCount;
    packed.InstanceCount = instanceCount;
    packed.TlasNodeCount = tlasNodeCount;
    packed.InstanceBase = materialCount * kUberMaterialStrideFloats;
    packed.TlasBase = packed.InstanceBase + instanceCount * kUberMaterialStrideFloats;
    packed.InstanceOrder = std::move(order);
    packed.WorldBounds = sceneBounds;
    packed.Buffer.assign(
        static_cast<size_t>(packed.TlasBase) + static_cast<size_t>(tlasNodeCount) * kTlasNodeStrideFloats,
        0.0f);

    for (uint32 i = 0; i < materialCount; ++i)
    {
        std::memcpy(&packed.Buffer[static_cast<size_t>(i) * kUberMaterialStrideFloats],
                    &materials[i], sizeof(UberMaterial));
    }

    for (uint32 slot = 0; slot < instanceCount; ++slot)
    {
        const TlasInstance& instance = instances[packed.InstanceOrder[slot]];
        const glm::mat4& world = instance.WorldFromLocal.GetGLM();
        const glm::mat4 inverse = glm::inverse(world);

        float32* record =
            &packed.Buffer[static_cast<size_t>(packed.InstanceBase) + slot * kUberMaterialStrideFloats];
        for (int row = 0; row < 3; ++row)
        {
            record[row * 4 + 0] = inverse[0][row];
            record[row * 4 + 1] = inverse[1][row];
            record[row * 4 + 2] = inverse[2][row];
            record[row * 4 + 3] = inverse[3][row]; // translation term
        }
        record[12] = static_cast<float32>(instance.BlasRoot);
        record[13] = static_cast<float32>(instance.BlasEnd);
        record[14] = glm::determinant(world) < 0.0f ? -1.0f : 1.0f;
        record[15] = instance.MaterialSlot == kTlasInstanceMaterialFromGeometry
                         ? -1.0f
                         : static_cast<float32>(instance.MaterialSlot);
        record[16] = instance.GIEmitter ? 1.0f : 0.0f;
    }

    for (uint32 i = 0; i < tlasNodeCount; ++i)
    {
        const TlasNodeRecord& source = records[i];
        float32* node =
            &packed.Buffer[static_cast<size_t>(packed.TlasBase) + i * kTlasNodeStrideFloats];
        node[0] = source.Bounds.min.x;
        node[1] = source.Bounds.min.y;
        node[2] = source.Bounds.min.z;
        node[3] = source.Bounds.max.x;
        node[4] = source.Bounds.max.y;
        node[5] = source.Bounds.max.z;
        node[6] = static_cast<float32>(source.Miss);
        node[7] = static_cast<float32>(source.InstanceOffset);
        node[8] = static_cast<float32>(source.InstanceCount);
    }

    return packed;
}

} // namespace GameEngine::SceneBvh
