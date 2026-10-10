// WorldDrawBuilder: per-view batch-key derivation for the GPU-driven draw path.

#pragma once

#include "Engine/Rendering/MeshGPURegistry.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/CameraTypes.h"
#include "Types/FalseSharing.h"
#include "Types/Types.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
namespace Engine::Renderer
{
class Material;

// Engine-level helper that consumes WorldSubmissionRecord batches from
// extraction systems and derives per-view sets of unique (Material*, mesh,
// materialIndex, meshIndex) batch keys for the GPU-driven indirect-draw
// path. The bucketer dispatch scheduler reads the keys to schedule one
// compute pass per (view, batch); the world color / depth-pass execute
// lambdas iterate the same keys and issue one DrawIndexedIndirectCount per
// batch, consuming the bucketer-written indirection slot for the per-batch
// firstInstance->GPUScene-index mapping.
//
// Ownership and lifetime are managed by RenderServices; this type is
// exposed so systems can reason about behavior in tests and advanced
// integrations.
class WorldDrawBuilder
{
  public:
    // Called once per frame on the render thread before any submissions.
    void BeginFrame();

    // Submit a batch of records. Thread-safe: multiple ECS systems and jobs
    // can call this concurrently from different threads. Each thread gets its
    // own submission buffer (no contention). Call MergeSubmissions() on the
    // render thread after all submissions are done.
    void Submit(std::span<const WorldSubmissionRecord> records);

    // Merge per-thread submission buffers into the per-view map. Must be called
    // on the render thread after all parallel submissions are done and before
    // BuildBatchKeys.
    void MergeSubmissions();

    // Derive per-view batch-key sets from the merged submissions. Calls
    // MergeSubmissions() internally. The mesh registry is required so
    // submissions whose mesh entry isn't resident yet (async upload race)
    // are filtered out before the unique sort. `materialColorClass` (indexed
    // by materialIndex) is the P2 color-class map: when non-empty, keys are
    // grouped/deduped on (colorClassId, meshIndex) so same-PSO opaque
    // materials share one batch; an empty span keeps the (materialIndex,
    // meshIndex) key identity (colorClassId == materialIndex), today's path.
    // Returns the number of views whose keys were (re)derived this call — the
    // A2 STEP-0 sort-bracket view count.
    uint32_t BuildBatchKeys(const Rendering::MeshGPURegistry& meshRegistry,
                            std::span<const uint32_t> materialColorClass = {});

    // Build batch keys for a single view without touching other views. Used
    // by editor-driven one-shot views (e.g. thumbnail renders) so a late
    // thumbnail submission doesn't clobber Scene/Game view state mid-frame.
    void BuildBatchKeysForView(Rendering::ViewId viewId,
                               const Rendering::MeshGPURegistry& meshRegistry,
                               std::span<const uint32_t> materialColorClass = {});

    // Unique (material, mesh, materialIndex, meshIndex) keys per view. The
    // bucketer dispatch scheduler and the world/depth-pass execute lambdas
    // both consume this set — one bucketer dispatch + one indirect draw per
    // entry. The (materialIndex, meshIndex) pair is the streamKey identity;
    // the (Material*, MeshGPUHandle) pair gives the execute lambdas the
    // resources they need to bind without an extra registry lookup.
    struct BatchKey
    {
        const Material* material;
        Rendering::MeshGPUHandle mesh;
        uint32_t materialIndex;
        uint32_t meshIndex;
        // P2 color-class id (== materialIndex when the merge is off). The color
        // pass + main-view depth prepass look up their draw range by this;
        // shadow cascades substitute a shared-depth sentinel for eligible
        // casters and keep this id for the rest. ResolveDrawStreamLookupKey
        // owns that derivation — never key a range lookup off these fields
        // directly.
        uint32_t colorClassId;
    };
    std::span<const BatchKey> GetBatchKeys(Rendering::ViewId viewId) const;
    // The subset of GetBatchKeys whose material is in the deforming-motion
    // lane (MaterialDeformationClassify.h). Derived in the same pass as the
    // keys, so it costs one predicate per surviving key and carries identical
    // staleness; empty for a view with no deforming material, which is what
    // makes a frame with none pay one branch and no work. The producer arms
    // record from this span and the mover lane excludes exactly its members.
    std::span<const BatchKey> GetDeformingBatchKeys(Rendering::ViewId viewId) const;

    // True when this view has at least one submission with the cast-shadows
    // bit, drawable or not. Derived alongside the batch keys, so the shadow
    // gates that ask it several times per view per frame read one flag.
    bool HasShadowCastingSubmissions(Rendering::ViewId viewId) const;

    // True when this view has at least one drawable submission that is BOTH
    // transmissive and a shadow caster — the only way the glass-tint shadow
    // cascade can receive content, because the shadow cull drops non-casters
    // (frustum_culling.comp:110, hzb_culling.comp:173) before the tint pass's
    // indirect draw ever reads a stream. Derived alongside the batch keys, so it
    // costs one branch in a loop the frame already walks; a submissions scan of
    // its own would cost more than the cascade it gates.
    bool HasTransmissiveCasterSubmissions(Rendering::ViewId viewId) const;

    // True when this view has at least one submission whose material moves its
    // own vertices, and the shadow-casting subset of the same question. Such a
    // surface is displaced in the vertex stage from the shared animation clock,
    // so its rasterized depth differs from last frame's while every instance
    // record stays byte-identical — the depth-derived caches have no other
    // signal for it. Derived alongside the batch keys, the one place every
    // producer's records meet, so it costs one branch in a loop the frame
    // already walks.
    bool HasAnimatedVertexModifierSubmissions(Rendering::ViewId viewId) const;
    bool HasAnimatedVertexModifierCasterSubmissions(Rendering::ViewId viewId) const;

    // Per-instance merged submissions for a view (pre-dedup, unlike GetBatchKeys).
    // The sorted transparent path (T2) needs per-instance identity — the GPUScene
    // instanceIndex on each record — which the deduped batch keys discard, so it
    // reads this list, filters to Blend materials, and sorts by view distance.
    // Populated by MergeSubmissions (invoked from BuildBatchKeys) before pipeline
    // declaration. Empty span for a view with no submissions.
    std::span<const WorldSubmissionRecord> GetSubmissions(Rendering::ViewId viewId) const;

    // Clear submissions + batch keys for a single view. This is useful for
    // editor-driven one-shot views (e.g. thumbnail renders) so the view
    // doesn't keep re-rendering stale state across frames.
    void ClearView(Rendering::ViewId viewId);

    // Optional: clear all internal state without starting a new frame.
    void Clear();

  private:
    // Derive the unique (matIdx, mshIdx) batch-key set for a view from its
    // merged submissions. Shared by BuildBatchKeys + BuildBatchKeysForView.
    void DeriveBatchKeysForView(
        Rendering::ViewId viewId,
        const std::vector<WorldSubmissionRecord>& submissions,
        const Rendering::MeshGPURegistry& meshRegistry,
        std::span<const uint32_t> materialColorClass);

    // --- Per-thread submission buffers (thread-safe Submit) ---
    static constexpr uint32 kMaxSubmitThreads = 16;

    struct ThreadSubmissionBuffer
    {
        std::vector<WorldSubmissionRecord> records;
        uint32 drained = 0; // records.size() at the time of last MergeSubmissions drain
    };
    // Each slot is written by its own submitting thread, so each sits alone on
    // its false-sharing span.
    std::array<FalseSharingPadded<ThreadSubmissionBuffer>, kMaxSubmitThreads> m_ThreadBuffers;
    std::atomic<uint32> m_NextThreadSlot{0};
    static thread_local uint32 t_MySlot;

    // Per-view submission records built during extraction (populated by MergeSubmissions).
    std::unordered_map<Rendering::ViewId, std::vector<WorldSubmissionRecord>> m_SubmissionsByView;
    // Unique batch keys per view, derived from m_SubmissionsByView.
    std::unordered_map<Rendering::ViewId, std::vector<BatchKey>> m_BatchKeysByView;
    // Shadow-caster and transmissive-caster presence per view, derived in the
    // same pass as the batch keys and therefore carrying identical staleness.
    std::unordered_map<Rendering::ViewId, bool> m_ShadowCasterByView;
    std::unordered_map<Rendering::ViewId, bool> m_TransmissiveCasterByView;
    // Animated-vertex presence per view (any renderable, and the caster
    // subset), same derivation and therefore the same staleness.
    std::unordered_map<Rendering::ViewId, bool> m_AnimatedVertexModifierByView;
    std::unordered_map<Rendering::ViewId, bool> m_AnimatedVertexModifierCasterByView;
    // The deforming subset of m_BatchKeysByView, same derivation, same staleness.
    std::unordered_map<Rendering::ViewId, std::vector<BatchKey>> m_DeformingKeysByView;
};

} // namespace Engine::Renderer
} // namespace GameEngine
