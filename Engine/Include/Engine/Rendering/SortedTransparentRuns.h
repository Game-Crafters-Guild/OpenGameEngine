#pragma once

// Run partitioning for the sorted transparent drain (transparency-scale S2,
// design §2.7). The drain draws the view's order-dependent Blend set as
// per-(surface×blend) RUNS: records that share a PSO + bind-compatible
// geometry pools collapse into one DrawIndexedIndirectCount, per-record
// material variance riding the bindless materialIndex fetch (MaterialParams
// SSBO + global texture array — both indexed per instance, so a run only
// needs ONE representative bind).
//
// Ordering contract: WITHIN a run the records are in exact global
// back-to-front order (the GPU sort key carries the run's draw slot in its
// priority field, so one sort both partitions the array into contiguous runs
// and depth-orders each run). ACROSS runs the order is coarse — runs are
// slot-ordered by their farthest member's view depth — which is the field's
// accepted limitation for state-bucketed transparency (UE loses batching
// entirely under its exact sort; we keep the batches and accept coarse
// cross-run order).
//
// Everything in this header is pure CPU logic (no GPU state) so the grouping,
// slot assignment, and the CPU-order reference are unit-testable
// (SortedTransparentRunTests).

#include "Engine/Rendering/Material.h"
#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/SortedTransparentKey.h"

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <span>
#include <string_view>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// One order-dependent Blend draw staged for the drain, CPU-built once per view
// per frame. RunGroup is the dense DISCOVERY-order group ordinal (the run key
// map hands these out); the draw slot comes later from the partition.
struct SortedTransparentRecord
{
    uint32_t InstanceIndex = 0; // GPUScene slot (indirection payload)
    uint32_t IndexCount = 0;    // LOD0 draw args from the MeshGPUEntry
    uint32_t FirstIndex = 0;
    int32_t VertexOffset = 0;
    uint32_t RunGroup = 0;
    float ViewDepth = 0.0f; // dot(center - camPos, camForward); larger = farther
};

// std430 mirror of the record the drain compute consumes. MUST match the
// SortedTransparentRecord struct in sorted_transparent_drain.comp — field
// order and types are ABI, change both in the same commit.
struct SortedTransparentRecordGPU
{
    uint32_t InstanceIndex;
    uint32_t IndexCount;
    uint32_t FirstIndex;
    int32_t VertexOffset;
    uint32_t RunSlot;
};
static_assert(sizeof(SortedTransparentRecordGPU) == 20,
              "must match the drain shader's std430 record layout");

// PSO identity of a run, complete for the TRANSPARENT lane. Extends the
// opaque merge's ColorClassSignature (MaterialColorClassify.h) with the three
// PSO inputs that signature never needed because Blend is not color-merge
// eligible: the authored blend equation, the authored depth-write override,
// and the open user-keyword set hash. Two materials equal on all of these
// compile to a byte-identical color GraphicsPipelineDesc for the same
// (effectiveFlags, topology, pass keywords, winding), so one may bind for the
// whole run. string_views alias material-owned strings — valid for the frame
// build that computes them, never stored across frames.
struct SortedTransparentRunMaterialKey
{
    std::string_view SurfaceShaderPath;
    std::string_view VertexModifierPath;
    std::string_view LightingModel;
    uint64_t MaterialKeywords = 0u;
    uint64_t UserKeywordHash = 0u;
    MaterialBlendState Blend{}; // resolved: authored or the default equation
    int8_t DepthWriteOverride = -1; // -1 unauthored, else 0/1
    // Fully-procedural vertex path (CUSTOM_VERTEX_SHADER): changes the
    // compiled variant AND clamps vertexFlags to None at compile, so the
    // geometry key alone cannot split it — it must ride this key or two Blend
    // materials differing only here would fuse and draw foreign records
    // through the representative's pipeline (S2 review R2-3).
    bool CustomVertexShader = false;
    bool DoubleSided = false;
    bool IgnoreVertexColor = false;

    bool operator==(const SortedTransparentRunMaterialKey&) const = default;
};

inline SortedTransparentRunMaterialKey MakeSortedTransparentRunMaterialKey(const Material& mat)
{
    const MaterialCompileSpec& spec = mat.GetCompileSpec();
    SortedTransparentRunMaterialKey key{};
    key.SurfaceShaderPath = spec.surfaceShaderPath;
    key.VertexModifierPath = spec.vertexModifierPath;
    key.LightingModel = spec.lightingModel;
    key.MaterialKeywords = static_cast<uint64_t>(mat.GetVariantKey().materialKeywords);
    key.UserKeywordHash = mat.GetVariantKey().userKeywordHash;
    key.CustomVertexShader = spec.customVertexShader;
    if (const auto& authored = mat.GetBlendState())
        key.Blend = *authored;
    if (const auto zWrite = mat.GetDepthWriteOverride())
        key.DepthWriteOverride = *zWrite ? 1 : 0;
    key.DoubleSided = mat.IsDoubleSided();
    key.IgnoreVertexColor = mat.IgnoresVertexColor();
    return key;
}

// Geometry-pool identity of a run: one DrawIndexedIndirectCount binds ONE set
// of VB/IB pools, so every record in a run must resolve to the same bucket
// pool handles (meshes may differ — the indirect commands carry per-record
// index ranges into the shared pools). Topology and the mesh-derived vertex
// flags ride the PSO, so they split runs too.
struct SortedTransparentRunGeometryKey
{
    Rendering::MeshGPUEntryBindings Bindings{};
    Rendering::PrimitiveTopology Topology = Rendering::PrimitiveTopology::TriangleList;
    Rendering::VertexAttributeFlags VertexFlags = Rendering::VertexAttributeFlags::None;

    bool operator==(const SortedTransparentRunGeometryKey&) const = default;
};

// Run layout derived from the records: draw slots (coarse back-to-front run
// order) and the contiguous [RunStart, RunStart+RunCount) region each run's
// sorted records occupy. Indexed by draw slot; SlotOfGroup maps a record's
// discovery-order RunGroup to its slot.
struct SortedTransparentRunPartition
{
    std::vector<uint32_t> SlotOfGroup;
    std::vector<uint32_t> GroupOfSlot;
    std::vector<uint32_t> RunStart; // exclusive prefix of RunCount, slot order
    std::vector<uint32_t> RunCount;
};

// Order groups coarsely back-to-front: descending farthest-member view depth,
// discovery ordinal as the deterministic tiebreak. Depths are compared through
// the same monotonic float→uint transform the GPU key uses so the CPU slot
// order and the GPU key order can never disagree on any input (±0, NaN).
inline SortedTransparentRunPartition BuildSortedTransparentRunPartition(
    std::span<const SortedTransparentRecord> records, uint32_t groupCount)
{
    SortedTransparentRunPartition part{};
    part.SlotOfGroup.assign(groupCount, 0u);
    part.GroupOfSlot.resize(groupCount);
    part.RunStart.assign(groupCount, 0u);
    part.RunCount.assign(groupCount, 0u);
    if (groupCount == 0u)
        return part;

    std::vector<uint32_t> countByGroup(groupCount, 0u);
    // Sortable-uint domain: 0 is the minimum, so "no member yet" folds away.
    std::vector<uint32_t> maxDepthByGroup(groupCount, 0u);
    for (const SortedTransparentRecord& rec : records)
    {
        ++countByGroup[rec.RunGroup];
        maxDepthByGroup[rec.RunGroup] =
            std::max(maxDepthByGroup[rec.RunGroup], FloatToSortableUint(rec.ViewDepth));
    }

    std::iota(part.GroupOfSlot.begin(), part.GroupOfSlot.end(), 0u);
    std::sort(part.GroupOfSlot.begin(), part.GroupOfSlot.end(),
              [&](uint32_t a, uint32_t b)
              {
                  if (maxDepthByGroup[a] != maxDepthByGroup[b])
                      return maxDepthByGroup[a] > maxDepthByGroup[b]; // farther run first
                  return a < b;
              });

    uint32_t start = 0u;
    for (uint32_t slot = 0; slot < groupCount; ++slot)
    {
        const uint32_t group = part.GroupOfSlot[slot];
        part.SlotOfGroup[group] = slot;
        part.RunStart[slot] = start;
        part.RunCount[slot] = countByGroup[group];
        start += countByGroup[group];
    }
    return part;
}

// The total drain order as record indices: run slot ascending, view depth
// descending (back-to-front) within the run, record index as the final
// tiebreak. This is bit-exactly the order the GPU key sort produces (same
// priority ▸ inverted-depth ▸ index key, compared through the same monotonic
// transform), so it serves as both the CPU-sorted drain path for views past
// kSortedTransparentSortCapacity and the reference the drain device test
// checks the GPU against.
inline std::vector<uint32_t> BuildSortedTransparentCpuOrder(
    std::span<const SortedTransparentRecord> records,
    const SortedTransparentRunPartition& partition)
{
    std::vector<uint32_t> order(records.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(),
              [&](uint32_t ia, uint32_t ib)
              {
                  const SortedTransparentRecord& a = records[ia];
                  const SortedTransparentRecord& b = records[ib];
                  const uint32_t slotA = partition.SlotOfGroup[a.RunGroup];
                  const uint32_t slotB = partition.SlotOfGroup[b.RunGroup];
                  if (slotA != slotB)
                      return slotA < slotB;
                  const uint32_t da = FloatToSortableUint(a.ViewDepth);
                  const uint32_t db = FloatToSortableUint(b.ViewDepth);
                  if (da != db)
                      return da > db; // farther first
                  return ia < ib;
              });
    return order;
}

} // namespace GameEngine::Engine::Renderer
