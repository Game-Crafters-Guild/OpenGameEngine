#pragma once

#include "Animation/SkeletonData.h"
#include "AssetCore/GUID.h"
#include "ECS/ECS.h"
#include "Types/Types.h"
#include <atomic>
#include <deque>
#include <mutex>
#include <vector>
#include <memory>

namespace GameEngine { namespace Engine { namespace Renderer {

// Phase 0b shim: SkeletonData was moved to GameEngine::Animation. The legacy
// alias keeps every existing callsite (`Engine::Renderer::SkeletonData`)
// compiling unchanged. New code should use `GameEngine::Animation::SkeletonData`.
using SkeletonData = ::GameEngine::Animation::SkeletonData;

// Reconstruction identity of one model instance, shared by its submeshes and
// helper nodes. Empty ModelGuid denotes an explicitly assigned procedural rig.
// Set during construction, before publishing SkeletonRefs; not scene data.
struct SkeletonRuntimeSource {
    GUID ModelGuid{};
    ECS::EntityHandle InstanceOwner{};
    uint64 WorldId = 0;
    uint64 WorldGeneration = 0;
    bool operator==(const SkeletonRuntimeSource&) const = default;
};

// Per-entity runtime skinning state. Each model instance (entity or group of
// submesh entities) gets its own copy of the bone palette and atlas offset so
// multiple instances of the same skeleton can animate independently.
struct SkeletonRuntimeState {
    SkeletonRuntimeSource Source;
    std::vector<float> CompactSkinMatrices;     // size = SkinJointCount * 16 (model-relative, column-major)
    // Offset (in bone-slot units; 1 slot = 3 vec4 rows in the mat3x4
    // packed atlas — see Engine/Include/Engine/Rendering/
    // BonePaletteLayout.h) into the SkinPaletteAtlas SSBO.
    // Producer: SkinningUploadSystem (CPU eval) or
    // HumanoidRetargetSystem / AnimationSystem (GPU eval reserves the slot).
    // Consumer: RenderExtractionSystem copies into GPUInstance::skinPaletteOffset.
    // Historically named AtlasPaletteOffsetMat4s when each bone occupied
    // a full mat4; the unit semantics (1 slot per bone) are preserved
    // across the layout change.
    // Publish through SetAtlasPaletteOffset so the previous-frame rollover
    // below cannot miss a producer.
    uint32 AtlasPaletteOffsetBones = 0;

    // Where this runtime's palette sat in LAST frame's atlas. Consumed by the
    // TAA skinned motion-vector pass, which needs both endpoints of the pose
    // to compute per-vertex motion. Meaningful only while
    // PrevAtlasPaletteValid is set: a runtime that did not publish an offset
    // last frame (spawn frame, animation just started, allocation failed) has
    // no previous pose, and extraction then reports the explicit
    // RenderServices::kNoPreviousSkinPalette so the consumer evaluates both
    // endpoints at the current pose — zero pose motion, never garbage.
    uint32 PrevAtlasPaletteOffsetBones = 0;
    bool PrevAtlasPaletteValid = false;

    // Set by SetAtlasPaletteOffset; consumed and cleared by
    // SkeletonStore::RollPaletteOffsets at the top of the next frame.
    bool AtlasPaletteWrittenThisFrame = false;

    // The one way to publish a palette offset. Stamping and assignment are
    // fused so a new producer cannot silently opt out of motion vectors.
    void SetAtlasPaletteOffset(uint32 offsetInBones)
    {
        AtlasPaletteOffsetBones = offsetInBones;
        AtlasPaletteWrittenThisFrame = true;
    }
};

// Simple singleton store for skeletons used by lightweight ECS components (Option 2).
class SkeletonStore {
public:
    static SkeletonStore& Instance();

    // Create a skeleton entry with a given bone count; returns skeletonId (index+1).
    // ID 0 is reserved for invalid.
    uint32 CreateSkeleton(uint32 boneCount);

    // Accessors
    SkeletonData* Get(uint32 skeletonId);
    const SkeletonData* Get(uint32 skeletonId) const;
    uint32 GetCount() const;

    void EnsureSizes(uint32 skeletonId, uint32 boneCount);

    // --- Per-instance runtime allocation ---
    // Each model instance gets its own runtime so that multiple instances of
    // the same skeleton animate independently. One instance is often several
    // entities (submeshes and imported bone nodes all carry a SkeletonRef
    // naming the same runtime), so runtimes are reference counted.
    //
    // Ownership rule: every live SkeletonRef component whose runtimeId != 0
    // holds exactly one reference. Whoever writes such a component takes a
    // reference; whoever destroys or re-keys one drops it. The slot is
    // recycled only when the last reference goes, so a surviving sibling can
    // never have its runtime reissued out from under it.

    // Allocate a runtime sized to the skeleton's joint count and return its
    // runtimeId (1-based; 0 = invalid) holding ONE reference owned by the
    // caller. Hand that reference to a SkeletonRef, or release it.
    uint32 CreateRuntime(uint32 skeletonId);

    // Take an additional reference. Returns false only when runtimeId names no
    // live runtime at all. It cannot distinguish the runtime a caller meant
    // from a different one already reissued into the same slot, so it is not a
    // validity test for a stored id: a caller that must survive a window in
    // which the slot could be recycled takes its reference BEFORE the window
    // opens rather than testing the id afterwards.
    bool RetainRuntime(uint32 runtimeId);

    // Drop one reference. The slot is cleared and recycled when the last one
    // goes; releasing an already-free id is a no-op.
    void ReleaseRuntime(uint32 runtimeId);

    // Live reference count; 0 for a free or out-of-range slot. Lock-free.
    uint32 GetRuntimeRefCount(uint32 runtimeId) const;

    // Allocation identity, unlike the recyclable slot index. Zero means absent.
    // Model-backed SkeletonRefs retain this stamp so stale snapshots cannot
    // release a different instance that was issued the same numeric runtimeId.
    uint64 GetRuntimeGeneration(uint32 runtimeId) const;

    // Monotonic count of CreateRuntime calls (new or free-list reuse). The
    // skinning visibility gate reads this to detect spawn frames: a just-
    // created runtime's GPU visibility word is one frame stale, so the gate
    // must not trust it that frame.
    uint64 GetRuntimeCreateEpoch() const
    {
        return m_RuntimeCreateEpoch.load(std::memory_order_relaxed);
    }

    // Look up a per-entity runtime by runtimeId. Lock-free — safe to call
    // from any thread during system updates. Relies on deque pointer stability
    // and atomic size counters; structural modifications (Create/Free) are
    // serialized separately and must not overlap with system ticks.
    SkeletonRuntimeState* GetRuntime(uint32 runtimeId);
    const SkeletonRuntimeState* GetRuntime(uint32 runtimeId) const;

    // Which skeleton does this runtime belong to? Lock-free.
    uint32 GetRuntimeSkeletonId(uint32 runtimeId) const;

    // High-water mark of allocated runtime slots (1-based; iterate 1..count).
    // Slots may be freed; combine with GetRuntimeSkeletonId(id) != 0 to skip
    // freed entries. Lock-free.
    size_t GetRuntimeHighWaterMark() const;

    // Age this frame's palette offsets into the previous-frame fields. Call
    // once per frame from the render loop, after the atlas has begun its new
    // frame and BEFORE any producer publishes an offset: at that instant
    // AtlasPaletteOffsetBones still names last frame's atlas slot, which is
    // exactly what the TAA skinned motion-vector pass needs.
    //
    // A runtime that published no offset last frame gets PrevAtlasPaletteValid
    // cleared, so its consumer is told there is no history instead of being
    // pointed at a slot another runtime now owns.
    void RollPaletteOffsets();

private:
    SkeletonStore() = default;

    // Writer mutex — only held by structural modifications
    // (Create/Retain/Release/EnsureSizes). Reader hot paths (Get/GetRuntime/
    // GetRuntimeSkeletonId/GetRuntimeRefCount) are lock-free, using atomic
    // size counters and deque pointer stability.
    std::mutex m_WriteMutex;

    std::deque<SkeletonData> m_Skeletons;                  // index = id-1; deque for pointer stability
    std::atomic<size_t> m_SkeletonCount{0};               // published after emplace_back

    // Per-entity runtime state, decoupled from SkeletonData.
    struct RuntimeEntry {
        std::atomic<uint32> SkeletonId{0}; // 0 when free; atomic for lock-free reader safety
        // Number of live SkeletonRef components naming this slot. Mutated only
        // under m_WriteMutex; atomic so GetRuntimeRefCount can stay lock-free
        // alongside the other runtime readers.
        std::atomic<uint32> RefCount{0};
        uint64 Generation = 0;
        SkeletonRuntimeState State;

        RuntimeEntry() = default;
        RuntimeEntry(RuntimeEntry&& o) noexcept
            : SkeletonId(o.SkeletonId.load(std::memory_order_relaxed))
            , RefCount(o.RefCount.load(std::memory_order_relaxed))
            , Generation(o.Generation)
            , State(std::move(o.State)) {}
        RuntimeEntry& operator=(RuntimeEntry&& o) noexcept {
            SkeletonId.store(o.SkeletonId.load(std::memory_order_relaxed), std::memory_order_relaxed);
            RefCount.store(o.RefCount.load(std::memory_order_relaxed), std::memory_order_relaxed);
            Generation = o.Generation;
            State = std::move(o.State);
            return *this;
        }
    };
    std::deque<RuntimeEntry> m_Runtimes;                  // index = runtimeId-1; deque for pointer stability
    std::atomic<size_t> m_RuntimeCount{0};                // published after emplace_back
    std::vector<uint32> m_FreeRuntimeIds;                 // recycled runtimeIds available for reuse
    std::atomic<uint64> m_RuntimeCreateEpoch{0};          // see GetRuntimeCreateEpoch()
};

}}} // namespace GameEngine::Engine::Renderer
