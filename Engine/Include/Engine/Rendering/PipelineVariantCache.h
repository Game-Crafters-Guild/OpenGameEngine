// Pipeline-variant compilation cache, owned by RenderServices and reached via
// RenderServices::PipelineVariants(). Owns the instanced color and depth-only
// pipeline variant caches (keyed on material x vertex flags x pass keywords x
// samples x topology x cull mode), the shared depth-only pipeline (Shape B:
// one pipeline per vertex-flags/bias combo regardless of material), and the
// cross-thread material-eviction queue the render thread drains each frame.

#pragma once

#include "Events/Event.h"
#include "AssetCore/GUID.h"
#include "Engine/Rendering/MaterialCompileSpec.h"
#include "Engine/Rendering/ServedGenerationTable.h"
#include "Engine/Rendering/ShaderCompilationCache.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/PipelineIdentifiers.h"
#include "Rendering/Materials/ShaderMeta.h"

#include <array>
#include <functional>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::Rendering::RenderGraph
{
class RGContext;
}

namespace GameEngine::Engine::Renderer
{
namespace CullModeFlagBits = ::GameEngine::Rendering::CullModeFlagBits;
using ::GameEngine::Rendering::CullModeFlags;
using ::GameEngine::Rendering::DescriptorSetLayoutId;
using ::GameEngine::Rendering::FrontFace;
using ::GameEngine::Rendering::GraphicsPipelineId;
using ::GameEngine::Rendering::MaterialKeyword;
using ::GameEngine::Rendering::PipelineHandle;
using ::GameEngine::Rendering::PrimitiveTopology;
using ::GameEngine::Rendering::ShaderMeta;
using ::GameEngine::Rendering::VertexAttributeFlags;
namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;

class Material;
class MaterialSystem;

class PipelineVariantCache
{
  public:
    // The cache reaches the material facade for the shader-compilation cache,
    // the material build context, EnsureMaterialBuildContextReady, the device,
    // and the two debug rasterizer overrides (A1.3 §0a-A1).
    explicit PipelineVariantCache(MaterialSystem& materials);

    // Fired after the serial publish drain (ApplyPendingVariantPublishes)
    // lands a compiled pass variant in the cache, with the owning material's
    // GUID — so when a subscriber reacts, the variant is already servable.
    // Consumers that render a material once and cache the pixels (thumbnails)
    // use this to re-render when a variant that was missing at draw time
    // lands: the publish-gate skips such draws, which self-heals for per-frame
    // scene meshes but leaves a render-once consumer holding a blank.
    // Subscribe() rather than a single setter: a second consumer would
    // otherwise silently replace the first, and the returned handle detaches on
    // destruction so a subscriber outliving the cache cannot dangle.
    Event<const GUID&> VariantPublished;

    // Instanced pipeline variant cache entry: the interned pipeline id plus the
    // reflection/descriptor metadata MaterialBinder needs per draw.
    struct InstancedVariantKey
    {
        const Material* material;
        VertexAttributeFlags VertexFlags;
        uint32_t SampleCount;
        MaterialKeyword PassKeywords;
        PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
        CullModeFlags CullMode = CullModeFlagBits::Back;
        // Front face is a PSO-key dimension (rasterizer state is baked, not
        // dynamic, on all backends). Mirrored (negative-determinant) instances
        // draw with the flipped winding, so the two windings of one material
        // must NOT alias to one pipeline. Defaults to the view's base winding.
        FrontFace Winding = FrontFace::CounterClockwise;
        // Depth-only raster state baked into the interned pipeline. Both are key
        // dimensions because the same material is drawn into passes that disagree
        // about them: the camera prepass (no bias, no clamp), the directional
        // cascades (both), and the punctual shadow maps (bias only). Without them
        // the first compile wins and later passes silently inherit its raster
        // state. Always false on the colour path.
        bool DepthBiasEnable = false;
        bool DepthClampEnable = false;
        bool operator==(const InstancedVariantKey& o) const
        {
            return material == o.material && VertexFlags == o.VertexFlags
                && PassKeywords == o.PassKeywords && SampleCount == o.SampleCount
                && Topology == o.Topology && CullMode == o.CullMode
                && Winding == o.Winding && DepthBiasEnable == o.DepthBiasEnable
                && DepthClampEnable == o.DepthClampEnable;
        }
    };
    // SampleCount shares the 8-byte slot after VertexFlags so the 64-bit
    // PassKeywords adds no padding hole. The size holds wherever pointers are
    // 8 bytes; the wasm32 build differs.
    static_assert(sizeof(void*) != 8 || sizeof(InstancedVariantKey) == 40,
                  "InstancedVariantKey changed size: order its fields so none leaves a padding hole, then update this size");
    struct InstancedVariantKeyHash
    {
        size_t operator()(const InstancedVariantKey& k) const noexcept
        {
            size_t h = std::hash<const void*>{}(k.material);
            h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.VertexFlags))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint64_t>{}(static_cast<uint64_t>(k.PassKeywords))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(k.SampleCount)
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.Topology))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.CullMode))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(k.Winding))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<uint32_t>{}((k.DepthBiasEnable ? 1u : 0u)
                                       | (k.DepthClampEnable ? 2u : 0u))
                 + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };
    // One published variant, served as an indivisible unit: the pipeline the
    // draw binds and the metadata the binder walks for it always describe the
    // same layout. Serving one unit's pipeline with another's meta would break
    // the binder invariant on VariantMeta below whenever an edit changes
    // reflected bindings.
    struct VariantServeUnit
    {
        // Interned id of the compiled variant pipeline. The record path
        // resolves the concrete PipelineHandle per draw with the device's
        // non-building TryGetWarmGraphicsPipeline probe (ResolveWarmPipeline);
        // a (id, formatKey) pair that is cold for the active pass is handed to
        // a prewarm worker instead of being built on the record thread.
        GraphicsPipelineId PipelineId{};
        // Cached descriptor-set-layout ids for this unit's pipeline —
        // MaterialBinder's sticky-bind invalidation reads these on SetPipeline
        // without a mutex-guarded device lookup.
        std::vector<DescriptorSetLayoutId> SetLayouts;
        // Reflected meta of the compiled VARIANT (base meta + per-pass keywords
        // like Instanced/ForwardPlus/Shadows, which add set-0 bindings).
        // MaterialBinder must walk this — not the base material's meta — so the
        // descriptor set it builds matches the pipeline layout it binds.
        std::shared_ptr<ShaderMeta> VariantMeta;
        // Material::GetVersion() this unit was compiled against — the shader
        // generation a draw that binds this unit actually renders. Travels
        // with the unit so a Previous unit served through a recompile window
        // still names its own generation, not the replacement's.
        uint32_t MaterialVersion = 0;
    };
    struct InstancedVariantEntry
    {
        // The freshness check compares Current.MaterialVersion against the
        // material's live version; an entry whose Current is behind it serves
        // stale (see the stale-serve path) while the replacement compiles.
        VariantServeUnit Current;
        // The unit this entry served before the latest republish (hot reload).
        // Its concrete pipelines are already built for the passes that drew it,
        // so a record worker keeps drawing the previous shader — pipeline, meta
        // and set layouts as one coherent snapshot — for the frames it takes a
        // worker to build the new pipeline's backend state; a hot-reloaded
        // object stays on screen instead of leaving a hole. Empty until the
        // entry has been republished at least once.
        VariantServeUnit Previous;
    };

    // Which fragment the depth-pass pipeline composes. Mutually exclusive:
    // None = pure depth (no fragment, keeps early-Z); CoverageDiscard = composed
    // fragment whose only job is to discard — cutout alpha, the LOD-crossfade
    // dither, or both, with no colour attachment; GlassTintColor = composed
    // fragment writing the glass tint to a multiplicatively-blended colour
    // target with depth-write off.
    // Which fragment shape a depth-family pipeline takes. None keeps early-Z
    // and no fragment stage at all; the other three compose one.
    // GlassTintColor and MotionColor each attach exactly one colour target and
    // leave depth write off — the tint blends into the cascade's transmittance
    // array, the motion payload is an unblended value written under the
    // camera's own read-only depth.
    enum class DepthFragmentMode : uint8_t { None, CoverageDiscard, GlassTintColor, MotionColor };

    // Compile (or fetch) a color-pass variant for material x vertex flags x
    // pass keywords against the active pass formats. The returned unit is the
    // one the handle was resolved from (current, or previous during a rebuild
    // window), so its meta/set layouts always match the bound pipeline.
    // {invalid, nullptr} on failure.
    std::pair<PipelineHandle, const VariantServeUnit*> GetOrCompileColorVariant(
        const Material& material,
        VertexAttributeFlags vertexFlags,
        PrimitiveTopology topology,
        MaterialKeyword passKeywords,
        FrontFace frontFace,
        const RenderGraph::RGContext& ctx);

    // Depth-pass mirror of GetOrCompileColorVariant: compiles with passKeywords
    // only (fragment-stage keywords are irrelevant depth-only).
    std::pair<PipelineHandle, const VariantServeUnit*> GetOrCompileDepthInstancedVariant(
        const Material& material,
        VertexAttributeFlags vertexFlags,
        PrimitiveTopology topology,
        MaterialKeyword passKeywords,
        uint32_t samples,
        bool depthBiasEnable,
        bool depthClampEnable,
        FrontFace frontFace,
        const RenderGraph::RGContext& ctx);

    // The published camera-prepass variant a draw producer pins on a head it
    // emits while the frame is declared (DrawCommand::InternedPipeline and
    // PipelineMeta): the prepass raster state (no bias, no clamp, the pass's
    // own sample count) and `passKeywords` as ChooseDepthHeadPipeline picked
    // them. Requests the compile on a miss and returns nullptr until the
    // variant compiled from the material's current version publishes, so the
    // producer knows at emit whether the prepass will draw its head; a stale
    // shader could place the head's vertices away from the colour draw's.
    // Declaration thread only, outside the record window (the nodes it reads
    // are written by the serial frame-begin publish).
    const VariantServeUnit* FindOrRequestPrepassVariant(const Material& material,
                                                        VertexAttributeFlags vertexFlags,
                                                        PrimitiveTopology topology,
                                                        MaterialKeyword passKeywords,
                                                        FrontFace frontFace);

    // Whether the device has built pipeline `id` for the pass formats `fk`.
    // When it has not, requests the build on a prewarm worker, once: a build
    // that failed is not requested again, so a pipeline the device cannot
    // build stays cold. A draw producer that pins `id` on a command it emits
    // at declare time (a forward producer's prepass head) emits that command
    // only once this holds, so a pass never relies on a draw it cannot record.
    // Declaration thread.
    bool EnsureConcreteWarm(const Material& material, GraphicsPipelineId id,
                            const Rendering::PipelineFormatKey& fk);

    // Build + intern a depth-only graphics pipeline for the given shader
    // variant and vertex flags. `variant` is shared by ptr so the pipeline desc
    // can alias the SPIR-V storage owned by the shader cache without a copy.
    // `declaredMeta`, when set, is the meta the pipeline's set layouts are built
    // from instead of the variant's own (CompatVertexStageDepthMeta); it must be
    // the meta the binder walks for this pipeline.
    GraphicsPipelineId InternDepthPipelineId(
        const std::shared_ptr<const SharedShaderVariant>& variant,
        VertexAttributeFlags vertexFlags,
        PrimitiveTopology topology,
        CullModeFlags cullMode,
        bool depthBiasEnable,
        bool depthClampEnable,
        FrontFace frontFace,
        DepthFragmentMode fragmentMode,
        bool patchBindlessTextureSet,
        const Rendering::ShaderMeta* declaredMeta = nullptr);

    // Shared depth-only pipeline (Shape B): one pipeline id per
    // (vertexFlags, topology, cullMode, depthBiasEnable, depthClampEnable) combo
    // regardless of material, so MaterialBinder's sticky-bind collapses N
    // pipeline binds per pass to 1 for any opaque draw. INVALID id if the shared
    // .spv isn't available — caller falls back to
    // GetOrCompileDepthInstancedVariant.
    // `vertexFlags` MUST match the bucket flags the bound vertex buffer was
    // filled with (the binding stride feeds position fetches).
    GraphicsPipelineId GetSharedDepthPipelineId(
        VertexAttributeFlags vertexFlags,
        PrimitiveTopology topology,
        CullModeFlags cullMode,
        bool depthBiasEnable,
        bool depthClampEnable,
        FrontFace frontFace);

    // Reflected meta + cached set-layout ids for the shared depth shader —
    // drives MaterialBinder's descriptor walk like a per-material variant's.
    const ShaderMeta* GetSharedDepthShaderMeta(bool isSkinned) const;
    std::span<const DescriptorSetLayoutId> GetSharedDepthSetLayouts(bool isSkinned) const;

    // Force both shared-depth variants (non-skinned + skinned) fully warm —
    // shader load (file I/O + reflection) AND the interned set-layout ids — so
    // no worker triggers a lazy load or mutates m_SharedDepthVariants mid-record
    // once parallel recording (A2.4) opens its window. A2.4-D6 prerequisite:
    // call serially before the record window (RenderServices drives it each
    // frame via MaterialSystem::BeginFrame). Idempotent after the first call.
    void PreloadSharedDepthShaders();

    // Cross-thread variant eviction. Any thread may enqueue (the Material* is
    // an opaque address, never dereferenced after enqueue); the render thread
    // drains at BeginWorldDrawFrame and erases matching cache entries.
    void EnqueueEviction(const Material* mat);
    size_t PendingEvictionCount() const;
    void DrainPendingEvictions();

    // Publish-gate drain. A record-worker variant miss does NOT compile inline
    // (a cold shaderc variant is 10+s and would stall the record thread / scene-
    // build pump); it enqueues the miss and skips the draw. This serial main-
    // thread drain — called from MaterialSystem::BeginFrame before any parallel
    // record window opens — submits each queued compile to the prewarm workers
    // (or compiles inline when the engine job system is down: headless/tests).
    // The compiled unit is queued as a publish, applied to the cache by
    // ApplyPendingVariantPublishes, and the draw self-heals to visible.
    // Deterministic seam: FlushAsyncMaterialCompiles drains the submitted work
    // and applies the publishes; after it the requested variants are warm.
    void SubmitPendingVariantCompiles();
    size_t PendingVariantCompileCount() const;

    // True while a draw of `material` is skipped because a pipeline it needs is
    // still being built: from a variant miss until the serial apply publishes
    // that variant (queued, compiling, or compiled and awaiting
    // ApplyPendingVariantPublishes), and from a cold backend pipeline for the
    // current pass format until the worker's build finishes. A render-once
    // consumer of the material that renders inside this window caches a frame
    // without those draws.
    bool HasPipelineBuildsInFlight(const Material& material) const;

    // Apply the variant units the compile workers (or the inline headless path)
    // queued since the last drain: insert/republish the cache nodes, clear the
    // in-flight markers, fire VariantPublished. Serial-frame-begin-thread only
    // (MaterialSystem::BeginFrame, after SubmitPendingVariantCompiles) — this is
    // the ONLY writer of live cache nodes, which is what makes the record path's
    // unlocked entry-field reads and stale-serve pointers safe: every node write
    // is phase-separated from the record window, exactly like the eviction
    // drain and ApplyPendingPipelinePublishes for base pipelines.
    void ApplyPendingVariantPublishes();

    // Drop the backend-build waits that are over: the build settled (Warm or
    // Failed) or no material waits on it any more. Without it a settled wait
    // nobody polls would outlive its build, and a later cache invalidation
    // would have HasPipelineBuildsInFlight request a pipeline no draw uses.
    // Serial frame-begin thread (MaterialSystem::BeginFrame).
    void PruneSettledConcreteWarmWaits();

    // Drop every queued/in-flight variant compile — every queued, not-yet-
    // applied publish, and the material's wait on a backend build — for a
    // material about to be freed (called from the MaterialRegistry
    // pre-unregister callback, like EnqueueEviction). Defends
    // the serial drain's Material deref against a future off-main Unregister;
    // purging a still-running compile is safe — the worker holds a value
    // snapshot and its guard's marker erase becomes a no-op.
    void PurgePendingVariantCompiles(const Material* mat);

    // Test seam: enqueue a color variant compile exactly as the record-worker
    // miss path would, and return the key it built so a test can assert warmth
    // via HasColorVariantForTesting after SubmitPendingVariantCompiles. Test-only
    // (the production entry is the private miss path, which needs a live RGContext).
    InstancedVariantKey EnqueueColorVariantCompileForTesting(
        const Material& material,
        VertexAttributeFlags vertexFlags,
        PrimitiveTopology topology,
        MaterialKeyword passKeywords,
        uint32_t sampleCount,
        FrontFace frontFace);
    bool HasColorVariantForTesting(const InstancedVariantKey& key) const;
    // Depth mirror of the color test seams (drives the RunDepthVariantCompile body,
    // which — unlike color — has no base-invalid short-circuit, so it also exercises
    // the failure-path marker clear when the surface can't compile).
    InstancedVariantKey EnqueueDepthVariantCompileForTesting(
        const Material& material,
        VertexAttributeFlags vertexFlags,
        PrimitiveTopology topology,
        MaterialKeyword passKeywords,
        uint32_t sampleCount,
        bool depthBiasEnable,
        bool depthClampEnable,
        FrontFace frontFace);
    bool HasDepthVariantForTesting(const InstancedVariantKey& key) const;
    // Test seam: value snapshot of a color entry's published state so a test can
    // pin WHEN a republish rewrites the live node (only in the serial apply,
    // never from the compile worker). Zero-initialized when the key has no entry.
    struct VariantEntrySnapshotForTesting
    {
        GraphicsPipelineId CurrentPipelineId{};
        GraphicsPipelineId PreviousPipelineId{};
        uint32_t MaterialVersion = 0;         // Current unit's generation
        uint32_t PreviousMaterialVersion = 0; // Previous unit's generation (0 when none)
    };
    VariantEntrySnapshotForTesting SnapshotColorVariantForTesting(
        const InstancedVariantKey& key) const;
    // Test seam: seed a queued color-variant publish exactly as a compile worker
    // would, without running a compile — the late-publish shape (a worker that
    // outlives its material's unregister queues after the purge). Drive
    // ApplyPendingVariantPublishes to exercise the drop-vs-insert decision.
    void EnqueueColorVariantPublishForTesting(const InstancedVariantKey& key,
                                              GraphicsPipelineId pipelineId,
                                              uint32_t materialVersion,
                                              const GUID& materialGuid);

    // Device-rebuilt hook (RenderServices::OnDeviceRebuilt, alongside the device
    // cache's ClearConcreteOnly): the waits describe builds against the
    // torn-down device, which cancelled them, so they are dropped; the next cold
    // draw requests again.
    void OnDeviceRebuilt();

    // Cache sizes, surfaced for diagnostics (each depth pipeline is a distinct
    // vkCmdBindPipeline per pass — meaningful for cascade cost).
    size_t InstancedCount() const { return m_InstancedVariantCache.size(); }
    size_t DepthInstancedCount() const { return m_DepthInstancedVariantCache.size(); }
    size_t SharedDepthCount() const
    {
        return (m_SharedDepthVariants[0].Variant ? 1u : 0u)
             + (m_SharedDepthVariants[1].Variant ? 1u : 0u);
    }
    // Total variant compiles the async publish-gate path has run — the compiles
    // that no longer block the record thread. Surfaced for cold-load diagnostics.
    uint64_t AsyncVariantCompileCount() const
    {
        return m_AsyncVariantCompilesTotal.load(std::memory_order_relaxed);
    }

    // Served-generation table. Every unit ResolveWarmPipeline hands to a draw
    // records the generation it was compiled against under (material index,
    // pass class) for the current frame, so a later pass can ask whether the
    // colour and depth draws of a material this frame rendered the generation
    // it is about to serve — during a recompile window the stale-serve path
    // makes them disagree. BeginServeFrame runs once per frame on the serial
    // frame-begin thread (MaterialSystem::BeginFrame), sized to the material
    // index domain; the recording itself is wait-free from the record workers.
    void BeginServeFrame(uint32_t materialIndexCount) { m_ServedGenerations.BeginFrame(materialIndexCount); }
    std::optional<uint32_t> ServedGeneration(uint32_t materialIndex, ServedPassClass passClass) const
    {
        return m_ServedGenerations.Get(materialIndex, passClass);
    }

  private:
    using VariantCacheMap =
        std::unordered_map<InstancedVariantKey, InstancedVariantEntry, InstancedVariantKeyHash>;

    // Shared bodies: both ctx types expose GetCurrentSampleCount() +
    // GetOrCreatePipelineVariant() name-identically; the caches they mutate are
    // shared state, so the logic is templated. Instantiated in the .cpp only.
    template <class Ctx>
    std::pair<PipelineHandle, const VariantServeUnit*> GetOrCompileColorVariantImpl(
        const Material& material,
        VertexAttributeFlags vertexFlags,
        PrimitiveTopology topology,
        MaterialKeyword passKeywords,
        FrontFace frontFace,
        const Ctx& ctx,
        VariantCacheMap& cache);
    template <class Ctx>
    std::pair<PipelineHandle, const VariantServeUnit*> GetOrCompileDepthInstancedVariantImpl(
        const Material& material,
        VertexAttributeFlags vertexFlags,
        PrimitiveTopology topology,
        MaterialKeyword passKeywords,
        uint32_t samples,
        bool depthBiasEnable,
        bool depthClampEnable,
        FrontFace frontFace,
        const Ctx& ctx);

    bool EnsureSharedDepthShaderLoaded(bool isSkinned);

    // Resolve the concrete pipeline for an entry WITHOUT building it here. A
    // record worker must never run a backend pipeline build: on Metal that is
    // ~500 ms of MSL compilation per stage, which freezes the editor for the
    // length of the build. Cold newest id -> hand the build to a worker and
    // serve the entry's PREVIOUS unit for this frame; invalid handle only when
    // there is no previous one either (first-ever use, which the publish gate
    // already handles by skipping the draw). The returned unit is the one the
    // handle came from, so the binder's meta always matches the bound pipeline.
    // The served unit's generation is recorded per (material, pass class) for
    // this frame (see ServedGeneration).
    template <class Ctx>
    std::pair<PipelineHandle, const VariantServeUnit*> ResolveWarmPipeline(
        const Material& material, const InstancedVariantEntry& entry, uint32_t materialIndex,
        ServedPassClass passClass, const Ctx& ctx);

    // Fire-and-forget backend build of one (pipeline id, pass format) pair,
    // requested from the device (IDevice::RequestGraphicsPipeline), which builds
    // each pair once and does not retry a failed one. `material` is recorded as
    // waiting on the build (HasPipelineBuildsInFlight).
    void RequestConcreteWarm(const Material& material, GraphicsPipelineId id,
                             const Rendering::PipelineFormatKey& fk);

    // Rate-limited (power-of-two) warning for a dual-cold serve — fires per draw
    // per frame during a backend-build window, so the raw line would flood.
    void WarnColdServe(const Material& material, GraphicsPipelineId pipelineId,
                       const Rendering::PipelineFormatKey& fk);

    // Publish-gate: a queued variant miss (enqueued by the record path, drained
    // by SubmitPendingVariantCompiles). Carries only the cache key — which now
    // includes the depth-only raster bits — the rest of the compile inputs are
    // snapshotted from the (process-lifetime) Material on the serial drain thread.
    struct PendingVariantCompile
    {
        InstancedVariantKey Key;
        bool IsDepth = false;
    };
    // A fully value-snapshotted compile request handed to a prewarm worker (or
    // run inline). Holds NO Material reference — the worker must not deref the
    // Material (hot-reload mutates it on the main thread); everything it needs is
    // captured here on the serial drain.
    struct VariantCompileRequest
    {
        InstancedVariantKey Key;
        ShaderCacheKey CacheKey;
        MaterialCompileSpec Spec;
        // Snapshotted by value on the serial drain: EnsureMaterialBuildContextReady
        // reassigns the live m_MaterialBuildContext's vectors every BeginFrame, which
        // would tear under an off-thread GetOrCompile mid-copy during source-set churn.
        MaterialBuildContext BuildContext;
        std::string Name;
        uint32_t MaterialVersion = 0;
        GraphicsPipelineId BaseGraphicsPipelineId{}; // color only (base template copy)
        bool IsDepth = false;
    };
    // Record-worker miss handoff: dedupe against the in-flight set and append.
    void EnqueueVariantCompile(const InstancedVariantKey& key, bool isDepth);
    // Prewarm-worker (or inline) bodies: compile the SPIR-V, intern the pipeline,
    // queue the compiled unit as a pending publish. Workers never write live
    // cache nodes — the serial ApplyPendingVariantPublishes does. On the success
    // path the in-flight marker survives the worker (the serial apply erases it
    // after the insert, so a record miss never coexists with an absent marker);
    // an RAII scope guard clears it on every FAILURE exit — early return or a
    // throw mid-compile — so a key is never wedged.
    void RunColorVariantCompile(const VariantCompileRequest& req);
    void RunDepthVariantCompile(const VariantCompileRequest& req);
    // Clear a key's in-flight marker and decrement the running counter. Called by
    // the Run* scope guard on failure exits and by the submit-failure path.
    void ReleaseVariantCompile(const InstancedVariantKey& key, bool isDepth);

    // A compiled variant unit handed back by a worker (or the inline headless
    // path), waiting for the serial apply. Carries the material GUID for the
    // VariantPublished event — Key.material is never dereferenced at apply.
    struct PendingVariantPublish
    {
        InstancedVariantKey Key;
        GraphicsPipelineId PipelineId{};
        uint32_t MaterialVersion = 0;
        std::vector<DescriptorSetLayoutId> SetLayouts;
        std::shared_ptr<ShaderMeta> VariantMeta;
        GUID MaterialGuid;
        bool IsDepth = false;
    };
    void QueueVariantPublish(PendingVariantPublish&& publish);

    // Double-checked insert/republish used by the serial apply: another publish
    // (or a since-bumped material version) may have landed first, so keep one
    // node per key under the exclusive lock. Serial-window only — record
    // threads read entry fields and hold stale-serve pointers without the lock.
    void InsertVariantEntry(VariantCacheMap& cache, const InstancedVariantKey& key,
                            GraphicsPipelineId pipelineId, uint32_t materialVersion,
                            std::vector<DescriptorSetLayoutId> setLayouts,
                            std::shared_ptr<ShaderMeta> meta);

    // Debug-only ownership check: a cold insert must hold the exclusive lock.
    // Replaces the old single-writer tripwire — A2.4-D6 makes multi-thread
    // inserts legal, so the invariant is "written under m_CacheMutex", not
    // "written from one thread". A no-op in release.
    void AssertInsertLockHeld() const noexcept;

    MaterialSystem* m_Materials = nullptr;

    // Per-frame (material index, pass class) -> served generation; see
    // BeginServeFrame / ServedGeneration. Written from the record workers by
    // ResolveWarmPipeline, grown only on the serial frame-begin thread.
    ServedGenerationTable m_ServedGenerations;

    // A2.4-D6 record-cache thread-safety. The three variant maps are read by up
    // to N record workers concurrently and cold-inserted rarely. Lookups take a
    // shared_lock (on an L1 miss); the cold insert takes a unique_lock. A
    // per-thread L1 (below) fronts each map so steady-state hits never touch the
    // mutex — the exact pattern the device PipelineCache ships.
    mutable std::shared_mutex m_CacheMutex;

    // Hands out the epoch values that stamp L1 slots. Strictly increasing and
    // never reused for the process lifetime, so an epoch identifies one cache
    // instance in one state. An address cannot: the allocator reissues a dead
    // cache's address, and the slots that cache filled hold pointers into map
    // nodes it has already freed.
    static uint64_t NextCacheEpoch() noexcept;

    // Redrawn whenever an eviction erases map nodes, which retires slots that
    // may hold pointers into now-erased nodes (the maps are unordered_map, so
    // surviving-node pointers stay valid — only erased nodes must be retired),
    // and drawn once at construction so no slot an earlier cache left behind
    // can validate against this one.
    std::atomic<uint64_t> m_CacheEpoch;
#if GE_DEBUG_INSTRUMENTATION
    // Set true inside the unique_lock scope of a cold insert; AssertInsertLockHeld
    // reads it. Serialized by the exclusive lock, so no atomic needed.
    bool m_InsertLockHeldDebug = false;
#endif

    // Fixed-size per-thread L1 in front of each map (256 slots, power-of-two).
    // Slot struct definitions live below SharedDepthPipelineKey (they name it).
    static constexpr size_t kVariantL1Slots = 256;

    VariantCacheMap m_InstancedVariantCache;
    // Depth-only twin: same key/value types, stores depth-only pipelines (no
    // fragment shader) compiled with passKeywords (GE_INSTANCED).
    VariantCacheMap m_DepthInstancedVariantCache;

    // Cross-thread eviction handoff (see EnqueueEviction).
    mutable std::mutex m_PendingVariantCacheEvictionsMutex;
    std::vector<const Material*> m_PendingVariantCacheEvictions;

    // Publish-gate handoff. m_PendingVariantCompiles is the this-frame submit
    // queue (record workers append on a cold miss, the serial drain swaps it out
    // and submits); m_PendingVariantPublishes is the return leg (compile workers
    // append the finished unit, the serial apply swaps it out and inserts). The
    // two in-flight sets dedupe across frames: a key stays in its set from
    // enqueue until the serial apply publishes its entry (cache insert THEN
    // erase), so a still-compiling variant is requested exactly once. Only
    // touched on a cold miss / at the serial drain / at compile completion —
    // never on the steady-state hit path. Guarded by one mutex; contention is
    // cold-only.
    mutable std::mutex m_PendingVariantMutex;
    std::vector<PendingVariantCompile> m_PendingVariantCompiles;
    std::vector<PendingVariantPublish> m_PendingVariantPublishes;
    // Fired by the serial publish apply, after the unit is in the cache.
    // Event::Invoke copies the subscriber list under its own lock and calls
    // outside it, so a subscription arriving mid-init never races the fire.
    void FireVariantPublished(const GUID& guid) const { VariantPublished.Invoke(guid); }
    std::unordered_set<InstancedVariantKey, InstancedVariantKeyHash> m_ColorCompilesInFlight;
    std::unordered_set<InstancedVariantKey, InstancedVariantKeyHash> m_DepthCompilesInFlight;

    // Diagnostic count of variant compiles the async publish-gate path ran (the
    // work that used to block the record thread on a cold miss). Bumped on the
    // worker/inline compile path; read for diagnostics like the cache-size getters.
    std::atomic<uint64_t> m_AsyncVariantCompilesTotal{0};

    // Occurrence count for the dual-cold serve warning (neither the current nor
    // the previous pipeline warm for the active pass). Fires per draw per frame
    // for the whole backend-build window, so the log line is rate-limited to
    // powers of two of this count.
    std::atomic<uint64_t> m_ColdServeWarnCount{0};

    // Variant compiles currently dispatched-but-not-finished. Incremented before
    // each submit, decremented on every Run* exit (via ReleaseVariantCompile). The
    // drain submits only while this is below a worker-count-derived cap so the cold
    // burst can't oversubscribe the pool (the compiles also ride the Background lane).
    std::atomic<uint32_t> m_VariantCompilesRunning{0};

    // The materials whose draws found a backend pipeline cold, keyed by the
    // device's combined (pipeline id, format key) (materials share interned
    // pipelines). The build itself, its single flight and its failure memory
    // belong to the device; this records only who waits, for
    // HasPipelineBuildsInFlight, which asks the device whether each build is
    // still pending. A key leaves when a request finds it settled, at the
    // per-frame prune once its build settles or its materials are purged, or at
    // a device rebuild. Generation counts the Pending requests recorded on the
    // key, so the prune never drops a wait a request re-recorded while the
    // prune was asking the device.
    struct ConcreteWarmWait
    {
        GraphicsPipelineId Id{};
        Rendering::PipelineFormatKey FormatKey{};
        std::vector<const Material*> Materials;
        uint64_t Generation = 0;
    };
    mutable std::mutex m_ConcreteWarmMutex;
    std::unordered_map<uint64_t, ConcreteWarmWait> m_ConcreteWarmWaits;

    // Lazy-loaded SPV + reflected variant for the shared depth-only shader.
    // [0] = non-skinned, [1] = skinned.
    struct SharedDepthVariantEntry
    {
        std::shared_ptr<SharedShaderVariant> Variant;
        std::vector<DescriptorSetLayoutId> InternedSetLayoutIds;
        bool LoadAttempted = false;
    };
    SharedDepthVariantEntry m_SharedDepthVariants[2];

    // Cardinality at steady state ~= buckets x sidedness x {prepass-noBias, shadow-bias}.
    struct SharedDepthPipelineKey
    {
        VertexAttributeFlags VertexFlags;
        PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
        CullModeFlags CullMode = CullModeFlagBits::Back;
        bool DepthBiasEnable;
        // Pancaking is per-pass raster state, so it keys the PSO exactly as the
        // bias does — the directional cascades clamp and nothing else does.
        bool DepthClampEnable = false;
        // Front face keys the shared depth PSO too — a mirrored single-sided
        // caster's shadow/prepass draw needs the flipped winding.
        FrontFace Winding = FrontFace::CounterClockwise;
        bool operator==(const SharedDepthPipelineKey& o) const noexcept
        {
            return VertexFlags == o.VertexFlags && Topology == o.Topology
                && CullMode == o.CullMode && DepthBiasEnable == o.DepthBiasEnable
                && DepthClampEnable == o.DepthClampEnable
                && Winding == o.Winding;
        }
    };
    struct SharedDepthPipelineKeyHash
    {
        size_t operator()(const SharedDepthPipelineKey& k) const noexcept
        {
            return static_cast<size_t>(static_cast<uint32_t>(k.VertexFlags))
                 ^ (static_cast<size_t>(static_cast<uint32_t>(k.Topology)) << 24)
                 ^ (static_cast<size_t>(k.CullMode) << 16)
                 ^ (k.DepthBiasEnable ? 0x80000000u : 0u)
                 ^ (k.Winding == FrontFace::Clockwise ? 0x40000000u : 0u)
                 ^ (k.DepthClampEnable ? 0x20000000u : 0u);
        }
    };
    std::unordered_map<SharedDepthPipelineKey, GraphicsPipelineId, SharedDepthPipelineKeyHash>
        m_SharedDepthPipelines;

    // Per-thread L1 slots (see kVariantL1Slots). thread_local, so they are
    // shared by every cache instance the thread touches and outlive the one
    // that filled them; the stamped epoch is what makes a slot this cache's and
    // this state's. Mirrors PipelineCache.cpp's tls_L1. The instanced slot
    // caches a pointer into the map node (unordered_map node pointers are
    // stable); the shared-depth slot caches the small pipeline id by value.
    // Zero is never issued as an epoch, so a zeroed slot never validates.
    struct InstancedL1Slot
    {
        InstancedVariantKey Key{};
        const InstancedVariantEntry* Entry = nullptr;
        uint64_t Epoch = 0;
    };
    struct SharedDepthL1Slot
    {
        SharedDepthPipelineKey Key{};
        GraphicsPipelineId Value{};
        uint64_t Epoch = 0;
    };
};

} // namespace GameEngine::Engine::Renderer
