#pragma once

// RenderGraph per-frame graph core: declaration, ref-count cull, scheduling,
//
// Immediate-mode: the whole graph is declared fresh each frame, compiled in a
// single sweep, then BeginFrame() clears it (capacity retained — zero
// steady-state heap allocs). There is no retained pass state, no enable/disable,
// no invalidate. A pass that should not run is simply not declared.
//
// barrier generation, and the submission plan — all over flat dense-id arrays.
// Physical realization and command recording live in RGFrame/RGRecord.

#include "Rendering/Core/RenderGraph/RGArena.h"
#include "Rendering/Core/RenderGraph/RGBarrier.h"
#include "Rendering/Core/RenderGraph/RGTypes.h"

#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

// Implementation scratch types for barrier generation / scheduling (RGBarriers.cpp,
// RGSchedule.cpp). At namespace scope only so the graph can own capacity-retained
// member scratch for them — not part of the public surface.
namespace Detail
{
struct ResolvedRange
{
    uint32_t BaseMip, MipCount, BaseLayer, LayerCount;

    bool operator==(const ResolvedRange&) const = default;
};
// A pass's combined requirement on one (resource, range).
struct CombinedAccess
{
    RGResourceId Res;
    ResolvedRange Range;
    uint32_t Stage;
    uint32_t Access;
    RGImageLayout Layout;
    bool HasWrite;
    bool HasSampled; // descriptor-sampled read (Sampled/SampledCompute) — see the fold assert
};
// Source scope of a needed transition (the rectangle-merge group key).
struct BarrierSrcScope
{
    uint32_t Stage;
    uint32_t Access;
    RGImageLayout OldLayout;
    uint8_t Queue;

    bool operator==(const BarrierSrcScope&) const = default;
};
struct BarrierGroup
{
    BarrierSrcScope Src;
    uint32_t NeedyBase; // offset of this group's (LayerCount × MipCount) mask in flat scratch
    // True only while EVERY contributing cell is on its first frame-touch — the
    // ground truth for the recording layer's submission-front hoist (a barrier
    // with any intra-frame access before it must stay in schedule position).
    bool FirstTouch = true;
};
// Cluster-DAG emission tiebreak: (component phase, component rank, phase, insertion).
using ClusterKey = std::tuple<int32_t, uint32_t, int32_t, uint32_t>;
} // namespace Detail

struct RGResourceDesc
{
    RGResourceKind Kind = RGResourceKind::Texture;
    // Scheduling-relevant fields only. Sizing/format/layout matter at realization;
    // they are intentionally absent from the scheduling logic.
    uint64_t SizeBytes = 0;         // buffers
    uint32_t Width = 0, Height = 0; // textures
    uint32_t Format = 0;
    uint32_t MipLevels = 1;   // textures: drives subresource state granularity
    uint32_t ArrayLayers = 1; // textures: drives subresource state granularity
    uint32_t SampleCount = 1; // textures: MSAA (drives the pipeline format key)
    // Descriptor-claim constraint, mirrored from TextureDesc::
    // sampledInGeneralLayout at import: the physical sampled descriptors claim
    // GENERAL (storage co-use), so every SAMPLED read must observe GENERAL — a
    // ShaderReadOnly transition would contradict the claimed layout
    // (VUID-09600) and ping-pong the layout against storage users across
    // queues. Storage accesses are natively General; non-sampled reads
    // (copies) keep their API-required layouts, ordered by the submission
    // plan's RAW/WAR-on-layout edges.
    bool SampledInGeneralLayout = false;
    const char* Name = nullptr;
};

struct RGPassDesc
{
    const char* Name = nullptr;
    int32_t Phase = 0;
    RGQueue Queue = RGQueue::Graphics;
    // Optional grouping hint for per-view resource lifetime/naming. NOT used by
    // the scheduler — clustering is derived from declared attachments.
    uint32_t ViewId = kInvalidId;
};

// Flat access record: appended in any order; carries its owning pass so cull and
// the scheduler can scan/group without per-pass nested containers.
struct RGAccessRecord
{
    RGPassId Pass = kInvalidId;
    RGResourceId Resource = kInvalidId;
    RGAccess Access = RGAccess::Sampled;
    RGRange Range{}; // subresource granularity (whole resource by default)
};

// A GPU submission: a maximal run of consecutive same-queue passes in the
// scheduled order. Cross-queue producer->consumer dependencies become
// timeline-semaphore waits; a single graphics queue collapses to one submission.
struct RGSemaphoreWait
{
    uint32_t Queue = 0; // PHYSICAL queue index
    uint64_t Value = 0;
    // Stages this wait must block (RGStage mask). None = all stages (the
    // conservative full-pipe wait); narrowed during command-recording wiring.
    uint32_t DstStage = RGStage::None;
};
struct RGSubmission
{
    RGQueue Queue = RGQueue::Graphics; // logical queue of the run's passes
    uint8_t PhysicalQueue = 0;         // after remap; partition/sync key
    uint32_t ScheduledFirst = 0;       // first index into ScheduledOrder
    uint32_t ScheduledCount = 0;
    uint64_t SignalValue = 0; // timeline value signaled on PhysicalQueue at completion
    // Waits are deduped per producer physical queue and a submission never waits
    // on its own queue, so the count is bounded by construction — flat storage,
    // no per-submission heap.
    RGSemaphoreWait Waits[kQueueCount]{};
    uint32_t WaitCount = 0;
};

class RGGraph
{
  public:
    RGGraph();

    // Begin a fresh frame: clear per-frame declarations (capacity retained) and
    // reset the arena. Call before declaring passes/resources.
    void BeginFrame();

    // ── Declaration ─────────────────────────────────────────────────────────
    RGResourceId CreateResource(const RGResourceDesc& desc);

    // Mark a resource as an external sink (presented / exported / read outside the
    // graph). Sinks anchor cull: any pass transitively feeding a sink is kept.
    void MarkExternal(RGResourceId id);

    // Mark a resource as imported from the cross-frame pool with a known current
    // layout, so barrier generation transitions from that real layout rather than
    // Undefined (import-at-tracked-layout; no execute-time staleness).
    void MarkImported(RGResourceId id, RGImageLayout initialLayout);

    // Contractual final layout for an exported texture (UI-sampled outputs):
    // GenerateBarriers emits one trailing transition after the resource's last
    // access, and FinalLayout()/the pool write-back then carry it. Only
    // ShaderReadOnly and General are supported consumer contracts today.
    void SetExportLayout(RGResourceId id, RGImageLayout layout);

    RGPassId AddPass(const RGPassDesc& desc);
    void Read(RGPassId pass, RGResourceId resource, RGAccess access, RGRange range = RGRange::All());
    void Write(RGPassId pass, RGResourceId resource, RGAccess access, RGRange range = RGRange::All());
    // Force a pass to survive cull even with no consumed output (side effects,
    // queries, debug). Equivalent intent to RDG's PreventCulling.
    void PreventCulling(RGPassId pass);
    // Explicit execution-order edge: `after` schedules after `before`. Emits NO
    // barrier (memory visibility stays with declared accesses / the caller's
    // manual barriers) and does NOT keep either pass alive — pair with
    // PreventCulling/MarkExternal for lifetime. Edges touching culled passes
    // are dropped; a cycle culls loudly like any hazard cycle. Cross-physical-
    // queue edges get a timeline wait in the submission plan.
    void AddOrderingEdge(RGPassId before, RGPassId after);

    // ── Compile ─────────────────────────────────────────────────────────────
    // Ref-count cull. Marks each pass live/culled (+reason) by
    // backward reachability from external sinks and prevent-cull passes.
    void Compile();

    // ── Schedule ──────────────────────────────────────────────────────────────
    // Compute dependency levels + an execution order over the LIVE passes.
    // Requires Compile() first. Structural working-set scheduling, derived
    // entirely from declared accesses (no hand-curated phases/tags):
    //  1. COMPONENTS — weakly-connected components of the hazard-edge graph are
    //     emitted contiguously. An independent secondary view (e.g. a thumbnail
    //     with its own target chain) therefore can never interleave with the
    //     main view's span: the Mac M1 TBDR thrash fix, by construction.
    //  2. CLUSTERS — within a component, passes binding the IDENTICAL attachment
    //     working-set group into one contiguous cluster (CSM cascades over one
    //     shadow array), splitting only where a data dependency forces an
    //     outside pass between members (keeps the cluster DAG acyclic).
    //  3. Cluster-DAG topological order, tiebroken (component, phase, insertion);
    //     within a cluster: dependency level, then phase, then insertion.
    // Deterministic and platform-independent: same graph ⇒ same schedule (the
    // contiguity that TBDR needs is harmless/slightly beneficial on desktop).
    // Hazard edges (RAW/WAR/WAW) derive from declared accesses in recording
    // order, at resource granularity (conservative w.r.t. subresource ranges).
    void Schedule();

    const std::vector<RGPassId>& ScheduledOrder() const { return m_ScheduledOrder; }
    uint32_t Level(RGPassId p) const { return m_Level[p]; }
    // Hazard-edge successors of p (RAW/WAR/WAW, deduped). Valid after
    // Schedule() until the next BeginFrame/Schedule. Drives the MCP
    // dependency listing — no parallel edge structure is retained.
    const std::vector<RGPassId>& PassSuccessors(RGPassId p) const { return m_ScratchSucc[p]; }
    // Interned attachment working-set id (kInvalidId = neutral: no attachments,
    // e.g. compute/transfer). Passes with equal id bind the same attachment set.
    uint32_t WorkingSetId(RGPassId p) const { return m_WorkingSetId[p]; }
    // Count of attachment working-set transitions between consecutive scheduled
    // raster passes — the integer cost the scheduler minimizes (lower = less thrash).
    uint32_t WorkingSetTransitions() const { return m_WorkingSetTransitions; }

    // Cross-physical layout transitions BuildSubmissionPlan considered relocating
    // onto their producer's tail, and how many of those it refused because the
    // resource's accessors spanned physical queues. A refused crossing keeps
    // today's behaviour — it records on the consumer with a source scope the
    // recording queue cannot express, sanitized away — so this pair is the size
    // of the residual any release/acquire pair design would still have to carry.
    //
    // Read them together. Declined alone has a false zero: a frame with no
    // crossings at all reports the same 0 as a frame where every crossing
    // relocated, and only Considered separates them.
    uint32_t CrossQueueTransitionsConsidered() const { return m_CrossQueueConsidered; }
    uint32_t CrossQueueRelocationsDeclined() const { return m_CrossQueueDeclined; }

    // ── Barriers ──────────────────────────────────────────────────────────────
    // Derive minimal resource transitions for the whole frame from the declared
    // accesses, walking the scheduled order and tracking per-resource state in a
    // flat array. Requires Schedule() first. All transitions a pass needs are
    // coalesced into one batch emitted before it (per-pass batching — correct for
    // the working-set-contiguous execution order); read-after-read with an
    // unchanged layout emits nothing.
    //
    // physicalOfLogical MUST be the same map BuildSubmissionPlan receives:
    // hazard coverage is keyed by PHYSICAL queue so that logical queues sharing
    // one physical queue (the collapsed default) share one coverage slot —
    // barrier generation and the submission plan's semaphores then partition
    // responsibility identically (same-physical = barrier, cross-physical =
    // semaphore), with no WAR falling between the two.
    void GenerateBarriers(const uint8_t physicalOfLogical[kQueueCount]);

    // Logical == physical (each logical queue on its own physical queue). The
    // production default is RGFrame's collapsed map; identity is the map for
    // core tests pinning cross-queue semantics.
    static constexpr uint8_t kIdentityQueueMap[kQueueCount] = {0, 1, 2};

    const std::vector<RGBarrier>& Barriers() const { return m_Barriers; }
    const std::vector<RGBarrierBatch>& BarrierBatches() const { return m_BarrierBatches; }
    size_t BarrierCount() const { return m_Barriers.size(); }

    // Final image layout of a resource after the whole frame (valid after
    // GenerateBarriers). Used to write tracked state back to the cross-frame
    // pool. Debug-asserts subresource layouts are uniform for imported
    // resources (a heterogeneous final state can't round-trip a name-keyed pool).
    RGImageLayout FinalLayout(RGResourceId r) const;

    // ── Submission plan / cross-queue sync ──────────────────────────────────
    // Partition the scheduled order into per-PHYSICAL-queue submissions and
    // derive the timeline-semaphore waits implied by cross-queue
    // producer->consumer dependencies. Requires Schedule() first.
    // physicalQueueOfLogical remaps RGQueue → the device's real queue: on
    // devices whose compute/transfer alias the graphics queue (MoltenVK), the
    // remap collapses adjacent runs into one submission and skips pointless
    // same-queue semaphores. Default is the identity (3 distinct queues).
    void BuildSubmissionPlan(const uint8_t physicalQueueOfLogical[3]);
    void BuildSubmissionPlan()
    {
        static constexpr uint8_t kIdentity[3] = {0, 1, 2};
        BuildSubmissionPlan(kIdentity);
    }
    const std::vector<RGSubmission>& Submissions() const { return m_Submissions; }
    // Derived placement, parallel to Barriers(): the submission index (into
    // Submissions()) at whose TAIL a barrier records, or kInvalidId for the
    // barriers that record in schedule position (their pass's own batch, or
    // the submission-front hoist when FirstTouch). Two classes carry a
    // placement — export/normalize sentinels, which no pass owns, and
    // cross-physical layout transitions relocated onto their producing queue.
    // Filled by BuildSubmissionPlan (the placement rules are documented
    // there); the recording layer emits each barrier exactly where this says.
    const std::vector<uint32_t>& BarrierSubmissions() const { return m_BarrierSub; }

    // ── Introspection (tests + future MCP) ───────────────────────────────────
    size_t PassCount() const { return m_Passes.size(); }
    size_t ResourceCount() const { return m_Resources.size(); }
    bool IsCulled(RGPassId p) const { return m_Passes[p].Culled; }
    RGCullReason CullReason(RGPassId p) const { return m_Passes[p].CullReason; }
    bool IsResourceNeeded(RGResourceId r) const { return m_ResourceNeeded[r]; }
    bool IsExternal(RGResourceId r) const { return m_ResourceExternal[r] != 0; }
    // True for cross-frame pool imports (MarkImported): the "persistent" class.
    bool IsImported(RGResourceId r) const { return m_ResourceImported[r] != 0; }
    // True when any LIVE pass accesses r. This is the REALIZATION gate: a live
    // pass's depth attachment must exist even if nothing downstream reads it
    // (needed = reaches a sink, drives culling; used = must be allocated).
    bool IsResourceUsed(RGResourceId r) const { return m_ResourceUsedByLive[r]; }
    size_t LivePassCount() const { return m_LivePassCount; }
    size_t CulledPassCount() const { return m_Passes.size() - m_LivePassCount; }
    // Debug-lint support (linear scan): did the pass declare ANY read on r?
    bool HasReadAccess(RGPassId p, RGResourceId r) const
    {
        for (const RGAccessRecord& a : m_Accesses)
            if (a.Pass == p && a.Resource == r && !IsWrite(a.Access))
                return true;
        return false;
    }
    bool HasWriteAccess(RGPassId p, RGResourceId r) const
    {
        for (const RGAccessRecord& a : m_Accesses)
            if (a.Pass == p && a.Resource == r && IsWrite(a.Access))
                return true;
        return false;
    }
    // Flat declared-access records (each carries its owning Pass). Valid after
    // declaration; drives the per-pass read/write listing in the inspector.
    const std::vector<RGAccessRecord>& Accesses() const { return m_Accesses; }
    const char* PassName(RGPassId p) const { return m_Passes[p].Desc.Name; }
    const char* ResourceName(RGResourceId r) const { return m_Resources[r].Name; } // arena-copied
    const RGResourceDesc& ResourceDesc(RGResourceId r) const { return m_Resources[r]; }
    RGQueue PassQueue(RGPassId p) const { return m_Passes[p].Desc.Queue; }
    int32_t PassPhase(RGPassId p) const { return m_Passes[p].Desc.Phase; }

    RGArena& Arena() { return m_Arena; }

  private:
    struct RGPassRec
    {
        RGPassDesc Desc{};
        bool PreventCull = false;
        bool Culled = false;
        RGCullReason CullReason = RGCullReason::NotCulled;
    };

    // Per-frame declarations (cleared in BeginFrame, capacity retained).
    std::vector<RGResourceDesc> m_Resources;
    std::vector<uint8_t> m_ResourceExternal;       // parallel to m_Resources
    std::vector<uint8_t> m_ResourceImported;       // parallel; 1 if imported with a known layout
    std::vector<RGImageLayout> m_ResourceInitLayout; // parallel; layout for imported resources
    std::vector<RGImageLayout> m_ResourceExportLayout; // parallel; Undefined = no export contract
    std::vector<RGPassRec> m_Passes;
    std::vector<RGAccessRecord> m_Accesses;
    std::vector<std::pair<RGPassId, RGPassId>> m_OrderingEdges; // {before, after}

    // Compile scratch / outputs (sized per frame, capacity retained).
    std::vector<uint8_t> m_ResourceNeeded;     // parallel to m_Resources (cull semantics)
    std::vector<uint8_t> m_ResourceUsedByLive; // parallel (realization gate)
    size_t m_LivePassCount = 0;

    // Schedule outputs (per pass id; culled passes excluded from m_ScheduledOrder).
    std::vector<RGPassId> m_ScheduledOrder;
    std::vector<uint32_t> m_Level;        // longest-path dependency level
    std::vector<uint32_t> m_WorkingSetId; // interned attachment-set id; kInvalidId = neutral
    uint32_t m_WorkingSetTransitions = 0;

    // Cross-queue relocation accounting; both reset at the top of every
    // BuildSubmissionPlan. See CrossQueueTransitionsConsidered().
    uint32_t m_CrossQueueConsidered = 0;
    uint32_t m_CrossQueueDeclined = 0;

    // Barrier outputs.
    std::vector<RGBarrier> m_Barriers;
    std::vector<RGBarrierBatch> m_BarrierBatches;

    // Per-subresource sync state (one cell per (mip, layer); buffers = 1 cell).
    // WriteStage/WriteAccess = the producer scope (last write or last layout
    // transition); ReadStages/ReadAccesses = reader scopes ALREADY synchronized
    // since that producer (accumulated — a reader at an uncovered stage gets an
    // expansion barrier from the producer; a later write WAR-waits on the union).
    // Queue = queue of the last PRODUCER — a write OR a read-layout transition,
    // both of which rewrite WriteStage/Layout and so must own the cell so a later
    // barrier computes the cross-queue edge correctly (RGBarriers.cpp:~350). PURE
    // reads still never take ownership (a cross-queue read needs a semaphore, not
    // an ownership transfer — RGBarriers.cpp:~374). Members so capacity is
    // retained frame-to-frame and final states can be exported.
    struct RGCellState
    {
        uint32_t WriteStage;
        uint32_t WriteAccess;
        // Reader scopes already synchronized against the producer, PER QUEUE.
        // Coverage must be queue-local: a reader on queue Q is only covered by
        // scopes synchronized on Q — a graphics reader's "compute stage" bit says
        // nothing about the compute queue's timeline (queue-blind accumulation
        // let W(C)→R1(G)→R2(C) skip R2's barrier entirely).
        uint32_t ReadStages[kQueueCount];
        uint32_t ReadAccesses[kQueueCount];
        RGImageLayout Layout;
        uint8_t Queue;
        bool Init;
        // Imported resources AND freshly-created pooled resources carry an
        // unknown prior producer (last frame's contents / a recycled physical a
        // prior in-flight frame still touches): the FIRST touch this frame must
        // emit an execution dependency (BottomOfPipe src) even when nothing else
        // requires a barrier. Cleared by the first emitted barrier on the cell.
        bool PendingImportSync;
    };
    std::vector<RGCellState> m_Cells;
    std::vector<uint32_t> m_CellOff;  // per-resource offset into m_Cells (+1 sentinel)
    std::vector<uint32_t> m_CellMips; // per-resource mip count (1 for buffers)

    // Pass -> access CSR index, shared by barrier gen + submission planning.
    std::vector<uint32_t> m_PassAccOff;
    std::vector<uint32_t> m_PassAccIdx;
    bool m_PassAccValid = false;
    void EnsurePassAccessIndex();

    // Per-queue timeline counters. Deliberately NOT reset per frame: frames
    // overlap in flight, so per-frame values would collide with frame N-1's
    // still-pending signals.
    uint64_t m_QueueTimelineNext[kQueueCount] = {0, 0, 0};

    // Submission plan (per frame).
    std::vector<RGSubmission> m_Submissions;
    std::vector<uint32_t> m_BarrierSub; // see BarrierSubmissions()

    // ── Frame scratch (members so capacity persists; cleared, never freed) ──
    // Inner vectors of vector<vector> members are cleared individually — an
    // outer clear() would free them. Outer sizes grow only.
    // Compile:
    std::vector<uint8_t> m_ScratchPassNeeded;
    std::vector<RGPassId> m_ScratchWorklist;
    std::vector<std::vector<RGPassId>> m_ScratchWritersOf;
    std::vector<std::vector<RGResourceId>> m_ScratchReadsOf;
    std::vector<uint8_t> m_ScratchResHasReader;
    // Schedule:
    std::vector<std::vector<RGResourceId>> m_ScratchAttSet;
    std::vector<std::vector<RGPassId>> m_ScratchSucc;
    std::vector<uint32_t> m_ScratchIndeg;
    std::vector<uint32_t> m_ScratchIndegWork;
    std::vector<RGPassId> m_ScratchLastWriter;
    std::vector<std::vector<RGPassId>> m_ScratchReadersSince;
    std::vector<RGPassId> m_ScratchTopo;
    std::vector<uint64_t> m_ScratchReach;
    std::vector<uint32_t> m_ScratchUnionFind;
    std::vector<uint32_t> m_ScratchClusterOf;
    std::vector<uint64_t> m_ScratchUnionReach;
    std::vector<uint64_t> m_ScratchGroupMask;
    std::vector<RGPassId> m_ScratchClusterCur;
    std::vector<uint32_t> m_ScratchReady;
    std::vector<std::vector<RGResourceId>> m_ScratchSets;     // interned working sets (logical count local)
    std::vector<std::vector<RGPassId>> m_ScratchWsMembers;    // per-set member lists
    std::vector<std::vector<RGPassId>> m_ScratchClusters;     // cluster member lists (logical count local)
    std::vector<std::vector<uint32_t>> m_ScratchCSucc;        // contracted cluster DAG edges
    std::vector<std::pair<int32_t, uint32_t>> m_ScratchCompRank; // per-component (phase, rank)
    std::vector<uint32_t> m_ScratchCIndeg;                       // cluster-DAG in-degrees
    std::vector<Detail::ClusterKey> m_ScratchCKey;               // cluster emission keys
    // Barriers (flat CSR scratch — see RGBarriers.cpp):
    std::vector<Detail::CombinedAccess> m_ScratchCombos;
    std::vector<Detail::BarrierGroup> m_ScratchGroups;
    std::vector<uint8_t> m_ScratchGroupNeedy;                 // flat per-group needy masks
    std::vector<std::pair<uint32_t, uint32_t>> m_ScratchRuns; // (startMip, mipCount), CSR by layer
    std::vector<uint32_t> m_ScratchRunOff;                    // per-layer offsets (+1 sentinel)
    // Submission:
    std::vector<uint32_t> m_ScratchSubOfSched;
    std::vector<uint32_t> m_ScratchSubOfPass;
    std::vector<uint32_t> m_ScratchSubLastWriter;
    // Parallel to m_ScratchSubLastWriter: the submission of the most-recent read
    // that transitioned a resource's layout (a producer in the barrier layer that
    // is NOT a write). A later cross-physical-queue accessor waits on it so it
    // cannot observe the layout mid-transition (Finding A).
    std::vector<uint32_t> m_ScratchSubLastLayoutProducer;
    // Pass -> its barrier batch index (kInvalidId if none), so the submission plan
    // can consult the already-generated barrier IR to classify read-transitions.
    std::vector<uint32_t> m_ScratchPassBatchOf;
    std::vector<std::vector<uint32_t>> m_SubReadersSince; // capacity retained

    // Copy a caller string into the frame arena (declarations must not dangle
    // when callers build names dynamically).
    const char* CopyName(const char* name);

    RGArena m_Arena;
};

} // namespace GameEngine::Rendering::RenderGraph
