#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Frustum.h"
#include "Rendering/Common/Math.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/PrevVisibleResetTracker.h"
#include "Rendering/Core/RecomputeElision.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

namespace GameEngine
{
namespace Rendering
{

class IDevice;
class GPUScene;
struct GPUCullingData;

struct GPUCullingConfig
{
    bool enableFrustumCulling = true;
    bool enableOcclusionCulling = false;
    uint32_t maxCullingThreads = 64;
};

// Sentinel cascadeIndex value for a non-shadow view (main camera, thumbnails,
// picking, etc.). Real cascades use 0..3 (CSM cap is 4). Mirrors the
// GPUDrawStreamBuilder::kCascadeIndexNone constant — drift between them would
// silently desync culling and bucketer dispatches.
inline constexpr uint8_t kCullingCascadeIndexNone = 0xFFu;

// Camera-relative frustum-plane translation (Earth-scale precision). Adds
// n*origin to each inward-pointing plane's offset so the GPU can test
// (boundingCenter - origin) in small magnitudes: dot(center, n) + d is
// big-minus-big at planetary distance (both ~1e6+) and collapses into garbage
// that culls everything, while the shader's small-minus-small dot on
// (center - origin) with the translated offset recovers the true distance.
// Mathematically identical to the world-space test, and a bit-for-bit no-op
// when origin is (0,0,0) (the dark-ship gate). Must stay paired with the shader
// subtracting the same origin (frustum_culling.comp / hzb_culling.comp). `planes`
// is a run of 6 inward-pointing, CPU-normalized planes from ExtractFrustumPlanes.
void MakeFrustumPlanesCameraRelative(Vector4* planes, const Vector3& origin);

// Maximum cascades a single fused culling dispatch can fan out into. Must
// match `kMaxViewsPerCullingDispatch` in GPUScene.h (which sizes
// `GPUCullingData::frustumPlanes`) and `kMaxShadowCascades` in
// ShadowMapRenderFeature.h. A static_assert in GPUCulling.cpp pins the
// GPUScene-side value to this one.
inline constexpr uint32_t kMaxCullingViewsPerDispatch = 4;

// CPU-side description of per-view culling inputs. This is intentionally
// close to GPUCullingData but keyed by ViewId so GPUCullingPipeline can
// remain agnostic of ECS and RenderGraph details while still being
// multi-view aware.
struct ViewCullingInput
{
    ViewId viewId = 0;
    // Shadow-cascade culling reuses the parent view's ViewId but distinguishes
    // its slice via this cascade index (0..3, or kCullingCascadeIndexNone for
    // non-cascade views). One ViewCullingInput per (viewId, cascadeIndex)
    // pair is submitted by ScheduleViewCullingDispatches (main views via
    // ICullingStrategy, cascades via ShadowMapRenderFeature's
    // OnScheduleCulling hook); the pipeline allocates an independent
    // visibility-buffer slice per pair.
    uint8_t cascadeIndex = kCullingCascadeIndexNone;
    Matrix4x4 viewMatrix;
    Matrix4x4 projMatrix;
    Matrix4x4 viewProjMatrix;
    Vector4 frustumPlanes[6];
    Vector3 cameraPosition;
    float nearPlane = 0.1f;
    Vector3 cameraForward;
    float farPlane = 1000.0f;
    // Camera-relative culling origin (Earth-scale precision): the view's render
    // origin in world meters (camera sector * sector size), or (0,0,0) when the
    // origin is inactive. The frustum test is done as dot(center - origin, n)
    // with plane offsets translated by n*origin, so a planetary boundingCenter
    // and its plane offset stay small-magnitude instead of the big-minus-big
    // collapse of dot(center, n) + d that culls everything. Origin (0,0,0) makes
    // the whole transform a no-op => byte-identical to the world-space test.
    Vector3 cameraRelativeOrigin{0.0f, 0.0f, 0.0f};
    // Per-view candidate set: [firstInstance, firstInstance + instanceCount)
    // within the GPUScene instance buffer. This allows different views to
    // cull different subranges of the global instance set while sharing the
    // same GPUScene backend.
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t renderLayerMask = 0xFFFFFFFFu;
    uint32_t frameIndex = 0;
    float deltaTime = 0.0f;
    // Two-phase HZB occlusion (R2.1): reserve a SECOND visibility slice for
    // this view in the frame's layout — published as a phase-B
    // ViewVisibilityRange but NOT dispatched by EndFrameRG. The deferred
    // ScheduleOcclusionCullPass() dispatches into it mid-pipeline, after the
    // phase-A raster its HZB input derives from.
    bool reserveOcclusionSlice = false;
};

// CPU-side description of a *group* of related views (a shadow view's
// cascades, a reflection probe's cube faces) sharing one candidate set.
// Producers (e.g. ShadowMapRenderFeature) submit one of these instead of N
// independent ViewCullingInputs; the GPUCullingPipeline collapses the group
// into a single compute dispatch using the `kViewCount=N` PSO variant. Empty
// cascades (no overlapping geometry) should be filled with degenerate frustum
// planes so every sphere test fails.
struct CascadeCullingGroup
{
    ViewId viewId = 0;
    uint32_t renderLayerMask = 0xFFFFFFFFu;
    // 1..kMaxCullingViewsPerDispatch; selects the PSO variant.
    uint32_t cascadeCount = 0;
    // Slice c of the group publishes its ViewVisibilityRange at cascadeIndex
    // cascadeIndexBase + c. 0 = the directional cascades 0..3; a producer
    // that fans a non-shadow view out (probe faces) bases its slices in its
    // own index block so they never collide with the view's cascade-None
    // slice or the shadow families.
    uint8_t cascadeIndexBase = 0;
    // true = every slice is a shadow-map dispatch: non-casters are dropped
    // and slices 1..3 run the tight cull margin (distant cascades). false =
    // the slices are color views fanned out for one dispatch: everything
    // stays visible and every slice keeps the conservative margin, exactly
    // as the single-view (cascadeIndexNone) dispatch does.
    bool shadowCasterDispatch = true;
    // Per-cascade light view-projection matrices. Currently unused by the
    // culling shader (cluster_cull tests against frustum planes only) but
    // kept here so downstream consumers can mirror what the bucketer expects.
    Matrix4x4 lightVP[kMaxCullingViewsPerDispatch];
    // Per-cascade frustum planes [c][L,R,B,T,N,F]. Slots [cascadeCount..]
    // are ignored by the selected PSO variant.
    Vector4 frustumPlanes[kMaxCullingViewsPerDispatch][6];
    Vector3 cameraPosition;
    float nearPlane = 0.1f;
    Vector3 cameraForward;
    float farPlane = 1000.0f;
    // Camera-relative culling origin shared by every cascade in the group — the
    // parent view's render origin (0,0,0 when inactive). See
    // ViewCullingInput::cameraRelativeOrigin.
    Vector3 cameraRelativeOrigin{0.0f, 0.0f, 0.0f};
    // Candidate set shared by every cascade in the group.
    uint32_t firstInstance = 0;
    uint32_t instanceCount = 0;
    uint32_t frameIndex = 0;
    float deltaTime = 0.0f;
};

// Describes how a view maps into the underlying visibility buffer. Every
// submitted view is assigned a distinct
// `[visibilityOffset, visibilityOffset + visibilityCount)` slice within a
// shared GPUScene visibility buffer, and each slice is written by a dedicated
// GPUScene culling pass using that view's own culling data and candidate set.
// `visibilityCount` currently reflects the per-view candidate count (the size
// of the per-view flag slice), not a compacted list of visible instance
// indices.
struct ViewVisibilityRange
{
    ViewId viewId = 0;
    // Mirrors ViewCullingInput::cascadeIndex. kCullingCascadeIndexNone for
    // main-view / thumbnail / picking slices; 0..3 for shadow cascades. The
    // bucketer dispatch and shadow-cascade depth pass look up the matching
    // slice by (viewId, cascadeIndex) instead of viewId alone so cascade 0's
    // visibility doesn't shadow (pun intended) the main view's slice.
    uint8_t cascadeIndex = kCullingCascadeIndexNone;
    // 0 = phase A (the frame's first culling generation — the only one for
    // frustum-only views); 1 = phase B (the HZB recovery generation, written
    // by the deferred occlusion dispatch). Consumers matching
    // (viewId, cascadeIndex) MUST also match the phase or a reserved phase-B
    // slice aliases the view's phase-A slice.
    uint8_t slicePhase = 0;
    uint32_t visibilityOffset = 0;
    uint32_t visibilityCount = 0;
};

class GPUCullingPipeline
{
  public:
    GPUCullingPipeline(IDevice* device, const GPUCullingConfig& config);
    ~GPUCullingPipeline();

    // Non-copyable and non-movable: owns GPU buffer lifetime state and is
    // always heap-owned via unique_ptr, so it never needs to relocate by value.
    GPUCullingPipeline(const GPUCullingPipeline&) = delete;
    GPUCullingPipeline& operator=(const GPUCullingPipeline&) = delete;

    GPUCullingPipeline(GPUCullingPipeline&&) = delete;
    GPUCullingPipeline& operator=(GPUCullingPipeline&&) = delete;

    // Multi-view aware API: begin a culling frame, register one or more
    // views with per-view culling inputs, then end the frame to schedule one
    // GPU culling pass per view. Each pass writes per-instance visibility
    // flags into that view's slice of a shared visibility buffer, consumed
    // GPU-side by the draw-stream scatter (never read back to the CPU).
    void SubmitView(const ViewCullingInput& input);
    // Cascade-group submit: schedules ONE compute dispatch using the
    // kViewCount=cascadeCount PSO variant, writing into `cascadeCount`
    // visibility-buffer slices. Each cascade appears as its own
    // ViewVisibilityRange entry so the bucketer/depth pass can address it
    // via (viewId, cascadeIndex) lookups.
    void SubmitCascadeGroup(const CascadeCullingGroup& group);
    void EndFrame();

    // Phase 6-iii: per-runtime visibility buffer + aggregate pass. The
    // aggregate compute zeros the buffer, then atomic-ORs anyViewVisible[i]
    // into runtimeVisible[GPUInstance.runtimeId]. animation_skinning.comp
    // reads runtimeVisible[inst.runtimeId] and early-outs when 0. Capacity
    // is bounded by GetRuntimeVisibleCapacity() — runtime ids beyond that
    // cap are skipped (skinning shader matches the bound).
    uint32_t GetRuntimeVisibleCapacity() const { return kRuntimeVisibleCapacity; }

    // ── Immediate-mode frame: BeginFrame(RGFrame*) routes the shared
    // EndFrame() to the declaration path. SubmitView/SubmitCascadeGroup are
    // declaration-order agnostic. ──
    void BeginFrame(RenderGraph::RGFrame* frame, GPUScene* scene);
    // Frame-local values (valid only for the RGFrame they were declared into):
    RenderGraph::RGBuffer GetVisibilityRG() const { return m_FrameVisRG; }        // post-EndFrame
    RenderGraph::RGBuffer GetAnyViewVisibleRG() const { return m_FrameAnyVisRG; } // post-Union
    RenderGraph::RGBuffer GetRuntimeVisibleRG() const { return m_FrameRuntimeVisRG; }
    // Import (lazily creating) the runtime-visible buffer for this frame —
    // called BEFORE the skinning pass declares so it can Read the value; the
    // aggregate pass reuses the same id (the declared WAR pins skinning first).
    RenderGraph::RGBuffer ImportRuntimeVisible(RenderGraph::RGFrame& frame);
    // Re-import the unified visibility buffer into a NON-OWNER frame stream
    // (slice 8a multi-window): same physical + name as EndFrameRG's owner
    // import (owner-frame calls dedup to the id EndFrameRG minted). Never
    // mutates m_FrameVisRG. Invalid until the first EndFrameRG created the
    // physical; callers gate on LastPublishArm()==RenderGraph for content freshness.
    RenderGraph::RGBuffer ImportVisibility(RenderGraph::RGFrame& frame);
    void ScheduleVisibilityUnion(RenderGraph::RGFrame& frame, GPUScene* scene);
    void ScheduleRuntimeVisibilityAggregate(RenderGraph::RGFrame& frame, GPUScene* scene);
    // Deferred phase-B (mode 2) dispatch into the slice EndFrameRG reserved for
    // `viewId` (ViewCullingInput::reserveOcclusionSlice). The R2.1 seam: called
    // MID-pipeline (between the phase-A raster and the phase-B scatter), not
    // from the EndFrameRG terminal batch. `hzb` is the freshly-built View.HZB
    // pyramid and `hzbMipCount` its level count; the dispatch runs the real
    // occlusion test (frustum ∧ HZB) and rewrites prevVisible per §5-A2. The
    // graph edges are real: this pass WRITES the visibility buffer the phase-A
    // scatter READ (WAR pins it after), READS the HZB (RAW after HZBBuild), and
    // the phase-B scatter's RAW read orders after it. No-op when the reservation
    // is absent, the HZB is invalid, or the HZB cull pipeline is unavailable
    // (phase B then leaves its slice as the phase-A pass left it).
    // One dispatch per reservation per frame; repeat calls no-op. Owner-frame
    // only (same bail rule as the union/aggregate epilogue passes).
    void ScheduleOcclusionCullPass(RenderGraph::RGFrame& frame, GPUScene* scene, ViewId viewId,
                                   RenderGraph::RGTexture hzb, uint32_t hzbMipCount);

    // Host-installed loader for the visibility_union.shaderpkg bytes.
    // Mirrors GPUScene::SetCullingShaderLoader — renderer-core does no
    // file I/O. Runtime code wires this to the AssetManager.
    using VisibilityUnionShaderLoaderFunc = std::vector<uint8_t> (*)(const char* name);
    static void SetVisibilityUnionShaderLoader(VisibilityUnionShaderLoaderFunc loader);

    // Phase 6-iii: same pattern for the runtime_visibility_aggregate shader.
    using RuntimeVisibilityShaderLoaderFunc = std::vector<uint8_t> (*)(const char* name);
    static void SetRuntimeVisibilityShaderLoader(RuntimeVisibilityShaderLoaderFunc loader);

    // R2.1 P2: loader for hzb_culling.shaderpkg (the two-phase occlusion cull
    // PSO). Same pattern; wired to the AssetManager by RenderServices.
    using HzbCullingShaderLoaderFunc = std::vector<uint8_t> (*)(const char* name);
    static void SetHzbCullingShaderLoader(HzbCullingShaderLoaderFunc loader);

    // The instance slots GPUScene recycled during THIS frame's EndFrameRG drain.
    // GPUScene's list is drained destructively exactly once, here, so any second
    // per-(instance, view) history that needs the same invalidation (the draw
    // stream's LOD crossfade state) reads the set from this accessor rather than
    // draining again. Valid from EndFrameRG until the next one; empty in the
    // steady state (no removes -> no recycled adds).
    const std::vector<uint32_t>& RecycledSlotsThisFrame() const
    {
        return m_RecycledSlotsThisFrame;
    }

    // Eventually-consistent per-slice occlusion stats (host-visible; values lag
    // by up to frames-in-flight, same caveat as ReadScatterStats). Accumulated
    // by the mode-2 phase-B dispatch; replaces the D6-deleted CPU visibility
    // metric for the P3 gates. Zero until the first phase-B dispatch runs.
    struct OcclusionStats
    {
        uint32_t frustumPassed = 0;  // instances that passed the frustum test
        uint32_t hzbCulled = 0;      // frustum-passers the HZB occluded (f ∧ ¬h)
        uint32_t recovered = 0;      // phase-B draws (visible ∧ not-drawn-in-A)
        uint32_t prevVisibleNew = 0; // instances written into next frame's prevVisible
    };
    OcclusionStats ReadOcclusionStats() const;

    // ── Idle recompute elision (frame-attribution lever #2) ──
    // The engine injects the frame's scene-content epochs + the resolved
    // kill-switch before EndFrame; EndFrameRG then byte-records its complete
    // dispatch input set (per-slice culling data minus the shader-unused
    // frameIndex/deltaTime, slice layout, physical identities, epochs) and,
    // when the record has been identical for kElisionSettleFrames consecutive
    // frames, skips every culling dispatch declaration this frame: the
    // persistent visibility buffer already holds the exact bits a re-run
    // would write. The layout + range publication still run (deterministic
    // CPU, consumers re-read per frame), phase-B reservations are marked
    // consumed so ScheduleOcclusionCullPass no-ops, and the union/aggregate
    // epilogue passes skip with the same decision. Pending prevVisible resets
    // force a recompute (the reset fill must be declared before any skip).
    void SetElisionFrameContext(const ElisionFrameContext& ctx) { m_ElisionCtx = ctx; }
    // Advances whenever a pass that writes the visibility buffer is DECLARED
    // (EndFrameRG dispatches, deferred phase-B occlusion). The scatter gates
    // key on it: an unchanged epoch proves the visibility content the scatter
    // would re-read is the content its retained records were built from.
    uint64_t GetVisibilityWriteEpoch() const { return m_VisibilityWriteEpoch; }
    bool WasElidedThisFrame() const { return m_ElidedThisFrame; }
    const RecomputeElisionGate::Stats& GetElisionStats() const { return m_ElisionGate.GetStats(); }

    // Which arm last PUBLISHED ranges (old EndFrame vs EndFrameRG).
    // Visibility-range offsets only pair with the SAME arm's visibility
    // buffer — a bucketer consuming ranges published by the other arm would
    // bind out-of-range offsets against its own buffer. None until the first
    // publication of a frame (BeginFrame of either arm resets).
    enum class PublishArm : uint8_t
    {
        None,
        RenderGraph
    };
    PublishArm LastPublishArm() const { return m_LastPublishArm; }
    const std::vector<ViewVisibilityRange>& GetViewVisibilityRanges() const { return m_ViewVisibilityRanges; }

    // Q6 device-lost re-provision (design §8 HZB row + §4 one-shot-guard sweep).
    // The in-place device rebuild freed every GPU object this pipeline owned (the
    // lazily-compiled union/runtime/HZB PSOs, the HZB sampler + sentinel, the
    // GPU-only visibility/occlusion buffers, and the per-view HZB history). Drop
    // the dead handles (WITHOUT Destroy* — the GPU objects are already gone) and
    // reset the one-shot Ensure* guards so each recompiles/recreates on next use
    // against the warm-reloaded backend pipeline cache. m_PrevVisible is cleared so the
    // per-view occlusion history re-imports all-visible (one frame of over-draw,
    // not a correctness bug — the history is designed to self-heal).
    void ReprovisionAfterDeviceRebuild();

  private:
    IDevice* m_Device = nullptr;
    GPUCullingConfig m_Config{};

    // Internal description of how each submitted view maps into the shared
    // visibility buffer. This is a staging structure on the path toward true
    // per-view visibility buffers: for now it is used only to size the
    // pipeline-owned visibility buffer conservatively across all views.
    struct PerViewVisibilityInfo
    {
        ViewId viewId = 0;
        uint32_t offset = 0;   // Planned start index within the visibility buffer
        uint32_t capacity = 0; // Planned per-view capacity in elements
    };


    GPUScene* m_CurrentScene = nullptr;
    std::vector<ViewCullingInput> m_PendingViews;
    // Phase-B slices laid out (and ranges published) by EndFrameRG but
    // dispatched later by ScheduleOcclusionCullPass. `consumed` guards
    // double-dispatch into the same slice.
    struct OcclusionSliceReservation
    {
        ViewCullingInput input{};
        uint32_t visibilityOffset = 0;
        uint32_t sliceInstanceCount = 0;
        bool consumed = false;
    };
    std::vector<OcclusionSliceReservation> m_OcclusionReservations;
    uint32_t m_FrameVisTotalBytes = 0; // EndFrameRG's buffer size, for deferred dispatch
    // Cascade-group submissions pending until EndFrame. Each one fans into
    // `cascadeCount` contiguous visibility-buffer slices via a single
    // dispatch of the kViewCount=N PSO variant.
    std::vector<CascadeCullingGroup> m_PendingCascadeGroups;
    std::vector<ViewVisibilityRange> m_ViewVisibilityRanges;
    std::vector<PerViewVisibilityInfo> m_ViewVisibilityLayout;

    PipelineHandle m_VisibilityUnionPipeline = INVALID_PIPELINE_HANDLE;
    bool m_VisibilityUnionPipelineAttempted = false;
    bool m_VisibilityUnionLoadFailureLogged = false;
    bool CreateVisibilityUnionPipeline();

    // Phase 6-iii: per-runtime visibility flags + aggregate pipeline.
    // kRuntimeVisibleCapacity is the hard cap on SkeletonStore runtime ids the
    // gate can index (4096 fits comfortably in a small scene; expand if a
    // future workload pushes past it). The shader bounds-checks against this
    // value and the skinning consumer uses the same cap to decide whether to
    // gate at all (out-of-range ids fall back to "always animate").
    static constexpr uint32_t kRuntimeVisibleCapacity = 4096;
    PipelineHandle m_RuntimeVisibilityPipeline = INVALID_PIPELINE_HANDLE;
    bool m_RuntimeVisibilityPipelineAttempted = false;
    bool m_RuntimeVisibilityLoadFailureLogged = false;
    bool CreateRuntimeVisibilityPipeline();

    PublishArm m_LastPublishArm = PublishArm::None;

    // ── Idle recompute elision state ──
    // Consecutive identical frames required before the first skip. Two are the
    // proven minimum (one executed frame with identical inputs makes the
    // one-frame-lagged GPU feedback — prevVisible, SDSM-fed cascade fits —
    // stationary by induction); the third is deliberate margin.
    static constexpr uint32_t kElisionSettleFrames = 3;
    RecomputeElisionGate m_ElisionGate;
    ElisionFrameContext m_ElisionCtx{};
    uint64_t m_ElisionFrameStamp = 0;     // ticks per BeginFrame (the gate's stamp)
    uint64_t m_VisibilityWriteEpoch = 0;
    bool m_ElidedThisFrame = false;
    bool m_ElisionLogState = false;       // transition-edge logging latch
    // Byte-record EndFrameRG's complete dispatch input set into `blob`.
    // Excluded by documented argument: GPUCullingData::frameIndex/deltaTime
    // (no culling shader consumes them — grep frustum_culling.comp /
    // hzb_culling.comp) and per-slot GPUScene buffer HANDLES (they cycle every
    // frame by design; content identity rides GPUScene::GetContentEpoch).
    void BuildCullingElisionBlob(ElisionInputBlob& blob, const GPUScene& scene) const;

    // ── RenderGraph-arm state ──
    void EndFrameRG();
    RenderGraph::RGFrame* m_CurrentFrame = nullptr;
    // The RGFrame the frame-local RGBuffer ids below belong to. Set by
    // BeginFrame(RGFrame*), persists past EndFrameRG (union/aggregate run
    // after) — the epilogue passes bail on a different frame, since dense ids
    // from frame A are usually in-range (and silently wrong) in frame B.
    RenderGraph::RGFrame* m_FrameRGOwner = nullptr;
    RenderGraph::RGBuffer m_FrameVisRG{};
    RenderGraph::RGBuffer m_FrameAnyVisRG{};
    RenderGraph::RGBuffer m_FrameRuntimeVisRG{};
    // Single GPU-only buffers (the old arm's per-slot rings collapse — RenderGraph's
    // first-touch import dependency covers cross-frame WAR). Lazily created on
    // first RenderGraph-arm use so the old-arm editor pays no duplicate VRAM.
    BufferHandle m_VisibilityBufferU{};
    size_t m_VisibilityBufferUBytes = 0;
    BufferHandle m_AnyViewVisibleU{};
    size_t m_AnyViewVisibleUBytes = 0;
    BufferHandle m_RuntimeVisibleU{};

    // ── R2.1 P2: two-phase HZB occlusion cull ──
    // A single PSO (own 6-binding layout: +prevVisible SSBO, +HZB combined
    // image sampler, +stats SSBO on top of the frustum set) drives both phases;
    // GPUCullingData::hzbMode selects mode 1 (phase A) vs mode 2 (phase B).
    PipelineHandle m_HzbCullingPipeline = INVALID_PIPELINE_HANDLE;
    bool m_HzbCullingPipelineAttempted = false;
    bool m_HzbCullingLoadFailureLogged = false;
    bool EnsureHzbCullingPipeline();
    // Point-clamp sampler for the R32F pyramid (nearest — the HZB test reads
    // exact texel depths, never interpolates). Lazily created, app-lifetime.
    SamplerHandle m_HzbSampler{};
    SamplerHandle EnsureHzbSampler();
    // 1x1 R32F sentinel bound at the HZB slot for the mode-1 phase-A dispatch,
    // which never samples it (the pyramid does not exist yet at phase A) but
    // still needs a valid combined-image-sampler descriptor. RG-imported each
    // frame so the graph owns its layout transition.
    TextureHandle m_HzbSentinel{};
    RenderGraph::RGTexture ImportHzbSentinel(RenderGraph::RGFrame& frame);

    // Persistent per-HZB-view visibility history (GPU-only, N×4 B, indexed by
    // GLOBAL instance index). First-touch filled to ALL-ONES (0xFFFFFFFF) so
    // frame 1 phase A draws everything — unknown visibility must mean VISIBLE,
    // mirroring ImportRuntimeVisible's first-touch pattern. Grown like
    // m_VisibilityBufferU. Keyed by viewId because each HZB view keeps its own
    // history.
    struct PrevVisibleView
    {
        BufferHandle buffer{};
        size_t bytes = 0;
        std::string importName; // stable per-view RenderDoc label
    };
    std::unordered_map<ViewId, PrevVisibleView> m_PrevVisible;
    RenderGraph::RGBuffer ImportPrevVisible(RenderGraph::RGFrame& frame, ViewId viewId,
                                            uint32_t instanceCount);

    // Per-view pending reset sets (design C1). EndFrameRG drains GPUScene's
    // recycled slots once per frame and fans them into EVERY tracked view via
    // the tracker; each HZB view flushes + clears only its OWN set before its
    // phase-A read, so a view idle for a frame (hidden viewport, shadow-only or
    // frustum-only frame) still applies the resets it missed when it returns —
    // draining without per-view accumulation would drop them permanently.
    PrevVisibleResetTracker m_PrevVisibleResetTracker;
    // Frame-local scratch: the drained slots (then, per view, the taken pending)
    // and the coalesced [startElement, countElements) runs handed to the fill
    // pass. Empty in steady state (no removes → no recycled adds → no pass).
    std::vector<uint32_t> m_PrevVisibleResetScratch;
    std::vector<std::pair<uint32_t, uint32_t>> m_PrevVisibleResetRuns;
    // The frame's drained set, kept intact for RecycledSlotsThisFrame — the
    // scratch above is reused per view and so cannot serve as the record.
    std::vector<uint32_t> m_RecycledSlotsThisFrame;
    // Fill each reset run in `prevVisible` with 0xFFFFFFFF, clamped to
    // prevVisibleBytes. Declared BEFORE the view's phase-A pass so the RG RAW
    // edge orders the reset first (same mechanism as the first-touch init).
    // No-op when there are no runs.
    void SchedulePrevVisibleResetPass(RenderGraph::RGFrame& frame, RenderGraph::RGBuffer prevVisible,
                                      uint32_t prevVisibleBytes,
                                      const std::vector<std::pair<uint32_t, uint32_t>>& runs);

    // Host-visible per-slice stats, reset + accumulated by the mode-2 dispatch.
    BufferHandle m_OcclusionStatsU{};
    RenderGraph::RGBuffer ImportOcclusionStats(RenderGraph::RGFrame& frame);

    // Records ONE hzb_culling dispatch into `visibility`'s slice. `hzb` is the
    // pyramid (mode 2) or the sentinel (mode 1); cullingData.hzbMode selects the
    // phase. Mode 1 reads prevVisible; mode 2 rewrites prevVisible and
    // resets+accumulates stats. The whole visibility buffer is bound at offset 0
    // (the shader indexes it via the slice offset push constant), sized from
    // m_VisibilityBufferUBytes.
    void ScheduleHzbCullPass(RenderGraph::RGFrame& frame, RenderGraph::RGBuffer instances,
                             uint32_t instanceBytes, RenderGraph::RGBuffer visibility,
                             RenderGraph::RGBuffer prevVisible, uint32_t prevVisibleBytes,
                             RenderGraph::RGBuffer stats, RenderGraph::RGTexture hzb,
                             const GPUCullingData& cullingData, uint32_t sliceOffsetElements,
                             uint32_t firstInstance, uint32_t sliceInstanceCount,
                             uint64_t sliceStableKey);
};

class GPUCullingFactory
{
  public:
    static std::unique_ptr<GPUCullingPipeline> CreateBalanced(IDevice* device);
    static std::unique_ptr<GPUCullingPipeline> CreateHighPerformance(IDevice* device);
    static std::unique_ptr<GPUCullingPipeline> CreateQuality(IDevice* device);
    static std::unique_ptr<GPUCullingPipeline> CreateCustom(IDevice* device, const GPUCullingConfig& config);
};

namespace GPUCullingUtils
{
// Delegate to the shared frustum helper so tests/examples and ECS use identical math
inline void ExtractFrustumPlanes(const Matrix4x4& viewProj, Vector4* outPlanes)
{
    ::GameEngine::Rendering::ExtractFrustumPlanes(viewProj, outPlanes);
}
} // namespace GPUCullingUtils

} // namespace Rendering
} // namespace GameEngine
