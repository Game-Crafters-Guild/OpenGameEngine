// The frame spine, extracted from RenderServices (A1.4).
//
// FrameOrchestrator owns the per-window RenderGraph stream slots (m_RGStreams +
// eviction/ABA resets), the app-frame epochs + declare sequence, the
// pipeline-asset lifecycle (path/blueprint/compiler/node-registry + the
// EnsureActiveRenderPipelineBlueprint asset->compile->fallback chain), the
// background shader-package cache, and the spine itself (BuildFrameGraph,
// OnFrameSubmittedRG, RemovePipelineInstanceForFrame, BeginAppFrame). It is owned
// by value on RenderServices and reached via rs.Spine().
//
// Unlike ViewRegistry/MaterialSystem (state owners), the orchestrator is glue by
// nature: it sequences GPUScene flushes, skinning/culling/bucketer scheduling,
// pipeline declaration, and MarkOutput across subsystems it does not own, so it
// holds a RenderServices& and reaches them through the same accessors external
// code uses (Views(), Materials(), subsystem getters) plus the friend grant for
// the frame-local state that STAYS RS-resident (m_FrameRG / m_ViewFrameRG /
// m_CullingScheduledThisFrame and the feature map — design §0a-A1). The
// stage-G scheduler bodies (skinning/culling/bucketer) also stay on RS; the
// orchestrator is the spine that CALLS them.

#pragma once

#include "AssetCore/GUID.h"                                // GameEngine::GUID member
#include "AssetCore/AssetReloadInvalidator.h"              // AssetReloadInvalidator member + AssetEventDispatcher + SharedPtr
#include "Rendering/Core/Device.h"                         // IDevice::GpuSyncToken
#include "Rendering/Core/RenderGraph/RGFrame.h"            // RGFrame, RGFrameStamp, RGBuffer
#include "Rendering/ShaderCache/ShaderPackageIO.h"         // ShaderPackage (cache value)
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h" // Pipeline::ViewTargetsRG
#include "Types/Types.h"                                   // String

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine
{
// The active render-pipeline asset backing the compiled blueprint. Cached by
// SharedPtr so the spine's steady-state refresh check is a single member read
// on it (design §0a-A10) rather than a per-frame AssetManager lookup.
class RenderPipelineAsset;

namespace Engine::Renderer
{
class RenderServices;

namespace Pipeline
{
class IRenderPipelineNode;
class RenderPipelineInstance;
class RenderPipelineCompiler;
class RenderPipelineNodeRegistry;
struct RenderPipelineBlueprint;
struct PipelineIssue;
} // namespace Pipeline

// ── RenderGraph arms.
// Frame-local RGBuffer VALUES thread through RenderServices::m_FrameRG (which
// STAYS on RenderServices per A1) and each stream slot's Values here — THE
// replacement for the GPUCullingDone tag + sentinel name-dedup: downstream
// passes declare Reads on these exact values, forming real RAW edges.
// Fixed declaration order per frame: skinning → culling → bucketer →
// (pipeline/world passes, slices 3-4) → union → aggregate. The bucketer
// MUST precede every pass that Reads DrawStreamOrdering: recording order
// gives the edge its direction, and a Read recorded before the Write
// derives WAR — the draws would consume frame N−1's slot buffers.
// PER VIEW, the producer arms (prepass, shadow cascades, area shadow)
// MUST precede the world pass for the same reason — the world's reads
// of the shadow array / area map / loaded depth chain from THIS frame's
// writes only when those writes were recorded first (ViewFrameRG carries
// a loud tripwire). ──
struct GpuDrivenFrameRG
{
    // Frame-identity: the ids below are frame-local; a re-begun incarnation
    // (same-epoch double renders: drag previews, tear-off repaints) must
    // never validate the previous one's ids, so validity is the
    // (pointer, FrameIndex) PAIR carried by the shared RGFrameStamp.
    Rendering::RenderGraph::RGFrameStamp For;
    Rendering::RenderGraph::RGBuffer SceneInstances{};
    // Coalesced 32 B/instance scatter-hot mirror (Scatter.World fetch lever).
    // Invalid when GE_SCATTER_COMPACT=0; the scatter then binds SceneInstances.
    Rendering::RenderGraph::RGBuffer SceneScatterHot{};
    Rendering::RenderGraph::RGBuffer SceneMeshes{};
    Rendering::RenderGraph::RGBuffer Visibility{};        // culling output
    Rendering::RenderGraph::RGBuffer DrawStreamOrdering{}; // consumers Read(_, Indirect)
    // THE atlas id world/depth/shadow/selection-mask passes MUST Read
    // (slice 3) — the first-ever real compute→VS barrier; the VS binds it
    // descriptor-direct, so only this declared read orders skinning.
    Rendering::RenderGraph::RGBuffer SkinPaletteAtlas{};
    Rendering::RenderGraph::RGBuffer RuntimeVisible{};
};

// Why FrameOrchestrator::ResolveActiveRenderPipelineNow could not make the
// requested pipeline the active one.
enum class PipelineResolveFailure : uint8_t
{
    None,       // the requested pipeline is active
    NotInMount, // no asset mount holds the file
    LoadFailed, // the file is in a mount but did not load or parse
    Rejected,   // it loaded, and its compile has errors (an unknown node type among them)
};

struct PipelineResolveResult
{
    PipelineResolveFailure Failure = PipelineResolveFailure::None;
    // Names the pipeline and the reason (for Rejected, one sentence per line
    // per compile error, naming its node), without a closing period: the caller
    // ends the sentence. Empty when Failure is None.
    std::string Message;
};

// Retained diagnostics from the last render-pipeline blueprint compile/load
// attempt, surfaced by the editor's RenderGraphPanel (S2.5). Recorded inside
// EnsureActiveRenderPipelineBlueprint after the requested compile (except a
// rejected one in Startup mode, whose caller reports it instead), after a
// refused stand-in compile, and at the requested asset's load/parse-failure
// branches. Recording is idempotent: Generation only advances on a genuinely
// new outcome, so the panel can key a refresh on it without rebuilding every
// frame while a transient fallback recompiles the same blueprint each frame.
//
// Threading: EnsureActiveRenderPipelineBlueprint runs only from BuildFrameGraph
// and ResolveActiveRenderPipelineNow, both strictly main-thread (see file
// header); the panel's Update is also main-thread. The read and the write never
// overlap, so this needs no locking.
struct PipelineCompileReport
{
    std::filesystem::path SourcePath;
    uint64_t SourceHash = 0;
    // Compile produced Error-severity issues => the active pipeline was NOT
    // swapped (last-good kept). Warnings alone never set this.
    bool Rejected = false;
    // Outcome came from the fallback ForwardPlus compile (the requested,
    // non-default pipeline was unavailable), not from the requested compile.
    bool FallbackCompile = false;
    std::vector<Pipeline::PipelineIssue> Issues;
    // Non-empty when the REQUESTED pipeline failed at load/parse/resolve. Those
    // branches produce no PipelineIssues, so the reason string is the banner text.
    std::string LoadFailureReason;
    // Advances on each distinct recorded outcome — the panel's refresh key.
    uint64_t Generation = 0;
    // Pipeline compiles the spine has run, requested and stand-in. It climbs only when a source, a
    // node registration or the request changes; one that climbs every frame is a pipeline compiled
    // every frame. Read by the debug port (get_render_graph_overview, "pipelineCompiles").
    uint64_t Compiles = 0;

    // Record a compile outcome (accepted-with-warnings or rejected). Clears any
    // prior load-failure state — a compile means the source loaded and parsed.
    // An accepted compile's DepthPrepassFeederMoved warnings are logged once per graph (see
    // m_LoggedFeederMoves); its other warnings are the panel's only.
    // A rejection that `awaitsModules` (the request waits for native modules that may register its
    // missing pass types) is recorded but not logged: it is not a refusal yet, and the refusal that
    // may follow the wait logs once.
    void RecordCompile(const Pipeline::RenderPipelineBlueprint& blueprint,
                       std::filesystem::path sourcePath, uint64_t sourceHash, bool fallback, bool awaitsModules);
    // Record a load/parse/resolve failure of the requested pipeline. `reason`
    // becomes the banner text; Issues are cleared (the failure precedes compile).
    void RecordLoadFailure(std::filesystem::path sourcePath, std::string reason);

  private:
    // A rejection as logged: the graph, its source hash and its issues.
    struct LoggedRejection
    {
        std::filesystem::path SourcePath;
        uint64_t SourceHash = 0;
        std::vector<Pipeline::PipelineIssue> Issues;
    };

    // The rejection last logged for the requested graph and for the fallback,
    // kept apart: while a fallback stands in, the requested graph is retried
    // every frame with the fallback recorded in between, and each role still
    // logs its rejection once. A role's entry is cleared when that role compiles
    // and, for the request, when it fails to load; a request that compiles also
    // clears the fallback's entry, since it ends the episode the fallback stood
    // in for. A later rejection then logs again. Empty SourcePath: nothing
    // logged since.
    LoggedRejection m_LoggedRequestedRejection;
    LoggedRejection m_LoggedFallbackRejection;
    // Whether the outcome last recorded awaited modules (and so was not logged).
    bool m_RecordedAwaitingModules = false;
    // Per graph (its source path), the DepthPrepassFeederMoved warnings its last accepted compile logged or
    // had already logged. A move is logged when it is not in its graph's entry, so the same move is not logged
    // again while that graph's outcome changes around it (issues that come and go while scripts load, a
    // fallback recorded in between, a save that keeps the order). An accepted compile of the graph replaces
    // its entry, so a move that went away and comes back is logged again.
    std::unordered_map<std::string, std::vector<std::string>> m_LoggedFeederMoves;
};

// The requested render pipeline is not the one drawing. Either it was refused or could not load
// (the compile rejected it, for an unknown pass type or a pass order it refuses, or its asset did
// not load or parse) and the views draw with a stand-in; or it names pass types no loaded scripts
// register while the project's scripts are still building or loading, and it waits for them: the
// request draws without those passes (PendingPasses), or the stand-in draws when the rest of it is
// refused too. In the editor the stand-in is the engine's own ForwardPlus
// (FrameOrchestrator::kEngineStandInPath in the 'editor' mount); in a packaged game there is none.
// When no stand-in draws, nothing draws and StandInFailure says why. Inactive while the requested
// pipeline draws, and while a stand-in only covers a request whose asset is not available yet (no
// failure). Main-thread-only, like PipelineCompileReport.
struct PipelineStandIn
{
    // The pipeline the project asked for; empty while it is the one drawing.
    std::filesystem::path RequestedPath;
    // The pipeline drawing in its place (its pipelineName); empty while nothing draws.
    std::string StandInName;
    // Why the request could not be used: the compiler's first error, or the load failure.
    std::string Reason;
    // Why the engine's pipeline does not stand in, as a sentence; empty while it draws.
    std::string StandInFailure;
    // While the request waits for the project's scripts: the ids of its passes whose types they
    // register, left out until then. With no stand-in named, the request draws without them.
    std::vector<std::string> PendingPasses;
    // While the request waits: the ids of its engine passes left out with the script passes because
    // they read their output.
    std::vector<std::string> WaitingReaders;
    // While the request waits: its last compiled version draws (it compiled fully before; this
    // compile left passes out and its output among them).
    bool LastVersionDraws = false;
    // The request waits for the project's scripts before it compiles again; meanwhile it draws
    // without its pending passes, or the stand-in draws, or its last compiled version.
    bool WaitingForModules = false;
    // Advances whenever any of the above changes.
    uint64_t Generation = 0;

    bool IsActive() const { return !RequestedPath.empty(); }
};

// See file header. Owned by value on RenderServices, reached via rs.Spine().
class FrameOrchestrator
{
  public:
    // Out-of-line (both ctor and dtor) so the members needing complete
    // forward-declared Pipeline types — the unique_ptr pipeline instances /
    // blueprint / compiler / node-registry and PipelineCompileReport's
    // std::vector<PipelineIssue> — are constructed and destroyed in the .cpp
    // where those types are complete (the RS.h PIMPL pattern, §0a-A4).
    FrameOrchestrator();
    ~FrameOrchestrator();

    FrameOrchestrator(const FrameOrchestrator&) = delete;
    FrameOrchestrator& operator=(const FrameOrchestrator&) = delete;

    // ── RenderGraph spine.
    // Declares one frame in the contract order: skinning → culling → bucketer
    // → pipeline (per-view passes) → visibility union/aggregate → MarkOutput
    // of each active view's FinalColor. Idempotent per frame — see the body.
    struct FrameGraphBuildParamsRG
    {
        std::span<const Pipeline::ViewTargetsRG> ViewTargets{};
        // Player (slice 6) passes false: its systems build draw lists in
        // Update. Same guard semantics as the old buildWorldDrawListsIfReady.
        bool BuildWorldDrawListsIfReady = true;
        float DeltaTime = 0.0f;
    };

    // Consolidates today's RenderServices::Initialize pipeline block: node
    // registry + compiler + node-type registrations, default-path seed, the
    // GE_ACTIVE_RENDER_PIPELINE env override, and the hash reset (§0a-A7).
    void Initialize(RenderServices& rs);
    // The pipeline-asset teardown (stream slots + blueprint/compiler/registry).
    // Runs at the RenderServices::Shutdown pipeline-block position. m_FrameRG is
    // RS-resident (A1) and reset by RenderServices::Shutdown alongside this call.
    void Shutdown();
    // Installs the active-.rendergraph hot-reload subscriber (the pipeline arm of
    // RenderServices::InstallAssetReloadInvalidators). Redundant fast path beside
    // the spine's cached-asset source-hash poll. Main-thread reloaded-only (S3).
    void InstallReloadInvalidator(AssetEventDispatcher& dispatcher);
    // Detaches the reload subscriber. Called from the RenderServices::Shutdown
    // invalidator-reset window, BEFORE this orchestrator's member teardown, so a
    // late AssetReloaded can't touch members mid-destruction (§0a-A7 Part 2).
    void ResetReloadInvalidator();
    // App-frame tick (spine half): epoch bump + blackboard ABA resets, plus the
    // three RS-resident frame-local clears reached back through the friend edge.
    void BeginAppFrame();
    // The app-frame epoch minted by BeginAppFrame — the only counter that is
    // global across window frame streams (per-window RGFrame counters advance
    // independently). Dynamic resolution keys its once-per-frame tick on it.
    uint64_t WorldFrameEpoch() const { return m_WorldFrameEpoch; }

    // Borrows the frame until RemovePipelineInstanceForFrame. The caller must
    // remove the stream while frame/device are alive, before destroying either.
    // Includes short-lived capture/test frames as well as window-owned streams.
    void BuildFrameGraph(Rendering::RenderGraph::RGFrame& frame, const FrameGraphBuildParamsRG& params);
    void OnFrameSubmittedRG(Rendering::RenderGraph::RGFrame& frame,
                            const Rendering::IDevice::GpuSyncToken& token);
    // Caller-thread only, outside frame recording/Execute. Retires callbacks and
    // their GPU work before destroying pipeline nodes. Repeated removal is safe.
    void RemovePipelineInstanceForFrame(Rendering::RenderGraph::RGFrame* frame);

    void SetActiveRenderPipelinePath(const std::filesystem::path& relOrAbsPath);
    const std::filesystem::path& GetActiveRenderPipelinePath() const { return m_ActiveRenderPipelinePath; }
    // Requests `relOrAbsPath` and resolves it now (load, compile, apply) instead
    // of at the next BuildFrameGraph, for a host that starts with the pipeline it
    // asked for or not at all. Call it after every module that declares pipeline
    // node types has registered them. Returns Failure None when the requested
    // pipeline is the active one; otherwise which failure, with a message that
    // names the pipeline and the reason. It never applies or announces a
    // stand-in, logs nothing about the failure, which the caller reports once,
    // and publishes no stand-in report for it. It does not wait for native
    // modules: a pass type that a pending module would register is Rejected.
    // On failure the active pipeline is unchanged and the next BuildFrameGraph
    // runs the ordinary refresh. Main thread only.
    PipelineResolveResult ResolveActiveRenderPipelineNow(const std::filesystem::path& relOrAbsPath);
    void SetActiveRenderPipelineBlueprint(Pipeline::RenderPipelineBlueprint bp);
    // Adds a node type the pipeline compiler can resolve, and returns whether it
    // was added. A successful registration also schedules one retry of an
    // authored pipeline whose last compile was rejected: a module that loads
    // after its graph was compiled recovers it with no asset edit, because the
    // graph's unchanged source hash cannot express "a type it needs now exists".
    // Main thread only — it writes the blueprint refresh state the frame build
    // reads.
    bool RegisterPipelineNodeType(const std::string& type,
                                  std::function<std::unique_ptr<Pipeline::IRenderPipelineNode>()> factory,
                                  bool perView);
    // The live node-type registry, for engine bring-up: EngineCore hands it to
    // each built-in module's RegisterXPipelineNodes so the module owns its own
    // node-type declarations. Null outside Initialize()..Shutdown().
    //
    // Registration made AFTER bring-up goes through RegisterPipelineNodeType
    // instead: only that path schedules the retry of an authored pipeline whose
    // last compile was rejected for the type being added.
    Pipeline::RenderPipelineNodeRegistry* GetPipelineNodeRegistry();
    const Pipeline::RenderPipelineNodeRegistry* GetPipelineNodeRegistry() const;

    // C12 module-reload reconcile for pipeline-node factories. Called (main
    // thread, via the loader's reconcile handler) after a module's replay
    // re-registered its node types and BEFORE the superseded image may be
    // unmapped: retires types the replay stopped declaring, and tears down
    // every live pipeline instance when the module owns node types — the
    // instances hold node objects (vtables, callbacks) built by the OLD
    // image's factories, and they must be destroyed while that image is still
    // mapped. Instances rebuild from the reconciled registry on the next
    // BuildFrameGraph. Must not run between a stream's declare and submit;
    // module loads are Tick-phase, outside the frame spine.
    void ReconcileModuleNodeRegistrations(std::string_view moduleId, uint64_t currentGeneration);
    // C12 load-abort purge / unload quiesce ledger forwarders (see
    // RenderPipelineNodeRegistry). Safe before Initialize (no registry: 0).
    std::size_t PurgeModulePipelineNodes(std::string_view moduleId, uint64_t generation);
    std::size_t CountSupersededModulePipelineNodes(std::string_view moduleId,
                                                   uint64_t currentGeneration) const;
    // The active blueprint's generation (bumped on each swap).
    uint64_t ActiveBlueprintGen() const { return m_ActiveBlueprintGen; }
    // The currently-applied blueprint (null before the first apply). Read by
    // RenderServices::ResolveWorldPassKeywordsForPrewarm.
    const Pipeline::RenderPipelineBlueprint* ActiveBlueprint() const { return m_ActiveBlueprint.get(); }

    // Diagnostics from the last blueprint compile/load attempt (S2.5). The
    // editor's RenderGraphPanel keys its Validation section on Generation.
    // Main-thread-only, see PipelineCompileReport.
    const PipelineCompileReport& LastCompileReport() const { return m_LastCompileReport; }
    // Whether a stand-in draws in place of the requested pipeline, and why.
    const PipelineStandIn& ActivePipelineStandIn() const { return m_PipelineStandIn; }

    // An unknown-pass-type refusal after a script failed to build: the compiler's generic fix
    // (build the scripts, install the package or remove the node) gives way to the build that
    // failed and the Script Errors tab that lists its errors.
    static std::string PointAtFailedBuild(const std::string& reason);
    // An unknown-pass-type refusal after the project's scripts built (and the request waited for
    // them) without registering the type: building them again is not the fix.
    static std::string ScriptsBuiltWithoutType(const std::string& reason);

    // The engine's default render pipeline: what a project draws with when it names none, and, in
    // the editor, the stand-in for a requested pipeline that cannot be used, read from the 'editor'
    // mount so a project file of the same name never stands in. A packaged game has no 'editor'
    // mount and no stand-in: a pipeline it cannot use draws nothing.
    static constexpr std::string_view kEngineStandInPath = "RenderPipelines/ForwardPlus.rendergraph";

    // Warm the package cache off the frame thread. `kind` is the form the
    // consuming device ingests — the cached package is handed to nodes as-is,
    // so it must be read in the form they will feed to pipeline creation.
    void PreLoadShaderPackage(const std::string& name, Rendering::ShaderSourceKind kind);
    bool TryConsumePreLoadedShaderPackage(const std::string& name, Rendering::ShaderPackage& out);

    // The declare sequence of the spine currently being declared.
    uint64_t RGDeclareSeq() const { return m_RGDeclareSeq; }

    // The stream's pipeline instance, keyed by pointer only — incarnation
    // validity is the blackboard's job (FrameResourcesFor compares FrameIndex).
    // Public so the RS thin readers (GetPipelineOutputRG, the world-pass buffer
    // binding) reach it.
    Pipeline::RenderPipelineInstance* PipelineInstanceForFrame(
        const Rendering::RenderGraph::RGFrame& frame) const;

    // Device-rebuild re-provision seam (Q6 slice 4): each live stream's pipeline
    // instance owns node objects that cached device handles now dangling on the
    // torn-down device. Force every instance to re-instantiate its nodes on the
    // next Declare. Called from RenderServices::OnDeviceRebuilt.
    void OnDeviceRebuilt();

  private:
    // Who handles a refresh that cannot apply the requested pipeline.
    enum class PipelineRefreshMode
    {
        // The frame spine: log the failure, apply the engine's stand-in where
        // there is one, publish the stand-in report and retry the request.
        Frame,
        // ResolveActiveRenderPipelineNow: no log, no stand-in and no stand-in
        // report for a failure; the caller reports the failure it returns, once.
        // An unknown pass type is refused, not left out to wait for scripts.
        Startup,
    };
    // Blueprint refresh shared by the spine: asset lookup → compile →
    // fallback chain → store + generation bump. rg-free.
    void EnsureActiveRenderPipelineBlueprint(PipelineRefreshMode mode);
    // Whether an asset mount holds the requested pipeline's file on disk.
    bool RequestedPipelineFileExists() const;
    enum class StandInCompile
    {
        // `out` holds the engine's pipeline, compiled.
        Compiled,
        // The engine's pipeline already stands in, from the same source; `out` is untouched.
        AlreadyApplied,
        // There is no stand-in here (a packaged game: no 'editor' mount); `failure` says so.
        NoStandIn,
        // It cannot stand in; `failure` says why.
        Failed,
    };
    // Compiles the engine's ForwardPlus to stand in for the requested pipeline into `out`. Fails,
    // with the reason as a sentence in `failure`, when it is the requested pipeline itself or it
    // cannot be resolved, loaded or compiled.
    StandInCompile CompileEngineStandIn(Pipeline::RenderPipelineBlueprint& out, std::string& failure);
    // Publishes m_PipelineStandIn from the refresh's outcome for `requestedPath`.
    void UpdatePipelineStandIn(const std::filesystem::path& requestedPath);

    // A2 STEP-0 render-timeline windowing. BuildFrameGraph stage G runs exactly
    // once per app frame (the owner-frame gate), so the orchestrator is the
    // natural publisher that turns RenderServices' last-frame sort/bucketer
    // brackets into the stable windowed means a single get_render_stats poll
    // reads. Mirrors RenderExtractionSystem's 64-frame stat window. `bucketerMs`
    // is measured at the ScheduleWorldBucketerDispatches call site; the sort
    // last-frame value is read from RenderServices (friend).
    static constexpr std::size_t kRenderTimelineWindow = 64;
    struct RenderTimelineSample
    {
        double SortMs = 0.0;
        double BucketerScheduleMs = 0.0;
    };
    std::array<RenderTimelineSample, kRenderTimelineWindow> m_TimelineWindow{};
    std::size_t m_TimelineWindowHead = 0;
    std::size_t m_TimelineWindowCount = 0;
    void PublishRenderTimelineStats(double bucketerScheduleMs);

    // RenderGraph spine state, slice 8a: one slot PER FRAME STREAM (= per window).
    // The spine splits into stage G (app-frame-global: draw lists, GPUScene
    // flush, skinning, culling, bucketer, union/aggregate — declared ONCE per
    // app frame into the first frame that calls BuildFrameGraph that epoch,
    // the "owner frame") and stage P (per-RGFrame: declare-seq mint, pipeline
    // declare on the slot's own instance/blackboard, GpuDrivenFrameRG values —
    // produced for the owner, RE-IMPORTED by physical handle for every other
    // stream — and the MarkOutput loop). RGFrame pointer = stream identity
    // (a stable per-window unique_ptr); the {DeclaredEpoch, DeclaredFrameIndex}
    // stamps are validity — pointers are reused across frames and recycled
    // addresses must never validate a stale incarnation.
    struct RGFrameStreamSlot
    {
        std::unique_ptr<Pipeline::RenderPipelineInstance> Instance;
        uint64_t AppliedBlueprintGen = 0;
        uint64_t DeclaredEpoch = 0;      // == m_WorldFrameEpoch when last declared
        uint64_t DeclaredFrameIndex = 0; // == frame.FrameIndex() when last declared
        // Declare seq recorded for OnFrameSubmittedRG's gate (7b semantics:
        // an evicted or never-declared stream's submit stamps nothing).
        uint64_t DeclareSeq = 0;
        GpuDrivenFrameRG Values; // this stream's frame-local ids
    };
    // Find-or-create by RGFrame*, bounded; FIFO-evicting a LIVE stream also
    // destroys its pipeline instance (its outputs die for that frame) —
    // acceptable at 16, which far exceeds any real window count.
    static constexpr size_t kMaxRGFrameStreams = 16;
    std::vector<std::pair<Rendering::RenderGraph::RGFrame*, RGFrameStreamSlot>> m_RGStreams;
    RGFrameStreamSlot* FindOrCreateRGStream(Rendering::RenderGraph::RGFrame& frame);

    // Glue by nature — documented, reviewed (§0a-A1). Set in Initialize.
    RenderServices* m_Services = nullptr;

    // App-frame epoch (bumped in BeginAppFrame) + the stage-G one-shot.
    // m_GlobalStagesFrame = the owner frame for this epoch. Starts at 1 so a
    // fresh stream slot (DeclaredEpoch = 0) can never read as already declared.
    // CONTRACT: BuildFrameGraph requires one BeginAppFrame per app frame —
    // without the tick, stage G runs once and never re-arms.
    uint64_t m_WorldFrameEpoch = 1;
    uint64_t m_GlobalStagesEpoch = 0;
    Rendering::RenderGraph::RGFrame* m_GlobalStagesFrame = nullptr;
    // Monotonic declare sequence, minted once per BuildFrameGraph spine call.
    uint64_t m_RGDeclareSeqCounter = 0;
    uint64_t m_RGDeclareSeq = 0;

    // Active data-driven render pipeline.
    std::filesystem::path m_ActiveRenderPipelinePath;
    // True when the currently-applied blueprint is a transient fallback; the
    // refresh re-tries the requested path each frame while set.
    bool m_ActiveRenderPipelineIsFallback = false;
    // Hash of the currently-applied compiled blueprint (bp.contentHash).
    uint64_t m_ActiveRenderPipelineHash = 0;
    // Hash of the pipeline asset source text; avoids per-frame recompile/log spam.
    uint64_t m_ActiveRenderPipelineSourceHash = 0;
    // Drives the spine's blueprint refresh: true forces an
    // EnsureActiveRenderPipelineBlueprint pass on the next BuildFrameGraph.
    // Initial true resolves the pipeline on the first frame. Set by a path
    // switch, by the reload invalidator, by the cached-asset source-hash poll,
    // and by a node-type registration that follows a rejected authored compile;
    // cleared once the refresh runs. The fallback-retry arm re-arms via
    // m_ActiveRenderPipelineIsFallback while a transient fallback is applied.
    bool m_BlueprintRefreshNeeded = true;
    // The asset backing the currently-applied blueprint (when it came from a
    // requested .rendergraph, not the stand-in). Captured at resolve inside
    // EnsureActiveRenderPipelineBlueprint; cleared on path switch / resolve
    // failure / shutdown. AssetManager already holds this via m_LoadedAssets, so
    // this cache does not extend lifetime in the resident case; reload is
    // in-place so GetSourceHash() reflects hot-reloads without a re-lookup.
    SharedPtr<RenderPipelineAsset> m_ActiveRenderPipelineAsset;
    // Path -> GUID resolution cache. Cleared on failed resolve.
    std::filesystem::path m_ActiveRenderPipelineLastResolvedPath;
    GameEngine::GUID m_ActiveRenderPipelineLastResolvedGuid;
    bool m_ActiveRenderPipelineLastCompileHadErrors = false;
    // The requested pipeline's compile errors, one sentence per line, from its
    // last rejected compile; empty after an accepted compile or a path switch.
    // The compile report cannot carry them (a refused stand-in compile
    // overwrites it), and m_RequestedFailureReason holds only the first.
    String m_ActiveRenderPipelineRejection;
    // Sticky last failure info so we can avoid spamming the same error every frame.
    std::filesystem::path m_ActiveRenderPipelineLastFailurePath;
    String m_ActiveRenderPipelineLastFailureReason;
    // Retained diagnostics of the last compile/load attempt for the editor panel.
    PipelineCompileReport m_LastCompileReport;
    PipelineStandIn m_PipelineStandIn;
    // The last requested compile left passes out for the project's scripts, still building: the
    // request draws without them (or, when it was refused, the stand-in draws), and it compiles once
    // more when a type registers or the modules are in.
    bool m_WaitingForNativeModules = false;
    // Why the requested pipeline was last refused or failed to load; empty once it compiles.
    std::string m_RequestedFailureReason;
    // Why the engine's pipeline could not stand in at the last attempt; empty once it does.
    std::string m_StandInFailureReason;
    // The asset and source hash of the stand-in last compiled, so an applied stand-in is neither
    // resolved nor compiled again on every refresh.
    SharedPtr<RenderPipelineAsset> m_StandInAsset;
    uint64_t m_StandInSourceHash = 0;
    // The applied blueprint was compiled from the requested pipeline (not a stand-in).
    bool m_ActiveBlueprintIsRequested = false;
    // The applied request draws without its waiting script passes (m_PendingScriptPasses).
    bool m_ActiveBlueprintIsPartial = false;
    // The script passes the last requested compile left out for the project's scripts, and the
    // passes left out with them because they read their output.
    std::vector<std::string> m_PendingScriptPasses;
    std::vector<std::string> m_WaitingReaders;
    // The current request waited for the project's scripts at some compile: a later refusal for a
    // type still unknown says the scripts built without registering it.
    bool m_RequestWaitedForScripts = false;
    std::unique_ptr<Pipeline::RenderPipelineNodeRegistry> m_PipelineNodeRegistry;
    std::unique_ptr<Pipeline::RenderPipelineCompiler> m_PipelineCompiler;
    // The active blueprint, applied lazily to each per-graph instance on its
    // next build (m_ActiveBlueprintGen is bumped whenever the blueprint changes).
    std::unique_ptr<Pipeline::RenderPipelineBlueprint> m_ActiveBlueprint;
    uint64_t m_ActiveBlueprintGen = 0;
    // True while a SetActiveRenderPipelineBlueprint install owns the slot
    // (the asset-driven refresh is suspended). Cleared by a path request.
    bool m_ActiveBlueprintPinned = false;

    // Background-preloaded shader packages (populated during Init / editor
    // background load, consumed by pipeline nodes at Declare).
    std::mutex m_ShaderPkgCacheMutex;
    std::unordered_map<std::string, Rendering::ShaderPackage> m_ShaderPkgCache;

    // Active-.rendergraph AssetReloaded subscriber. ReloadedOnly: only
    // AssetReloaded dispatches on the main thread, and blueprint compile is
    // main-thread only. Handler flips m_BlueprintRefreshNeeded; the recompile
    // defers to the next spine call. Reset before member teardown (§0a-A7).
    AssetReloadInvalidator m_PipelineReloadInvalidator;
};

} // namespace Engine::Renderer
} // namespace GameEngine
