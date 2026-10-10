#include "ECSModules/Rendering/SkeletonStore.h"
#include <cassert>

// SkeletonData methods now live in
// Engine/Modules/Animation/Source/SkeletonData.cpp; Engine links the
// Animation static lib publicly so the symbols still resolve here.

namespace GameEngine { namespace Engine { namespace Renderer {

// --- SkeletonStore ---

SkeletonStore& SkeletonStore::Instance() {
    static SkeletonStore g_instance;
    return g_instance;
}

uint32 SkeletonStore::CreateSkeleton(uint32 boneCount) {
    std::lock_guard lock(m_WriteMutex);
    SkeletonData data{};
    data.BoneCount = boneCount;
    data.Parent.resize(boneCount, -1);
    data.BindPose.resize(static_cast<size_t>(boneCount) * 16, 0.0f);
    data.InverseBind.resize(static_cast<size_t>(boneCount) * 16, 0.0f);
    m_Skeletons.emplace_back(std::move(data));
    m_SkeletonCount.store(m_Skeletons.size(), std::memory_order_release);
    return static_cast<uint32>(m_Skeletons.size());
}

// Lock-free readers — safe during system updates.
// Atomic loads on the size counters pair with release-stores in writers.

SkeletonData* SkeletonStore::Get(uint32 skeletonId) {
    if (skeletonId == 0) return nullptr;
    size_t idx = static_cast<size_t>(skeletonId - 1);
    if (idx >= m_SkeletonCount.load(std::memory_order_acquire)) return nullptr;
    return &m_Skeletons[idx];
}

const SkeletonData* SkeletonStore::Get(uint32 skeletonId) const {
    if (skeletonId == 0) return nullptr;
    size_t idx = static_cast<size_t>(skeletonId - 1);
    if (idx >= m_SkeletonCount.load(std::memory_order_acquire)) return nullptr;
    return &m_Skeletons[idx];
}

uint32 SkeletonStore::GetCount() const {
    return static_cast<uint32>(m_SkeletonCount.load(std::memory_order_acquire));
}

void SkeletonStore::EnsureSizes(uint32 skeletonId, uint32 boneCount) {
    std::lock_guard lock(m_WriteMutex);
    if (skeletonId == 0) return;
    size_t idx = static_cast<size_t>(skeletonId - 1);
    if (idx >= m_Skeletons.size()) return;
    auto* sk = &m_Skeletons[idx];
    if (sk->BoneCount != boneCount) {
        sk->BoneCount = boneCount;
        sk->Parent.resize(boneCount, -1);
        sk->BindPose.resize(static_cast<size_t>(boneCount) * 16, 0.0f);
        sk->InverseBind.resize(static_cast<size_t>(boneCount) * 16, 0.0f);
    }
}

// --- Per-instance runtime ---

static void InitIdentityPalette(SkeletonRuntimeState& state, uint32 jointCount)
{
    state.CompactSkinMatrices.assign(static_cast<size_t>(jointCount) * 16, 0.0f);
    for (uint32 i = 0; i < jointCount; ++i)
    {
        const size_t base = static_cast<size_t>(i) * 16;
        state.CompactSkinMatrices[base + 0] = 1.0f;
        state.CompactSkinMatrices[base + 5] = 1.0f;
        state.CompactSkinMatrices[base + 10] = 1.0f;
        state.CompactSkinMatrices[base + 15] = 1.0f;
    }
    state.AtlasPaletteOffsetBones = 0;
}

uint32 SkeletonStore::CreateRuntime(uint32 skeletonId)
{
    std::lock_guard lock(m_WriteMutex);

    if (skeletonId == 0) return 0;
    size_t skelIdx = static_cast<size_t>(skeletonId - 1);
    if (skelIdx >= m_Skeletons.size()) return 0;
    auto* skel = &m_Skeletons[skelIdx];

    const uint64 generation = m_RuntimeCreateEpoch.fetch_add(1, std::memory_order_relaxed) + 1;

    const uint32 jointCount = (skel->SkinJointCount > 0) ? skel->SkinJointCount : skel->BoneCount;

    // Reuse a freed slot if available.
    if (!m_FreeRuntimeIds.empty())
    {
        uint32 reusedId = m_FreeRuntimeIds.back();
        auto& entry = m_Runtimes[static_cast<size_t>(reusedId - 1)];
        if (jointCount > 0)
            InitIdentityPalette(entry.State, jointCount);
        entry.State.Source = {};
        entry.Generation = generation;
        m_FreeRuntimeIds.pop_back();
        entry.RefCount.store(1, std::memory_order_relaxed);
        // Publish SkeletonId AFTER state is initialized so readers see valid data.
        entry.SkeletonId.store(skeletonId, std::memory_order_release);
        return reusedId;
    }

    RuntimeEntry entry{};
    entry.Generation = generation;
    entry.SkeletonId.store(skeletonId, std::memory_order_relaxed); // not yet visible to readers
    entry.RefCount.store(1, std::memory_order_relaxed);
    if (jointCount > 0)
        InitIdentityPalette(entry.State, jointCount);

    m_Runtimes.emplace_back(std::move(entry));
    m_RuntimeCount.store(m_Runtimes.size(), std::memory_order_release);
    return static_cast<uint32>(m_Runtimes.size()); // 1-based
}

// Lock-free runtime readers.

SkeletonRuntimeState* SkeletonStore::GetRuntime(uint32 runtimeId)
{
    if (runtimeId == 0) return nullptr;
    const size_t idx = static_cast<size_t>(runtimeId - 1);
    if (idx >= m_RuntimeCount.load(std::memory_order_acquire)) return nullptr;
    if (m_Runtimes[idx].SkeletonId.load(std::memory_order_acquire) == 0) return nullptr; // freed
    return &m_Runtimes[idx].State;
}

const SkeletonRuntimeState* SkeletonStore::GetRuntime(uint32 runtimeId) const
{
    if (runtimeId == 0) return nullptr;
    const size_t idx = static_cast<size_t>(runtimeId - 1);
    if (idx >= m_RuntimeCount.load(std::memory_order_acquire)) return nullptr;
    if (m_Runtimes[idx].SkeletonId.load(std::memory_order_acquire) == 0) return nullptr; // freed
    return &m_Runtimes[idx].State;
}

uint32 SkeletonStore::GetRuntimeSkeletonId(uint32 runtimeId) const
{
    if (runtimeId == 0) return 0;
    const size_t idx = static_cast<size_t>(runtimeId - 1);
    if (idx >= m_RuntimeCount.load(std::memory_order_acquire)) return 0;
    return m_Runtimes[idx].SkeletonId.load(std::memory_order_acquire);
}

size_t SkeletonStore::GetRuntimeHighWaterMark() const
{
    return m_RuntimeCount.load(std::memory_order_acquire);
}

uint32 SkeletonStore::GetRuntimeRefCount(uint32 runtimeId) const
{
    if (runtimeId == 0) return 0;
    const size_t idx = static_cast<size_t>(runtimeId - 1);
    if (idx >= m_RuntimeCount.load(std::memory_order_acquire)) return 0;
    if (m_Runtimes[idx].SkeletonId.load(std::memory_order_acquire) == 0) return 0;
    return m_Runtimes[idx].RefCount.load(std::memory_order_acquire);
}

uint64 SkeletonStore::GetRuntimeGeneration(uint32 runtimeId) const
{
    if (runtimeId == 0) return 0;
    const size_t idx = static_cast<size_t>(runtimeId - 1);
    if (idx >= m_RuntimeCount.load(std::memory_order_acquire)) return 0;
    if (m_Runtimes[idx].SkeletonId.load(std::memory_order_acquire) == 0) return 0;
    return m_Runtimes[idx].Generation;
}

bool SkeletonStore::RetainRuntime(uint32 runtimeId)
{
    std::lock_guard lock(m_WriteMutex);
    if (runtimeId == 0) return false;
    const size_t idx = static_cast<size_t>(runtimeId - 1);
    if (idx >= m_Runtimes.size()) return false;

    auto& entry = m_Runtimes[idx];
    if (entry.SkeletonId.load(std::memory_order_relaxed) == 0) return false; // free slot

    entry.RefCount.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void SkeletonStore::ReleaseRuntime(uint32 runtimeId)
{
    std::lock_guard lock(m_WriteMutex);
    if (runtimeId == 0) return;
    const size_t idx = static_cast<size_t>(runtimeId - 1);
    if (idx >= m_Runtimes.size()) return;

    auto& entry = m_Runtimes[idx];
    if (entry.SkeletonId.load(std::memory_order_relaxed) == 0) return; // already freed

    // Invariant: a live slot (SkeletonId != 0) always holds at least one
    // reference, so the subtraction below cannot wrap.
    const uint32 refs = entry.RefCount.load(std::memory_order_relaxed);
    if (refs > 1)
    {
        entry.RefCount.store(refs - 1, std::memory_order_relaxed);
        return; // a sibling SkeletonRef still names this runtime
    }
    entry.RefCount.store(0, std::memory_order_relaxed);

    // Mark as freed FIRST (release) so lock-free readers see 0 before
    // we deallocate the state. Readers acquire-load SkeletonId and bail
    // if 0, so they never touch the cleared CompactSkinMatrices.
    entry.SkeletonId.store(0, std::memory_order_release);
    entry.State.CompactSkinMatrices.clear();
    entry.State.CompactSkinMatrices.shrink_to_fit();
    entry.State.AtlasPaletteOffsetBones = 0;
    // A recycled slot must not hand its next occupant this one's pose history.
    entry.State.PrevAtlasPaletteOffsetBones = 0;
    entry.State.PrevAtlasPaletteValid = false;
    entry.State.AtlasPaletteWrittenThisFrame = false;
    entry.State.Source = {};
    m_FreeRuntimeIds.push_back(runtimeId);
}

void SkeletonStore::RollPaletteOffsets()
{
    // Lock-free like the other per-frame readers: this touches only the
    // per-entry palette scalars, and structural modification is serialized
    // against system ticks by the same contract GetRuntime() relies on.
    const size_t count = m_RuntimeCount.load(std::memory_order_acquire);
    for (size_t i = 0; i < count; ++i)
    {
        auto& entry = m_Runtimes[i];
        if (entry.SkeletonId.load(std::memory_order_acquire) == 0)
            continue; // freed slot
        auto& state = entry.State;
        state.PrevAtlasPaletteValid = state.AtlasPaletteWrittenThisFrame;
        state.PrevAtlasPaletteOffsetBones = state.AtlasPaletteOffsetBones;
        state.AtlasPaletteWrittenThisFrame = false;
    }
}

}}} // namespace GameEngine::Engine::Renderer
