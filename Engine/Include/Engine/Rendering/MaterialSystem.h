// The material stack, extracted from RenderServices (A1.3).
//
// MaterialSystem owns the runtime material registry, the material compiler, the
// shader-compilation cache, the pipeline-variant cache, the async prewarm
// service + its drain machinery, the material build context, the MaterialParams
// SSBO packing state (the R0.1 idle-skip gate), the depth/color classification
// maps, the material hot-reload invalidator, and the MaterialBinder. It is owned
// by value on RenderServices and reached via rs.Materials().
//
// It is deliberately NOT self-contained (design §0.4): PackMaterialSSBO writes
// through the frame's PerFrameWritePool and reads TextureService bindless
// defaults; registration binds material texture refs through TextureService and
// interns PSOs on the device. Those blend points are INJECTED (device, per-frame
// write pool, texture service) rather than reached-back for. The world-pass
// keyword resolver stays on RenderServices and passes its result into the
// prewarm methods (the facade never touches blueprint/view state); the project
// warm-up, which admits its batches from BeginFrame, gets the resolver itself
// injected at Initialize. Exactly TWO
// documented reach-back edges to RenderServices remain (§0a-A3): the binder ctor
// (MaterialBinder(RenderServices&, IDevice&)) and the narrow m_CullMode/
// m_FrontFace debug-override read consumed at PSO build.
//
// Material::s_GlobalContentEpoch is untouched (design §2): the pack stamp keeps
// reading the single Material static so the cross-module idle-skip stays engaged.

#pragma once

#include "AssetCore/AssetReloadInvalidator.h"
#include "Engine/Rendering/MaterialRegistry.h"
#include "Engine/Rendering/ShaderCompilationCache.h"
#include "Engine/Rendering/ShaderCompileErrorLog.h"
#include "Rendering/Core/Device.h"
#include "Rendering/Core/FrameBufferAllocator.h"
#include "Rendering/Core/Handle.h"
#include "Rendering/Core/GPUInstanceDepthClass.h" // MaterialDepthClass
#include "JobSystem/JobCounter.h"
#include "JobSystem/TaskTypes.h"
#include "Rendering/Materials/MaterialBuildService.h" // MaterialBuildContext
#include "Rendering/Materials/ShaderVariantKey.h" // MaterialKeyword
#include "Types/StringId.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace GameEngine::Rendering
{
struct ShaderPropertyTable;
}

namespace GameEngine
{
// GUID, MaterialDocument, AssetEventDispatcher come in fully via the includes
// above (MaterialRegistry.h -> GUID.h / MaterialDocument.h; AssetReloadInvalidator.h
// -> AssetEvents.h).

namespace Engine::Renderer
{
using ::GameEngine::Rendering::BufferHandle;
using ::GameEngine::Rendering::CullModeFlags;
using ::GameEngine::Rendering::FrontFace;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::MaterialBuildContext;
using ::GameEngine::Rendering::MaterialDepthClass;
using ::GameEngine::Rendering::MaterialKeyword;

class CompileConcurrencyGate;
class Material;
class MaterialBinder;
class MaterialCompiler;
class PerFrameWritePool;
class PipelineVariantCache;
class ProjectMaterialPrewarmService;
class RenderServices;
class TextureService;

// Owns the material stack behind rs.Materials(). See file header.
class MaterialSystem
{
    // The variant cache reaches the moving trio (shader-compilation cache,
    // material build context, EnsureMaterialBuildContextReady) + the device +
    // the debug-override accessors on this facade (design §0a-A1).
    friend class PipelineVariantCache;

  public:
    // Defined out-of-line (with ~MaterialSystem) so the unique_ptr<CompileConcurrencyGate>
    // member's special members instantiate where that type is complete.
    MaterialSystem();
    ~MaterialSystem();

    MaterialSystem(const MaterialSystem&) = delete;
    MaterialSystem& operator=(const MaterialSystem&) = delete;

    // Consolidates today's RenderServices::Initialize material lines: prewarm/
    // binder/variants construction, the eager compiler creation, and the
    // registry init + pre-unregister callback wiring. `rs` is used only for the
    // binder ctor and the debug-override read (§0a-A3). `resolveWorldPassKeywords`
    // is RenderServices' world-pass keyword resolver, called only when the
    // project warm-up submits a batch.
    void Initialize(Rendering::IDevice* device, PerFrameWritePool& framePool,
                    TextureService& textures, RenderServices& rs,
                    std::function<Rendering::MaterialKeyword()> resolveWorldPassKeywords);
    // §1.3 two-phase teardown. Phase A runs BEFORE RenderServices tears down its
    // features (a feature destructor can unregister materials, and the
    // pre-unregister callback is still armed / the variant cache still alive at
    // that point); phase B runs after.
    void ShutdownPhaseA(); // invalidator reset, prewarm drains, binder reset
    void ShutdownPhaseB(); // shader cache clear, callback disarm, variants,
                           // registry, prewarm/compiler destroy, SSBO/fallback reset
    // §1.2 frame reset: variant-eviction drain then binder frame-begin, in that
    // order (adjacent in the current spine at :945 / :950-951). Ends by pumping
    // the project material warm-up.
    void BeginFrame();
    // The active render pipeline blueprint (RenderServices-owned) just swapped:
    // drain in-flight PSO prewarm before the old nodes are destroyed and drop
    // stale per-material variant entries. Both are material-cache concerns.
    void OnActiveRenderPipelineChanged();
    // A different project was just opened in this process. Every material-side
    // cache keyed on the old project's paths has to go; the reasons differ per
    // cache and are stated at the definition.
    void OnProjectSwitched();
    // A1.2 device discipline: nulled at the SAME Shutdown point as RS::m_Device.
    void SetDevice(Rendering::IDevice* device) { m_Device = device; }
    // §0a-A5: the RS fallback-buffer loop owns the physical MaterialParams
    // fallback (m_FallbackBuffers) and hands its handle here so binding 13 reads
    // valid zeros before PackMaterialSSBO first runs.
    void SeedMaterialParamsFallback(Rendering::BufferHandle buffer, size_t bytes);
    // Installs the .material hot-reload subscriber (the material arm of
    // RenderServices::InstallAssetReloadInvalidators, main-thread reloaded-only)
    // and the shader-source subscriber: an edited surface/vertex-modifier .glsl
    // enqueues from the watcher thread; DrainPendingShaderSourceEdits applies it.
    void InstallReloadInvalidator(AssetEventDispatcher& dispatcher);

    // A shader source file (surface .glsl, vertex modifier) changed on disk.
    // Thread-safe enqueue; the next BeginFrame invalidates every cached shader
    // referencing the file and recompiles the registered materials that use it.
    // Direct-call seam (editor recompile button, tests) — the asset event path
    // enqueues GUIDs via the subscriber instead.
    void NotifyShaderSourceEdited(const std::filesystem::path& sourcePath);

    // Drain NotifyShaderSourceEdited / shader asset events on the serial
    // frame-begin thread. Called by BeginFrame; public so tests can drive the
    // edit->recompile propagation without a frame loop. Recompiles are budgeted
    // (kShaderEditRecompileBudgetPerFrame): one call may leave work carried, so a
    // caller that needs a many-material surface fully recompiled must keep
    // draining — successive calls are how the carry drains.
    void DrainPendingShaderSourceEdits();

    // Live record of failed material shader compiles (failure upserts, success
    // clears) — the data source for the editor's Shader Errors panel.
    ShaderCompileErrorLog& ShaderErrors() { return m_ShaderErrors; }

    // True while the material's base pipeline compiles on a worker
    // (BaseCompileMode::Async) and its publish has not been applied. Main thread only.
    bool IsBaseCompileInFlight(const GUID& guid) const;

    // --- registration / compile / prewarm ---
    // BaseCompileMode::Async defers a first-registration material's base-shader
    // compile to a worker thread so a cold scene load doesn't block the main
    // thread — frames keep pumping. The material stays uncompiled (and is thus
    // skipped by the draw stream, the "dark fallback") until its pipeline is
    // published on a following BeginFrame. Synchronous keeps the compile inline
    // (deterministic; used by direct callers and the headless test path).
    enum class BaseCompileMode
    {
        Synchronous,
        Async,
    };
    Material* RegisterMaterialFromDocument(const GUID& guid, const MaterialDocument& doc,
                                           Rendering::MaterialKeyword additionalKeywords =
                                               Rendering::MaterialKeyword::None,
                                           BaseCompileMode baseCompileMode =
                                               BaseCompileMode::Synchronous);
    // Recompiles the material's base pipeline. The compile runs on a worker and
    // publishes on a later BeginFrame (ApplyPendingPipelinePublishes); the
    // material's pipeline id is retired immediately, so the draw stream and
    // thumbnail renders skip it until the publish lands. Main-thread only; falls
    // back to an inline compile when async can't run (headless/test paths).
    void RecompileMaterialPipeline(const GUID& guid, const MaterialDocument& doc);
    // colorPassKeywords is resolved RenderServices-side (ResolveWorldPassKeywordsForPrewarm
    // reads blueprint/view state, which stays on RS) and passed in — the facade
    // never reaches back into blueprint/view state (design §0.3).
    Material* RegisterAndPrewarmMaterial(const GUID& guid, const MaterialDocument& doc,
                                         Rendering::MaterialKeyword additionalKeywords,
                                         Rendering::MaterialKeyword colorPassKeywords);
    bool IsOwnerThread() const { return std::this_thread::get_id() == m_OwnerThreadId; }
    // Warms the registered material's draw variants from its own variant key and
    // compile inputs.
    void PrewarmMaterialVariants(Material& mat, Rendering::MaterialKeyword colorPassKeywords);
    // `materialAssetPath` is the .material file the document came from — part
    // of the shader cache-key identity and the anchor for material-relative
    // surface references (project-side surfaces can't resolve without it).
    void PrewarmMaterialBaseShader(const MaterialDocument& doc,
                                   const std::filesystem::path& materialAssetPath);
    // Project warm-up: compile without registering a runtime material or
    // uploading its textures, on the Background lane, counted by
    // GetProjectWarmUpProgress.
    void PrewarmMaterialShaders(const MaterialDocument& doc,
                                const std::filesystem::path& materialAssetPath,
                                Rendering::MaterialKeyword colorPassKeywords);

    // Blocks until every async base-shader compile submitted so far has finished
    // and its pipeline has been applied to the owning material. Test/tooling
    // determinism seam: after this returns, a material registered with
    // BaseCompileMode::Async has the same observable state (a valid pipeline id)
    // it would have had under Synchronous. Must be called from a non-worker
    // thread — it waits on the prewarm JobCounter. Not for per-frame use.
    void FlushAsyncMaterialCompiles();

    // Test seam: enqueue a synthetic base-pipeline publish record (as a worker
    // would) and mark it in flight, so a test can drive ApplyPendingPipelinePublishes'
    // guard directions (valid-pipeline skip, unregistered skip, stale-key re-drive,
    // failed-compile restore) through FlushAsyncMaterialCompiles without a live
    // job system. Test-only.
    void EnqueueBasePipelinePublishForTesting(const GUID& guid,
                                              Rendering::GraphicsPipelineId pipelineId,
                                              std::shared_ptr<Rendering::ShaderMeta> meta,
                                              const ShaderCacheKey& compiledKey,
                                              Rendering::GraphicsPipelineId lastGoodPipelineId = {});

    // --- async prewarm progress (editor "Compiling shaders…" indicator) ---
    // Cumulative counts of prewarm tasks (one task compiles a material's full
    // variant set), kept apart by origin: GetPrewarmProgress counts the compiles
    // something on screen is waiting for (registration, draw-path variant
    // misses), GetProjectWarmUpProgress the background project warm-up, which
    // no user-facing indicator shows. `submitted >= completed` always holds, so
    // pending = submitted - completed and drained means submitted == completed.
    // Write side is per-task only (bumped at prewarm submit/complete, never per
    // frame), so it adds zero idle cost. Read side is a cheap per-frame poll:
    // the editor reads this every frame to catch the idle->busy edge (two
    // relaxed atomic loads and a subtraction — negligible).
    struct PrewarmProgress
    {
        uint64_t submitted = 0;
        uint64_t completed = 0;
    };
    PrewarmProgress GetPrewarmProgress() const { return m_OnDemandPrewarm.Read(); }
    PrewarmProgress GetProjectWarmUpProgress() const { return m_ProjectWarmUp.Read(); }

    // --- per-frame ---
    void FinalizeFrameBuffers();
    uint64_t GetMaterialSSBOPackCount() const { return m_MaterialSSBOPackCount; }
    // Frames whose material rows did not fit the MaterialParams ring, each of
    // which shaded from the zero-filled fallback instead of real parameters.
    uint64_t GetMaterialParamsOverflowCount() const { return m_MaterialParamsOverflowCount; }
    struct PackedSSBO
    {
        Rendering::BufferHandle Buffer;
        size_t Offset;
        size_t Size;
    };
    PackedSSBO PackedMaterialParams() const
    {
        return {m_MaterialParamsSSBOBuffer, m_MaterialParamsSSBOOffset, m_MaterialParamsSSBOSize};
    }

    // --- classification (bucketer/scatter read side) ---
    Rendering::MaterialDepthClass GetMaterialDepthClass(uint32_t materialIndex) const
    {
        return materialIndex < m_MaterialDepthClass.size()
                   ? static_cast<Rendering::MaterialDepthClass>(m_MaterialDepthClass[materialIndex])
                   : Rendering::MaterialDepthClass::MaterialDependent;
    }
    std::span<const uint8_t> MaterialDepthClassSpan() const { return m_MaterialDepthClass; }
    // Deforming-motion membership per materialIndex (MaterialDeformationClassify.h),
    // refreshed on the same register / re-register edge as the depth class above.
    // The deformer lane's batch-key subset and the mover lane's exclusion both
    // read this one array, which is what makes "written exactly once" a
    // property of a single verdict rather than of two predicates agreeing.
    bool SupportsDeformationMotionAt(uint32_t materialIndex) const
    {
        return materialIndex < m_MaterialDeformationMotion.size()
               && m_MaterialDeformationMotion[materialIndex] != 0u;
    }
    // Index-mapping generation: bumped on GpuSceneMaterialIndex assignment,
    // unregister-free, and shutdown. Together with MaterialDepthClassSpan()
    // this is the extraction fast path's material-dirtiness digest (E7):
    // equal (generation, depth-class bytes) implies the extraction-visible
    // derived material state did not move.
    uint64_t MaterialSSBOGeneration() const { return m_MaterialSSBOGeneration; }
    // Rebuilds the color-class map if the material set changed, then returns it.
    std::span<const uint32_t> MaterialColorClassSpan();

    // --- component access ---
    MaterialRegistry& Registry() { return m_RuntimeMaterialRegistry; }
    const MaterialRegistry& Registry() const { return m_RuntimeMaterialRegistry; }
    MaterialCompiler& Compiler();
    MaterialBinder& Binder();
    const MaterialBinder& Binder() const;
    PipelineVariantCache& Variants() { return *m_PipelineVariants; }
    const PipelineVariantCache& Variants() const { return *m_PipelineVariants; }

    // The declared-property table of the program this document composes:
    // adapter + resolved surface + resolved vertex modifier, through the
    // ShaderPropertyTableCache the composer reads too. Serves the pipeline binds
    // (the lanes the CPU cache writes, re-laid whenever a compiled program
    // becomes the material's) and the inspector (the rows it renders).
    // nullptr when the build context is cold.
    std::shared_ptr<const Rendering::ShaderPropertyTable> ResolveDeclaredProperties(
        const MaterialDocument& doc, const std::filesystem::path& materialAssetPath);

    // How the document's surface takes its textures, decided by the rule registration applies,
    // for the document as it stands whether or not a material has registered it: the surface's
    // declared `// @texture` names in declaration order (empty for a surface that declares none,
    // which keeps the fixed ladder), and why the relief march refuses a bound height map, as the
    // message that states the fix (empty when nothing is refused). The material inspector lays out
    // its texture rows and its Relief Depth row from it, so an edit shows its effect at the next
    // rebuild. Both are empty when the surface cannot be resolved (build context cold, surface
    // unreadable or rejected): the compose reports that surface by name.
    struct SurfaceTextureUse
    {
        std::vector<std::string> DeclaredNames;
        std::string ParallaxRefusal;
    };
    SurfaceTextureUse ResolveSurfaceTextureUse(const MaterialDocument& doc,
                                               const std::filesystem::path& materialAssetPath);

    // True when the document's surface resolves outside the engine shader tree
    // and the package shader dirs — a project-authored surface. Such a surface
    // exposes parameters only by declaring them; the hand-typed lane table (and
    // the inspector rows that go with it) is for the engine and package
    // surfaces it still serves. False too when the build context is cold.
    bool IsSurfaceProjectOwned(const MaterialDocument& doc,
                               const std::filesystem::path& materialAssetPath);

    void SetMaterialBuildContext(const Rendering::MaterialBuildContext& ctx)
    {
        m_MaterialBuildContext = ctx;
        // Injected contexts are never re-derived from the mount table
        // (version 0 = "not self-derived" in EnsureMaterialBuildContextReady).
        m_MaterialBuildContextSourceVersion = 0;
    }

  private:
    // The registration/prewarm parity test reads DeriveShaderIdentity directly.
    friend class MaterialShaderIdentityTest;

    // The base variant key, compile inputs and drawn alpha mode of a material
    // registered from `doc` at `materialAssetPath` (before caller-injected keywords).
    // Every shader-only prewarm derives through this, so a warmed entry is the
    // entry the draw path looks up.
    struct MaterialShaderIdentity
    {
        Rendering::ShaderVariantKey Key;
        MaterialCompileSpec Spec;
        MaterialAlphaMode AlphaMode = MaterialAlphaMode::Opaque;
    };
    MaterialShaderIdentity DeriveShaderIdentity(const MaterialDocument& doc,
                                                const std::filesystem::path& materialAssetPath);

    // The document's surface (the engine default when it authors none) resolved
    // through the shader-root chain, from the material's own directory first
    // when its asset path is known.
    std::filesystem::path ResolveSurfacePath(const MaterialDocument& doc,
                                             const std::filesystem::path& materialAssetPath) const;

    void CompileMaterialPipeline(Material& mat, const MaterialDocument& doc);
    // Runs up to kShaderEditRecompileBudgetPerFrame carried recompiles, front to
    // back, and drops the entries it finishes. Called at the end of every drain,
    // including drains with no new edits — that is how the carry makes progress.
    // Returns the number of recompiles still carried, so the caller can log the
    // clip once per save burst instead of once per frame.
    size_t SpendShaderEditRecompileBudget();
    // One material's shader-edit recompile: inspector-lane clear, document
    // re-synthesis from the live compile spec, then the public async recompile.
    // No eviction: the submit's version bump retires the pass-variant PSO rows,
    // and PipelineVariantCache stale-serves them while workers rebuild. A
    // registry miss (material freed since the scan) is a no-op.
    void RecompileMaterialForShaderEdit(const GUID& guid);
    // Demotes an authored Mask that provably cannot discard to Opaque on the
    // registration-local document copy. Runtime-only by design: the authored
    // asset/document is never mutated, so editor saves re-emit the authored
    // Mask and a texture that later gains alpha re-evaluates on the next
    // (re)registration. Any doubt keeps Mask. The demotion is logged once per
    // session for each material GUID; a null `logAs` (a warm-up derivation of a
    // material that registration will log) logs nothing.
    void DemoteMaskWithoutAlphaSource(MaterialDocument& doc, const GUID& logAs);
    // Reports a material that discards every fragment it will ever be asked
    // about, so invisible-and-free content stops being indistinguishable from
    // content that works. Deliberately never mutates the material: an automatic
    // clamp or demotion would hide the authoring error it is meant to surface.
    void WarnIfMaterialCannotDraw(const GUID& guid, const MaterialDocument& doc);
    // Forces the next MaterialColorClassSpan() to rebuild. Needed because the
    // cache is keyed on m_MaterialSSBOGeneration, which tracks the index mapping
    // and therefore does NOT move when a re-registration edits an existing
    // material's PSO signature in place.
    void InvalidateMaterialColorClassMap()
    {
        m_MaterialColorClassGeneration = kInvalidColorClassGeneration;
    }
    // True when `path` provably cannot bring sampled alpha below `cutoff`:
    // either its container encodes no alpha channel at all, or a full decode
    // proves every texel clears the cutoff by the filtering safety margin.
    // Verdicts are cached per path and validated by (size, mtime) so an
    // overwritten texture re-probes; the cache is mutex-guarded because
    // prewarm paths may probe off the main thread. The cached facts are
    // cutoff-independent, so materials sharing a texture at different cutoffs
    // decode it once.
    bool ProbedFileCannotDiscard(const std::filesystem::path& path, float cutoff);
    // Resolve the surface's `// @texture <name>` declarations to name->slot
    // pairs (declaration order) via ShaderComposer::ResolveTextureSlots (the
    // same resolver the composer emits GE_TEXSLOT_* macros from) and hand them
    // to the Material. Empty for legacy surfaces (no @texture tags) — those
    // keep the fixed ladder. nullopt when resolution could not run (context
    // cold, surface unreadable/rejected): callers must then LEAVE the
    // material's existing map untouched — a failed recompile keeps the
    // previous pipeline running, and wiping the routing map under a live
    // pipeline misroutes its GE_TEXSLOT_* reads.
    std::optional<std::vector<std::pair<std::string, uint8_t>>> ResolveSurfaceTextureSlots(
        const MaterialDocument& doc, const std::filesystem::path& materialAssetPath);
    // Populates m_MaterialBuildContext (lazy, NOT thread-safe). Call only on the
    // serial path — driven each frame from BeginFrame so the context is warm
    // before the A2.4-P0-R parallel record window opens.
    bool EnsureMaterialBuildContextReady();
    // Names the shader roots the save->recompile lane can see through the asset
    // file watchers, and warns about the roots it cannot. Logged on the first
    // verified build context and again whenever the mount table moves, since an
    // edit in an unwatched root produces no ContentEvent and so can never
    // report itself: this manifest is the only way to diagnose that afterwards.
    void LogShaderEditWatchCoverage() const;
    // Read-only readiness query safe to call from record workers (no populate).
    // Returns true only once EnsureMaterialBuildContextReady has fully populated
    // and verified the context on the serial path.
    bool IsMaterialBuildContextReady() const
    {
        return !m_MaterialBuildContext.AdapterShaderDir.empty() && m_AdapterShaderDirVerified;
    }
    void PackMaterialSSBO();
    // Counts the frame, publishes the fallback binding, and logs (rate-limited)
    // when the material rows do not fit the ring slot.
    void HandleMaterialParamsOverflow(uint32_t materialCount, size_t requestedBytes);
    // Fill the app-lifetime fallback row with unassigned-slot defaults, once
    // the TextureService's default bindless indices exist. An overflow frame
    // binds this row for every draw in the scene, so it must never be sampled
    // as zeros.
    void EnsureFallbackRowSeeded();
    void EnsureMaterialColorClassMap();
    // Who waits for a prewarm task: something on screen, or nobody (the
    // project warm-up). Selects the progress counters it is accounted in.
    enum class PrewarmOrigin : uint8_t
    {
        OnDemand,
        ProjectWarmUp,
    };
    // priority routes the worker task's lane. Base-shader / variant-set prewarm use
    // Normal; the publish-gate variant warm-up and the device's pipeline builds
    // pass Background so a cold burst can't starve extraction/streaming on the
    // Normal lane, and so the compile gate admits them ahead of the Normal backlog.
    void SubmitTrackedPrewarm(std::function<void()> work,
                              JobSystem::JobPriority priority = JobSystem::JobPriority::Normal,
                              PrewarmOrigin origin = PrewarmOrigin::OnDemand);
    void DrainPrewarmJobs();
    // The device's pipeline build dispatcher (IDevice::SetPipelineBuildDispatcher).
    void DispatchPipelineBuild(std::function<void()> build);
    void PrewarmMaterialVariantSet(const Rendering::ShaderVariantKey& key,
                                  const MaterialCompileSpec& spec,
                                  MaterialAlphaMode alphaMode,
                                  bool ignoresVertexColor,
                                  const std::string& materialName,
                                  const std::filesystem::path& materialAssetPath,
                                  Rendering::MaterialKeyword colorPassKeywords,
                                  PrewarmOrigin origin);
    // Registration's variant key for an already-resolved document (Mask demoted,
    // shader references reconciled): the registry's base key plus the vertex
    // flags, the modifier form and the Parallax keyword, which need the asset
    // path and the shader roots. `surfaceSlots` is the surface's declared texture
    // set (ResolveSurfaceTextureSlots), read once by the caller and shared with the
    // material's slot map. `outParallaxRefusal` receives why a bound height map was
    // refused, or "" when none was.
    Rendering::ShaderVariantKey DeriveRegistrationVariantKey(
        const MaterialDocument& resolvedDoc, const std::filesystem::path& materialAssetPath,
        const std::optional<std::vector<std::pair<std::string, uint8_t>>>& surfaceSlots,
        std::string& outParallaxRefusal);
    // The Parallax keyword on `key` for an already-resolved document (ApplyParallaxKeyword), and
    // why a bound height map was refused, as the message that states the fix, or "" when none
    // was. Registration and ResolveSurfaceTextureUse both decide through it, so the inspector's
    // row and the variant registration builds cannot disagree.
    std::string ApplyParallaxKeywordDescribingRefusal(
        Rendering::ShaderVariantKey& key, const MaterialDocument& resolvedDoc,
        const std::filesystem::path& materialAssetPath,
        const std::optional<std::vector<std::pair<std::string, uint8_t>>>& surfaceSlots);
    // Warns with `refusal` when it differs from the one last reported for `mat`.
    static void ReportParallaxRefusal(Material& mat, std::string refusal);
    // Hands the project warm-up the open project, or an empty root when there is
    // none to warm: no engine assets, not the primary RenderServices, or the
    // startup staging workspace.
    void PumpProjectPrewarm();

    // Submits a first-registration material's base-shader compile to a worker.
    // Every compile input is captured by value on the calling (main) thread; the
    // worker compiles the SPIR-V, interns the base pipeline (both device-thread-
    // safe), and enqueues a publish record tagged with the compiled ShaderCacheKey.
    // Falls back to the inline synchronous compile when async can't run (device or
    // engine down). Main-thread only (asserted).
    void SubmitAsyncBaseCompile(const GUID& guid, Material& mat, const MaterialDocument& doc);
    // Re-drives an async base compile for a material whose in-flight compile was
    // superseded by a re-registration that changed pipeline-affecting state (F1).
    // Rebuilds the compile inputs from the material's now-current compile spec
    // (ApplyDocumentToMaterial keeps it in lockstep with the latest document), not
    // a stale captured document. Main-thread only.
    void RedriveAsyncBaseCompile(const GUID& guid, Material& mat);
    // Applies worker-produced base pipelines to their materials. Runs on the
    // serial frame-begin thread (before the parallel record window opens) so the
    // Material field writes never race a draw-time read. Main-thread only (asserted).
    void ApplyPendingPipelinePublishes();
    // True when an async submit can actually run: device present, the device
    // creates resources off its owning thread, and the engine / job system are
    // up. Off => compile inline instead, so headless, test and WebGPU paths keep
    // the deterministic synchronous behavior.
    bool CanSubmitAsyncBaseCompile() const;

    // Reach points used by the re-pointed PipelineVariantCache (friend) and the
    // facade's own CompileMaterialPipeline. GetDevice is the facade's injected
    // device; the two debug-override reads go through the RenderServices edge.
    Rendering::IDevice* GetDevice() const { return m_Device; }
    Rendering::CullModeFlags DebugCullMode() const;
    Rendering::FrontFace DebugFrontFace() const;

    // Injected dependencies (design §0.4). Pointers, not references, because the
    // facade is default-constructed then Initialize()d.
    Rendering::IDevice* m_Device = nullptr;
    PerFrameWritePool* m_FramePool = nullptr;
    TextureService* m_Textures = nullptr;
    RenderServices* m_RenderServices = nullptr;

    // --- services ---
    MaterialRegistry m_RuntimeMaterialRegistry;
    ShaderCompilationCache m_ShaderCompilationCache;
    ShaderCompileErrorLog m_ShaderErrors;
    std::unique_ptr<MaterialCompiler> m_MaterialCompiler;
    std::unique_ptr<PipelineVariantCache> m_PipelineVariants;
    std::unique_ptr<MaterialBinder> m_MaterialBinder;
    // Idle-time warm-up of the open project's material shaders; pumped by
    // BeginFrame for the engine's primary RenderServices only.
    std::unique_ptr<ProjectMaterialPrewarmService> m_ProjectPrewarm;
    std::function<Rendering::MaterialKeyword()> m_ResolveWorldPassKeywords;

    // --- async prewarm drain (worker-thread lifecycle) ---
    // Slice 5: the hand-rolled in-flight/mutex/cv barrier moved into
    // JobCounter. The gate mutex remains: SubmitTrackedPrewarm counts a job in
    // under it, so a shutdown (flag set under the same mutex) can never miss a
    // racing submit when it drains.
    JobSystem::JobCounter m_PrewarmCounter;
    bool m_PrewarmShutdown = false;      // guarded by m_PrewarmDrainMutex
    bool m_PrewarmEverSubmitted = false; // guarded by m_PrewarmDrainMutex
    std::mutex m_PrewarmDrainMutex;

    // Global admission gate for shader-compile tasks. Every SubmitTrackedPrewarm
    // funnels through it so the total number of concurrent shaderc compiles is
    // bounded, leaving worker lanes and cores free for extraction/streaming and
    // the main thread during cold-load bursts. Created in Initialize (needs the
    // engine job system); null on the default-constructed-but-never-initialized
    // path, where SubmitTrackedPrewarm falls back to a direct enqueue.
    //
    // Must OUTLIVE every DrainPrewarmJobs: a tracked task releases the prewarm
    // counter (drain's wait condition) and the gate's slot at separate points in
    // its teardown, so a completed drain does not prove the gate is quiescent.
    // The gate is a member destroyed with MaterialSystem, after ~MaterialSystem's
    // drain — never reset or replace it while prewarm work is in flight.
    std::unique_ptr<CompileConcurrencyGate> m_CompileGate;

    // Cumulative prewarm-task accounting behind the progress getters: submitted
    // bumped under the gate lock alongside the JobCounter Add, completed bumped
    // from the task's completion guard (and rebalanced if the publish throws).
    struct PrewarmCounters
    {
        std::atomic<uint64_t> Submitted{0};
        std::atomic<uint64_t> Completed{0};

        PrewarmProgress Read() const
        {
            // Read completed FIRST, then submitted: submitted is bumped before a
            // task can run and completed only from a task's completion path, so a
            // completed-then-submitted snapshot can never observe completed >
            // submitted (which would underflow the pending subtraction).
            const uint64_t completed = Completed.load(std::memory_order_acquire);
            const uint64_t submitted = Submitted.load(std::memory_order_acquire);
            return {submitted, completed};
        }
    };
    PrewarmCounters m_OnDemandPrewarm;
    PrewarmCounters m_ProjectWarmUp;

    // Materials whose Mask demotion has been logged this session (main thread,
    // like registration).
    std::unordered_set<GUID> m_LoggedMaskDemotions;

    // --- async base-shader compile publish (worker -> serial frame-begin) ---
    // A worker-produced base pipeline for a material whose first-registration
    // compile ran off-thread. Keyed by GUID (not Material*) so a material freed
    // before the drain is a safe registry miss, never a freed-pointer deref.
    // CompiledKey is the ShaderCacheKey the pipeline was built against; at publish
    // it is compared to the material's live key so a re-registration during the
    // in-flight window can't publish a stale pipeline (F1). Applied on the serial
    // BeginFrame thread by ApplyPendingPipelinePublishes.
    struct PendingPipelinePublish
    {
        GUID Guid;
        Rendering::GraphicsPipelineId PipelineId;
        std::shared_ptr<Rendering::ShaderMeta> Meta;
        ShaderCacheKey CompiledKey;
    };
    std::vector<PendingPipelinePublish> m_PendingPipelinePublishes; // guarded by m_PendingPublishMutex
    std::mutex m_PendingPublishMutex;
    // Per-GUID state of the base compile currently in flight: the ShaderCacheKey
    // it was submitted against (coalesces duplicate async submits — one compile
    // per GUID at a time before the first publish), and whether an async
    // recompile superseded it mid-flight (same key, edited source bytes — F1
    // can't tell them apart), in which case the publish drain discards the
    // result and re-drives instead of applying it. Touched only on the main
    // thread (submit adds, the publish drain removes), so it needs no lock.
    struct BaseCompileState
    {
        ShaderCacheKey Key;
        bool RedriveAfterPublish = false;
        // The pipeline id a recompile retired at submit. A FAILED compile
        // publishes an invalid id, and the drain restores this one instead of
        // leaving the material skipped (fail-visible: the mesh keeps rendering
        // the last good shader until the edit compiles). Invalid for a first
        // registration — there is nothing to restore.
        Rendering::GraphicsPipelineId LastGoodPipelineId{};
    };
    std::unordered_map<GUID, BaseCompileState> m_BaseCompileInFlight;
    // Thread that constructed this system (Initialize). The async submit + publish
    // paths mutate main-thread-only state without locks; a debug assert against
    // this id turns a future off-main caller into a loud failure instead of silent
    // unordered_map corruption.
    std::thread::id m_OwnerThreadId;

    // --- build context (test-injection seam) ---
    Rendering::MaterialBuildContext m_MaterialBuildContext;
    bool m_AdapterShaderDirVerified = false;
    // AssetManager source-set version captured when PackageShaderDirs was
    // derived; re-derived on the serial path when the mount table moves.
    uint64_t m_MaterialBuildContextSourceVersion = 0;

    // --- MaterialParams SSBO packing state (the R0.1 skip gate) ---
    uint32_t m_NextMaterialSSBOIndex = 0;
    std::vector<uint32_t> m_FreeMaterialSSBOIndices;
    std::vector<const Material*> m_MaterialsBySSBOIndex;
    std::vector<uint8_t> m_MaterialDepthClass;
    std::vector<uint8_t> m_MaterialDeformationMotion;
    std::vector<uint32_t> m_MaterialColorClass;
    // Sentinel meaning "no valid build": m_MaterialSSBOGeneration counts up from
    // 0, so it can never compare equal to this.
    static constexpr uint64_t kInvalidColorClassGeneration = ~0ull;
    uint64_t m_MaterialColorClassGeneration = kInvalidColorClassGeneration;
    Rendering::BufferHandle m_MaterialParamsSSBOFallback{};
    uint64_t m_MaterialSSBOGeneration = 0;
    struct PackedMaterialSlot
    {
        Rendering::BufferHandle Buffer{};
        size_t Offset = 0;
        uint64_t Stamp = 0;
    };
    // Indexed by PerFrameWritePool::GetCurrentSlot(MaterialParams), a RING slot —
    // deeper than the device's pacing, so this is sized by the deepest ring any
    // caller can ask for rather than by frames in flight.
    PackedMaterialSlot m_PackedMaterialSlots[Rendering::FrameBufferAllocator::kMaxRingSlots] = {};
    uint64_t m_MaterialSSBOPackCount = 0;
    uint64_t m_MaterialParamsOverflowCount = 0;
    bool m_FallbackRowSeeded = false;
    Rendering::BufferHandle m_MaterialParamsSSBOBuffer{};
    size_t m_MaterialParamsSSBOOffset = 0;
    size_t m_MaterialParamsSSBOSize = 0;

    // --- hot-reload ---
    AssetReloadInvalidator m_MaterialReloadInvalidator;
    std::mutex m_PendingShaderEditsMutex;
    std::vector<GUID> m_PendingShaderEditGuids;
    std::vector<std::filesystem::path> m_PendingShaderEditPaths;
    // One edited shader file whose material recompiles did not all fit in a
    // single drain's budget. Main-thread-only (the drain owns it end to end, so
    // no lock), and the vector is FIFO so edit arrival order is preserved.
    struct CarriedShaderEdit
    {
        std::string FileName;       // lowercased filename key
        std::vector<GUID> Affected; // materials to recompile, in registry scan order
        size_t NextIndex = 0;       // cursor: entries before this are done
    };
    std::vector<CarriedShaderEdit> m_CarriedShaderEdits;
    // Shader-source edits (ContentEvents: Modified/Created land on the watcher
    // thread) — handler is enqueue-only; DrainPendingShaderSourceEdits resolves
    // and recompiles on the serial frame-begin thread.
    //
    // Declared LAST in this block on purpose: members destruct in reverse
    // declaration order, so the subscriber detaches before the mutex and queues
    // its handler writes into. ShutdownPhaseA's explicit Reset() is still the
    // primary detach; this makes the destruction order self-sufficient rather
    // than dependent on that call having run.
    AssetReloadInvalidator m_ShaderSourceInvalidator;

    // --- Mask-demotion albedo probe cache (see ProbedFileCannotDiscard) ---
    // Holds the cutoff-INDEPENDENT probe facts so one decode serves every
    // material bound to the texture: the header verdict, plus the exact
    // minimum alpha when the decode tier ran (absent when it could not).
    struct AlphaProbeCacheEntry
    {
        std::filesystem::file_time_type MTime{};
        std::uintmax_t Size = 0;
        bool NoAlphaSource = false;
        std::optional<uint8_t> MinAlpha;
    };
    std::unordered_map<std::string, AlphaProbeCacheEntry> m_AlphaProbeCache;
    std::mutex m_AlphaProbeCacheMutex;

    // Materials already reported by WarnIfMaterialCannotDraw, so the warning is
    // once per entry into the failing state rather than once per registration.
    // Unguarded: written only from RegisterMaterialFromDocument, which is serial.
    std::unordered_set<GUID> m_CannotDrawWarnedMaterials;
};

} // namespace Engine::Renderer
} // namespace GameEngine
