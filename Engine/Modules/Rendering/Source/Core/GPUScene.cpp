/**
 * @file GPUScene.cpp
 * @brief Implementation of GPU Scene Management System
 */

#include "Rendering/Core/GPUScene.h"
#include "Logger/Logger.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/GPUCulling.h"
#include "Rendering/Core/GPUInstanceDepthClass.h"
#include "Types/GeometricReserve.h"
#include "Rendering/Core/PipelineTypes.h"
#include "Rendering/Core/SpecializationConstants.h"

static_assert(GameEngine::Rendering::kMaxCullingViewsPerDispatch ==
                  GameEngine::Rendering::kMaxViewsPerCullingDispatch,
              "kMaxCullingViewsPerDispatch (GPUCulling.h) and "
              "kMaxViewsPerCullingDispatch (GPUScene.h) must stay in sync — "
              "both pin the shader's frustumPlanes[N][6] / sliceOffsets[N] sizes.");
#include "Rendering/ShaderCache/ShaderPackageIO.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <unordered_set>

namespace GameEngine
{
namespace Rendering
{

namespace
{
// Optional, host-provided loader for the culling compute shader. When set,
// CreateCullingPipeline() will call this to retrieve SPIR-V bytes instead of
// performing any file I/O itself. This keeps renderer-core independent of
// the asset system and concrete paths.
GPUScene::CullingShaderLoaderFunc g_CullingShaderLoader = nullptr;

// Descriptor set layout used both for pipeline creation and per-dispatch
// descriptor sets in the GPU culling pass. Matches frustum_culling.comp:
//   set = 0, binding 0: GPUInstance buffer (SSBO)
//   set = 0, binding 1: GPUCullingData buffer (SSBO)
//   set = 0, binding 2: frustumResults / visibility buffer (SSBO)
DescriptorSetLayoutDesc MakeCullingDescriptorSetLayout()
{
    DescriptorSetLayoutDesc layout;
    layout.debugName = "CullingDescriptorSetLayout";

    DescriptorBinding instanceBinding;
    instanceBinding.binding = 0;
    instanceBinding.type = DescriptorType::StorageBuffer;
    instanceBinding.count = 1;
    instanceBinding.shaderStages = kShaderStageCompute;
    instanceBinding.debugName = "InstanceDataBuffer";
    layout.bindings.push_back(instanceBinding);

    DescriptorBinding cullingDataBinding;
    cullingDataBinding.binding = 1;
    cullingDataBinding.type = DescriptorType::StorageBuffer;
    cullingDataBinding.count = 1;
    cullingDataBinding.shaderStages = kShaderStageCompute;
    cullingDataBinding.debugName = "CullingDataBuffer";
    layout.bindings.push_back(cullingDataBinding);

    DescriptorBinding visibilityBinding;
    visibilityBinding.binding = 2;
    visibilityBinding.type = DescriptorType::StorageBuffer;
    visibilityBinding.count = 1;
    visibilityBinding.shaderStages = kShaderStageCompute;
    visibilityBinding.debugName = "VisibilityResultsBuffer";
    layout.bindings.push_back(visibilityBinding);

    return layout;
}

GPUInstance MakeTombstoneInstance()
{
    GPUInstance instance{};
    instance.meshIndex = 0xFFFFFFFFu;
    instance.materialIndex = 0xFFFFFFFFu;
    instance.boundingRadius = 0.0f;
    return instance;
}

// GE_SCATTER_COMPACT gates the coalesced scatter-hot mirror (Scatter.World
// fetch lever). Default on; "0" restores the pre-lever full-fat fetch path for
// A/B benching. Read once per process — GPUDrawStreamBuilder reads the same var
// to pick the matching pipeline/binding, so they agree by construction.
bool ScatterHotEnabledFromEnv()
{
    static const bool kEnabled = []()
    {
        const char* env = std::getenv("GE_SCATTER_COMPACT");
        return env == nullptr || env[0] != '0';
    }();
    return kEnabled;
}

struct CullingPushConstants
{
	    // Number of candidate instances processed by this pass. The first
	    // candidate index is provided via GPUCullingData::firstInstance so the
	    // compute shader can address arbitrary subranges of the global instance
	    // buffer.
	    uint32_t instanceCount;
	    uint32_t padding1;
	    uint32_t padding2;
	    uint32_t padding3;
	    // Per-view output base offset (in *element* units, i.e. uint32_t slots)
	    // into the shared visibility buffer. The shader writes to
	    //   frustumResults[sliceOffsets[c] + localIndex]
	    // The whole visibility buffer is bound at descriptor offset 0 so this
	    // is the sole control surface for per-view fan-out. The N=1 PSO
	    // variant reads only sliceOffsets[0].
	    uint32_t sliceOffsets[kMaxViewsPerCullingDispatch];
};
// 4-uint header + 4-uint sliceOffsets array. Must match the std430 push-constant
// block declared in frustum_culling.comp. Tripping this assert means the shader
// and host structs have drifted — fix BEFORE shipping or every culling
// dispatch silently reads garbage offsets.
static_assert(sizeof(CullingPushConstants) == 32,
              "CullingPushConstants must match frustum_culling.comp push-constant block layout");
} // anonymous namespace

void GPUScene::SetCullingShaderLoader(CullingShaderLoaderFunc loader)
{
    g_CullingShaderLoader = loader;
}

GPUScene::GPUScene(IDevice* device)
    : m_Device(device),
      m_DebugName("GPUScene"),
      m_InstanceBuffer(INVALID_BUFFER_HANDLE),
      m_ScatterHotBuffer(INVALID_BUFFER_HANDLE),
      m_MeshBuffer(INVALID_BUFFER_HANDLE),
      m_VisibilityBuffer(INVALID_BUFFER_HANDLE),
      m_IndirectArgsBuffer(INVALID_BUFFER_HANDLE),
      m_MaxInstances(0),
      m_MaxMeshes(0),
      m_InstanceCount(0),
      m_MeshCount(0),
      m_InstancesDirty(false),
      m_MeshesDirty(false),
      m_FrameIndex(0),
      m_FrameSlot(0),
      m_FramesInFlight(device ? device->GetFramesInFlight() : 1u),
      m_ScatterHotEnabled(ScatterHotEnabledFromEnv()),
      m_CullingPipeline(INVALID_PIPELINE_HANDLE)
{
    assert(m_Device != nullptr);
}

GPUScene::~GPUScene()
{
    Shutdown();
}

bool GPUScene::Initialize(uint32_t maxInstances, uint32_t maxMeshes)
{
    std::cout << "GPUScene: Initializing with " << maxInstances << " instances, "
              << maxMeshes << " meshes" << std::endl;

    m_MaxInstances = maxInstances;
    m_MaxMeshes = maxMeshes;
    m_InstanceCapacityErrorLogged = false;
    m_MeshCapacityErrorLogged = false;

    // Each slot's first instance upload must be a full one.
    for (uint32_t s = 0; s < kMaxFramesInFlight; ++s)
        m_InstanceDirtyAllSlots[s] = true;

    // Reserve CPU-side storage
    m_Instances.reserve(maxInstances);
    m_InstanceSlotFree.reserve(maxInstances);
    if (m_ScatterHotEnabled)
        m_ScatterHot.reserve(maxInstances);
    m_Meshes.reserve(maxMeshes);

    // Create GPU buffers
    CreateBuffers();

    // Defer internal compute pipelines until shaders/layouts are finalized.
    // GPUScene owns only the GPU culling compute pipeline; HZB and per-view
    // visibility resources live under GPUCullingPipeline.
    // CreateCullingPipeline();

    std::cout << "✅ GPUScene initialized successfully" << std::endl;
    return true;
}

void GPUScene::Shutdown()
{
    if (m_Device)
    {
        // Destroy buffers
        if (m_InstanceBuffer.IsValid())
        {
            m_Device->DestroyBuffer(m_InstanceBuffer);
            m_InstanceBuffer = INVALID_BUFFER_HANDLE;
        }
        if (m_ScatterHotBuffer.IsValid())
        {
            m_Device->DestroyBuffer(m_ScatterHotBuffer);
            m_ScatterHotBuffer = INVALID_BUFFER_HANDLE;
        }
        if (m_MeshBuffer.IsValid())
        {
            m_Device->DestroyBuffer(m_MeshBuffer);
            m_MeshBuffer = INVALID_BUFFER_HANDLE;
        }
        if (m_VisibilityBuffer.IsValid())
        {
            m_Device->DestroyBuffer(m_VisibilityBuffer);
            m_VisibilityBuffer = INVALID_BUFFER_HANDLE;
        }
        if (m_IndirectArgsBuffer.IsValid())
        {
            m_Device->DestroyBuffer(m_IndirectArgsBuffer);
            m_IndirectArgsBuffer = INVALID_BUFFER_HANDLE;
        }

        for (uint32_t i = 0; i < kMaxFramesInFlight; ++i)
        {
            auto& fr = m_Frames[i];
            if (fr.instanceBuffer.IsValid() && fr.instanceBuffer != m_InstanceBuffer)
                m_Device->DestroyBuffer(fr.instanceBuffer);
            if (fr.scatterHotBuffer.IsValid() && fr.scatterHotBuffer != m_ScatterHotBuffer)
                m_Device->DestroyBuffer(fr.scatterHotBuffer);
            if (fr.meshBuffer.IsValid() && fr.meshBuffer != m_MeshBuffer)
                m_Device->DestroyBuffer(fr.meshBuffer);
            if (fr.visibilityBuffer.IsValid() && fr.visibilityBuffer != m_VisibilityBuffer)
                m_Device->DestroyBuffer(fr.visibilityBuffer);
            if (fr.indirectArgsBuffer.IsValid() && fr.indirectArgsBuffer != m_IndirectArgsBuffer)
                m_Device->DestroyBuffer(fr.indirectArgsBuffer);
            fr = {};
        }

        // Destroy pipelines
        if (m_CullingPipeline.IsValid())
        {
            m_Device->DestroyPipeline(m_CullingPipeline);
            m_CullingPipeline = INVALID_PIPELINE_HANDLE;
        }
    }

    // Clear CPU data
    m_Instances.clear();
    m_ScatterHot.clear();
    m_InstanceSlotFree.clear();
    m_Meshes.clear();
    m_BatchRegistry.Clear();
    m_FreeInstanceSlots.clear();
    m_FreeMeshSlots.clear();

    m_PrevVisibleResetSlots.clear();
    m_InstanceContinuityStamps.clear();
    m_ContinuityResetSlots.clear();
    m_InstanceSlotHighWater = 0;

    m_InstanceCount = 0;
    m_MeshCount = 0;
    m_InstancesAABBDirty = true;
}

void GPUScene::ReprovisionAfterDeviceRebuild()
{
    if (!m_Device)
        return;

    // Recreate the GPU buffers over the dead handle members. CreateBuffers
    // assigns fresh handles unconditionally; the old VkBuffers were already freed
    // by the device-rebuild teardown, so there is nothing to destroy here (and
    // DestroyBuffer on a dead handle would double-free a recycled slot).
    CreateBuffers();

    // The lazily-created culling pipeline handles are dead; drop them ALL so they
    // recompile on next use (the device's concrete pipeline cache was cleared too).
    // The single-view handle AND its "attempted" latch must reset together — a
    // reset handle whose latch stayed true never recreates (CreateCullingPipeline
    // early-returns), leaving culling pipeline-less. The cascade (N=2..4) variants
    // are worse: a stale-but-still-IsValid() handle is returned as-is by
    // GetOrCreateCascadeCullingPipeline, so its SetPipeline resolves null on the
    // rebuilt device, the bind is silently skipped, and the next SetConstants
    // pushes against whatever pipeline was last bound — tripping the push-constant
    // range assert on the first resumed frame.
    m_CullingPipeline = INVALID_PIPELINE_HANDLE;
    m_CullingPipelineCreationAttempted = false;
    for (uint32_t v = 0; v <= kMaxViewsPerCullingDispatch; ++v)
    {
        m_CascadeCullingPipelines[v] = INVALID_PIPELINE_HANDLE;
        m_CascadeCullingPipelineAttempted[v] = false;
    }

    // Force a full re-upload of the retained CPU mirror. UpdateGPUBuffers only
    // touches the current slot, so mark every slot fully dirty: the first frame
    // after resume uploads immediately and the rest converge as they rotate in.
    for (uint32_t s = 0; s < kMaxFramesInFlight; ++s)
    {
        m_InstanceDirtyAllSlots[s] = true;
        m_InstancesDirtySlots[s] = true;
        m_MeshesDirtySlots[s] = true;
    }
    m_InstancesAABBDirty = true;

    // Upload the current slot now; remaining slots flush as they become current.
    UpdateGPUBuffers();
}

namespace
{
// Sets instance `instanceIndex`'s dirty bit in one slot's bit array and
// reports whether its word was clean before, which is when the word joins the
// slot's dirty-word list (exactly once: bits are only cleared at the flush).
// Atomic, because UpdateInstances' concurrent ranges can share a word.
bool SetInstanceDirtyBit(std::vector<uint64_t>& bits, uint32_t instanceIndex)
{
    const uint64_t bit = 1ull << (instanceIndex % 64u);
    return std::atomic_ref<uint64_t>(bits[instanceIndex / 64u]).fetch_or(bit, std::memory_order_relaxed) == 0ull;
}
} // namespace

void GPUScene::MarkInstanceDirtyAllSlots(uint32_t instanceIndex)
{
    const uint32_t word = instanceIndex / 64u;
    for (uint32_t s = 0; s < m_FramesInFlight; ++s)
    {
        if (m_InstanceDirtyAllSlots[s])
            continue;
        auto& bits = m_InstanceDirtyBits[s];
        if (word >= bits.size())
            bits.resize(word + 1u, 0ull);
        if (SetInstanceDirtyBit(bits, instanceIndex))
            m_InstanceDirtyWords[s].push_back(word);
    }
}

uint32_t GPUScene::AddInstance(const GPUInstance& instance)
{
    assert(!m_RowBatchInFlight.load(std::memory_order_relaxed) &&
           "GPUScene::AddInstance during UpdateInstances: no other mutator may run while its rows copy");
    // Removal is also called by ECS resource-release hooks. Provision its
    // free-slot and dirty-word storage before a new row becomes owned/live.
    const size_t rows = m_Instances.size() + (m_FreeInstanceSlots.empty() ? 1 : 0);
    ReserveGeometric(m_FreeInstanceSlots, rows);
    const size_t words = (rows + 63) / 64;
    for (uint32_t slot = 0; slot < m_FramesInFlight; ++slot)
    {
        ReserveGeometric(m_InstanceDirtyWords[slot], words);
        auto& bits = m_InstanceDirtyBits[slot];
        ReserveGeometric(bits, words);
        if (bits.size() < words)
            bits.resize(words, 0);
    }
    uint32_t index = AllocateInstanceSlot();

    if (index < m_Instances.size())
    {
        m_Instances[index] = instance;
    }
    else
    {
        m_Instances.push_back(instance);
        // Free-membership mirror stays size-locked to m_Instances; a fresh
        // slot is live, not free. (Reused slots were cleared in
        // AllocateInstanceSlot.)
        m_InstanceSlotFree.push_back(0u);
    }

    // Continuity stamp: sized to the high-water mark, never trimmed, so a
    // recycled slot AND a re-appended trimmed index both continue the prior
    // tenant's count and the two tenants can never share a stamp.
    AdvanceContinuityStamp(index);

    // Mirror stays index-locked to m_Instances (same append/reuse decision).
    if (m_ScatterHotEnabled)
    {
        if (index < m_ScatterHot.size())
            m_ScatterHot[index] = MakeScatterHot(instance);
        else
            m_ScatterHot.push_back(MakeScatterHot(instance));
    }

    // Two-phase HZB history reset: a slot the scene has used before (free-list
    // reuse or re-append into a trimmed index) may still hold the prior tenant's
    // prevVisible occlusion state, so queue it for a 0xFFFFFFFF reset. A slot at
    // the high-water mark is genuinely fresh — its prevVisible is the buffer's
    // 0xFFFFFFFF first-touch fill, so no reset is owed.
    if (index < m_InstanceSlotHighWater)
        m_PrevVisibleResetSlots.push_back(index);
    else
        m_InstanceSlotHighWater = index + 1u;

    m_InstanceCount++;
    m_InstancesDirty = true;
    m_InstancesAABBDirty = true;
    MarkInstanceDirtyAllSlots(index);
    for (uint32_t s = 0; s < m_FramesInFlight; ++s) m_InstancesDirtySlots[s] = true;

    // Reused free-list slots always hold a tombstone (RemoveInstance wrote it
    // before freeing), so Add never owes a decrement for the previous tenant.
    m_BatchRegistry.OnInstanceAdded(instance.materialIndex, instance.meshIndex,
                                    (instance.flags & kInstanceFlagMirrored) != 0u);

    return index;
}

void GPUScene::UpdateInstance(uint32_t instanceIndex, const GPUInstance& instance)
{
    assert(!m_RowBatchInFlight.load(std::memory_order_relaxed) &&
           "GPUScene::UpdateInstance during UpdateInstances: no other mutator may run while its rows copy");
    if (instanceIndex >= m_Instances.size())
    {
        Logger::Log::Warning("GPUScene: Invalid instance index {}", instanceIndex);
        return;
    }

    // Callers that rebuild instances every frame (asset previews, feature
    // renderers) usually produce identical bytes — don't dirty the row for
    // a no-op write. (Byte-identical rows imply identical batch keys, so the
    // registry never misses a membership change through this early-out.)
    if (std::memcmp(&m_Instances[instanceIndex], &instance, sizeof(GPUInstance)) == 0)
        return;

    m_BatchRegistry.OnInstanceUpdated(
        m_Instances[instanceIndex].materialIndex, m_Instances[instanceIndex].meshIndex,
        (m_Instances[instanceIndex].flags & kInstanceFlagMirrored) != 0u,
        instance.materialIndex, instance.meshIndex,
        (instance.flags & kInstanceFlagMirrored) != 0u);
    m_Instances[instanceIndex] = instance;
    // The no-op skip above compared the whole 240 B; a change reaching here can
    // touch scatter-hot fields, so refresh the mirror row unconditionally (it
    // is a byte-cheap subset of what we just wrote).
    if (m_ScatterHotEnabled && instanceIndex < m_ScatterHot.size())
        m_ScatterHot[instanceIndex] = MakeScatterHot(instance);
    m_InstancesDirty = true;
    m_InstancesAABBDirty = true;
    MarkInstanceDirtyAllSlots(instanceIndex);
    for (uint32_t s = 0; s < m_FramesInFlight; ++s) m_InstancesDirtySlots[s] = true;
}

#ifndef NDEBUG
void GPUScene::AssertEachRowWrittenOnce(std::span<const InstanceWrite> writes)
{
    if (++m_RowBatchNumber == 0)
    {
        std::fill(m_RowBatchStamp.begin(), m_RowBatchStamp.end(), 0u);
        m_RowBatchNumber = 1;
    }
    if (m_RowBatchStamp.size() < m_Instances.size())
        m_RowBatchStamp.resize(m_Instances.size(), 0u);
    for (const InstanceWrite& write : writes)
    {
        if (write.InstanceIndex >= m_RowBatchStamp.size())
            continue; // the bookkeeping phase warns about an out-of-range index
        uint32_t& stamp = m_RowBatchStamp[write.InstanceIndex];
        assert(stamp != m_RowBatchNumber &&
               "GPUScene::UpdateInstances: an instance index appears twice in the span; "
               "write each row once (dedupe the writes, keeping the last)");
        stamp = m_RowBatchNumber;
    }
}
#endif

void GPUScene::UpdateInstances(std::span<const InstanceWrite> writes, const RangeRunner& runRanges)
{
    if (writes.empty())
        return;
    assert(!m_RowBatchInFlight.load(std::memory_order_relaxed) && "GPUScene::UpdateInstances re-entered");
#ifndef NDEBUG
    AssertEachRowWrittenOnce(writes);
#endif

    // The row phase marks dirty bits in place, so every slot's bit array must
    // already cover every row; AddInstance keeps it so, this restates it.
    const size_t words = (m_Instances.size() + 63u) / 64u;
    for (uint32_t s = 0; s < m_FramesInFlight; ++s)
        if (m_InstanceDirtyBits[s].size() < words)
            m_InstanceDirtyBits[s].resize(words, 0ull);

    // Row phase, concurrent over disjoint writes (see CopyInstanceRows): rows,
    // scatter-hot rows and dirty bits, with each range's outcome merged once.
    m_RowWriteOutcomes.resize(writes.size());
    m_RowBatchExceptions.clear();
    m_RowBatchAnyChanged = false;
    m_RowBatchInFlight.store(true, std::memory_order_relaxed);
    runRanges(writes.size(), [this, writes](size_t begin, size_t end) { CopyInstanceRows(writes, begin, end); });
    m_RowBatchInFlight.store(false, std::memory_order_relaxed);

    // The writes the row phase could not finish, in span order: an index past
    // the scene is reported, and a changed batch key updates the registry in
    // the order per-write UpdateInstance calls would. Usually none.
    std::sort(m_RowBatchExceptions.begin(), m_RowBatchExceptions.end());
    for (const size_t i : m_RowBatchExceptions)
    {
        const RowWriteOutcome& outcome = m_RowWriteOutcomes[i];
        if (outcome.InstanceIndex >= m_Instances.size())
        {
            Logger::Log::Warning("GPUScene: Invalid instance index {}", outcome.InstanceIndex);
            continue;
        }
        m_BatchRegistry.OnInstanceUpdated(outcome.OldMaterialIndex, outcome.OldMeshIndex,
                                          outcome.OldMirrored, outcome.NewMaterialIndex,
                                          outcome.NewMeshIndex, outcome.NewMirrored);
    }
    if (!m_RowBatchAnyChanged)
        return;
    m_InstancesDirty = true;
    m_InstancesAABBDirty = true;
    for (uint32_t s = 0; s < m_FramesInFlight; ++s) m_InstancesDirtySlots[s] = true;
}

void GPUScene::CopyInstanceRows(std::span<const InstanceWrite> writes, size_t begin, size_t end)
{
    // Touches only the rows of writes [begin, end) (indices are unique across
    // the span) and their outcome slots, so disjoint ranges run concurrently.
    // Two ranges can share a dirty word: the bit is set with an atomic OR, and
    // the range whose OR turned the word nonzero lists it, so each word is
    // listed once per slot. The list's order is free (the flush sorts it).
    const size_t rowCount = m_Instances.size();
    uint32_t newWords[kMaxFramesInFlight][kRowBatchWordStage];
    uint32_t newWordCount[kMaxFramesInFlight]{};
    std::vector<size_t> exceptions;
    bool anyChanged = false;
    for (size_t i = begin; i < end; ++i)
    {
        const InstanceWrite& write = writes[i];
        RowWriteOutcome& outcome = m_RowWriteOutcomes[i];
        outcome.InstanceIndex = write.InstanceIndex;
        if (write.InstanceIndex >= rowCount)
        {
            exceptions.push_back(i);
            continue;
        }
        GPUInstance& row = m_Instances[write.InstanceIndex];
        if (std::memcmp(&row, &write.Instance, sizeof(GPUInstance)) == 0)
            continue;
        outcome.OldMaterialIndex = row.materialIndex;
        outcome.OldMeshIndex = row.meshIndex;
        outcome.OldMirrored = (row.flags & kInstanceFlagMirrored) != 0u;
        outcome.NewMaterialIndex = write.Instance.materialIndex;
        outcome.NewMeshIndex = write.Instance.meshIndex;
        outcome.NewMirrored = (write.Instance.flags & kInstanceFlagMirrored) != 0u;
        anyChanged = true;
        if (outcome.OldMaterialIndex != outcome.NewMaterialIndex || outcome.OldMeshIndex != outcome.NewMeshIndex ||
            outcome.OldMirrored != outcome.NewMirrored)
            exceptions.push_back(i);
        row = write.Instance;
        if (m_ScatterHotEnabled && write.InstanceIndex < m_ScatterHot.size())
            m_ScatterHot[write.InstanceIndex] = MakeScatterHot(write.Instance);

        const uint32_t word = write.InstanceIndex / 64u;
        for (uint32_t s = 0; s < m_FramesInFlight; ++s)
        {
            if (m_InstanceDirtyAllSlots[s])
                continue;
            if (!SetInstanceDirtyBit(m_InstanceDirtyBits[s], write.InstanceIndex))
                continue;
            if (newWordCount[s] == kRowBatchWordStage)
            {
                AppendRowBatchWords(s, newWords[s], newWordCount[s]);
                newWordCount[s] = 0;
            }
            newWords[s][newWordCount[s]++] = word;
        }
    }
    for (uint32_t s = 0; s < m_FramesInFlight; ++s)
        if (newWordCount[s] != 0)
            AppendRowBatchWords(s, newWords[s], newWordCount[s]);
    if (!exceptions.empty() || anyChanged)
    {
        std::lock_guard lock(m_RowBatchMutex);
        m_RowBatchExceptions.insert(m_RowBatchExceptions.end(), exceptions.begin(), exceptions.end());
        m_RowBatchAnyChanged = m_RowBatchAnyChanged || anyChanged;
    }
}

void GPUScene::AppendRowBatchWords(uint32_t slot, const uint32_t* words, uint32_t count)
{
    std::lock_guard lock(m_RowBatchMutex);
    m_InstanceDirtyWords[slot].insert(m_InstanceDirtyWords[slot].end(), words, words + count);
}

void GPUScene::RemoveInstance(uint32_t instanceIndex)
{
    // Delegate to the batched path so the invariant-heavy remove logic lives in
    // one place and single/batched behavior can't drift.
    RemoveInstances(std::span<const uint32_t>(&instanceIndex, 1));
}

void GPUScene::RemoveInstances(std::span<const uint32_t> instanceIndices)
{
    assert(!m_RowBatchInFlight.load(std::memory_order_relaxed) &&
           "GPUScene::RemoveInstances during UpdateInstances: a release hook ran inside the extraction "
           "wave; structural changes must go through the world's command buffer");
    if (instanceIndices.empty())
        return;

    // Dense free-membership already folds duplicates in O(1), so walk the
    // caller's span without an allocating copy (including single removals).
    // Slot reuse order is unspecified; the free set and final rows are stable.
    bool anyRemoved = false;
    for (uint32_t index : instanceIndices)
    {
        if (index >= m_Instances.size())
        {
            Logger::Log::Warning("GPUScene: Invalid instance index {}", index);
            continue;
        }
        // Dense free-membership check replaces the single path's O(free-list)
        // std::find double-remove guard. A slot already on the free-list (this
        // call or a prior one) is skipped silently, matching the old guard.
        if (m_InstanceSlotFree[index] != 0u)
            continue;

        // Read the outgoing pair before tombstoning. A row that is already a
        // tombstone (disable-then-delete: ClearMeshGpuInstance wrote it via
        // UpdateInstance) is untracked and decrements nothing.
        m_BatchRegistry.OnInstanceRemoved(m_Instances[index].materialIndex,
                                          m_Instances[index].meshIndex,
                                          (m_Instances[index].flags & kInstanceFlagMirrored) != 0u);

        m_Instances[index] = MakeTombstoneInstance();
        // Mirror the tombstone (meshIndex=0xFFFFFFFF self-rejects in the scatter).
        if (m_ScatterHotEnabled && index < m_ScatterHot.size())
            m_ScatterHot[index] = MakeScatterHot(m_Instances[index]);
        MarkInstanceDirtyAllSlots(index);
        FreeInstanceSlot(index);
        if (m_InstanceCount > 0)
            --m_InstanceCount;
        anyRemoved = true;
    }

    if (!anyRemoved)
        return;

    // Single trailing-trim pass: strip the maximal suffix of free slots so the
    // instance array (and its mirrors) shrink back. O(1) membership per step
    // via m_InstanceSlotFree; the free-list vector is reconciled in one
    // erase_if afterward (every trimmed slot is >= the new size by
    // construction, and every surviving free slot is < it).
    const size_t sizeBefore = m_Instances.size();
    while (!m_Instances.empty() && m_InstanceSlotFree.back() != 0u)
    {
        m_Instances.pop_back();
        m_InstanceSlotFree.pop_back();
        if (m_ScatterHotEnabled && !m_ScatterHot.empty())
            m_ScatterHot.pop_back();
    }
    if (m_Instances.size() != sizeBefore)
    {
        const uint32_t newSize = static_cast<uint32_t>(m_Instances.size());
        std::erase_if(m_FreeInstanceSlots, [newSize](uint32_t s) { return s >= newSize; });
    }

    m_InstancesDirty = true;
    m_InstancesAABBDirty = true;
    for (uint32_t s = 0; s < m_FramesInFlight; ++s) m_InstancesDirtySlots[s] = true;
}

void GPUScene::DrainPrevVisibleResetSlots(std::vector<uint32_t>& outSlots)
{
    // Ping-pong the buffers so neither side reallocates in steady state: outSlots
    // is emptied (capacity kept) then swapped with the queue, leaving the queue
    // holding outSlots' freed buffer.
    outSlots.clear();
    outSlots.swap(m_PrevVisibleResetSlots);
}

void GPUScene::BumpInstanceContinuityStamp(uint32_t instanceIndex)
{
    if (instanceIndex >= m_InstanceContinuityStamps.size())
    {
        Logger::Log::Warning("GPUScene: Invalid instance index {}", instanceIndex);
        return;
    }
    AdvanceContinuityStamp(instanceIndex);
}

void GPUScene::AdvanceContinuityStamp(uint32_t instanceIndex)
{
    if (instanceIndex < m_InstanceContinuityStamps.size())
        ++m_InstanceContinuityStamps[instanceIndex];
    else
        m_InstanceContinuityStamps.push_back(1u);
    m_ContinuityResetSlots.push_back(instanceIndex);
}

std::vector<uint32_t> GPUScene::DrainContinuityResetSlots()
{
    std::vector<uint32_t> drained;
    drained.swap(m_ContinuityResetSlots);
    return drained;
}

uint32_t GPUScene::AddMesh(const GPUMesh& mesh)
{
    ReserveGeometric(m_FreeMeshSlots, m_Meshes.size() + (m_FreeMeshSlots.empty() ? 1 : 0));
    uint32_t index = AllocateMeshSlot();

    if (index < m_Meshes.size())
    {
        m_Meshes[index] = mesh;
    }
    else
    {
        m_Meshes.push_back(mesh);
    }

    m_MeshCount++;
    m_MeshesDirty = true;
    for (uint32_t s = 0; s < m_FramesInFlight; ++s) m_MeshesDirtySlots[s] = true;

    return index;
}

void GPUScene::UpdateMesh(uint32_t meshIndex, const GPUMesh& mesh)
{
    if (meshIndex >= m_Meshes.size())
    {
        Logger::Log::Warning("GPUScene: Invalid mesh index {}", meshIndex);
        return;
    }

    m_Meshes[meshIndex] = mesh;
    m_MeshesDirty = true;
    for (uint32_t s = 0; s < m_FramesInFlight; ++s) m_MeshesDirtySlots[s] = true;
}

void GPUScene::RemoveMesh(uint32_t meshIndex)
{
    if (meshIndex >= m_Meshes.size())
    {
        Logger::Log::Warning("GPUScene: Invalid mesh index {}", meshIndex);
        return;
    }

    // Zero-fill the slot so any GPU read before the slot is reused sees a
    // well-defined empty record. Avoid double-free by treating a row whose
    // count fields are already zero as already-freed.
    if (m_Meshes[meshIndex].indexCount == 0 && m_Meshes[meshIndex].vertexCount == 0)
        return;
    m_Meshes[meshIndex] = GPUMesh{};

    FreeMeshSlot(meshIndex);
    if (m_MeshCount > 0)
        --m_MeshCount;
    m_MeshesDirty = true;
    for (uint32_t s = 0; s < m_FramesInFlight; ++s) m_MeshesDirtySlots[s] = true;
}

void GPUScene::BeginFrame()
{
    AdvanceFrameSlot();
    UpdateGPUBuffers();
}

void GPUScene::AdvanceFrameSlot()
{
    m_FrameIndex++;
    m_FrameSlot = m_Device->GetFrameIndex() % m_FramesInFlight;
}

void GPUScene::EndFrame()
{
    // Reset dirty flags
    m_InstancesDirty = false;
    m_MeshesDirty = false;
}

// ── RenderGraph arm ──────────────────────────────────────────────────────────────────

GPUScene::GPUSceneFrameRG GPUScene::ImportFrameResources(RenderGraph::RGFrame& frame) const
{
    GPUSceneFrameRG out;
    out.Instances = frame.ImportExternalBuffer("GPUScene.Instances", GetInstanceBuffer());
    if (BufferHandle hot = GetScatterHotBuffer(); hot.IsValid())
        out.ScatterHot = frame.ImportExternalBuffer("GPUScene.ScatterHot", hot);
    out.Meshes = frame.ImportExternalBuffer("GPUScene.Meshes", GetMeshBuffer());
    return out;
}

void GPUScene::ScheduleCullingPassForRange(RenderGraph::RGFrame& frame, const GPUSceneFrameRG& sceneRG,
                                           RenderGraph::RGBuffer visibility, uint32_t visibilityTotalBytes,
                                           uint32_t visibilityOffsetElements,
                                           uint32_t firstInstance, uint32_t instanceCount,
                                           const GPUCullingData& cullingData, uint64_t sliceStableKey)
{
    if (!m_CullingPipeline.IsValid())
    {
        CreateCullingPipeline();
        if (!m_CullingPipeline.IsValid())
            return;
    }
    const uint32_t sliceOffsets[1] = {visibilityOffsetElements};
    ScheduleCullingPassImpl(frame, sceneRG, visibility, visibilityTotalBytes, m_CullingPipeline,
                            sliceOffsets, /*viewCount=*/1u, firstInstance, instanceCount,
                            cullingData, sliceStableKey);
}

void GPUScene::ScheduleCullingPassForCascadeGroup(RenderGraph::RGFrame& frame, const GPUSceneFrameRG& sceneRG,
                                                  RenderGraph::RGBuffer visibility, uint32_t visibilityTotalBytes,
                                                  const uint32_t* sliceOffsetsElements, uint32_t viewCount,
                                                  uint32_t firstInstance, uint32_t instanceCount,
                                                  const GPUCullingData& cullingData, uint64_t groupStableKey)
{
    if (instanceCount == 0u || viewCount == 0u)
        return;
    if (viewCount > kMaxViewsPerCullingDispatch)
        viewCount = kMaxViewsPerCullingDispatch;

    // Single-cascade groups are degenerate — fall back to the single-view PSO
    // so we don't lazy-compile a redundant N=1 variant.
    PipelineHandle pso;
    if (viewCount == 1u)
    {
        if (!m_CullingPipeline.IsValid())
        {
            CreateCullingPipeline();
            if (!m_CullingPipeline.IsValid())
                return;
        }
        pso = m_CullingPipeline;
    }
    else
    {
        pso = GetOrCreateCascadeCullingPipeline(viewCount);
        if (!pso.IsValid())
            return;
    }
    ScheduleCullingPassImpl(frame, sceneRG, visibility, visibilityTotalBytes, pso,
                            sliceOffsetsElements, viewCount, firstInstance, instanceCount,
                            cullingData, groupStableKey);
}

void GPUScene::ScheduleCullingPassImpl(RenderGraph::RGFrame& frame, const GPUSceneFrameRG& sceneRG,
                                       RenderGraph::RGBuffer visibility, uint32_t visibilityTotalBytes,
                                       PipelineHandle pipeline,
                                       const uint32_t* sliceOffsetsElements, uint32_t viewCount,
                                       uint32_t firstInstance, uint32_t instanceCount,
                                       const GPUCullingData& cullingData, uint64_t sliceStableKey)
{
    if (!visibility.IsValid() || !pipeline.IsValid() || instanceCount == 0u || viewCount == 0u)
        return;

    // Per-dispatch params written NOW, at declaration, into the frame's upload
    // ring (256-aligned ≥ minStorageBufferOffsetAlignment). The old arm's
    // 256-slot grid, slot indices, and exec-time UpdateBuffer all dissolve.
    auto cd = frame.AllocUpload<GPUCullingData>();
    if (!cd.Valid())
        return;
    *cd.Ptr = cullingData;
    cd.Ptr->instanceCount = instanceCount;
    cd.Ptr->firstInstance = firstInstance;

    char passName[64];
    std::snprintf(passName, sizeof(passName), "GPUCulling.View%u.C%u",
                  static_cast<uint32_t>(sliceStableKey >> 16),
                  static_cast<uint32_t>(sliceStableKey & 0xFFFF)); // arena-copied by AddPass

    CullingPushConstants pc{};
    pc.instanceCount = instanceCount;
    for (uint32_t c = 0; c < kMaxViewsPerCullingDispatch; ++c)
        pc.sliceOffsets[c] = (c < viewCount) ? sliceOffsetsElements[c] : 0u;

    const size_t kMinInstanceBufferSize = 1024;
    const size_t instBytes =
        std::max(kMinInstanceBufferSize, static_cast<size_t>(m_MaxInstances) * sizeof(GPUInstance));

    // Graphics queue deliberately (slice-2 decision D1): the old graph recorded
    // these inline on the graphics CL; AddComputePass would smuggle in real
    // async compute + cross-queue semaphores. Flip-to-compute is a post-gate-2
    // experiment.
    frame.AddPass(passName, static_cast<int32_t>(PassPhase::kEarlySetup),
                  [&](RenderGraph::RGPassBuilder& p)
                  {
                      // Meshes deliberately not declared: frustum_culling.comp
                      // never reads it (the old declaration existed only for
                      // import-name dedup).
                      p.Read(sceneRG.Instances);
                      p.Write(visibility);
                  },
                  [pipeline, cdBuf = cd.Buffer, cdOff = cd.Offset, instances = sceneRG.Instances,
                   visibility, pc, instBytes, visibilityTotalBytes, instanceCount](RenderGraph::RGContext& ctx)
                  {
                      DescriptorSetLayoutDesc layout = MakeCullingDescriptorSetLayout();
                      DescriptorSetDesc setDesc{};
                      setDesc.layout = layout;
                      setDesc.transient = true;
                      setDesc.debugName = "GPUScene_Culling_DS0";
                      DescriptorSetHandle set = ctx.GetDevice()->CreateDescriptorSet(setDesc);
                      if (!set.IsValid())
                          return;
                      // Explicit byte sizes everywhere — range=0 semantics are
                      // contested between backends (slice-2 risk R1).
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 0, ctx.GetBuffer(instances), 0,
                                                                  instBytes);
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 1, cdBuf, cdOff,
                                                                  sizeof(GPUCullingData));
                      ctx.GetDevice()->UpdateStorageBufferBinding(set, 2, ctx.GetBuffer(visibility), 0,
                                                                  visibilityTotalBytes);
                      ctx.Cmd->SetPipeline(pipeline);
                      ctx.Cmd->BindDescriptorSet(0, set, pipeline);
                      ctx.Cmd->SetPushConstants(pc);
                      ctx.Cmd->Dispatch((instanceCount + 63u) / 64u, 1, 1);
                  });
}


void GPUScene::CreateBuffers()
{
    std::cout << "GPUScene: Creating GPU buffers..." << std::endl;

    // Fresh (or re-provisioned) physicals: whatever GPU content existed is
    // gone, so the elision epoch must advance even with no CPU dirty bits.
    ++m_ContentEpoch;

    // Create instance buffer
    {
        BufferDesc d{};
        // Allocate based on the configured maximum instance capacity so that
        // stress tests and large worlds cannot overflow the buffer when
        // UpdateGPUBuffers uploads all instances.
        const size_t kMinInstanceBufferSize = 1024;
        const size_t maxInstanceBytes = static_cast<size_t>(m_MaxInstances) * sizeof(GPUInstance);
        d.size = std::max(kMinInstanceBufferSize, maxInstanceBytes);
        d.usage = static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::Storage | BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::Upload;
        d.flags = BufferCreateFlags::None;
        d.debugName = "GPUScene_InstanceBuffer";
        m_InstanceBuffer = m_Device->CreateBuffer(d);
    }

    // Coalesced scatter-hot mirror (32 B/instance). Storage + TransferDst only:
    // read by the scatter compute via a descriptor binding (no vertex fetch, no
    // buffer-reference/BDA), uploaded host-side like the instance buffer.
    if (m_ScatterHotEnabled)
    {
        BufferDesc d{};
        const size_t kMinScatterHotBufferSize = 256;
        const size_t maxScatterHotBytes =
            static_cast<size_t>(m_MaxInstances) * sizeof(GPUInstanceScatterHot);
        d.size = std::max(kMinScatterHotBufferSize, maxScatterHotBytes);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::Upload;
        d.flags = BufferCreateFlags::None;
        d.debugName = "GPUScene_ScatterHotBuffer";
        m_ScatterHotBuffer = m_Device->CreateBuffer(d);
    }

    // Mesh metadata table: one GPUMesh row per declared mesh slot. What keeps
    // an upload inside the allocation is this size TOGETHER WITH the row clamp
    // in UpdateGPUBuffers; the size alone bounds nothing.
    //
    // Instance buffers follow the same upload clamp. CPU-side admission remains
    // unrestricted; rows beyond either declared GPU capacity are reported and
    // omitted from uploads so they cannot invalidate writes to valid rows.
    {
        BufferDesc d{};
        const size_t kMinMeshBufferSize = 1024;
        const size_t maxMeshBytes = static_cast<size_t>(m_MaxMeshes) * sizeof(GPUMesh);
        d.size = std::max(kMinMeshBufferSize, maxMeshBytes);
        d.usage = static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Storage | BufferUsage::TransferDst);
        d.memoryUsage = BufferMemoryUsage::Upload;
        d.flags = BufferCreateFlags::None;
        d.debugName = "GPUScene_MeshBuffer";
        m_MeshBuffer = m_Device->CreateBuffer(d);
    }

    // Visibility buffer (storage + transfer)
    {
        BufferDesc d{};
        // One visibility uint per potential instance.
        const size_t kMinVisibilityBufferSize = 4096;
        const size_t maxVisibilityBytes = static_cast<size_t>(m_MaxInstances) * sizeof(uint32_t);
        d.size = std::max(kMinVisibilityBufferSize, maxVisibilityBytes);
        d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        d.flags = BufferCreateFlags::None;
        d.debugName = "GPUScene_VisibilityBuffer";
        m_VisibilityBuffer = m_Device->CreateBuffer(d);
    }

    // Indirect args buffer (legacy single-buffer for demo paths)
    {
        BufferDesc d{};
        d.size = 4 * 1024;
        d.usage = static_cast<uint32_t>(BufferUsage::Indirect | BufferUsage::TransferDst | BufferUsage::TransferSrc);
        d.memoryUsage = BufferMemoryUsage::DeviceLocal;
        d.flags = BufferCreateFlags::None;
        d.debugName = "GPUScene_IndirectArgs";
        m_IndirectArgsBuffer = m_Device->CreateBuffer(d);
    }

    for (uint32_t i = 0; i < m_FramesInFlight; ++i)
    {
        {
            BufferDesc d{};
            const size_t kMinInstanceBufferSize = 1024;
            const size_t maxInstanceBytes = static_cast<size_t>(m_MaxInstances) * sizeof(GPUInstance);
            d.size = std::max(kMinInstanceBufferSize, maxInstanceBytes);
            // ShaderDeviceAddress: instanced vertex shaders consume this via
            // GL_EXT_buffer_reference (instance_io.glsl GE_INSTANCED path),
            // dropping the SSBO binding from set 0 so the instanced-depth
            // descriptor set becomes app-lifetime-cacheable. Requested only
            // when the device has BDA; without it consumers already take the
            // SSBO-binding path (they bail on a zero address).
            d.usage = static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::Storage |
                                             BufferUsage::TransferDst);
            if (m_Device->GetCapabilities().supportsBufferDeviceAddress)
                d.usage |= static_cast<uint32_t>(BufferUsage::ShaderDeviceAddress);
            d.memoryUsage = BufferMemoryUsage::Upload;
            // FrameSlotted: one element of a ring exactly as deep as the device paces,
            // so the write is safe only behind BeginFrame — where UpdateGPUBuffers runs
            // (FlushGPUBuffers is called from the render graph build, not from the
            // update-phase AdvanceFrameSlot that picks the slot).
            d.flags = BufferCreateFlags::FrameSlotted;
            d.debugName = "GPUScene_InstanceBuffer_PerFrame";
            m_Frames[i].instanceBuffer = m_Device->CreateBuffer(d);
        }
        if (m_ScatterHotEnabled)
        {
            BufferDesc d{};
            const size_t kMinScatterHotBufferSize = 256;
            const size_t maxScatterHotBytes =
                static_cast<size_t>(m_MaxInstances) * sizeof(GPUInstanceScatterHot);
            d.size = std::max(kMinScatterHotBufferSize, maxScatterHotBytes);
            d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst);
            d.memoryUsage = BufferMemoryUsage::Upload;
            d.flags = BufferCreateFlags::FrameSlotted;
            d.debugName = "GPUScene_ScatterHotBuffer_PerFrame";
            m_Frames[i].scatterHotBuffer = m_Device->CreateBuffer(d);
        }
        {
            BufferDesc d{};
            const size_t kMinMeshBufferSize = 1024;
            const size_t maxMeshBytes = static_cast<size_t>(m_MaxMeshes) * sizeof(GPUMesh);
            d.size = std::max(kMinMeshBufferSize, maxMeshBytes);
            d.usage = static_cast<uint32_t>(BufferUsage::Vertex | BufferUsage::Index | BufferUsage::Storage | BufferUsage::TransferDst);
            d.memoryUsage = BufferMemoryUsage::Upload;
            d.flags = BufferCreateFlags::FrameSlotted;
            d.debugName = "GPUScene_MeshBuffer_PerFrame";
            m_Frames[i].meshBuffer = m_Device->CreateBuffer(d);
        }
        {
            const size_t kMinVisibilityBufferSize = 4096;
            const size_t maxVisibilityBytes = static_cast<size_t>(m_MaxInstances) * sizeof(uint32_t);
            BufferDesc d{};
            d.size = std::max(kMinVisibilityBufferSize, maxVisibilityBytes);
            d.usage = static_cast<uint32_t>(BufferUsage::Storage | BufferUsage::TransferDst | BufferUsage::TransferSrc);
            d.memoryUsage = BufferMemoryUsage::DeviceLocal;
            d.flags = BufferCreateFlags::None;
            d.debugName = "GPUScene_VisibilityBuffer_PerFrame";
            m_Frames[i].visibilityBuffer = m_Device->CreateBuffer(d);
        }
        {
            BufferDesc d{};
            d.size = 4 * 1024;
            d.usage = static_cast<uint32_t>(BufferUsage::Indirect | BufferUsage::TransferDst | BufferUsage::TransferSrc);
            d.memoryUsage = BufferMemoryUsage::DeviceLocal;
            d.flags = BufferCreateFlags::None;
            d.debugName = "GPUScene_IndirectArgs_PerFrame";
            m_Frames[i].indirectArgsBuffer = m_Device->CreateBuffer(d);
        }
    }

    std::cout << "✅ GPU buffers created successfully" << std::endl;
}

void GPUScene::UpdateGPUBuffers()
{
// Optional verbose diagnostics (compile-time gated)
#if defined(GE_DEBUG_GPU_SCENE)
    std::cout << "GPUScene::UpdateGPUBuffers() called:" << std::endl;
    std::cout << "  m_InstancesDirty: " << (m_InstancesDirty ? "true" : "false") << std::endl;
    std::cout << "  m_Instances.empty(): " << (m_Instances.empty() ? "true" : "false") << std::endl;
    std::cout << "  m_Instances.size(): " << m_Instances.size() << std::endl;
    std::cout << "  current instance buffer: " << GetInstanceBuffer() << std::endl;
#endif

    // Content-epoch bump BEFORE the per-array uploads: any slot flush this
    // frame advances the generation the elision gates key on (the epoch quiets
    // only once the whole slot ring converged — see GetContentEpoch).
    if (IsDirty())
        ++m_ContentEpoch;

    // Upload to the current frame slot only; the other slots retain their own
    // data and converge via their own dirty-instance bits.
    if (m_InstancesDirtySlots[m_FrameSlot])
    {
        const GraphicsAPI api = m_Device->GetAPI();
        // ConvertMatrixForShader is the identity on Vulkan/Metal (column-major
        // matrices upload as-is) — upload straight from m_Instances and pay
        // the transpose scratch copy only on DirectX 12.
        const bool needsConvert = (api == GraphicsAPI::DirectX12);
        std::vector<GPUInstance> convertScratch;
        m_InstanceUploadRanges.clear();
        m_ScatterHotUploadRanges.clear();
        uint64_t uploadedBytes = 0;

        auto uploadRange = [&](uint32_t firstInstance, uint32_t instanceCount)
        {
            if (instanceCount == 0)
                return;
            const size_t offset = static_cast<size_t>(firstInstance) * sizeof(GPUInstance);
            const size_t bytes = static_cast<size_t>(instanceCount) * sizeof(GPUInstance);
            const GPUInstance* src = m_Instances.data() + firstInstance;
            if (needsConvert)
            {
                convertScratch.assign(src, src + instanceCount);
                for (auto& gpuInstance : convertScratch)
                {
                    gpuInstance.transform = ConvertMatrixForShader(gpuInstance.transform, api);
                    gpuInstance.prevTransform = ConvertMatrixForShader(gpuInstance.prevTransform, api);
                }
                src = convertScratch.data();
                // Scratch is reused by the next range; consume converted
                // bytes immediately on the backend that needs transposition.
                m_Device->UpdateBuffer(GetInstanceBuffer(), offset, bytes, src);
            }
            else
                m_InstanceUploadRanges.push_back({offset, bytes, src});
            uploadedBytes += bytes;

            // Mirror the SAME run into the compact scatter-hot buffer. It never
            // needs the DX12 matrix transpose (no matrices), so it uploads
            // straight from m_ScatterHot. Uploading on identical runs keeps the
            // mirror's per-frame bytes at exactly 32/240 of the instance bytes.
            if (m_ScatterHotEnabled)
            {
                const size_t hotOffset =
                    static_cast<size_t>(firstInstance) * sizeof(GPUInstanceScatterHot);
                const size_t hotBytes =
                    static_cast<size_t>(instanceCount) * sizeof(GPUInstanceScatterHot);
                m_ScatterHotUploadRanges.push_back(
                    {hotOffset, hotBytes, m_ScatterHot.data() + firstInstance});
                uploadedBytes += hotBytes;
            }
        };

        const uint32_t instanceTotal = static_cast<uint32_t>(
            std::min(m_Instances.size(), static_cast<size_t>(m_MaxInstances)));
        if (instanceTotal < m_Instances.size() && !m_InstanceCapacityErrorLogged)
        {
            m_InstanceCapacityErrorLogged = true;
            Logger::Log::Error(
                "GPUScene: {} instances exceed the declared capacity of {}; rows {}..{} will not "
                "reach the GPU. Raise maxInstances in GPUScene::Initialize.",
                m_Instances.size(), m_MaxInstances, m_MaxInstances, m_Instances.size() - 1);
        }
        if (m_InstanceDirtyAllSlots[m_FrameSlot])
        {
            uploadRange(0u, instanceTotal);
        }
        else
        {
            auto& words = m_InstanceDirtyWords[m_FrameSlot];
            std::sort(words.begin(), words.end());
            uint32_t first = 0;
            uint32_t end = 0;
            for (uint32_t word : words)
            {
                uint64_t mask = m_InstanceDirtyBits[m_FrameSlot][word];
                while (mask != 0)
                {
                    const uint32_t bit = static_cast<uint32_t>(std::countr_zero(mask));
                    const uint32_t count = static_cast<uint32_t>(std::countr_one(mask >> bit));
                    const uint32_t next = word * 64u + bit;
                    if (next >= instanceTotal)
                        break; // Trimmed or over-capacity rows have no upload.
                    if (next != end)
                    {
                        uploadRange(first, end - first);
                        first = next;
                    }
                    end = next + std::min(count, instanceTotal - next);
                    const uint32_t consumed = bit + count;
                    mask = consumed == 64u ? 0ull : mask & (~0ull << consumed);
                }
            }
            uploadRange(first, end - first);
        }

        m_Device->UpdateBufferRanges(GetInstanceBuffer(), m_InstanceUploadRanges);
        if (m_ScatterHotEnabled)
            m_Device->UpdateBufferRanges(GetScatterHotBuffer(), m_ScatterHotUploadRanges);
        m_InstanceUploadBytesTotal.fetch_add(uploadedBytes, std::memory_order_relaxed);
        for (uint32_t word : m_InstanceDirtyWords[m_FrameSlot])
            m_InstanceDirtyBits[m_FrameSlot][word] = 0;
        m_InstanceDirtyWords[m_FrameSlot].clear();
        m_InstanceDirtyAllSlots[m_FrameSlot] = false;
        m_InstancesDirtySlots[m_FrameSlot] = false;
    }

    // The mesh GPU table holds exactly m_MaxMeshes rows (CreateBuffers sizes it
    // from the declared capacity), so the upload clamps to that row count. Rows
    // past the capacity never reach the GPU — reported once via the latched
    // error, never silently.
    if (m_MeshesDirtySlots[m_FrameSlot] && !m_Meshes.empty())
    {
        const size_t rowCount = std::min(m_Meshes.size(), static_cast<size_t>(m_MaxMeshes));
        if (rowCount < m_Meshes.size() && !m_MeshCapacityErrorLogged)
        {
            m_MeshCapacityErrorLogged = true;
            Logger::Log::Error(
                "GPUScene: {} meshes exceed the declared capacity of {}; rows {}..{} will not "
                "reach the GPU. Raise maxMeshes in GPUScene::Initialize.",
                m_Meshes.size(), m_MaxMeshes, m_MaxMeshes, m_Meshes.size() - 1);
        }
        if (rowCount > 0)
            m_Device->UpdateBuffer(GetMeshBuffer(), 0, rowCount * sizeof(GPUMesh), m_Meshes.data());
        m_MeshesDirtySlots[m_FrameSlot] = false;
    }
}

// Internal helper — load shaderpkg bytes once and cache on the GPUScene.
// Both the default N=1 PSO and every per-N cascade variant use the same
// SPIR-V module; only the specialization constant differs at pipeline-
// creation time. Returns false (after logging once) when the bytes are not
// yet available — callers must NOT latch their creation attempt in that
// case, so a loader environment that becomes ready later still recovers.
bool GPUScene::EnsureCullingShaderBytes()
{
    if (m_CullingShaderBytes)
        return true;

    std::string loadErr;
    std::vector<uint8_t> shaderBytes = LoadComputeStageBytes(
        "Shaders/frustum_culling.shaderpkg", m_Device->PreferredShaderSource(), g_CullingShaderLoader, &loadErr);
    if (shaderBytes.empty())
    {
        if (!m_CullingShaderLoadFailureLogged)
        {
            Logger::Log::Warning(
                "GPUScene: frustum_culling.shaderpkg unavailable ({}); GPU culling pipeline creation deferred",
                loadErr);
            m_CullingShaderLoadFailureLogged = true;
        }
        return false;
    }
    m_CullingShaderBytes = std::make_shared<const std::vector<uint8_t>>(std::move(shaderBytes));
    return true;
}

void GPUScene::CreateCullingPipeline()
{
    // Attempted lazily on first use. Pipeline creation is latched once the
    // shader bytes are in hand (a device-level create failure won't improve
    // on retry); a missing-bytes failure is NOT latched — see
    // EnsureCullingShaderBytes.
    if (m_CullingPipelineCreationAttempted)
    {
        return;
    }
    if (!EnsureCullingShaderBytes())
        return;
    m_CullingPipelineCreationAttempted = true;

    // Descriptor set layout matches frustum_culling.comp.
    DescriptorSetLayoutDesc cullingLayout = MakeCullingDescriptorSetLayout();

    ComputePipelineDesc cd{};
    cd.ComputeShader = m_CullingShaderBytes;
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(cullingLayout));
    cd.PushConstants.Size      = sizeof(CullingPushConstants);
    cd.PushConstants.StageMask = kShaderStageCompute;
    cd.DebugName = "GPUCullingPipeline.kViewCount1";

    // Default GPUScene culling pipeline is the single-view (N=1) variant.
    // Cascade-group variants (N=2..4) are lazily compiled via
    // GetOrCreateCascadeCullingPipeline.
    SpecializationConstants specConstants;
    constexpr uint32_t kViewCountConstantId = 0u;
    specConstants.AddConstant<uint32_t>(kViewCountConstantId, 1u, "kViewCount");
    cd.Specialization = std::move(specConstants);
    const auto id = m_Device->InternComputePipeline(cd);
    m_CullingPipeline = m_Device->GetOrCreateComputePipeline(id);

    if (!m_CullingPipeline.IsValid())
    {
        Logger::Log::Error("GPUScene: failed to create GPU culling pipeline");
    }
}

PipelineHandle GPUScene::GetOrCreateCascadeCullingPipeline(uint32_t viewCount)
{
    if (viewCount < 2u || viewCount > kMaxViewsPerCullingDispatch)
        return INVALID_PIPELINE_HANDLE;

    if (m_CascadeCullingPipelines[viewCount].IsValid())
        return m_CascadeCullingPipelines[viewCount];
    if (m_CascadeCullingPipelineAttempted[viewCount])
        return INVALID_PIPELINE_HANDLE;

    // Missing bytes are not latched — retried on a later call once the
    // loader environment is ready (EnsureCullingShaderBytes logs once).
    if (!EnsureCullingShaderBytes())
        return INVALID_PIPELINE_HANDLE;
    m_CascadeCullingPipelineAttempted[viewCount] = true;

    DescriptorSetLayoutDesc cullingLayout = MakeCullingDescriptorSetLayout();

    ComputePipelineDesc cd{};
    cd.ComputeShader = m_CullingShaderBytes;
    cd.DescriptorSetLayouts.push_back(m_Device->InternDescriptorSetLayout(cullingLayout));
    cd.PushConstants.Size      = sizeof(CullingPushConstants);
    cd.PushConstants.StageMask = kShaderStageCompute;
    std::string dbgName = "GPUCullingPipeline.kViewCount" + std::to_string(viewCount);
    cd.DebugName = dbgName;

    SpecializationConstants specConstants;
    constexpr uint32_t kViewCountConstantId = 0u;
    specConstants.AddConstant<uint32_t>(kViewCountConstantId, viewCount, "kViewCount");
    cd.Specialization = std::move(specConstants);

    const auto id = m_Device->InternComputePipeline(cd);
    m_CascadeCullingPipelines[viewCount] = m_Device->GetOrCreateComputePipeline(id);

    if (m_CascadeCullingPipelines[viewCount].IsValid())
    {
        Logger::Log::Info("GPUScene: compiled cascade culling PSO N={}", viewCount);
    }
    else
    {
        Logger::Log::Error("GPUScene: FAILED to compile cascade culling PSO N={}", viewCount);
    }
    return m_CascadeCullingPipelines[viewCount];
}

// Note: HZB generation and ownership live under GPUCullingPipeline. GPUScene
// does not create or manage an HZB compute pipeline; it only provides the
// scene data (instances, meshes) consumed by the culling/HZB
// pipeline.

uint32_t GPUScene::AllocateInstanceSlot()
{
    if (!m_FreeInstanceSlots.empty())
    {
        uint32_t slot = m_FreeInstanceSlots.back();
        m_FreeInstanceSlots.pop_back();
        if (slot < m_InstanceSlotFree.size())
            m_InstanceSlotFree[slot] = 0u; // reused → no longer free
        return slot;
    }
    return static_cast<uint32_t>(m_Instances.size());
}

uint32_t GPUScene::AllocateMeshSlot()
{
    if (!m_FreeMeshSlots.empty())
    {
        uint32_t slot = m_FreeMeshSlots.back();
        m_FreeMeshSlots.pop_back();
        return slot;
    }
    return static_cast<uint32_t>(m_Meshes.size());
}

bool GPUScene::GetInstancesWorldBounds(Vector3& outMin, Vector3& outMax) const
{
    if (m_InstancesAABBDirty)
    {
        m_InstancesAABBDirty = false;

        if (m_InstanceCount == 0u)
        {
            const float kInf = std::numeric_limits<float>::infinity();
            m_InstancesAABBMin = Vector3{kInf, kInf, kInf};
            m_InstancesAABBMax = Vector3{-kInf, -kInf, -kInf};
        }
        else
        {
            // Free-list slots hold stale instance data (boundingRadius is
            // not zeroed on RemoveInstance). Skip them so the AABB only
            // bounds genuinely live geometry.
            std::unordered_set<uint32_t> freeSet(
                m_FreeInstanceSlots.begin(), m_FreeInstanceSlots.end());

            const float kInf = std::numeric_limits<float>::infinity();
            Vector3 mn{kInf, kInf, kInf};
            Vector3 mx{-kInf, -kInf, -kInf};
            for (uint32_t i = 0; i < m_Instances.size(); ++i)
            {
                if (freeSet.find(i) != freeSet.end())
                    continue;
                const GPUInstance& inst = m_Instances[i];
                const float r = inst.boundingRadius;
                if (r <= 0.0f)
                    continue;
                mn.x = std::min(mn.x, inst.boundingCenter.x - r);
                mn.y = std::min(mn.y, inst.boundingCenter.y - r);
                mn.z = std::min(mn.z, inst.boundingCenter.z - r);
                mx.x = std::max(mx.x, inst.boundingCenter.x + r);
                mx.y = std::max(mx.y, inst.boundingCenter.y + r);
                mx.z = std::max(mx.z, inst.boundingCenter.z + r);
            }
            m_InstancesAABBMin = mn;
            m_InstancesAABBMax = mx;
        }
    }

    if (m_InstancesAABBMin.x > m_InstancesAABBMax.x)
        return false; // empty / all-zombie scene
    outMin = m_InstancesAABBMin;
    outMax = m_InstancesAABBMax;
    return true;
}

void GPUScene::FreeInstanceSlot(uint32_t slot)
{
    m_FreeInstanceSlots.push_back(slot);
    if (slot < m_InstanceSlotFree.size())
        m_InstanceSlotFree[slot] = 1u;
}

void GPUScene::FreeMeshSlot(uint32_t slot)
{
    m_FreeMeshSlots.push_back(slot);
}

// HZB-specific shader creation helpers were removed when HZB ownership moved
// under GPUCullingPipeline. GPUScene no longer constructs an HZB compute
// pipeline directly.

// GPUSceneFactory implementation
std::unique_ptr<GPUScene> GPUSceneFactory::CreateLargeScene(IDevice* device)
{
    auto scene = std::make_unique<GPUScene>(device);
    scene->Initialize(1000000, 100000); // 1M instances, 100K meshes
    scene->SetDebugName("LargeGPUScene");
    return scene;
}

std::unique_ptr<GPUScene> GPUSceneFactory::CreateMediumScene(IDevice* device)
{
    auto scene = std::make_unique<GPUScene>(device);
    scene->Initialize(100000, 10000); // 100K instances, 10K meshes
    scene->SetDebugName("MediumGPUScene");
    return scene;
}

std::unique_ptr<GPUScene> GPUSceneFactory::CreateSmallScene(IDevice* device)
{
    auto scene = std::make_unique<GPUScene>(device);
    scene->Initialize(10000, 1000); // 10K instances, 1K meshes
    scene->SetDebugName("SmallGPUScene");
    return scene;
}

std::unique_ptr<GPUScene> GPUSceneFactory::CreateCustomScene(IDevice* device,
                                                             uint32_t maxInstances,
                                                             uint32_t maxMeshes)
{
    auto scene = std::make_unique<GPUScene>(device);
    scene->Initialize(maxInstances, maxMeshes);
    scene->SetDebugName("CustomGPUScene");
    return scene;
}

} // namespace Rendering
} // namespace GameEngine
