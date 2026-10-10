#pragma once

#include "Particles/Rendering/ParticleExtraction.h"
#include "Types/ScopedSubscription.h"
#include "ECS/ChangeFilter.h"
#include "ECS/Systems.h"
#include "ECS/SwapGenerationGuard.h"
#include "ECS/World.h"
#include "Engine/Rendering/PointShadowAtlasPlanner.h" // ShadowCasterChangeSphere
#include "Engine/Rendering/PostProcessVolumeExtract.h"
#include "Engine/Rendering/DrawCommand.h"
#include "Engine/Rendering/DrawCommandProducer.h"
#include "Engine/Rendering/VolumetricFogTypes.h"
#include "Engine/Rendering/WorldDrawTypes.h"
#include "Rendering/Core/Device.h"
#include "AssetCore/GUID.h"
#include "Components/Rendering/MeshRenderer.h"
#include "Components/Rendering/LocalBounds.h"
#include "Components/Rendering/WorldSectorCoord.h"
#include "Components/Transform.h"
#include "Types/Types.h"
#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine { namespace Components { struct MeshGPUData; } }
namespace GameEngine { namespace Rendering { class GPUScene; class MeshGPURegistry; struct GPUInstance; } }

namespace GameEngine::Particles { struct ParticleWorldState; }

namespace GameEngine { namespace Engine::Renderer {

class RenderServices;
class MaterialRegistry;

// A2.1: parallel-extraction scratch state (index-aligned prepared instances +
// per-chunk accumulators). Defined in the .cpp so the GPUScene/GPUInstance
// dependency stays out of this public header. Allocated at construction.
struct RenderExtractionParallelState;

// Maps ECS entities and authoring components into GPUScene instances/materials/meshes
class RenderExtractionSystem : public ECS::ISystem {
public:
    explicit RenderExtractionSystem(RenderServices* renderServices);
    RenderExtractionSystem(RenderServices* renderServices, std::shared_ptr<Particles::ParticleWorldState> particles);
    // Test seam for the GE_EXTRACTION_FEED fast path (fusion S2b): the env
    // flag is a process-static latch, so unit tests select the lane here
    // instead of pinning per-fixture env vars (the flip arc's G1 lesson; the
    // TransformHierarchySystem precedent). The env latch only wires the
    // default through the other constructor.
    RenderExtractionSystem(RenderServices* renderServices, bool feedFastPathEnabled);
    ~RenderExtractionSystem() override;

    const char* GetName() const override { return "RenderExtractionSystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Per-entity record published by the extraction walk. Public so
    // downstream Extraction-phase systems (Scene TLAS, future CPU pre-
    // prune) can consume the same per-entity data without re-walking the
    // ECS query. Read-only after Update returns; consumers run later in
    // the Extraction phase via system-schedule dependency.
    struct WorldRenderableRecord
    {
        GameEngine::ECS::EntityHandle entity;
        GameEngine::Components::WorldTransform worldTransform;
        GameEngine::Components::MeshRenderer meshRenderer;
        // Components::GIEmitter presence, resolved once at gather time.
        bool giEmitter = false;
        bool hasBounds = false;
        GameEngine::Components::LocalBounds bounds{};
        // E.5.2: optional sector authoring. When hasSectorCoord is true, the
        // worldTransform.matrix is interpreted as sector-local and the true
        // world matrix is Translate(sectorCoord * sectorSize) * worldTransform.
        // When false, worldTransform is already in world-space (legacy path).
        bool hasSectorCoord = false;
        GameEngine::Components::WorldSectorCoord sectorCoord{};
        uint32 skeletonId = 0;      // Non-zero when entity has SkinnedMeshRenderer
        uint32 runtimeId = 0;       // Per-entity skeleton runtime for independent animation
        bool hasActiveAnimation = false; // True when entity has AnimatorRef with a valid clip
        float lodBias = 0.0f;       // LODGroup.Bias (log2 coverage scale for ge_SelectLOD)

        // Cached GPUScene-index component pointer, captured cache-coherently by
        // the gather query. Non-null after the entity's first extraction. Reading
        // this in the parallel prepare pass avoids the per-entity shared_lock in
        // World::GetComponent; it is only ever WRITTEN back on the serial apply
        // thread. Null means the entity has no MeshGPUData yet (first frame /
        // freshly spawned) — the serial apply adds it. The pointer is stable for
        // the frame: extraction's only structural change is adding MeshGPUData to
        // entities that lack it, which relocates only other lacking entities, not
        // the has-component entities whose pointers are cached here.
        GameEngine::Components::MeshGPUData* meshGpu = nullptr;
    };

    // Read-only view of the records as of the LAST FULL PASS. On dirty-feed
    // fast frames (GE_EXTRACTION_FEED) the vector is not regathered, so the
    // transforms in it are NOT per-frame fresh. Downstream extraction-phase
    // consumers deliberately re-walk their own queries instead of consuming
    // this: the editor constructs a second extraction instance for thumbnail
    // worlds, so a process-wide pointer to "the" extraction would read
    // another world's records.
    const std::vector<WorldRenderableRecord>& GetRecords() const { return m_Records; }

    // Test-only seam for the lane-parity locks (fusion T4): the per-view
    // submissions most recently handed to SubmitWorldSubmissions — freshly
    // built on full-lane frames, replayed verbatim from the cache on fast
    // frames.
    const std::vector<WorldSubmissionRecord>& GetSubmissionsForTest() const { return m_Submissions; }

    // Escalation reason bits published in RenderExtractionStats (fusion D7).
    // E-numbers per the extraction-fusion design §3.3; E9 is split out so
    // legitimate OnDemand view churn (probe rebakes, mirrors) is
    // distinguishable from real escalation storms in soak.
    enum EscalationReason : uint32
    {
        kEscalationFeedDisabled       = 1u << 0,  // E1: flag off / world unsubscribed
        kEscalationFeedOverflow       = 1u << 1,  // E2
        kEscalationMissedWindow       = 1u << 2,  // E3
        kEscalationStructuralChange   = 1u << 3,  // E4
        kEscalationProbeMeshRenderer  = 1u << 4,  // E5
        kEscalationProbeLocalBounds   = 1u << 5,  // E5
        kEscalationProbeSectorCoord   = 1u << 6,  // E5
        kEscalationProbeLodGroup      = 1u << 7,  // E5
        kEscalationMeshReload         = 1u << 8,  // E6
        kEscalationMaterialDigest     = 1u << 9,  // E7
        kEscalationPendingTransient   = 1u << 10, // E8
        kEscalationViewSet            = 1u << 11, // E9
        kEscalationSubsetOverBudget   = 1u << 13, // E10 (always-refresh subset)
        kEscalationCachesUnprimed     = 1u << 14, // E11
        kEscalationValidator          = 1u << 15, // D9 shadow validator (debug builds)
        kEscalationHlodResidency      = 1u << 16, // HLOD cluster flip: proxy/member residency changed
    };

    // Force the next extraction Update onto the full lane. The HLODSelectSystem
    // calls this when a cluster flips (proxy-enter/exit): eviction frees member
    // slots directly (batched RemoveInstances), but re-adding the winning set —
    // the proxy on enter, the members on exit — happens only in the full lane
    // (Op::Add on instanceIndex == 0xFFFFFFFF). A residency flip is not a
    // WorldTransform edit, so it never reaches the dirty feed on its own; this is
    // the explicit escalation, mirroring the E6 mesh-reload atomic. Defined in the
    // .cpp (routes through RenderServices, which is only forward-declared here).
    void RequestFullExtractionNextFrame();

private:
    RenderServices* m_RenderServices = nullptr; // non-owning

    void ExtractPostProcessVolumes(ECS::World& world, uint64 worldId, RenderServices* rs);

    // --- GE_EXTRACTION_FEED fast path (fusion S2b) ---
    // All-or-nothing per frame: every precondition E1-E11 is evaluated every
    // frame regardless of lane (triggers consumed, never latched stale — D3),
    // any failure runs the shipped full lane, which self-repairs everything
    // and re-primes every cache below. All mutable consumer state lives on
    // this system object (the image-split rule concerns inline globals, not
    // object members).
    bool EvaluateFastPathPreconditions(ECS::World& world, uint64 worldId, uint32& reasons);
    uint32 PatchDirtyInstances(ECS::World& world, Rendering::GPUScene& scene);
    uint32 RefreshAlwaysRefreshSubset(ECS::World& world, Rendering::GPUScene& scene,
                                      Rendering::MeshGPURegistry& meshReg,
                                      MaterialRegistry& matReg, uint64 worldId);
    void RebuildAlwaysRefreshSubset(ECS::World& world);
    uint64 ComputeMaterialDigest() const;
    // E9 fingerprints. All covers every view matching the world and gates the
    // lane; Persistent covers only the Always-participation views, whose
    // submissions are what the world's content versions describe.
    struct ViewFingerprints
    {
        uint64 All = 0;
        uint64 Persistent = 0;
    };
    ViewFingerprints ComputeViewFingerprints(uint64 worldId) const;
    // True when this frame's full lane runs only because an OnDemand view (a
    // probe capture, a mirror) started or stopped participating: nothing a
    // persistent view draws changed, so the content versions stay put.
    bool IsOnDemandViewChurnOnly(const ECS::World& world, uint32 reasons) const;
    // L1b: record a changed shadow caster's world sphere (old ∪ new bounds) for
    // caster-proximity keying. Non-casters are ignored; past the tracking cap the
    // frame degrades to an unattributed (every-light) bump. Call BEFORE the
    // GPUScene UpdateInstance that overwrites `previous`.
    void TrackChangedCasterSphere(const Rendering::GPUInstance& previous,
                                  const Rendering::GPUInstance& current);

    bool m_FeedFastPath = false;      // constructor latch (env default / test seam)
    uint32 m_MaxAlwaysRefresh = 0;    // E10 always-refresh subset ceiling
    ECS::SwapGenerationGuard m_FeedGuard;         // E3
    ECS::ChangeGate m_ProbeGateMeshRenderer;      // E5 — one gate per probe query (R8)
    ECS::ChangeGate m_ProbeGateLocalBounds;
    ECS::ChangeGate m_ProbeGateSectorCoord;
    ECS::ChangeGate m_ProbeGateLodGroup;
    std::size_t m_FullPassStructuralVersion = 0;  // E4 — sampled at full-lane END (post-apply)
    std::atomic<bool> m_MeshReloadPending{false}; // E6
    // Unsubscribes on destruction; safe even if the registry died first.
    ScopedSubscription m_MeshReloadSubscription;

    // Particle emitters: per-view sort, particle upload and the late forward draws; the particle
    // simulation system simulates them.
    Particles::ParticleExtraction m_Particles;
    // This system is owned by the ECS world and destroyed AFTER RenderServices
    // (and the registry inside it). Both raw pointers above are dangling by
    // then, so the destructor consults these before calling back in.
    uint64 m_LastMaterialDigest = 0;              // E7 — entry-computed, stored at full-lane end
    // L1b caster-proximity keying: world-space spheres (old ∪ new bounds) of the
    // shadow casters whose instance record changed this frame — the attribution
    // published alongside the caster content-version bump so far-away point
    // lights keep their cached shadow maps. Cleared every Update; overflow past
    // the cap degrades the frame to an unattributed (every-light) bump.
    std::vector<ShadowCasterChangeSphere> m_ChangedCasterSpheres;
    bool m_ChangedCasterOverflow = false;
    // Log gate: set on the first overflow of an episode, cleared by the first
    // frame that completes without overflowing (persistent crowds log once).
    bool m_OverflowLogLatched = false;
    uint64 m_LastViewFingerprint = 0;             // E9
    uint64 m_LastPersistentViewFingerprint = 0;   // E9, Always views only
    uint32 m_LastPendingTransient = 0;            // E8 — total at the last full pass
    uint32 m_LastPendingMeshEntry = 0;            // E8 per-shape breakdown (D7/A6)
    uint32 m_LastPendingMaterialLoad = 0;
    uint32 m_LastPendingPipeline = 0;
    uint32 m_LastPendingSentinelIndex = 0;
    bool m_CachesPrimed = false;                  // E11
    uint64 m_CachesWorldId = 0;
    uint64 m_CachedMeshDigest = 0;                // mesh-only digest term (ocean re-added per frame)
    uint64 m_FrameMaterialDigest = 0;             // entry samples carried to the full-lane prime
    ViewFingerprints m_FrameViewFingerprints;
    std::vector<ECS::EntityHandle> m_FeedScratch;
    // prevTransform settle-window guard: the LastPrevDiffers settle rebuild
    // fires only on the first extraction run of a feed swap window (a same-
    // window re-run steps the same frame again and must not zero a fresh
    // mover's MV). Computed once per Update from the feed's SwapGeneration.
    uint64 m_SettleSwapGen = ~0ull;
    uint64 m_SettleWorldId = 0;
    bool m_AllowPrevSettle = true;
    std::vector<ECS::EntityHandle> m_AlwaysRefresh;       // D5 subset, rebuilt each full pass
    std::unordered_set<ECS::EntityId> m_AlwaysRefreshSet; // patch-loop membership test
#if GE_DEBUG_INSTRUMENTATION
    uint32 m_ValidateCounter = 0;     // D9 shadow validator cadence (every 256th fast frame)
    bool m_ValidatorForceFull = false;
#endif

    std::vector<WorldRenderableRecord> m_Records;
    std::vector<WorldSubmissionRecord> m_Submissions;

    // Sub-phase timing window (A2.1). Rolling ring of the last N ticks so a
    // single get_render_stats poll returns a stable mean/max under the bench's
    // low FPS instead of one noisy frame. Cheap: no per-instance timers, just
    // a handful of steady_clock reads at phase boundaries per Update.
    static constexpr std::size_t kExtractionStatsWindow = 64;
    struct ExtractionStatSample
    {
        double totalMs = 0.0;
        double gatherMs = 0.0;
        double processMs = 0.0;
        double prepareMs = 0.0; // A2.1 parallel prepare (0 on fast frames)
        double applyMs = 0.0;   // A2.1 serial apply (0 on fast frames)
        bool fastFrame = false; // GE_EXTRACTION_FEED fast path taken this tick
    };
    std::array<ExtractionStatSample, kExtractionStatsWindow> m_StatWindow{};
    std::size_t m_StatWindowHead = 0;
    std::size_t m_StatWindowCount = 0;

    // A2.1: the full lane runs PROCESS as parallel prepare + serial apply.
    std::unique_ptr<RenderExtractionParallelState> m_ParallelState;

    std::vector<PostProcessExtractedVolume> m_Volumes; // retains capacity across frames

};

} } // namespace GameEngine::Engine::Renderer
