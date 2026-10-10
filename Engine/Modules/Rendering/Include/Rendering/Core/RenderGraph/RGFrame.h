#pragma once

// RenderGraph public per-frame layer: typed handles, lambda-scoped pass declaration,
// and the single Execute() driver over the proven core (RGGraph) + pools.
//
//   RGFrame frame(device, &persistentPool, &transientPool, &uploadRing);
//   frame.BeginFrame(frameIndex);
//   RGTexture color = frame.CreateTexture("SceneColor", desc);
//   frame.AddPass("World", Phase,
//       [&](RGPassBuilder& p) { p.AttachColor(0, color, {.Load = RGLoadOp::Clear}); },
//       [=](RGContext& ctx) { /* record draws */ });
//   frame.MarkOutput(color);
//   frame.Execute();
//
// Footguns removed by construction: no enable/disable/invalidate (don't declare
// what shouldn't run), typed RGTexture/RGBuffer (wrong-kind misuse is a compile
// error), split read/write enums, accesses declared inside the owning pass's
// builder, ONE upload ring instead of per-resource perFrame copies. Execute
// lambdas are placement-new'd into the frame arena (never std::function) and
// MUST capture by value — they run after declaration returns.
//
// Single-threaded declaration; command recording (RGContext::Cmd) lands with
// the backend wiring increment (null until then).

#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Core/QueryPool.h"
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/RenderGraph/RGFrameStamp.h"
#include "Rendering/Core/RenderGraph/RGGraph.h"
#include "Rendering/Core/RenderGraph/RGResourcePool.h"
#include "Rendering/Core/RenderGraph/RGStateMap.h"
#include "Rendering/Core/RenderGraph/RGTransientPool.h"
#include "Rendering/Core/RenderGraph/RGUploadRing.h"

#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <utility>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{

class RGFrame;

// ── Typed frame-local handles (dense per-frame ids underneath) ──────────────
struct RGTexture
{
    RGResourceId Id = kInvalidId;
    bool IsValid() const { return Id != kInvalidId; }
};
struct RGBuffer
{
    RGResourceId Id = kInvalidId;
    bool IsValid() const { return Id != kInvalidId; }
};
// A ray-query acceleration structure (a TLAS slot): ordered and waited on like any
// resource, never transitioned (RGResourceKind::AccelerationStructure).
struct RGAccelerationStructure
{
    RGResourceId Id = kInvalidId;
    bool IsValid() const { return Id != kInvalidId; }
};
struct RGPass
{
    RGPassId Id = kInvalidId;
    bool IsValid() const { return Id != kInvalidId; }
};

// ── Split read/write access enums (read-vs-write misuse = compile error) ────
enum class RGTextureRead : uint8_t
{
    Sampled,        // default (fragment-stage consumer on graphics passes)
    SampledCompute, // sampled by a compute dispatch recorded in a GRAPHICS pass
    SampledVertex,  // sampled from the vertex stage onward (vertex-displacement fields)
    Storage,
    CopySrc
};
enum class RGBufferRead : uint8_t
{
    Storage, // default
    Uniform,
    Indirect,
    Index,
    Vertex,
    CopySrc
};
enum class RGTextureWrite : uint8_t
{
    Storage, // default (attachments go through Attach*)
    CopyDst
};
enum class RGBufferWrite : uint8_t
{
    Storage, // default
    CopyDst
};

// ── Attachment ops ──────────────────────────────────────────────────────────
enum class RGLoadOp : uint8_t
{
    Load,
    Clear,
    DontCare
};
enum class RGStoreOp : uint8_t
{
    Store,
    DontCare,
    // Preserve the contents, write nothing. What a read-only attachment does:
    // Store would be an attachment WRITE the graph's access model never saw
    // (the pass declared a read), and DontCare would discard live contents.
    // Set by AttachDepth for RGDepthAccess::ReadOnly. Never valid on an
    // attachment the pass writes — the write voids the preservation guarantee
    // and the contents become undefined — and every attach helper asserts it.
    None
};
struct RGClearValue
{
    float Color[4] = {0, 0, 0, 0};
    float Depth = 0.0f; // reverse-Z: clear to far
    uint32_t Stencil = 0;
};
struct RGAttachmentOps
{
    RGLoadOp Load = RGLoadOp::Load;
    RGStoreOp Store = RGStoreOp::Store;
    RGClearValue Clear{};
};

// Readable at call sites where a bare bool would not be ("what's true?").
enum class RGDepthAccess : uint8_t
{
    ReadWrite, // depth test + write (a Load additionally derives the test's read)
    ReadOnly   // bound read-only (DepthReadOnly layout; pairs with sampling)
};

constexpr uint32_t kMaxColorAttachments = 8;
static_assert(kMaxColorAttachments <= PipelineFormatKey::kMaxColors,
              "the pipeline format key's color array must cover every attachable slot");

// Recorded attachment binding (consumed by the recording increment; exposed for
// tests/MCP).
struct RGAttachmentRec
{
    RGPassId Pass = kInvalidId;
    uint32_t Slot = 0;
    RGResourceId Tex = kInvalidId;
    RGResourceId Resolve = kInvalidId; // MSAA resolve target, if any
    RGAttachmentOps Ops{};
    bool IsDepth = false;
    bool ReadOnly = false; // depth read-only attach
    RGRange Range = RGRange::All();
};

// ── Pass declaration scope ──────────────────────────────────────────────────
class RGPassBuilder
{
  public:
    void Read(RGTexture t, RGTextureRead access = RGTextureRead::Sampled,
              RGRange range = RGRange::All());
    void Read(RGBuffer b, RGBufferRead access = RGBufferRead::Storage);
    void Write(RGTexture t, RGTextureWrite access = RGTextureWrite::Storage,
               RGRange range = RGRange::All());
    void Write(RGBuffer b, RGBufferWrite access = RGBufferWrite::Storage);
    // A pass that traverses the structure with ray queries reads it; the pass
    // that records its build writes it.
    void Read(RGAccelerationStructure as);
    void Write(RGAccelerationStructure as);

    void AttachColor(uint32_t slot, RGTexture t, const RGAttachmentOps& ops = {},
                     RGRange range = RGRange::All());
    void AttachColorResolve(uint32_t slot, RGTexture msaa, RGTexture resolve,
                            const RGAttachmentOps& ops = {});
    // A ReadOnly attach declares a DepthRead and nothing else, so the recorded
    // store op is forced to RGStoreOp::None whatever `ops.Store` says: a store
    // is an attachment write, and one the graph did not model leaves the next
    // writer's barrier without it in its source scope (WAW on the depth image).
    void AttachDepth(RGTexture t, const RGAttachmentOps& ops = {},
                     RGDepthAccess access = RGDepthAccess::ReadWrite,
                     RGRange range = RGRange::All());
    // MSAA depth resolved to a single-sample target (device depthResolveMode
    // default SampleZero). Consumer: depth capture/readback paths.
    void AttachDepthResolve(RGTexture msaa, RGTexture resolve, const RGAttachmentOps& ops = {});

    // Survive cull without an in-graph consumer (output read by the CPU /
    // outside the graph: readback rings, letterbox-style terminal draws).
    void PreventCulling();

    // A2.4-P0-R opt-in: this pass's exec is self-contained (sets its own
    // viewport/scissor/binds — inherits NO primary state) and its draw order is
    // immaterial, so under GE_PARALLEL_RECORD its interior may be recorded into a
    // secondary command buffer on a job-system worker. The record loop begins the
    // render pass with useSecondaryCommandBuffers and vkCmdExecuteCommands the
    // worker's secondary. Only raster passes with attachments are eligible; the
    // flag is inert when the parallel-record window is off (serial fallback).
    void RecordInSecondary();

  private:
    friend class RGFrame;
    RGFrame* m_Frame = nullptr;
    RGPassId m_Pass = kInvalidId;
};

// ── Execute-time context ────────────────────────────────────────────────────
// Cmd is non-null inside exec lambdas; record draws/dispatches directly on it
// (no forwarding wrappers — the full CommandList surface is the API). Bindless:
// the global descriptor set is bound by pass code AFTER SetPipeline*
// (`Cmd->BindDescriptorSet(set, handle, pipe)`) — binds route through the bind
// point the pipeline established, so compute needs its own bind.
class RGContext
{
  public:
    CommandList* Cmd = nullptr;

    TextureHandle GetTexture(RGTexture t) const;
    BufferHandle GetBuffer(RGBuffer b) const;
    RGPassId CurrentPass() const { return m_Pass; }
    IDevice* GetDevice() const;

    // Single-mip view of a POOL-IMPORTED texture, for passes that bind one mip
    // level at a time (mip-chain compute) — 2D for a single-layer image, 2D
    // array covering every layer for an array image. The persistent pool owns the
    // view and releases it with the image, so a pass asks per use and must NOT
    // cache the handle across frames: a pass-side cache cannot see the
    // desc-change realloc or idle age-out that kills the image underneath it,
    // and one cache shared by several views destroys views the current frame is
    // still binding. Returns an invalid handle for transient or externally
    // imported textures, whose lifetime the pool does not track.
    TextureViewHandle GetOrCreatePooledMipView(RGTexture t, uint32_t mip) const;

    // Pipeline variants keyed by this pass's attachment formats + sample count
    // (the device-owned PipelineCache does the caching; RenderGraph only derives the
    // key — lazily, memoized per pass). Compute variants need no key.
    PipelineFormatKey BuildCurrentFormatKey() const;
    uint32_t GetCurrentSampleCount() const;
    PipelineHandle GetOrCreatePipelineVariant(GraphicsPipelineId id) const;
    PipelineHandle GetOrCreatePipelineVariant(ComputePipelineId id) const;
    // Resolve the variant and bind it (logs and leaves state untouched when the
    // variant can't be built). Bind BEFORE SetConstants — push-constant stage
    // flags come from the bound pipeline's layout.
    void SetPipelineAuto(GraphicsPipelineId id) const;
    void SetPipelineAuto(ComputePipelineId id) const;

  private:
    friend class RGFrame;
    const RGFrame* m_Frame = nullptr;
    RGPassId m_Pass = kInvalidId;
    uint32_t m_AttFirst = 0; // this pass's range in RGFrame::m_Attachments
    uint32_t m_AttCount = 0;
    mutable PipelineFormatKey m_CachedKey{};
    mutable bool m_KeyDirty = true;
};

// ── Recording rules (pure; pinned by unit tests) ────────────────────────────
// Minimal queue-valid scope recorded in place of a scope that names another
// queue's stages. Two users, one rule: a cross-queue consumer barrier's SOURCE
// (the producer's stages are not valid masks on the consuming queue; ordering
// and availability come from the submission plan's timeline-semaphore wait),
// and an export barrier's DESTINATION when it records on a non-graphics tail
// (the contract consumer is next frame's graphics sampling, ordered by
// cross-frame sync). Outputs are engine PipelineStageMask/ResourceAccessMask
// bits.
void CrossQueueConsumerSrcScope(RGQueue consumerQueue, uint64_t& outStageMask,
                                uint64_t& outAccessMask);

// Pure core of the pipeline format-key derivation (device-free, unit-testable
// incl. the swapchain fallback the headless harness can't reach). Inputs are
// the pass's attachments with formats already resolved; out-of-range color
// slots are skipped (release-safe — mirrors the old context's clamp). The
// fallback applies ONLY when the pass binds no attachments at all.
struct RGAttachmentKeyInput
{
    uint32_t Slot = 0;
    bool IsDepth = false;
    TextureFormat Format{};
    uint32_t SampleCount = 1;
};
PipelineFormatKey DeriveFormatKey(const RGAttachmentKeyInput* atts, uint32_t count,
                                  TextureFormat swapchainFallback);

// ── Declaration rules (pure; pinned by unit tests) ──────────────────────────
// The backbuffer belongs to the graphics queue. The device owns its present
// choreography: the acquire wait is injected into the first GRAPHICS submit and
// the PRESENT_SRC transition is recorded there, so a pass that declares the
// backbuffer on compute or transfer runs unordered against the acquire — it may
// write an image the presentation engine has not released yet. The
// swapchain-recreate drain likewise reasons about which queues can reference
// swapchain-derived objects. Moving the final blit onto compute therefore means
// moving the acquire wait with it, not just changing the pass's queue.
// Returns the first offending pass, or kInvalidId when the graph is clean
// (including a kInvalidId backbuffer — headless declares none).
RGPassId FindNonGraphicsBackbufferAccess(const RGGraph& graph, RGResourceId backbuffer);

// ── The per-frame graph front end ───────────────────────────────────────────
class RGFrame
{
  public:
    // Transients idle longer than this are evicted; a hidden view's pooled
    // persistents age out after kPersistentMaxIdleFrames (~5 s at 60 fps).
    static constexpr uint64_t kTransientMaxIdleFrames = 8;
    static constexpr uint64_t kPersistentMaxIdleFrames = 300;

    // The pools may be SHARED across frame streams (multi-window); the upload
    // ring may NOT — BeginFrame rotates/rewinds it, so each RGFrame needs its
    // own ring or declaration-time allocs get stomped (see RGUploadRing.h).
    RGFrame(IDevice* device, RGResourcePool* persistentPool, RGTransientPool* transientPool,
            RGUploadRing* uploadRing);
    ~RGFrame();
    RGFrame(const RGFrame&) = delete;
    RGFrame& operator=(const RGFrame&) = delete;

    // Logical→physical queue remap. Same-physical runs collapse into one
    // submission recorded and submitted on that physical family; cross-queue
    // semaphores/QFOT scope rewrites only apply across PHYSICAL queues.
    // DEFAULT IS COLLAPSED (kSingleQueueMap): one graphics submission per
    // frame — the design default; async compute is a deliberate opt-in via
    // kIdentityQueueMap (or a custom map) per frame driver. Tests pinning
    // true-async shapes set identity explicitly.
    void SetPhysicalQueueMap(const uint8_t physicalOfLogical[3]);

    // All logical queues aliased onto the graphics family (the default).
    static constexpr uint8_t kSingleQueueMap[3] = {0, 0, 0};
    // True async: each logical queue on its own family (opt-in).
    static constexpr uint8_t kIdentityQueueMap[3] = {0, 1, 2};

    void BeginFrame(uint64_t frameIndex);

    // Retire recorded CPU callbacks before their code or captured owners disappear.
    // Caller-thread only, outside Execute/recording. Waits this frame's submitted
    // work, then discards its declaration; subsequent Execute is inert until new
    // passes are declared. Does not advance the frame or reset GPU pools/upload ring.
    void DiscardRecordedPasses();

    // Transient (this frame only; realized from the transient pool AFTER cull —
    // a culled pass's resources are never allocated).
    RGTexture CreateTexture(const char* name, const TextureDesc& desc);
    RGBuffer CreateBuffer(const char* name, const BufferDesc& desc);

    // Cross-frame persistence from the pool (history, hi-Z, shadow atlas…),
    // imported at its tracked state, written back after Execute. Names are a
    // global keyspace: namespace per view ("SceneView.TAA.History").
    // Same-frame re-import of one name (same physical) returns the SAME
    // resource id — producer and consumer arms may both import without
    // coordinating. A desc CHANGE mid-frame is a misuse the dedup cannot
    // catch: the pool reallocs, the new physical misses the by-physical
    // scan, and a SECOND id is minted over the new physical (split hazard
    // state). Re-import a name only with the desc it was imported with.
    // The physical may carry more usage than `desc` states: the pool folds in
    // the transfer usage earlier frames derived from their declared copies of
    // this name (the same rule a transient gets in-frame), so an importer
    // never restates a copy in its desc.
    // outNeedsFreshInit (optional) reports that the physical is fresh — first
    // use, desc-change realloc (resize), the usage-widening realloc a copy or
    // RequireTransferUsage triggers, idle age-out of a hidden view, or a device
    // rebuild — so nothing this consumer previously wrote survives in it.
    // Pool memory is never zeroed and is recycled from evicted entries, so a
    // consumer that trusts prior contents (TAA / SSSR history) MUST treat true
    // as "history absent" rather than blending against undefined memory. The
    // arm persists across declared-but-abandoned frames; only an EXECUTED frame
    // that declared MarkPersistentTextureInitialized discharges it.
    RGTexture ImportPersistentTexture(const char* name, const TextureDesc& desc,
                                      bool* outNeedsFreshInit = nullptr);

    // Declare that this frame (re)writes the imported texture's full contents,
    // discharging its freshness arm once the frame actually executes. The
    // texture counterpart of AddBufferZeroInit: history owners call it for the
    // target they write each frame.
    void MarkPersistentTextureInitialized(RGTexture t);

    // Transfer usage the texture's physical must carry beyond what this
    // frame's declared copies derive: for a copy the graph cannot see when the
    // physical is realized — a graph-external consumer capturing this texture
    // in a LATER frame (a pipeline output read back by a screenshot). A
    // transient realizes with it this frame. A pool import's physical already
    // exists by the time anything can call this, so the pool carries the
    // requirement into the texture's next materialization — one realloc,
    // then a stable desc — which is why an output policy states it every
    // frame rather than a capture stating it once. An external import has no
    // pool to carry it and stays as its owner declared it.
    void RequireTransferUsage(RGTexture t, TextureUsage usage);
    // outNeedsZeroInit (optional) reports that the physical is fresh (first use
    // or desc-change realloc) and no EXECUTED frame has zero-filled it yet.
    // Pool memory is never zero-initialized and can be recycled from an
    // evicted entry — a consumer that trusts prior contents (cross-frame
    // history state) must AddBufferZeroInit whenever this reports true. The
    // arm persists across declared-but-abandoned frames; Execute discharges it
    // only for fills that actually recorded.
    RGBuffer ImportPersistentBuffer(const char* name, const BufferDesc& desc,
                                    bool* outNeedsZeroInit = nullptr);

    // Zero-fill of an imported buffer, as a declared CopyDst transfer write.
    // The pool's zero-init primitive: schedule it whenever the import reports
    // needs-zero-init so recycled pool memory can never leak stale contents
    // into a reader; the pool's arm is cleared only when a frame that declared
    // the fill executes. The buffer's usage must include TransferDst; the fill
    // covers the size rounded DOWN to 4 bytes (vkCmdFillBuffer granularity).
    void AddBufferZeroInit(RGBuffer b, const char* passName);

    // Externally-owned physical (swapchain image, UI-owned target). The caller
    // supplies the texture's CURRENT state truthfully. `format` feeds the
    // pipeline format key when the texture is used as an attachment; Unknown
    // falls back to a device query at key-build time (Vulkan backend supports
    // it). Typed deliberately — a positional mips/layers argument cannot land
    // in the format slot.
    // External imports DEDUP BY PHYSICAL HANDLE within a frame: re-importing
    // the same handle returns the SAME resource (first import's declaration
    // wins; a conflicting redeclaration logs in debug). Two ids for one
    // physical would mean two independent hazard states — a silent race.
    RGTexture ImportExternalTexture(const char* name, TextureHandle handle,
                                    ResourceState currentState,
                                    TextureFormat format = TextureFormat{}, uint32_t mips = 1,
                                    uint32_t layers = 1);
    // Externally-owned buffer (PerFrameWritePool atlas slots, GPUScene
    // buffers, draw-stream slots). Buffers carry no layout; the first touch
    // still gets the cross-frame execution dependency. Same dedup rule.
    // sizeBytes is the physical's size when the caller knows it; the graph
    // never needs it for scheduling, but a reader that does (readback, bounds
    // reporting) has no other way to learn it. Zero = unknown, as before.
    RGBuffer ImportExternalBuffer(const char* name, BufferHandle handle, uint64_t sizeBytes = 0);
    // A backend-owned TLAS slot. Same dedup rule, keyed by the slot, so the
    // builder and every reader that import it land on one resource.
    RGAccelerationStructure ImportAccelerationStructure(const char* name, TlasSlotHandle slot);

    // Import the device's currently-acquired swapchain image and mark it as
    // the frame's output sink. Call between IDevice::BeginFrame (which
    // acquires) and Execute. The DEVICE owns the present choreography — the
    // acquire-binary wait is injected into the first graphics QueueSubmit and
    // Present() records the PRESENT_SRC transition + present-ready signal —
    // so the graph only renders into it. Imported at a discarding state:
    // swapchain content never survives frames, so the first write must Clear
    // or DontCare, never Load. Invalid handle (headless / not acquired)
    // returns an invalid RGTexture and declares nothing.
    RGTexture ImportBackbuffer(const char* name = "Backbuffer");

    // True when t is this frame's imported backbuffer. Output policies that
    // stamp a sampling export layout (MarkOutput at ShaderReadOnly) must skip
    // it — the device presents it and owns its final PRESENT_SRC transition.
    bool IsBackbuffer(RGTexture t) const
    {
        return m_Backbuffer.IsValid() && t.Id == m_Backbuffer.Id;
    }

    // External sinks anchor culling (presented / sampled by the UI / read back).
    void MarkOutput(RGTexture t) { m_Graph.MarkExternal(t.Id); }
    void MarkOutput(RGBuffer b) { m_Graph.MarkExternal(b.Id); }
    // Output with a contractual final layout (replaces the old ExportTexture):
    // one trailing transition after the last access; the pool write-back then
    // carries it, so a UI sampling the pooled physical next frame sees
    // ShaderReadOnly without a graph-external transition.
    void MarkOutput(RGTexture t, RGImageLayout finalLayout)
    {
        m_Graph.MarkExternal(t.Id);
        m_Graph.SetExportLayout(t.Id, finalLayout);
    }

    // Explicit execution-order edge: `after` schedules after `before`. No
    // barrier, no keep-alive — see RGGraph::AddOrderingEdge. For passes whose
    // work has an order the graph can't see from declared accesses (e.g.
    // compute sims with manual barriers feeding a later consumer).
    void AddOrderingEdge(RGPass before, RGPass after)
    {
        m_Graph.AddOrderingEdge(before.Id, after.Id);
    }

    // CPU-written per-frame data: sub-allocate from the single upload ring.
    RGUploadRing::Alloc AllocUpload(uint64_t size, uint64_t align = 256)
    {
        return m_UploadRing->Allocate(size, align);
    }
    template <class T>
    struct TypedUpload
    {
        T* Ptr = nullptr;
        BufferHandle Buffer{};
        uint64_t Offset = 0;
        bool Valid() const { return Ptr != nullptr; }
    };
    template <class T>
    TypedUpload<T> AllocUpload()
    {
        static_assert(std::is_trivially_copyable_v<T>, "upload data must be trivially copyable");
        const RGUploadRing::Alloc a = m_UploadRing->Allocate(sizeof(T), alignof(T) > 256 ? alignof(T) : 256);
        return TypedUpload<T>{static_cast<T*>(a.Ptr), a.Buffer, a.Offset};
    }

    // Declare a pass. `setup` runs IMMEDIATELY (by-ref captures fine); `exec`
    // is stored — placement-new'd into the frame arena — and runs inside
    // Execute() in scheduled order. `exec` MUST capture by value.
    template <class SetupFn, class ExecFn>
    RGPass AddPass(const char* name, int32_t phase, SetupFn&& setup, ExecFn&& exec)
    {
        return AddPassImpl(name, phase, RGQueue::Graphics, std::forward<SetupFn>(setup),
                           std::forward<ExecFn>(exec));
    }
    template <class SetupFn, class ExecFn>
    RGPass AddComputePass(const char* name, int32_t phase, SetupFn&& setup, ExecFn&& exec)
    {
        return AddPassImpl(name, phase, RGQueue::Compute, std::forward<SetupFn>(setup),
                           std::forward<ExecFn>(exec));
    }

    // cull → schedule → realize → barriers → submission plan → run pass lambdas
    // → pool state write-back → pool maintenance.
    void Execute();

    // A2.4-P0-R (§D6.6): a fork-join "parallel for" the record loop uses to record
    // the RecordInSecondary passes' interiors on workers. Type-erased so the
    // low-level render-graph module stays decoupled from JobSystem (RGFrame can't
    // reach EngineCore either); the engine-layer host injects one backed by
    // WorkStealingThreadPool::Run + a participating Wait (never ParallelFor —
    // §A2.4-D3). It MUST run body(i) for every i in [0,count) and JOIN before
    // returning. Unset ⇒ serial fallback (byte-identical to GE_PARALLEL_RECORD off).
    using RecordParallelFn =
        std::function<void(uint32_t count, const std::function<void(uint32_t)>& body)>;
    void SetRecordParallel(RecordParallelFn fn) { m_RecordParallel = std::move(fn); }

    // Drain THIS graph's submitted GPU work (waits its queue timelines to the
    // last signaled values). For engine-owned external texture re-spec — NOT
    // device-wide WaitForIdle (MoltenVK multi-window drawable recycling).
    void WaitForPendingWork();

    // Completion token for this graph's last submitted work on `queue`
    // (replaces the old GetSubmissionToken — readback consumers poll
    // QueryGpuSyncToken or block WaitGpuSyncToken; no device stall).
    // Invalid when nothing has been submitted there, which reads as
    // GpuSyncStatus::Unknown — never as completion.
    IDevice::GpuSyncToken SubmissionToken(RGQueue queue = RGQueue::Graphics) const;

    // Recording statistics for the last Execute() (tests + MCP).
    struct FrameStats
    {
        uint32_t SubmissionsMade = 0;
        uint32_t BarrierBatchesEmitted = 0;
        uint32_t BarriersEmitted = 0;
        uint32_t BarriersHoisted = 0; // BottomOfPipe-sourced, batched once at submission front
        uint32_t RenderPassesBegun = 0;

        // A2 STEP-0 render-thread CPU brackets, wall-ms for the last Execute()
        // (reset with the rest of FrameStats in BeginFrame). The RG phases are
        // the pre-record serial floor; QueueSubmit + BarrierEmit are the record
        // floor A2.4 cannot remove (neither lands in RGPassTiming::CpuMs, which
        // times only the per-pass exec interior). get_render_stats stitches
        // these with the RenderServices sort/bucketer brackets + extraction.
        double CompileMs = 0.0;            // RGGraph::Compile (cull + resource lifetime)
        double ScheduleMs = 0.0;           // RGGraph::Schedule
        double GenerateBarriersMs = 0.0;   // RGGraph::GenerateBarriers (barrier COMPUTE)
        double BuildSubmissionPlanMs = 0.0; // RGGraph::BuildSubmissionPlan
        double QueueSubmitMs = 0.0;        // Σ IDevice::QueueSubmit across submissions
        double BarrierEmitMs = 0.0;        // Σ vkCmd*Barrier recording (hoist + per-pass + export)
    };
    const FrameStats& Stats() const { return m_Stats; }

    // ── Per-pass GPU profiling (device-owned query pool; results resolve when
    // the device frame slot returns ⇒ FramesInFlight latency). Contract: call
    // RGFrame::BeginFrame AFTER IDevice::BeginFrame — the pool caches the
    // slot's completed timestamps there; a misordering shows up as
    // ReadFailures, never a stall. ──
    struct RGPassTiming
    {
        // Copied at write time — graph names are arena-reset every frame while
        // results resolve FramesInFlight frames later.
        char Name[64] = {};
        // The per-frame dense RGPassId — stable ONLY within its frame (joins
        // against same-frame pass data; cross-frame consumers key on Name).
        uint32_t PassId = kInvalidId;
        int32_t Phase = 0;
        RGQueue Queue = RGQueue::Graphics;
        double CpuMs = 0.0; // record-side wall time of the pass bracket
        // GPU cost of the pass bracket, 0 until resolved (or on read failure).
        // What exactly it measures depends on the backend — see
        // TimingSemantics(). Under EncoderSpan it is an upper bound that may be
        // shared with other passes, so per-pass values do NOT sum to a frame
        // cost.
        double GpuSpanMs = 0.0;
        uint32_t BeginQuery = ~0u;
        uint32_t EndQuery = ~0u;
        // The GPU work units the two timestamps were sampled at. Passes with
        // an equal pair read one measurement (SpanShared). Both
        // IQueryPool::kInvalidTimestampUnit under PipelinePoint semantics.
        uint32_t BeginUnit = IQueryPool::kInvalidTimestampUnit;
        uint32_t EndUnit = IQueryPool::kInvalidTimestampUnit;
        // This span is the same measurement as another pass's this frame.
        bool SpanShared = false;
        // This pass's span is the one counted in
        // RGProfilingResolveStats::DistinctSpanGpuMs. Summing GpuSpanMs over
        // the passes with this set is the ONLY correct way for a consumer to
        // total a frame: the fold in ResolveProfiling is the single source of
        // that decision, so a second de-duplication rule cannot disagree with
        // the payload's total.
        bool SpanCounted = false;
    };
    struct RGProfilingResolveStats
    {
        uint32_t InvalidQueryIndices = 0; // pool cap exceeded at write time
        uint32_t ReadFailures = 0;        // result unavailable at resolve
        // end < begin on the raw timestamp pair, which is only the measurement
        // where the backend has no per-unit accounting for the span
        // (PipelinePoint). Under EncoderSpan the per-unit sum is what is
        // measured and each unit is clamped on its own, so encoders the GPU ran
        // out of order do not land here. Discarded.
        uint32_t NonMonotonic = 0;
        uint32_t ResolvedPasses = 0;
        // Passes whose span duplicates a measurement another pass already
        // reported, so ResolvedPasses - SharedSpanPasses == distinct spans.
        // (RGPassTiming::SpanShared flags every member of such a group,
        // including the one counted here.)
        uint32_t SharedSpanPasses = 0;
        // Σ over DISTINCT measurements, not over passes — a shared span counts
        // once. Under EncoderSpan the distinct spans still overlap each other on
        // the GPU, so this is an upper bound on the frame's GPU cost, never a
        // busy total.
        double DistinctSpanGpuMs = 0.0;
    };
    // CONSTRAINT: one profiled RGFrame per device frame stream. The pending
    // ring keys on the GLOBAL device frame index, which multi-window editors
    // advance once per window — a second profiled graph (or window) on the
    // same device cross-attributes/loses timings. Inherited from the old
    // graph verbatim; the real fix (the pool exposing per-collection
    // generations) is a 2c-6/2e device-layer item.
    void SetProfilingEnabled(bool enabled); // forced off when GE_ENABLE_GPU_PROFILING=0
    bool ProfilingEnabled() const { return m_ProfilingEnabled; }
    // What a GpuSpanMs interval measures on this device. A property of the
    // device's query pool, so it is the same answer before the first resolve
    // and while disarmed.
    TimestampSemantics TimingSemantics() const;
    // Entries are in scheduled submission order (the order passes ran).
    std::vector<RGPassTiming> LastFrameTimings() const;   // copies under mutex (MCP reads cross-thread)
    RGProfilingResolveStats LastResolveStats() const;
    // GPU timestamp resolve for one pending slot. `qp` may be null: browser
    // WebGPU has no encoder writeTimestamp, so CPU-only rows keep GpuSpanMs = 0
    // and expected ~0u query indices are not counted as InvalidQueryIndices
    // (that stat is "pool cap exceeded at write time"). DRS ignores samples
    // unless ResolvedPasses > 0, so CPU-only must not fake a GPU sample.
    // Not static: the work-unit fold uses the m_ScratchSpan* members.
    RGProfilingResolveStats ResolvePassGpuTimestamps(std::vector<RGPassTiming>& pend,
                                                     IQueryPool* qp);

    // Introspection (tests + MCP).
    IDevice* Device() const { return m_Device; }
    const RGGraph& Graph() const { return m_Graph; }
    // Resolve a resource declared this frame by its exact name, KIND-CHECKED:
    // a name that names a buffer never returns a valid RGTexture (and vice
    // versa), so a caller cannot mint a typed handle over the wrong kind — the
    // silent type confusion the typed handles exist to prevent. Invalid when
    // the name was not declared this frame. Names are not unique by
    // construction (per-view namespacing is a convention, not a rule); the
    // FIRST declaration wins, and a caller that must disambiguate enumerates
    // Graph() itself. Linear scan — declaration-scope diagnostics only, never
    // a per-pass path.
    RGTexture FindTexture(const char* name) const;
    RGBuffer FindBuffer(const char* name) const;
    // The index passed to the last BeginFrame. RGFrame pointers are stream
    // identity but frames are re-begun every app frame (and mid-app-frame by
    // passive RenderSingle paths) — callers caching per-frame state must key
    // on (RGFrame*, FrameIndex) so a stale incarnation never validates.
    uint64_t FrameIndex() const { return m_FrameIndex; }
    // How many times this frame stream has been executed with at least one
    // live pass — recorded and submitted — since construction. Declare-side
    // state that may only become a later frame's history once its own frame
    // was submitted (the per-view temporal sample) stamps this when it stores
    // and compares on its next declare: a value that has not moved means the
    // frame it stored in was declared and then abandoned before Execute, or
    // every pass of it was culled, and nothing it described reached a pixel.
    uint64_t SubmittedFrameCount() const { return m_SubmittedFrameCount; }
    const std::vector<RGAttachmentRec>& Attachments() const { return m_Attachments; }
    // The exact RenderPassDesc the recording loop hands the backend for a
    // contiguous attachment range — the RECORDING-level test seam (the
    // declaration-level Attachments() recs cannot prove a sub-range view
    // actually reaches the device; the per-layer cascade contract depends
    // on it).
    void BuildRenderPassDesc(uint32_t attFirst, uint32_t attCount, RenderPassDesc& out) const;
    TextureHandle PhysicalTexture(RGTexture t) const
    {
        return TextureHandle(PhysicalOf(t.Id));
    }
    BufferHandle PhysicalBuffer(RGBuffer b) const { return BufferHandle(PhysicalOf(b.Id)); }

  private:
    void ResetRecording();
    friend class RGPassBuilder;
    friend class RGContext;

    template <class SetupFn, class ExecFn>
    RGPass AddPassImpl(const char* name, int32_t phase, RGQueue queue, SetupFn&& setup,
                       ExecFn&& exec)
    {
        RGPassDesc d;
        d.Name = name;
        d.Phase = phase;
        d.Queue = queue;
        const RGPassId id = m_Graph.AddPass(d);
        RGPassBuilder b;
        b.m_Frame = this;
        b.m_Pass = id;
        setup(b);
        // Decision (b): templated placement-new into the frame arena — the
        // concrete closure type, never std::function (no per-pass heap).
        using F = std::decay_t<ExecFn>;
        F* fn = m_Graph.Arena().New<F>(std::forward<ExecFn>(exec));
        m_Exec.push_back(ExecThunk{&InvokeThunk<F>, fn});
        return RGPass{id};
    }

    template <class F>
    static void InvokeThunk(void* fn, RGContext& ctx)
    {
        (*static_cast<F*>(fn))(ctx);
    }

    uint64_t PhysicalOf(RGResourceId id) const
    {
        // Loud in release too: a transient queried before Execute's
        // realization (or a culled/stale id) returns a null handle with an
        // error log instead of silently handing out garbage. Imports and
        // pool resources have physicals from declaration; TRANSIENTS only
        // after Execute realizes them.
        if (id >= m_HasPhysical.size() || !m_HasPhysical[id])
            return MissingPhysical(id);
        return m_Physical[id];
    }
    void SetPhysical(RGResourceId id, uint64_t raw)
    {
        if (m_Physical.size() <= id)
        {
            m_Physical.resize(id + 1, 0);
            m_HasPhysical.resize(id + 1, 0);
        }
        m_Physical[id] = raw;
        m_HasPhysical[id] = 1;
    }
    // Cold error path for PhysicalOf (asserts in debug, logs in release).
    uint64_t MissingPhysical(RGResourceId id) const;

    struct ExecThunk
    {
        void (*Invoke)(void*, RGContext&);
        void* Fn;
    };
    struct PendingTex
    {
        RGResourceId Id;
        TextureDesc Desc;
    };
    struct PendingBuf
    {
        RGResourceId Id;
        BufferDesc Desc;
    };
    struct RequiredTransferUsage
    {
        RGResourceId Id;
        uint32_t Usage;
    };
    struct ImportRec
    {
        RGResourceId Id;
        RGResourceKind Kind;
        bool PoolBacked; // false = external (no write-back)
    };
    // Dedup scan by physical handle within one kind (linear — imports are few
    // per frame). One physical = one resource = one hazard state.
    const ImportRec* FindImport(uint64_t physical, RGResourceKind kind) const;

    // ── Command recording / submission (RGRecord.cpp) ──
    void RecordAndSubmit();
    CommandList* GetOrCreateCommandList(uint8_t physicalQueue);
    void EnsureTimeline(uint8_t physicalQueue);
    void ResolveProfiling(); // drains the returning slot's pending timings (RGRecord.cpp)

    // A2.4-P0-R fork helpers (RGRecord.cpp). MarkRecordSecondary is the
    // RGPassBuilder::RecordInSecondary seam; RecordsSecondaryPass is the record
    // loop's per-pass query. Phase R1 records each flagged pass's interior into a
    // worker secondary; Phase R2 executes them. Both no-op when the pool is null.
    void MarkRecordSecondary(RGPassId p);
    bool RecordsSecondaryPass(RGPassId p) const
    {
        return p < m_PassRecordSecondary.size() && m_PassRecordSecondary[p] != 0;
    }
    // Records the flagged raster passes into per-worker secondaries (Phase R1).
    // Returns the count forked (0 ⇒ serial path unchanged in RecordAndSubmit).
    uint32_t RecordSecondariesParallel();

    IDevice* m_Device = nullptr;
    RGResourcePool* m_PersistentPool = nullptr;
    RGTransientPool* m_TransientPool = nullptr;
    RGUploadRing* m_UploadRing = nullptr;
    uint64_t m_FrameIndex = 0;
    uint64_t m_SubmittedFrameCount = 0;

    RGGraph m_Graph;
    std::vector<ExecThunk> m_Exec; // indexed by pass id
    std::vector<PendingTex> m_PendingTex;
    std::vector<PendingBuf> m_PendingBuf;
    std::vector<ImportRec> m_Imports;
    // Pool buffers whose zero-fill pass this frame declared; Execute clears
    // their pool arm (MarkBufferZeroFilled) — an abandoned frame leaves the
    // arm set so the next importer re-schedules the fill.
    std::vector<RGResourceId> m_ZeroInitBufferIds;
    std::vector<RGResourceId> m_InitializedTextureIds;
    // RequireTransferUsage's statements for this frame, joined to the derived
    // transfer usage at Execute: transients realize with it, pool imports
    // carry it to their next materialization.
    std::vector<RequiredTransferUsage> m_RequiredTransferUsage;
    std::vector<RGAttachmentRec> m_Attachments;
    std::vector<uint64_t> m_Physical; // raw device handle per resource id
    std::vector<uint8_t> m_HasPhysical;
    RGTexture m_Backbuffer{}; // this frame's ImportBackbuffer result (IsBackbuffer)

    // Recording state (persistent CLs per logical queue; Begin() re-acquires a
    // fresh buffer per submission) + per-physical-queue timeline semaphores.
    std::unique_ptr<CommandList> m_CmdLists[kQueueCount];
    // Device rebuild generation this frame's cross-frame GPU state (cached command
    // lists + the persistent resource pool) was last valid under (Q6 slice 4). A
    // mismatch in BeginFrame means an in-place rebuild freed the command pools and
    // every pooled texture/buffer; both caches are dropped so fresh resources bind
    // to the rebuilt device.
    uint64_t m_DeviceRebuildGen = 0;
    SemaphoreHandle m_QueueTimelines[kQueueCount]{};
    bool m_TimelineCreated[kQueueCount] = {false, false, false};
    uint64_t m_LastSignaled[kQueueCount] = {0, 0, 0}; // per physical queue (WaitForPendingWork)
    uint8_t m_PhysicalQueueOf[3] = {0, 0, 0}; // kSingleQueueMap (the design default)
    FrameStats m_Stats;
    // Profiling state. m_ProfilingCurrent is record-thread-only; pending slots
    // are keyed by the DEVICE frame index (the query pool's latency ring);
    // latest/stats are published under the mutex for cross-thread readers.
    bool m_ProfilingEnabled = false;
    std::vector<RGPassTiming> m_ProfilingCurrent;
    std::vector<std::vector<RGPassTiming>> m_ProfilingPendingBySlot;
    std::vector<RGPassTiming> m_ProfilingLatest;
    RGProfilingResolveStats m_LastResolveStats;
    bool m_LoggedFirstGpuResolve = false;
    // Work-unit ranges already counted this resolve, and the widest-first visit
    // order over them (keeps the fold allocation-free).
    std::vector<uint64_t> m_ScratchSpanUnits;
    std::vector<uint32_t> m_ScratchSpanOrder;
    mutable std::mutex m_ProfilingMutex;
    // Recording scratch (capacity retained across frames).
    std::vector<ResourceBarrier> m_ScratchHoist;
    std::vector<ResourceBarrier> m_ScratchPassBarriers;
    std::vector<std::pair<uint32_t, uint32_t>> m_ScratchAttRange; // per pass (first,count)
    std::vector<const RGBarrierBatch*> m_ScratchBatchOfSched;
    std::vector<CommandList*> m_ScratchSubmitLists;
    std::vector<std::pair<SemaphoreHandle, uint64_t>> m_ScratchWaits;
    std::vector<std::pair<SemaphoreHandle, uint64_t>> m_ScratchSignals;

    // ── A2.4-P0-R parallel record ──
    // Injected per-frame by the host; empty ⇒ serial fallback.
    RecordParallelFn m_RecordParallel;
    // Per-pass RecordInSecondary opt-in (indexed by pass id; reset each frame).
    std::vector<uint8_t> m_PassRecordSecondary;
    // Worker-recorded secondary command lists (indexed by pass id; a null slot =
    // that pass records inline). Kept alive from Phase R1 through submit; retired
    // to the device after the last QueueSubmit, then cleared next BeginFrame.
    std::vector<std::unique_ptr<CommandList>> m_Secondary;
};

} // namespace GameEngine::Rendering::RenderGraph
