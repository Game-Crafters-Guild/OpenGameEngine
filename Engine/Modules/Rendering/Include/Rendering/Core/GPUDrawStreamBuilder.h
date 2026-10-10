/**
 * @file GPUDrawStreamBuilder.h
 * @brief Host-side driver for the draw_command_scatter.comp bucketer.
 *
 * Owns the scatter compute pipeline and the shared per-app-frame arena
 * (records / cursors / indirection / batch table / tripwire stats) for the
 * GPU-driven indirect-draw path, plus the per-frame range map consumers use
 * to issue DrawIndexedIndirectCount at CPU-known offsets.
 *
 * Lifetime: one instance per RenderServices, mirrors GPUScene's lifetime.
 */
#pragma once

#include "Rendering/Core/BatchRegistry.h"
#include "Rendering/Core/CommandList.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PrevVisibleResetTracker.h"
#include "Rendering/Core/RecomputeElision.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace Rendering
{

class IDevice;
class GPUScene;

class GPUDrawStreamBuilder
{
  public:
    // Host-installed loader for the draw_command_scatter.shaderpkg bytes.
    // Same pattern as GPUScene::SetCullingShaderLoader -- renderer-core does
    // no file I/O, runtime code wires this to the AssetManager.
    using ShaderLoaderFunc = std::vector<uint8_t> (*)(const char* name);
    static void SetShaderLoader(ShaderLoaderFunc loader);

    explicit GPUDrawStreamBuilder(IDevice* device);
    ~GPUDrawStreamBuilder();

    GPUDrawStreamBuilder(const GPUDrawStreamBuilder&)            = delete;
    GPUDrawStreamBuilder& operator=(const GPUDrawStreamBuilder&) = delete;

    /// Initialise: creates the ordering sentinel (load-bearing for consumers)
    /// and the scatter pipeline lazily on first schedule.
    bool Initialize();
    void Shutdown();

    /// Q6 slice 4: after an in-place device rebuild, the teardown already freed
    /// every GPU buffer and pipeline this builder owns, but the C++ handles still read
    /// IsValid() (the wholesale VMA/pool teardown does not bump per-handle
    /// generations), so EnsureSharedBuffers would skip recreation and the next
    /// scatter would map/dispatch dead buffers — a crash on the first resumed
    /// frame. Drop every handle WITHOUT DestroyBuffer (that would double-free a
    /// recycled slot) and re-create the up-front sentinel, so the arenas, ring, and
    /// scatter pipeline all rebuild lazily on the next schedule.
    void ReprovisionAfterDeviceRebuild();

    /// Sentinel value for `cascadeIndex` meaning "this stream is not a shadow
    /// cascade — main view, depth prepass, color, etc.". Real cascades use
    /// 0..3 (CSM cap is 4); 0xFF disambiguates main-view streams from cascade
    /// 0's stream so they don't collide on the same slot.
    static constexpr uint8_t kCascadeIndexNone = 0xFFu;

    /// Which culling generation a slice's records come from (two-phase HZB
    /// occlusion, R2.1). Phase A = the first-pass generation (frustum ∧
    /// prevVisible for HZB views; the ONLY generation for frustum-only
    /// views). Phase B = the same-frame recovery generation (frustum ∧ HZB ∧
    /// ¬drawn-in-A). Deliberately NOT encoded as a cascade code: the cascade
    /// byte is part of the range-map key, and a phase-B pseudo-cascade would
    /// publish under a key no consumer looks up (design §5-A3).
    enum class SlicePhase : uint8_t
    {
        A = 0,
        B = 1
    };

    /// Which batch table a slice scatters from, and therefore how it routes:
    /// Color = the (material | colorClass, mesh) table the world color pass and
    /// depth prepass consume, keyed on the color class; Shadow = the
    /// (depth-class, mesh) table the cascade / area / spot / point depth passes
    /// consume, keyed on the R1.5 shared-depth sentinels. Orthogonal to
    /// cascadeIndex: a probe face is a Color slice at a non-None cascade index.
    enum class SliceTable : uint8_t
    {
        Color = 0,
        Shadow = 1
    };

    /// Compose the range-map key the scatter scheduler + consumer lookups
    /// share (FindBatchDrawRange). Centralising the encoding keeps the
    /// scheduling site in sync with consumer lookups (`recordDraw` for color,
    /// `recordEntityBatch` for depth) — drift between them silently broke
    /// whole batches before. Under draw consolidation (a non-empty
    /// meshPoolGroup span) the mesh field carries the POOL GROUP id instead of
    /// the meshIndex — publisher and consumers switch in lockstep on the same
    /// signal. Layout (LSB->MSB):
    ///   bits  0..23  meshIndex      (24, capped at 0xFFFFFF; pool-group id
    ///                                   when consolidation is active)
    ///   bits 24..47  materialIndex  (24, capped at 0xFFFFFF)
    ///   bits 48..55  cascadeIndex   ( 8, kCascadeIndexNone for camera streams;
    ///                                   shadow families and color fan-outs
    ///                                   (probe faces) use their own blocks)
    ///   bits 56..62  viewKey        ( 7, view ids truncated — editor peak is
    ///                                   well under 128 active views; add an
    ///                                   assert if that ever stops being true)
    ///   bit  63      slicePhase     ( 1, SlicePhase::B for the HZB recovery
    ///                                   generation)
    static constexpr uint64_t MakeStreamKey(uint32_t viewId,
                                            uint8_t  cascadeIndex,
                                            uint32_t materialIndex,
                                            uint32_t meshIndex,
                                            SlicePhase phase)
    {
        return (static_cast<uint64_t>(phase == SlicePhase::B ? 1u : 0u) << 63)
             | (static_cast<uint64_t>(viewId & 0x7Fu)                   << 56)
             | (static_cast<uint64_t>(cascadeIndex)                     << 48)
             | (static_cast<uint64_t>(materialIndex & 0xFFFFFFu)        << 24)
             |  static_cast<uint64_t>(meshIndex     & 0xFFFFFFu);
    }

    /// A single shared "ordering sentinel" buffer (16 bytes Storage). The
    /// scatter pass declares an RG Write on this buffer, and every consumer
    /// pass declares an RG Read(Indirect). The resulting RAW edge forces RG
    /// to schedule the scatter before its consumers and to emit the
    /// cross-queue barrier. The sentinel's own memory is not consumed; the
    /// scatter separately issues a manual end-of-exec memory barrier
    /// (CreateStreamReadyBarrier) to make both the transfer zero-fills and
    /// the compute-written shared record/cursor/indirection buffers visible
    /// to subsequent indirect/vertex reads. The two-mechanism split is what
    /// keeps the shared buffers graph-UNDECLARED (O(1) declarations per
    /// pass); consumers do NOT emit barriers for these buffers themselves.
    BufferHandle GetSentinelBuffer() const { return m_SentinelBuffer; }

    // Per-view/slice LOD selection parameters. The scatter selects a LOD per
    // instance from screen-space coverage. Defaults force LOD0
    // (projScaleY <= 0), so slices that don't set this (shadow-only,
    // thumbnails, ortho previews) render full-res. The owning view fills
    // these from its camera before RegisterSlice.
    struct ViewLODParams
    {
        float    cameraPos[3] = {0.0f, 0.0f, 0.0f};
        float    projScaleY   = 0.0f;          // proj[1][1]; <= 0 forces LOD0
        float    lodBiasGlobal = 0.0f;         // log2 bias; positive keeps detail
        uint32_t forceLod     = 0xFFFFFFFFu;   // 0xFFFFFFFF = auto-select
        // Small-object cull threshold (PROTOTYPE, default 0 = off): instances
        // whose projected coverage falls below this are dropped by the scatter
        // like a visibility cull. Camera slices only — the caller must leave
        // this 0 for shadow buckets (culled casters would lose shadows larger
        // than their own coverage) and ortho/thumb slices.
        float    smallCullCoverage = 0.0f;
        // SSE-budget LOD selection: 2*budgetPx / viewportH for the default and
        // tight (skinned/character) mesh classes (MeshLODThresholds.h::
        // LodSseThresholdToCoverage). Multiplies an SSE-normalized threshold to
        // give its coverage-space switch point. <= 0 (the default) fail-safes
        // SSE slots to keep-detail — slices that never set these (tests, tools)
        // select exactly as before the SSE change.
        float    sseThresholdToCoverage = 0.0f;
        float    sseThresholdToCoverageTight = 0.0f;
        // Dithered LOD crossfade duration in seconds (0 = off; the engine
        // default is Rendering::LodProjectSettings::kDefaultCrossfadeDuration,
        // but this struct is slice-level input and defaults to off so tests and
        // tools that never set it select exactly as they did before the
        // feature). Non-zero on a camera slice makes a level change dissolve over
        // this many seconds instead of popping: the scatter emits both levels
        // with complementary dither phases for the duration. The caller must
        // leave it 0 for shadow buckets — a dithered caster punches holes in its
        // shadow map, which PCF averages into a washed-out shadow.
        float    crossfadeDuration = 0.0f;
        // LOD dwell band as a fraction of the switch coverage (0 = off, the
        // stateless pick). Gaining detail needs coverage >= threshold*(1+band);
        // holding or losing it needs only threshold. Callers set this ONLY for
        // camera slices (Color table, cascadeIndex == kCascadeIndexNone): a positive band
        // makes the slice's view allocate persistent per-instance history, and
        // shadow buckets have no measured thrash to justify it. "Camera slice"
        // is a structural test, not a purpose test — ScheduleWorldBucketerCommon
        // registers a cascade-None slice for EVERY registered view, so a
        // positive band here also reaches thumbnail, planar-reflection and
        // probe-face views. The scheduler's own gate additionally excludes only
        // projScaleY <= 0 (which MakeViewLODParams zeroes for EDITOR 2D ortho
        // views specifically, not for every orthographic camera) and slices
        // carrying a forced LOD.
        float    lodHysteresisBand = 0.0f;
        // Ask this view's scatter to keep a rendered level and phase history
        // (bindings 12/13): the previous rendered frame's pair for every
        // instance, readable by a later dispatch of the same frame. Independent
        // of lodHysteresisBand — a consumer that pairs this frame's surface with
        // the previous frame's needs the history whether or not the project
        // enabled the dwell band. A view that leaves this false allocates
        // nothing and binds the never-read stand-in, which is every view today.
        bool     renderedLevelHistoryRequested = false;
    };

    /// A crossfading slice splits its records into two regions with independent
    /// cursors — the head/tail split:
    ///
    ///   HEAD  one record per NON-fading instance. Drawn by the depth prepass
    ///         AND the colour pass, exactly as before the crossfade existed.
    ///   TAIL  two records per FADING instance (incoming + outgoing level,
    ///         complementary dither phases). Drawn by BOTH passes too, through
    ///         the dither variant.
    ///
    /// Both passes drawing the pair is what makes the dither correct: they run
    /// the same GE_LodCrossfadeKeep against the same record code at the same
    /// gl_FragCoord, so the prepass writes depth for exactly the fragments the
    /// colour pass keeps, each level on its own half of the dither, and neither
    /// level ever writes where the other does. Issuing the tail in only ONE of
    /// the two is the break — see Segments() for what each failure looks like.
    ///
    /// The tail is a SEPARATE per-slice record block that reuses the head
    /// block's per-row offsets, so no table byte, record offset or Σ capacity
    /// changes when it is present: the head region is byte-identical whether the
    /// feature is on or off, and the tail is absent entirely when it is off.
    ///
    /// A row's tail holds `capacity` records = `capacity / 2` simultaneous
    /// fades; the scatter degrades to a single opaque head record (today's pop)
    /// when that is exhausted.
    static constexpr uint32_t kCrossfadeTailRecordsPerFade = 2u;

    /// Exact draw bound for a tail region of `capacity` records. Tail claims are
    /// PAIRS from slot 0, so the written prefix is always even and a row with an
    /// odd capacity can never fill its last slot — the raw cursor must not be
    /// allowed to reach it.
    static constexpr uint32_t TailDrawBound(uint32_t capacity) { return capacity & ~1u; }

    /// Sentinel in a slice's tail-block index: this slice reserves no tail.
    static constexpr uint32_t kNoTailBlock = 0xFFFFFFFFu;

    /// Fixed row count for the per-slice stats array (m_SharedStats). Slices
    /// beyond this (many-view editor peaks) fall back to a shared scratch row and
    /// lose per-slice attribution. Public because it is a correctness boundary,
    /// not only an instrumentation one: an unattributed TAIL-CAPABLE slice is a
    /// hole in the crossfade census, and the reduce turns that into an invalid
    /// reading rather than a false zero.
    static constexpr uint32_t kStatsSliceCapacity = 256u;

    /// Cursor-block base of every slice in one scatter call, in elements
    /// relative to the call's claim, plus the claim total. A TAIL-CAPABLE slice
    /// (tailIndex != kNoTailBlock) owns two half-blocks — head half then tail
    /// half, so a tail claim never contends with a head claim — and every other
    /// slice owns one. Non-aliasing is the prefix sum, NOT a uniform stride: only
    /// camera slices crossfade, so a uniform stride would make each shadow bucket
    /// of a crossfading call reserve a dead tail half. Every cursor base in the
    /// call must come from this table.
    static std::vector<uint32_t> BuildSliceCursorOffsets(std::span<const uint32_t> tailIndex,
                                                         uint32_t halfStride,
                                                         uint32_t* totalOut);

    /// Bit layout of the LOD-crossfade channel the scatter packs into the
    /// indirection word. MUST match Shaders/Includes/lod_crossfade.glsl — the
    /// same kind of cross-language invariant as kMeshParityBit, and pinned by
    /// BatchScatterComputeTests. Bits 0..23 are the instance index; 24..30 the
    /// dither weight (0 == not fading); 31 the complementary-test phase.
    static constexpr uint32_t kLodFadeIndexMask   = 0x00FFFFFFu;
    static constexpr uint32_t kLodFadeWeightShift = 24u;
    static constexpr uint32_t kLodFadeWeightMask  = 0x7Fu;
    static constexpr uint32_t kLodFadeMaxWeight   = 127u;
    static constexpr uint32_t kLodFadePhaseBit    = 0x80000000u;
    /// Fade-state packing in the binding-11 buffer's uvec2.x (.y is the start
    /// time as float bits). Four bits per level covers kMaxMeshLODs.
    static constexpr uint32_t kLodFadeLevelMask    = 0xFu;
    static constexpr uint32_t kLodFadeToLevelShift = 4u;
    /// Bytes of fade state per instance — the shader's uvec2.
    static constexpr size_t   kLodFadeStateBytes   = 2 * sizeof(uint32_t);

    /// Bit layout of one rendered level and phase entry (bindings 12/13). MUST
    /// match draw_command_scatter.comp's kRenderedHistory* constants. An entry
    /// left at kLodNoHistory means "this instance rendered nothing into this
    /// view that frame" — culled, table-missed, or never dispatched.
    static constexpr uint32_t kRenderedHistoryLevelMask     = 0xFFu;
    static constexpr uint32_t kRenderedHistoryFadingBit     = 0x100u;
    static constexpr uint32_t kRenderedHistoryContinuousBit = 0x200u;

    /// Fill value for both per-instance LOD histories: "no usable history".
    /// The dwell band falls back to the stateless pick; the crossfade adopts
    /// the first pick as `from == to` rather than dissolving in from a level
    /// the instance never displayed. Distinct from 0, which is a real settled
    /// state in both buffers (level 0 / displaying LOD0).
    /// MUST match draw_command_scatter.comp's kLodNoHistory.
    static constexpr uint32_t kLodNoHistory = 0xFFFFFFFFu;

    /// One row of the GPU batch table consumed by draw_command_scatter.comp.
    /// 16 B, std430-compatible; MUST match the shader's BatchTableEntry.
    /// Rows are sorted by (materialIndex, meshIndex) — the shader's binary
    /// search and BatchRegistry::MakeKey share that order.
    struct BatchTableEntry
    {
        uint32_t materialIndex;
        uint32_t meshIndex;
        uint32_t recordOffset; // slice-relative exclusive prefix of capacities
        uint32_t capacity;     // snapshot live-instance count = per-batch draw bound
    };

    /// Snapshot the registry into a sorted table with exclusive-prefix record
    /// offsets. `outTotalRecords` receives the per-slice record count
    /// (Σ capacities = live instances at snapshot). `meshPoolGroup` (indexed
    /// by meshIndex, MeshPoolGroupPlan values) is the draw-consolidation mesh
    /// axis: non-empty remaps every row's mesh field to its pool-group id and
    /// merges rows that collapse — one region per (classKey, group) instead of
    /// per (classKey, mesh). Σ capacities is invariant either way. An empty
    /// span is the consolidation-OFF signal: today's per-mesh table,
    /// bit-for-bit. Pure; no GPU work.
    static std::vector<BatchTableEntry> BuildBatchTable(const BatchRegistry& registry,
                                                        std::span<const uint32_t> meshPoolGroup,
                                                        uint32_t* outTotalRecords = nullptr);

    /// Reserved materialIndex values (top of the 24-bit sort domain) that stand
    /// in for the two shared-depth class buckets in the SHADOW batch table.
    /// They sort ABOVE every real materialIndex (< 0xFFFFFE), so the shader's
    /// (materialIndex, meshIndex) binary search finds the merged class group
    /// unchanged. Registration asserts no real materialIndex reaches them.
    static constexpr uint32_t kSharedDepthSingleSidedSentinel = 0xFFFFFFu;
    static constexpr uint32_t kSharedDepthDoubleSidedSentinel = 0xFFFFFEu;

    /// Base for opaque color-class ids in the cascade=None COLOR batch table
    /// (P2 cross-material merge). Allocated DOWNWARD from here so a class group
    /// sorts ABOVE every real materialIndex the binary search sees, and stays
    /// strictly BELOW the R1.5 shadow sentinels (0xFFFFFF/0xFFFFFE) so the two
    /// schemes never share a value. The cascade=None table simultaneously holds
    /// merged opaque rows (class ids) AND real-materialIndex rows
    /// (transmissive / blend / material-dependent keep identity), so
    /// BuildColorBatchTable asserts the two domains never alias:
    /// maxRealMaterialIndex < kColorClassBase − numClasses.
    static constexpr uint32_t kColorClassBase = 0xFFFFFDu;

    /// Draw-consolidation MESH-axis sentinel: pseudo pool group for mesh rows
    /// absent from the pool-group map. MUST equal MeshPoolGroupPlan::
    /// kAbsentGroup — renderer-core cannot include that engine-layer header,
    /// so the two constants are pinned equal by MeshPoolGroupPlanTests — and
    /// the shader's kAbsentPoolGroup.
    static constexpr uint32_t kAbsentPoolGroup = 0xFFFFFFu;

    /// draw_command_scatter.comp classMode push-constant values. MUST match the
    /// shader's kClassMode* constants. Off = key on the instance's own
    /// materialIndex (color/depth-prepass with merge disabled); Shadow = the
    /// R1.5 depth-class sentinels routed via the instance flags bits; Color =
    /// the P2 color-class table, searchMat read from materialColorClass[].
    static constexpr uint32_t kClassModeOff    = 0u;
    static constexpr uint32_t kClassModeShadow = 1u;
    static constexpr uint32_t kClassModeColor  = 2u;

    /// Shadow-table twin of BuildBatchTable, grouped by (depth-class, mesh)
    /// instead of (material, mesh). `materialDepthClass` is indexed by
    /// materialIndex and holds MaterialDepthClass values (RenderServices'
    /// single source): eligible materials of a mesh MERGE their counts into
    /// the mesh's single/double-sided class sentinel, material-dependent ones
    /// keep their (materialIndex, mesh) identity. Σ capacities == the color
    /// table's total (merging only reduces the row count, never the record
    /// count). Pure; no GPU work.
    static std::vector<BatchTableEntry> BuildShadowBatchTable(
        const BatchRegistry& registry, std::span<const uint8_t> materialDepthClass,
        std::span<const uint32_t> meshPoolGroup, uint32_t* outTotalRecords = nullptr);

    /// Color twin of BuildShadowBatchTable for the cascade=None color/depth-
    /// prepass table (P2 cross-material merge). `materialColorClass` is indexed
    /// by materialIndex and holds, per the CPU domain rule, EITHER a downward-
    /// allocated colorClassId (opaque casters that share a PSO) OR the real
    /// materialIndex (blend/transmissive/material-dependent keep identity).
    /// Rows sharing a (classKey, mesh) MERGE their counts; Σ capacities is
    /// preserved (only the row count drops, so the shadowRecords==colorRecords
    /// contract survives). Asserts the class-id domain never aliases a real
    /// materialIndex present in the same table. An empty span is the flag-OFF
    /// signal (callers use BuildBatchTable instead). Pure; no GPU work.
    static std::vector<BatchTableEntry> BuildColorBatchTable(
        const BatchRegistry& registry, std::span<const uint32_t> materialColorClass,
        std::span<const uint32_t> meshPoolGroup, uint32_t* outTotalRecords = nullptr);

    /// Lazily create the scatter compute pipeline
    /// (Shaders/draw_command_scatter.shaderpkg via the same loader as the
    /// per-bucket pipeline). Returns an invalid handle until the package is
    /// available.
    PipelineHandle GetOrCreateScatterPipeline();

    /// True once the scatter pipeline has compiled to the instrumentation
    /// variant — i.e. GE_SCATTER_STATS=1 AND a stats shaderpkg loaded. When
    /// false the per-slice drawnTriangles counter is compiled out, so the
    /// shadow-arc readback reports drawnTriangles == 0 (passedCasters and the
    /// tripwires are unaffected — they never rode that atomic). Consumers use
    /// this to distinguish "gated off" from a genuine zero triangle count.
    /// Valid only after GetOrCreateScatterPipeline succeeds.
    bool IsScatterStatsActive() const { return m_ScatterStatsActive; }

    /// What an introspection reader (debug-server render stats) may know about
    /// the scatter pipeline. Reported through one const snapshot so a reader
    /// cannot reach GetOrCreateScatterPipeline: that call loads SPIR-V, creates
    /// a Vulkan pipeline and latches the variant axes, so an introspection read
    /// routed through it changes the very state it reports — PipelineReady and
    /// StatsActive would then describe the reader's own side effect rather than
    /// what the render path built.
    struct ScatterIntrospection
    {
        /// The render path has built the scatter pipeline. False on a cold
        /// builder — a genuine "not warmed yet", not a failure.
        bool PipelineReady = false;
        /// The only axis here that can move without PipelineReady moving with
        /// it: its source is re-probed from a live package load on every
        /// GetOrCreateScatterPipeline, whereas StatsActive mirrors a value
        /// latched once behind m_ScatterVariantInit. That makes it the one
        /// witness that a read reached the create path on a build that then
        /// threw — which is the failure the purity case exists to catch.
        bool CompactActive = false;
        bool StatsActive   = false;
        /// Views holding a rendered level and phase history pair (bindings
        /// 12/13). Zero unless a view asked for the history, which is the
        /// no-consumer proof: these buffers are created directly on the device,
        /// so the render graph's resource list cannot report them.
        uint32_t RenderedHistoryViews = 0;
        /// Rendered-history role rotations over this builder's lifetime, summed
        /// over views. A rotation happens only on a frame following one whose
        /// scatter pass was actually recorded, so a frame that was declared and
        /// then abandoned leaves this count unchanged.
        uint64_t RenderedHistoryRotations = 0;

        bool operator==(const ScatterIntrospection&) const = default;
    };

    /// Read-only scatter state for diagnostics. Never creates the pipeline.
    ScatterIntrospection IntrospectScatter() const;

    /// Test-only: build a scatter pipeline for a specific (fetch, stats)
    /// variant, bypassing the GE_SCATTER_COMPACT/GE_SCATTER_STATS selection so
    /// ONE test run can exercise every combination — the compact 32 B mirror vs
    /// the full-fat 240 B instance buffer, with the drawnTriangles atomic on or
    /// off — and assert identical scatter records across all of them. Returns an
    /// invalid handle if that variant's shaderpkg is unavailable. Never used by
    /// the render path (which goes through GetOrCreateScatterPipeline).
    PipelineHandle CreateScatterPipelineForVariant(bool compact, bool stats);

    /// Push constants for draw_command_scatter.comp. Field order MUST match
    /// the shader's PushConstants block (100 B).
    struct ScatterPushConstants
    {
        uint32_t instanceCount;   // call-captured at snapshot time
        uint32_t batchCount;      // table entry count B — never a draw bound
        uint32_t cursorBase;      // arena-absolute
        uint32_t recordBase;      // arena-absolute
        uint32_t disableVisCheck;
        uint32_t viewWorldKey;
        float    cameraPosX;
        float    cameraPosY;
        float    cameraPosZ;
        float    projScaleY;
        float    lodBiasGlobal;
        uint32_t forceLod;
        // 3-way batch-search routing (kClassMode* above):
        //   Off    = key on instance.materialIndex (color/depth-prepass, merge off)
        //   Shadow = R1.5 depth class via the flags bits (sentinel for eligible,
        //            real materialIndex for material-dependent)
        //   Color  = P2 color class: searchMat = materialColorClass[materialIndex]
        uint32_t classMode;
        // Arena-absolute per-slice stats row (mirrors cursorBase/recordBase).
        // P0 shadow-arc instrumentation: each slice attributes its triangle /
        // tripwire counters to sliceStats[statsBase].
        uint32_t statsBase;
        // Draw consolidation: != 0 routes the batch search's mesh axis through
        // the binding-9 pool-group map (grouped tables). 0 = per-mesh tables.
        uint32_t meshGroupMode;
        // Small-object cull threshold (prototype; see ViewLODParams). 0 = off.
        float    smallCullCoverage;
        // Per-view SSE coverage scales (see ViewLODParams). <= 0 = SSE slots
        // fail-safe to keep-detail.
        float    sseThresholdToCoverage;
        float    sseThresholdToCoverageTight;
        // Dithered LOD crossfade (see ViewLODParams). nowSeconds is the
        // process-global animation clock; invFadeDuration is 1/duration.
        float    nowSeconds;
        float    invFadeDuration;
        // Non-zero when this slice has a TAIL region — the shader's ON test, and
        // its promise that tailCursorBase/tailRecordBase are addressable. Always
        // 0 for shadow buckets, and 0 everywhere when the feature is off, which
        // is what stops the scatter touching the binding-11 stand-in.
        uint32_t crossfadeTailActive;
        // Arena-absolute bases of this slice's tail row cursors and tail record
        // block. Read only when crossfadeTailActive != 0. The tail reuses the
        // head's per-row recordOffset, so a row's tail region is
        // [tailRecordBase + recordOffset, + capacity).
        uint32_t tailCursorBase;
        uint32_t tailRecordBase;
        // LOD dwell band (see ViewLODParams). 0 = off; the scatter then takes
        // the stateless path and never touches the binding-10 history buffer.
        float    lodHysteresisBand;
        // Rendered level and phase history (bindings 12/13). 0 = off: the shader
        // reads neither binding, which is what makes binding a stand-in at both
        // legal. Non-zero only where a real per-view pair is bound, and no view
        // asks for one until a motion consumer exists.
        uint32_t renderedHistoryEnabled;
    };

    /// Descriptor set layout for the scatter pipeline (14 storage buffers:
    /// visibility, instances, meshes, batch table, records, cursors,
    /// indirection, stats, materialColorClass, meshPoolGroup, prevLod,
    /// lodFade, prevRendered, currentRendered). Exposed so tests and the P3
    /// schedule path share one definition.
    static DescriptorSetLayoutDesc MakeScatterDescriptorSetLayout();

    /// Push constants for draw_stream_group_compact.comp (mesh-coherent group
    /// compaction). Field order MUST match the shader's block (28 B).
    struct GroupCompactPushConstants
    {
        uint32_t recordCount;    // slice capacity domain (snapshotRecords)
        uint32_t rowCount;       // slice table rows
        uint32_t cursorBase;     // arena-absolute base of the slice's row cursors
        uint32_t countBase;      // arena-absolute base of the slice's run count slots
        uint32_t srcRecordBase;  // arena-absolute staging block base (scatter output)
        uint32_t dstRecordBase;  // arena-absolute draw block base (consumer input)
        // != 0 for a crossfade TAIL block: its records are claimed in pairs, so
        // the live count rounds DOWN to even. Without this an odd-capacity row
        // whose tail filled would compact one unwritten slot into the draw
        // block. 0 for the head block, where claims are single.
        uint32_t pairedRecords;
    };

    /// Descriptor set layout for the group-compact pipeline (5 storage
    /// buffers: batch table, row-run map, cursors, records, indirection).
    static DescriptorSetLayoutDesc MakeGroupCompactDescriptorSetLayout();

    /// Lazily create the group-compact pipeline
    /// (Shaders/draw_stream_group_compact.shaderpkg via the shared loader).
    /// Required whenever consolidation is active — a consolidated schedule
    /// call with no compact pipeline is dropped loudly (same staging-gap
    /// semantics as a missing scatter package). Exposed for the GPU tests.
    PipelineHandle GetOrCreateGroupCompactPipeline();

    // ---- Per-slice scatter scheduling: arena + range map --------------------
    //
    //   1. BeginArenaFrame() ONCE per app frame (owner spine, global-stages
    //      gate) — resets the arena cursor and the range map, ticks retired
    //      buffers, applies pending growth.
    //   2. RegisterSlice(...) per (view, cascade-slice) — C entries per call
    //      instead of one per (view, cascade, material, mesh).
    //   3. ScheduleUnifiedScatter(...) — snapshots the registry NOW (the
    //      caller must have flushed GPUScene first so buffer content matches
    //      the snapshot — the captured-snapshot invariant), claims a disjoint
    //      arena range (cursorBase/recordBase push constants; disjointness is
    //      the intra-frame sync for these graph-undeclared buffers), emits ONE
    //      RG pass (fills → per-slice dispatches → stream-ready barrier), and
    //      publishes (view, cascade, mat, mesh) → BatchDrawRange entries.
    //      Later calls in the same frame (thumbnail seam, GameView) APPEND.
    //
    // Consumers swap FindSlot for FindBatchDrawRange and issue
    // DrawIndexedIndirectCount at the returned offsets with
    // maxDrawCount = range.maxDrawCount (the batch's snapshot capacity).

    struct SliceRegistration
    {
        uint32_t viewId               = 0;
        uint8_t  cascadeIndex         = kCascadeIndexNone;
        // Culling generation this slice's ranges publish under. Orthogonal to
        // cascadeIndex and table.
        SlicePhase phase              = SlicePhase::A;
        // Batch table + routing (classMode). The cascade byte is only a key.
        SliceTable table              = SliceTable::Color;
        uint32_t visibilityOffsetBytes = 0;
        bool     disableVisibilityCheck = false;
        uint32_t viewWorldKey         = 0;
        ViewLODParams lod{};
    };

    void BeginArenaFrame();
    void RegisterSlice(const SliceRegistration& slice);
    size_t GetPendingSliceCount() const { return m_FrameSlices.size(); }

    // `materialDepthClass` (indexed by materialIndex, MaterialDepthClass
    // values) drives the shadow batch table + the per-slice search mode. The
    // shadow table is built and uploaded only when a registered slice is a
    // shadow slice (table == SliceTable::Shadow); an empty span degrades
    // every material to material-dependent (safe: no eligible merging).
    // `rgPassPhase` is the RG scheduling phase for the emitted pass (a
    // PassPhase value; kEarlySetup for the frame's phase-A generation). It is
    // a scheduling TIEBREAK only — dependencies win — but the HZB phase-B
    // scatter must not be pulled ahead of the raster its HZB input derives
    // from when edges alone leave the order free (design §5-A1).
    // `materialColorClass` (indexed by materialIndex, colorClassId values from
    // BuildColorBatchTable's domain rule) drives the cascade=None COLOR table
    // merge + the color-class scatter routing (classMode). An empty span is the
    // merge-OFF signal: the color table is built unmerged (BuildBatchTable) and
    // color slices key on the raw materialIndex (kClassModeOff) — today's
    // behaviour bit-for-bit. The span is global + frame-invariant; it is
    // uploaded to the table ring ONCE per app frame (the first schedule call)
    // and the same GPU copy is bound by every later call this frame.
    // `scatterHot` is GPUScene's coalesced 32 B/instance mirror (lever 1). When
    // the compact fetch is active AND this buffer is valid, the scatter binds it
    // at binding 1 in place of `instances`; otherwise `instances` (240 B) is
    // bound, exactly as before. Callers pass both; the builder picks.
    // `meshPoolOrdered` + `orderedToGroup` (MeshPoolGroupPlan's ordered mesh
    // axis and its rank->group inverse) are the draw-consolidation switch:
    // non-empty keys BOTH tables' mesh axis on the per-mesh ORDERED ranks
    // (group members contiguous), uploads the map to the ring per call (same
    // one-snapshot discipline as materialColorClass), scatters into per-mesh
    // staging rows, mesh-major-compacts each (class, group, parity) run into
    // the draw block, and publishes ONE range per run keyed by GROUP id —
    // consumers look up by group id on the same signal. Empty = the per-mesh
    // path, byte-identical to pre-consolidation. Pass both spans or neither.
    RenderGraph::RGBuffer ScheduleUnifiedScatter(RenderGraph::RGFrame& frame,
                                                 const char* passNameSuffix,
                                                 int32_t rgPassPhase,
                                                 RenderGraph::RGBuffer visibility,
                                                 RenderGraph::RGBuffer instances,
                                                 RenderGraph::RGBuffer scatterHot,
                                                 RenderGraph::RGBuffer meshes,
                                                 const BatchRegistry& registry,
                                                 std::span<const uint8_t> materialDepthClass,
                                                 std::span<const uint32_t> materialColorClass,
                                                 std::span<const uint32_t> meshPoolOrdered,
                                                 std::span<const uint32_t> orderedToGroup,
                                                 uint32_t instanceCount,
                                                 uint32_t meshTableCount);

    /// One DrawIndexedIndirectCount's worth of a batch: where its records start,
    /// where its GPU-written count lives, and the CPU-known upper bound on that
    /// count. maxDrawCount 0 means the segment is ABSENT and nothing is issued.
    struct BatchDrawSegment
    {
        size_t   cmdByteOffset   = 0;
        size_t   countByteOffset = 0;
        uint32_t maxDrawCount    = 0;
        bool IsPresent() const { return maxDrawCount > 0u; }
    };

    /// One issuable segment with every pipeline-affecting property resolved: a
    /// parity-1 (mirrored) segment draws with a flipped front face, and a
    /// crossfading segment's records carry a non-zero fade code that only the
    /// LodCrossfade variant decodes.
    struct SegmentDraw
    {
        BatchDrawSegment segment{};
        bool             mirrored = false;
        /// True for TAIL segments, false for heads. EVERY consumer — depth and
        /// colour alike — MUST select the MaterialKeyword::LodCrossfade variant
        /// for these and MUST NOT select it for the heads: the dither's
        /// `discard` forfeits early-Z on every fragment of every draw it is
        /// compiled into, and only tail records can ever carry a non-zero fade
        /// code.
        bool             crossfading = false;
    };

    /// A batch's segments for one slice. Two axes: winding parity (a mirrored
    /// sibling row draws with a flipped front face) and the head/tail record
    /// split (a tail carries the crossfading instances' record pairs).
    ///
    /// Recorders iterate Segments() rather than the raw fields — the consumer
    /// rule lives on that accessor.
    struct BatchDrawRange
    {
        BufferHandle recordBuffer{};   // shared records (Indirect)
        BufferHandle countBuffer{};    // shared cursors (Indirect)
        // Parity-0 (non-mirrored) rows — the only ones on mirror-free scenes.
        BatchDrawSegment even{};
        BatchDrawSegment evenTail{};
        // Parity-1 (mirrored) rows. Present only when a mirrored sibling row
        // exists for this batch (the uncommon case).
        BatchDrawSegment mirrored{};
        BatchDrawSegment mirroredTail{};
        // A batch may be all-mirrored (only the parity-1 row exists), so IsValid
        // accepts either parity. A row always has a head segment when it exists
        // at all — capacity is its snapshot live count — so the tails need no
        // part in this test.
        bool IsValid() const
        {
            return recordBuffer.IsValid() && (even.IsPresent() || mirrored.IsPresent());
        }

        /// Present segments in issue order, fixed capacity (at most the four
        /// fields above).
        struct SegmentDraws
        {
            std::array<SegmentDraw, 4> items{};
            uint32_t                   count = 0;
            const SegmentDraw* begin() const { return items.data(); }
            const SegmentDraw* end() const { return items.data() + count; }
        };

        /// What a raster pass issues: heads AND crossfade tails, per parity —
        /// the SAME set for the depth prepass and the colour pass.
        ///
        /// That equality is the crossfade's load-bearing invariant. A fading
        /// instance writes only a tail record PAIR (incoming + outgoing level,
        /// complementary dither phases) and no head record, so the pair is that
        /// instance's only presence anywhere. Both passes draw it through
        /// MaterialKeyword::LodCrossfade, whose discard runs the one shared
        /// GE_LodCrossfadeKeep (Shaders/Includes/lod_crossfade.glsl) against the
        /// same record code at the same gl_FragCoord — so the prepass writes
        /// depth for exactly the fragments the colour pass keeps, each level on
        /// its own half of the dither, and neither level ever writes where the
        /// other does. Issuing a tail in only ONE of the two passes is what
        /// breaks it: depth-only would leave the colour pass z-killed on the
        /// loser's half; colour-only leaves the instance absent from prepass
        /// depth for the whole fade, and every pre-world consumer of that depth
        /// (GTAO, SDSM fitting, cluster depth bounds) reads a hole.
        ///
        /// Shadow and cascade slices are tail-free by CONSTRUCTION, not by a
        /// check here: only a cascade=None slice reserves or publishes a tail
        /// block (ScheduleUnifiedScatter's sliceCrossfades), so their ranges
        /// carry absent tail segments and this yields heads alone. Dithering a
        /// caster would punch holes in its shadow map, which PCF averages into a
        /// washed-out shadow.
        ///
        /// `crossfading` rides on the segment rather than the view: only these
        /// tail draws need the dither variant, so the heads — every non-fading
        /// instance, which is nearly all of them — keep the early-Z the
        /// `discard` would forfeit.
        ///
        /// COST CONTRACT. A tail region exists on every frame the feature is
        /// enabled, its GPU-written count simply being 0 at rest, so AT REST
        /// each pass still records one extra indirect draw plus one extra
        /// pipeline bind per present tail — per batch row (a row IS a winding
        /// parity) whose capacity is >= 2, since TailDrawBound rounds a
        /// capacity-1 row's tail away. Those draws rasterize nothing, so no
        /// fragment shader runs for them in either pass. Only with the feature
        /// OFF does the issued segment set match the pre-feature one.
        ///
        /// Publication keys on the duration, never on live fades, which is what
        /// puts the tail variant's compile REQUEST on a batch's first enabled
        /// frame. It does NOT make that variant ready: the compile is async and a
        /// fade can start before it lands, independently per pass.
        ///  - Cold COLOUR variant: the colour recorder draws the tail with the
        ///    HEAD variant rather than skipping it — dither absent, both levels
        ///    opaque, a pop rather than a vanish. Each level still passes the
        ///    depth its own dither half wrote (equal), so coverage stays complete
        ///    and the overdraw is confined to the fade.
        ///  - Cold DEPTH variant: the depth recorder skips that tail for the
        ///    frame. The instance is then absent from prepass depth exactly as it
        ///    was before this design, self-healing on publish; the colour pass
        ///    still draws it, so nothing vanishes.
        /// First touch cannot start a fade at all: the fade slots fill with
        /// kLodNoHistory and adopt their first pick (see
        /// BatchScatterComputeTest.CrossfadeFirstTouchAdoptsItsPickInsteadOfFadingIn).
        SegmentDraws Segments() const
        {
            SegmentDraws out;
            if (even.IsPresent())
                out.items[out.count++] = {even, /*mirrored=*/false, /*crossfading=*/false};
            if (evenTail.IsPresent())
                out.items[out.count++] = {evenTail, /*mirrored=*/false, /*crossfading=*/true};
            if (mirrored.IsPresent())
                out.items[out.count++] = {mirrored, /*mirrored=*/true, /*crossfading=*/false};
            if (mirroredTail.IsPresent())
                out.items[out.count++] = {mirroredTail, /*mirrored=*/true, /*crossfading=*/true};
            return out;
        }
    };

    /// Range published by this app frame's schedule calls. An invalid result
    /// means "skip the draw" (batch absent from the snapshot, or this call's
    /// scatter was skipped by the arena guard). `phase` selects the culling
    /// generation (SlicePhase::A everywhere except HZB phase-B recovery
    /// consumers).
    BatchDrawRange FindBatchDrawRange(uint32_t viewId, uint8_t cascadeIndex,
                                      uint32_t materialIndex, uint32_t meshIndex,
                                      SlicePhase phase) const;

    /// This frame's CURRENT rendered level and phase history for `viewId` — the
    /// buffer the scatter wrote, which a later graphics pass of the same frame
    /// reads for per-instance continuity. The roles rotate once per rendered
    /// frame, so a consumer pairing this frame's surface with the previous
    /// frame's reads THIS buffer, never the one the scatter read.
    /// Invalid when the view did not request the history.
    struct RenderedHistoryRead
    {
        BufferHandle Buffer{};
        size_t Bytes = 0;
        bool IsValid() const { return Buffer.IsValid() && Bytes > 0; }
    };
    RenderedHistoryRead GetRenderedHistoryForRead(uint32_t viewId) const;

    /// True when this app frame published at least one range for the given
    /// (view, cascade, phase). A consumer that declares a pass per culling
    /// generation asks this at DECLARATION time, where it cannot yet resolve
    /// any particular batch's lookup key: a view that reserved no phase-B
    /// slice has no recovery ranges at all, and declaring a pass to record
    /// nothing costs a render pass per frame per view.
    bool HasPublishedRangesForPhase(uint32_t viewId, uint8_t cascadeIndex,
                                    SlicePhase phase) const;

    /// BDA of the shared indirection buffer — constant across all batches and
    /// slices (records carry arena-global firstInstance).
    uint64_t GetSharedIndirectionAddress() const { return m_SharedIndirectionAddress; }

    /// Shared indirection buffer handle (diagnostics/readback). A batch's
    /// region starts at element (range.cmdByteOffset / 20) — one uint per
    /// draw record.
    BufferHandle GetSharedIndirectionBuffer() const { return m_SharedIndirection; }

    size_t GetRangeCount() const { return m_RangeMap.size(); }

    /// Eventually-consistent tripwire counters (host-visible; values lag by
    /// up to frames-in-flight). Non-zero means the captured-snapshot
    /// invariant broke or the table diverged from the registry. The values
    /// are the SUM over every per-slice ScatterSliceStats row this frame.
    struct ScatterStats
    {
        uint32_t tableMisses = 0;
        uint32_t overflows   = 0;
    };
    ScatterStats ReadScatterStats() const;

    /// Per-slice scatter counters (P0 shadow-arc instrumentation). 16-byte
    /// std430; MUST match draw_command_scatter.comp's ScatterSliceStats. The
    /// scatter atomic-accumulates one row per (view, cascade) slice, indexed by
    /// the arena-absolute statsBase push constant; the CPU reads the array
    /// one-frame-stale. drawnTriangles is the subgroup-reduced LOD triangle
    /// count (passed-casters are derived separately from the cursor buffer).
    struct ScatterSliceStats
    {
        uint32_t tableMisses    = 0;
        uint32_t overflows      = 0;
        uint32_t drawnTriangles = 0;
        // Instances whose selected LOD differs from the previous frame's — the
        // dwell band's thrash instrument. SCATTER_STATS-gated and zero unless
        // lodHysteresisBand > 0. The first frame after the band is enabled
        // counts every instance (empty history), so measurements must discard
        // it. It is also an EVENT counter read through the single unfenced
        // readback mirror, which holds whichever frame's copy last completed:
        // under sustained churn a reduce can count one frame twice or skip one
        // entirely, so oscillation measurements need a LEVEL readout
        // (drawnTriangles) rather than this counter.
        uint32_t lodChanges     = 0;
    };

    /// Per-slice shadow-arc instrumentation result, reduced once per app frame
    /// in BeginArenaFrame from the previous frame's scatter output (one-frame-
    /// stale MapBuffer read; valid only at steady state — the bench warmup
    /// protocol guarantees it). `passedCasters` is Σ of this slice's cursor
    /// entries (the DrawIndexedIndirectCount counts — no extra GPU atomic);
    /// `drawnTriangles` is the subgroup-reduced per-slice triangle count.
    struct ShadowArcSliceStat
    {
        uint32_t viewId         = 0;
        uint8_t  cascadeIndex   = kCascadeIndexNone; // 0..3 directional; other = area/spot/point; none = main
        uint8_t  phase          = 0;                 // SlicePhase
        uint32_t passedCasters  = 0;
        uint64_t drawnTriangles = 0;
        // Tail rows only, and ALSO included in passedCasters above (which stays
        // "every record this slice emitted" — the quantity the M2a early-out and
        // the measurement harnesses read). Non-zero means a transition was
        // mid-dissolve when this slice last dispatched.
        uint32_t tailRecords    = 0;
        // Instances that changed LOD this frame (dwell-band thrash instrument).
        // SCATTER_STATS-gated, and zero unless the slice carries a band.
        uint32_t lodChanges     = 0;
    };
    /// Per-slice counts from the most recently completed app frame's scatter.
    /// Refreshed in BeginArenaFrame; empty until the first frame reduces.
    const std::vector<ShadowArcSliceStat>& GetShadowArcStats() const { return m_ShadowArcStats; }

    /// Sentinel for PreviousFrameShadowSurvivors: no per-slice stat exists for
    /// the queried (view, cascade, phase) — the slice wasn't scheduled last
    /// frame, or its stats row overflowed the fixed capacity. Callers MUST treat
    /// this as "must record" (fail-safe), never as zero survivors.
    static constexpr uint32_t kShadowSurvivorsUnknown = 0xFFFFFFFFu;

    /// A prior frame's passed-caster (survivor) count for one shadow depth slice
    /// (view, cascade/face, phase), from the cursor readback reduced in
    /// BeginArenaFrame — no GPU->CPU stall on the frame path. The reduction is
    /// fence-gated (IsPreviousFrameGraphicsComplete): the stats reflect the most
    /// recent COMPLETED frame's scatter, and are empty on frames whose graphics
    /// work is still in flight (the readback would otherwise be up to
    /// frames-in-flight stale under a DIFFERENT arena layout). Returns
    /// kShadowSurvivorsUnknown when the slice has no stat. This is the M2a
    /// batch-walk early-out signal: a zero here means the GPU cull emptied the
    /// slice, so its depth pass can skip the whole PSO/descriptor/draw walk.
    uint32_t PreviousFrameShadowSurvivors(uint32_t viewId, uint8_t cascadeIndex,
                                          SlicePhase phase) const;

    /// Pure (view, cascade, phase) lookup over a reduced-stats span — the body of
    /// PreviousFrameShadowSurvivors, factored out so the key-match logic is unit-
    /// testable without a device. Returns kShadowSurvivorsUnknown when absent.
    static uint32_t LookupShadowSurvivors(const std::vector<ShadowArcSliceStat>& stats,
                                          uint32_t viewId, uint8_t cascadeIndex, SlicePhase phase);

    /// Reject-rate instrumentation for the ReduceShadowArcStats fence gate: how
    /// many BeginArenaFrame reductions ran (Attempts) vs. how many left the stats
    /// empty because the prior frame's graphics work had not completed
    /// (StaleRejects). A reject ratio near 1.0 means the frame pacing keeps the
    /// readback in flight so the early-out almost never has data — i.e. the whole
    /// M2a slice has degenerated to a no-op and the gate needs rethinking.
    uint64_t GetShadowArcReduceAttempts() const { return m_ShadowArcReduceAttempts; }
    uint64_t GetShadowArcStaleRejects() const { return m_ShadowArcStaleRejects; }

    /// Whole-frame crossfade tail census from the same reduce, for the elision
    /// suppression rule (Engine/Rendering/LodCrossfadeLiveness.h). Read the three
    /// fields together: a count without its recompute stamp cannot be judged
    /// fresh. A reading survives a reduce that merely could not run (its stamp still
    /// matches the live recompute count); `valid` false means no reading exists or
    /// the last one was structurally invalidated.
    struct LiveTailObservation
    {
        bool     valid          = false;
        uint32_t tailRecords    = 0;
        uint64_t recomputeCount = 0;
    };
    LiveTailObservation GetLiveTailObservation() const { return m_LiveTail; }

    /// Scatter dispatches that recomputed rather than skipped, summed over every
    /// suffix gate. `Skipped` and `NotSettled` are excluded, so a frame held
    /// awake by crossfade suppression alone does NOT advance this — which is what
    /// lets a suppression window close instead of re-arming itself.
    uint64_t GetScatterRecomputeCount() const;

    static ResourceBarrier CreateStreamReadyBarrier();

    // ── Idle recompute elision (frame-attribution lever #2) ──
    // The engine injects the frame's content epochs (scene content, GPUScene
    // upload generation, culling visibility-write generation) + the resolved
    // kill-switch immediately before EACH ScheduleUnifiedScatter call site.
    // Per pass-name-suffix ("World", "World.B", "View%u"), the call
    // byte-records its complete dispatch input set (slices, both batch
    // tables, class/group maps, arena bases, physical identities, epochs);
    // when the record has been identical for kElisionSettleFrames consecutive
    // frames the RG pass (fills → dispatches → compact → readback copy) is
    // skipped: the shared records/cursors already hold exactly what a re-run
    // would write at the same arena offsets. Range publication, arena claims,
    // and stats bindings still run — consumers re-read them per frame, and
    // identical bases mean the retained GPU regions line up. A suffix not
    // evaluated last frame recomputes (EvaluationGap): its retained regions
    // may have been rewritten by other calls while it was absent.
    void SetElisionFrameContext(const ElisionFrameContext& ctx) { m_ElisionCtx = ctx; }

    /// Frame clock the LOD crossfade differences its per-instance start stamps
    /// against (the process-global animation timeline, in seconds). Frame state,
    /// not view state, so it is set once per app frame ahead of the frame's
    /// schedule calls and deliberately stays OUT of the elision input record —
    /// a value that changes every frame would disable idle elision outright.
    /// Suppressing elision is the caller's job
    /// (RenderServices::RefreshScatterElisionContext), and it suppresses on an
    /// OBSERVED live-tail count — GetLiveTailObservation, reduced from the cursor
    /// mirror — so a settled scene stops paying for the possibility of a fade.
    /// Unread while every slice's crossfadeDuration is 0.
    void SetFrameTimeSeconds(float seconds) { m_FrameTimeSeconds = seconds; }
    // Aggregate stats over every suffix gate (instrumentation).
    RecomputeElisionGate::Stats GetElisionStats() const;

    /// Fan this frame's recycled GPUScene instance slots into every tracked
    /// view's pending reset sets for both per-view persistent state buffers,
    /// each refilled with the no-history sentinel: the LOD-fade state adopts
    /// the new tenant's first pick as settled, and the dwell band falls back to
    /// the stateless pick. A recycled slot must have both cleared before
    /// its view next reads it: an inherited transition dissolves the new
    /// tenant on spawn, and an inherited level inside the dwell band is a
    /// PERMANENT fixed point at a still camera (the rule is idempotent), not
    /// a one-frame transient. Same invalidation the two-phase HZB applies to
    /// its occlusion history — the SAME tracker type — driven off the SAME
    /// once-per-frame GPUScene drain, which the culling pipeline owns. Call
    /// once per app frame, before the frame's schedule calls. No-op when
    /// nothing was recycled (the steady state).
    void OnInstanceSlotsRecycled(std::vector<uint32_t> recycledSlots);

    /// Fan the slots whose GPUScene continuity stamp moved this frame
    /// (GPUScene::DrainContinuityResetSlots) into every tracked view's pending
    /// reset set for the rendered level and phase history. This is the device
    /// end of the processor-side invalidation: the stamp itself never reaches
    /// the device, so a slot whose tenant or payload changed has its PREVIOUS
    /// entry refilled with the no-history value before the frame reads it, and
    /// the frame that changed it writes no valid pair for it. The set covers
    /// recycled slots too — a new tenant moves the stamp — so this is the only
    /// drain the rendered history needs. Call once per app frame, before the
    /// frame's schedule calls. No-op when no view holds a history, which is
    /// every frame until a motion consumer exists.
    void OnInstanceContinuityBroken(std::vector<uint32_t> changedSlots);

  private:
    IDevice*       m_Device   = nullptr;
    PipelineHandle m_ScatterPipeline;
    // Mesh-coherent group compaction (draw consolidation). Lazy like the
    // scatter pipeline, guarded by the same init mutex.
    PipelineHandle m_GroupCompactPipeline;
    bool           m_GroupCompactFailureLogged = false;
    // Serializes GetOrCreateScatterPipeline's lazy init: on guard-refused frames
    // the spine doesn't pre-warm it, so record workers (A2.4-P0-R) can race the
    // one-time SPIR-V load + pipeline create + fallback-log member writes.
    mutable std::mutex m_ScatterInitMutex;
    bool           m_ScatterPipelineFailureLogged = false;
    bool           m_SentinelFailureLogged        = false;
    // Coalesced scatter-hot fetch (Scatter.World lever 1). Requested reflects
    // GE_SCATTER_COMPACT (default on), read once; Active is the variant the
    // pipeline actually compiled to — false if the compact shaderpkg was
    // unavailable and we fell back to the full-fat package. When Active, the
    // scatter binds GPUScene's 32 B mirror at binding 1 instead of the 240 B
    // instance buffer. GPUScene reads the same env var, so the mirror it
    // maintains and this variant agree by construction.
    bool           m_ScatterCompactRequested      = false;
    bool           m_ScatterCompactActive         = false;
    bool           m_ScatterCompactFallbackLogged = false;
    // Per-slice drawnTriangles instrumentation (P0 shadow-arc). Requested
    // reflects GE_SCATTER_STATS (default off), read once alongside compact;
    // Active is what the pipeline compiled to. Orthogonal to the compact axis —
    // the fetch-fallback keeps the requested stats setting. Off = the shipping
    // default: the ~68% single-address atomic is compiled out and drawnTriangles
    // reads 0 (buffer still zeroed per frame; tripwires + passedCasters stay live).
    bool           m_ScatterStatsRequested        = false;
    bool           m_ScatterStatsActive           = false;
    // Latches the one-time env read of both variant axes (compact + stats).
    bool           m_ScatterVariantInit           = false;
    // Latches the "compact active but scatter-hot buffer invalid" hard-guard log
    // (a per-frame path); the condition cannot arise with correct wiring.
    bool           m_ScatterHotMissingLogged      = false;

    // Build a scatter compute pipeline from cooked stage bytes (descriptor
    // layout + push-constant size + debug name). Shared by
    // GetOrCreateScatterPipeline and CreateScatterPipelineForVariant so those
    // settings never drift. May throw on a device create failure — callers
    // wrap it.
    PipelineHandle BuildScatterPipeline(std::vector<uint8_t> shaderBytes);

    BufferHandle m_SentinelBuffer{};

    // ---- Scatter arena state ----
    bool EnsureArenaBuffers();
    void RetireBuffer(BufferHandle handle);
    // Drop every arena buffer a pending capacity has outgrown so the next
    // EnsureArenaBuffers recreates it larger. Sound only where no pass of the
    // CURRENT frame has been recorded against those buffers: BeginArenaFrame,
    // and a schedule call that trips the guard before claiming anything.
    void RetireOutgrownArenaBuffers();

    // Zero every buffer/pipeline handle + arena cursor + cached tracking WITHOUT
    // touching the device. Shared by Shutdown (which destroys the buffers first)
    // and ReprovisionAfterDeviceRebuild (whose buffers are already dead).
    void ResetTrackingState();

    BufferHandle m_SharedRecords{};      // indexed indirect draw commands, Indirect
    BufferHandle m_SharedCursors{};      // uint[], Indirect (the count buffer)
    BufferHandle m_SharedIndirection{};  // uint[], BDA
    BufferHandle m_SharedStats{};        // ScatterSliceStats[], host-visible readback
    BufferHandle m_TableRing{};          // BatchTableEntry[], host-visible ring
    // Host-CACHED mirrors of the cursor arena + per-slice stats. The scatter
    // copies each call's written range into these at the end of the pass, so
    // BeginArenaFrame reads them one-frame-stale. A GPU-device-write followed by
    // a device→host COPY into host-cached (Readback) memory is the reliable
    // GPU→CPU path; a direct CPU read of the host-visible (write-combined Upload)
    // scatter buffers does NOT reflect device writes on all GPUs. Cursors give
    // passed-casters WITHOUT a GPU atomic (they ARE the DrawIndexedIndirectCount
    // counts, methodology F1); stats give the subgroup-reduced triangle counts.
    BufferHandle m_SharedCursorReadback{};
    BufferHandle m_SharedStatsReadback{};

    // ---- LOD dwell-band history (binding 10) ----
    // One persistent GPU-only buffer per camera view, indexed by the GLOBAL
    // instance index — the shape hzb_culling.comp's prevVisible[] established.
    // Keyed by viewId alone: only cascadeIndex == kCascadeIndexNone slices carry
    // a band, and a view's phase-A and phase-B slices SHARE one buffer because
    // their visibility slices are disjoint. Allocated lazily on the first slice
    // that asks (band > 0), so the default-off configuration allocates nothing.
    struct PrevLodView
    {
        BufferHandle buffer{};
        size_t       bytes = 0;
        // Owned so the handle's debug label outlives CreateBuffer (the
        // m_PrevVisible pattern keeps its name alive for the same reason).
        std::string  debugName;
    };
    std::unordered_map<uint32_t, PrevLodView> m_PrevLod;
    // Per-view pending reset sets, fed by OnInstanceSlotsRecycled — the
    // lodFade tracker's twin for the dwell-band history. Separate instances
    // because each Ensure* call TAKES its view's pending set: sharing one
    // tracker would let whichever buffer resolves first starve the other.
    PrevVisibleResetTracker m_PrevLodResetTracker;
    // Frame-local scratch: a view's taken pending slots and their coalesced
    // [startElement, countElements) runs. Empty in steady state.
    std::vector<uint32_t>                      m_PrevLodResetScratch;
    std::vector<std::pair<uint32_t, uint32_t>> m_PrevLodResetRuns;
    // Ensure this view's history buffer exists and covers `instanceCount`
    // entries. Returns the handle plus the element ranges the caller must
    // refill with kLodNoHistory before the dispatches read them: the whole
    // buffer on first touch/grow, else the runs this view has pending. Both
    // are emitted as fills inside the scatter pass, ahead of its existing
    // transfer->compute barrier.
    struct PrevLodBinding
    {
        BufferHandle handle{};
        size_t       bytes = 0;
        bool         clearAll = false;
        std::vector<std::pair<uint32_t, uint32_t>> resetRuns; // element runs
    };
    PrevLodBinding EnsurePrevLodBuffer(uint32_t viewId, uint32_t instanceCount);
    // Never-read stand-in bound at bindings 10, 12 and 13 for slices that carry
    // neither a dwell band nor a rendered history, so those bindings are always
    // valid (the binding-8/9 precedent). Allocated in EnsureArenaBuffers so it
    // shares that function's validity gate.
    BufferHandle m_HistoryStandIn{};

    // ---- Rendered level and phase history (bindings 12/13) ----
    // Per view, a PAIR of GPU-only buffers indexed by the global instance index.
    // The roles rotate once per rendered frame: the scatter reads the previous
    // buffer and writes the current one, and nothing in a frame writes what that
    // frame reads. That is what lets a later dispatch of the same frame — the
    // other culling phase, or a consumer that pairs this frame's surface with
    // the previous frame's — see exactly the previous RENDERED frame rather than
    // a value this frame's first dispatch already overwrote. Allocated only for
    // views that ask (ViewLODParams::renderedLevelHistoryRequested), so the
    // default configuration allocates nothing and binds the stand-in.
    struct RenderedHistoryView
    {
        BufferHandle buffers[2]{};
        size_t       bytes = 0;
        // Which of the two the scatter writes this frame; 1 - current is the
        // buffer it reads.
        uint32_t     currentIndex = 0;
        // Arena frame stamp the role assignment above was made for. A second
        // schedule call of the same frame sees its own stamp and keeps the
        // assignment, so every dispatch of one frame agrees on both roles.
        uint64_t     assignedStamp = 0;
        // Set by the scatter pass's exec closure, which runs only when the
        // frame is recorded and submitted. Read on the next frame's rotation:
        // a frame that was declared and then abandoned leaves it false, and its
        // half-written current buffer is re-cleared and reused rather than
        // becoming a later frame's previous. Shared with the closure so it
        // outlives a shutdown mid-frame; one allocation per view, not per frame.
        std::shared_ptr<std::atomic<bool>> recorded;
        // Owned so the handles' debug labels outlive CreateBuffer.
        std::string  debugNames[2];
    };
    std::unordered_map<uint32_t, RenderedHistoryView> m_RenderedHistory;
    // Per-view pending reset sets for the PREVIOUS buffer, fed by
    // OnInstanceContinuityBroken. Its own tracker instance for the same reason
    // the dwell band has one: each Ensure* call TAKES its view's pending set.
    PrevVisibleResetTracker m_RenderedHistoryResetTracker;
    std::vector<uint32_t>                      m_RenderedHistoryResetScratch;
    std::vector<std::pair<uint32_t, uint32_t>> m_RenderedHistoryResetRuns;
    // Introspection mirrors of the map above and of the rotations performed,
    // summed over views. Atomic because the render path writes them while the
    // debug server reads them through IntrospectScatter, and a std::unordered_map
    // cannot be sized concurrently with an insert. A reader may see a value from
    // the frame in flight; this is instrumentation, not a gate.
    std::atomic<uint32_t> m_RenderedHistoryViewCount{0};
    std::atomic<uint64_t> m_RenderedHistoryRotations{0};
    // What one slice binds at 12/13 this frame, plus the fills the pass must
    // emit before its dispatches read them.
    struct RenderedHistoryBinding
    {
        BufferHandle previous{};
        BufferHandle current{};
        size_t       bytes = 0;
        // The current buffer is cleared to kLodNoHistory once per frame, before
        // the frame's first dispatch: an instance that claims no record leaves
        // the no-history value, which is what makes a culled instance read as
        // discontinuous when it reappears. True for the frame's first call only.
        bool         clearCurrent = false;
        // Whole previous buffer on first touch or growth; else the slots whose
        // continuity broke since this view last ran.
        bool         clearPreviousAll = false;
        std::vector<std::pair<uint32_t, uint32_t>> previousResetRuns;
        // Handed to the exec closure, which sets it to mark the frame recorded.
        std::shared_ptr<std::atomic<bool>> recorded;
    };
    // Ensure this view's pair exists, covers `instanceCount` entries and holds
    // this frame's role assignment; returns what to bind and what to fill.
    // Rotates roles on the first call of a new frame, and only if the frame that
    // wrote the outgoing current buffer was actually recorded.
    RenderedHistoryBinding EnsureRenderedHistoryBuffers(uint32_t viewId, uint32_t instanceCount);
    // Retire a view's pair and stop tracking it (the view stopped asking).
    void ReleaseRenderedHistory(uint32_t viewId);
    // GE_LOD_HYSTERESIS=0 forces every slice's band to 0, restoring stateless
    // selection for A/B measurement. Read once in Initialize.
    bool m_LodHysteresisEnabled = true;
    uint64_t     m_SharedIndirectionAddress = 0;
    uint32_t     m_RecordCapacity   = 0;  // records
    uint32_t     m_CursorCapacity   = 0;  // uints
    uint32_t     m_TableRingBytes   = 0;
    uint32_t     m_ArenaRecordCursor = 0;
    uint32_t     m_ArenaCursorCursor = 0;
    uint32_t     m_ArenaStatsCursor  = 0; // per-slice stats rows claimed this frame
    uint32_t     m_TableRingCursor   = 0; // bytes; monotonic, wraps at capacity
    uint32_t     m_TableRingFrameBytes = 0; // this frame's ring consumption (n6 headroom guard)
    // materialColorClass upload (rides the table ring). The map is NOT
    // frame-invariant: mid-frame material registration (scene-load pump,
    // thumbnail extraction) bumps MaterialSSBOGeneration, so a later schedule
    // call in the same frame rebuilds its color table from a newer span. When
    // m_ColorMapPerCall is set (default) the map is re-uploaded per call so the
    // (map, table) pair a slice binds always comes from one snapshot; the
    // cached offset/bytes hold the CURRENT call's upload. The kill-switch
    // (GE_SCATTER_COLORMAP_PERCALL=0) restores the old once-per-frame upload
    // (m_ClassMapUploaded latch) to A/B-reproduce the tableMiss tripwire.
    uint32_t     m_ClassMapRingOffset = 0;
    uint32_t     m_ClassMapBytes      = 0;
    bool         m_ClassMapUploaded   = false;
    // GE_SCATTER_COLORMAP_PERCALL (default on = fixed). Read once in Initialize.
    bool         m_ColorMapPerCall    = true;
    uint32_t     m_PendingRecordCapacity = 0; // grow requests applied next BeginArenaFrame
    uint32_t     m_PendingCursorCapacity = 0;
    // Parity ~2x's the per-frame table bytes on mirror-heavy scenes; without a
    // growth path the fixed ring would wedge (drop-every-frame) once a call's
    // two tables exceed it. Symmetric with records/cursors: requested when the
    // per-call fit check or the frames-in-flight headroom guard trips, applied
    // (retire + recreate larger) next BeginArenaFrame. Table ring is cheap
    // host-visible Upload memory, so growth is low-risk.
    uint32_t     m_PendingTableRingBytes = 0;
    bool         m_ArenaSkipLogged       = false;

    std::vector<SliceRegistration> m_FrameSlices;
    // Keyed by MakeStreamKey(viewId, cascadeIndex, materialIndex, meshIndex, phase).
    std::unordered_map<uint64_t, BatchDrawRange> m_RangeMap;

    // ── Idle recompute elision state ──
    // Settle mirrors GPUCullingPipeline::kElisionSettleFrames: two are the
    // proven minimum (one executed frame under identical inputs makes the
    // one-frame-lagged readback mirrors stationary), the third is margin.
    static constexpr uint32_t kElisionSettleFrames = 3;
    struct SuffixElisionGate
    {
        RecomputeElisionGate Gate;
        bool LogState = false; // transition-edge logging latch
    };
    std::unordered_map<std::string, SuffixElisionGate> m_ElisionGates;
    ElisionFrameContext m_ElisionCtx{};
    uint64_t m_ArenaFrameStamp = 0; // ticks per BeginArenaFrame (the gates' stamp)
    float    m_FrameTimeSeconds = 0.0f; // see SetFrameTimeSeconds

    // ---- P0 shadow-arc per-slice instrumentation ----
    // One entry per registered scatter slice this frame, enough to reduce the
    // per-slice counts on the next BeginArenaFrame: which stats row it wrote and
    // which cursor entries hold its passed-caster counts.
    struct SliceStatsBinding
    {
        uint32_t viewId       = 0;
        uint8_t  cascadeIndex = kCascadeIndexNone;
        uint8_t  phase        = 0;
        uint32_t statsIndex   = 0; // row in the per-slice stats array
        uint32_t cursorBase   = 0; // element offset into cursors[] for this slice
        uint32_t batchCount   = 0; // cursor entries to sum for passed-casters
        // Element offset of this slice's TAIL row cursors; 0 = no tail region.
        // Summed alongside the head so passed-casters keeps counting every
        // record the slice emitted, fading pairs included.
        uint32_t tailCursorBase = 0;
    };
    std::vector<SliceStatsBinding>  m_SliceStatsBindings;
    // Tail-capable slices this frame that got no binding because their stats row
    // fell past kStatsSliceCapacity. Their tail cursors are unreadable, so the
    // census cannot distinguish "no fade" from "a fade nobody counted" — the
    // reduce reports the whole reading invalid rather than a silent zero, which
    // is the difference between suppressing elision and freezing a record pair.
    // Same lifetime as m_SliceStatsBindings: filled while scheduling, consumed by
    // the next reduce, cleared by the per-frame reset after it.
    uint32_t                        m_UnobservableTailSlices = 0;
    std::vector<ShadowArcSliceStat> m_ShadowArcStats;
    // Fence-gate reject-rate counters (surfaced by Get*). Attempts counts every
    // ReduceShadowArcStats call; StaleRejects counts the ones that left the stats
    // empty because the prior frame's graphics work was still in flight.
    uint64_t                        m_ShadowArcReduceAttempts = 0;
    uint64_t                        m_ShadowArcStaleRejects   = 0;
    // Σ tail rows over the reduced slices, stamped with the recompute count the
    // reading covers. Cleared to invalid on every reduce that produces no rows.
    LiveTailObservation             m_LiveTail{};
    // Tripwire counters summed over the previous frame's real slices, cached by
    // ReduceShadowArcStats. ReadScatterStats returns this (no per-call remap).
    ScatterStats                    m_LastTripwire{};
    bool                            m_StatsSliceOverflowLogged = false;
    // Per-slice attribution for the tripwire log: which (view, cascade, phase)
    // slice actually recorded a miss/overflow. Populated by ReduceShadowArcStats
    // only for slices with a non-zero counter, consumed by the BeginArenaFrame
    // tripwire log so a fire names the offending slice instead of a bare sum.
    struct TripwireSliceHit
    {
        uint32_t viewId       = 0;
        uint8_t  cascadeIndex = kCascadeIndexNone;
        uint8_t  phase        = 0;
        uint32_t tableMisses  = 0;
        uint32_t overflows    = 0;
    };
    std::vector<TripwireSliceHit>   m_LastTripwireSlices;
    // Rising-edge latch for the tripwire log: log fully the frame the counters
    // go non-zero, suppress while they stay non-zero, re-arm when they clear.
    // Stops per-frame ERROR spam during a multi-frame transient.
    bool                            m_TripwireLatched = false;
    // Reduce the just-completed frame's per-slice stats + cursor mirror into
    // m_ShadowArcStats and refresh m_LastTripwire; returns the tripwire.
    ScatterStats ReduceShadowArcStats();

    // ---- LOD crossfade state (binding 11) ----
    // Persistent per-camera-view transition state (GPU-only, N x 8 B, indexed by
    // the GLOBAL instance index), the same shape as GPUCulling's per-view
    // occlusion history. Keyed by viewId because each camera picks its own
    // levels; a view's phase-A and phase-B slices share one buffer, which is
    // sound because their visibility slices are disjoint. Created lazily and
    // only while the feature is on — an off frame allocates nothing.
    struct LodFadeView
    {
        BufferHandle buffer{};
        size_t       bytes = 0;
    };
    std::unordered_map<uint32_t, LodFadeView> m_LodFade;
    // Per-view pending reset sets, fed by OnInstanceSlotsRecycled. A view idle
    // for a frame still applies the resets it missed when it returns, which is
    // why the pending set is per view rather than a single global list.
    PrevVisibleResetTracker m_LodFadeResetTracker;
    // Frame-local scratch: a view's taken pending slots and their coalesced
    // [startElement, countElements) runs. Empty in steady state.
    std::vector<uint32_t>                     m_LodFadeResetScratch;
    std::vector<std::pair<uint32_t, uint32_t>> m_LodFadeResetRuns;
    // Ensure this view's fade buffer exists and covers `instanceCount` entries.
    // Returns the handle plus the byte ranges the caller must refill with
    // kLodNoHistory before the dispatches read them: the whole buffer on first
    // touch/grow, else the runs this view has pending. Both are emitted as fills
    // inside the scatter pass, ahead of its existing transfer->compute barrier.
    struct LodFadeBinding
    {
        BufferHandle handle{};
        size_t       bytes = 0;
        bool         clearAll = false;
        std::vector<std::pair<uint32_t, uint32_t>> resetRuns; // element runs
    };
    LodFadeBinding EnsureLodFadeBuffer(uint32_t viewId, uint32_t instanceCount);

    struct RetiredBuffer
    {
        BufferHandle handle{};
        uint32_t     framesLeft = 0;
    };
    std::vector<RetiredBuffer> m_RetiredBuffers;

    static ShaderLoaderFunc s_ShaderLoader;
};

} // namespace Rendering
} // namespace GameEngine
