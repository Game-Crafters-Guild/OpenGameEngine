#pragma once

#include "Assets/RenderPipelineAsset.h"
#include "Engine/Rendering/Pipeline/RenderPipelineNodes.h"
#include "Rendering/CameraTypes.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Rendering/Core/PassPhase.h"
#include "Rendering/Core/Device.h"

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
namespace PassPhase = ::GameEngine::Rendering::PassPhase;
namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
using ::GameEngine::Rendering::GraphicsPipelineId;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::ViewDesc;
using ::GameEngine::Rendering::ViewId;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline
{
enum class PipelineIssueSeverity : uint8_t
{
    Info = 0,
    Warning = 1,
    Error = 2
};

// What an issue is about, for a reader that acts on the kind rather than the wording.
enum class PipelineIssueCode : uint8_t
{
    General,
    // A pass names a type no registered node factory provides; a native module that registers it
    // may not have loaded yet.
    UnknownPassType,
    // A pass left out while the project's scripts build (an Info issue of a compile with
    // UnknownPassTypes::PendingScripts): its type is one the scripts may register. It joins once
    // the type registers.
    PendingScriptPass,
    // A registered pass left out with a waiting script pass because it reads a resource that pass
    // may write and no kept pass before it writes (an Info issue of a PendingScripts compile).
    PendingScriptReader,
    // The pipeline's output comes from a pass left out while the scripts build, so the rest of the
    // pipeline draws nothing (an Info issue of a PendingScripts compile).
    PendingScriptOutput,
    // A pass that feeds the depth prepass is listed after it, so the compile declares it above the prepass
    // (a Warning). PipelineCompileReport logs it once per graph.
    DepthPrepassFeederMoved,
};

// What a compile does with a pass whose type no registered factory provides.
enum class UnknownPassTypes : uint8_t
{
    // The pass is an error and the blueprint is refused.
    Refused,
    // The project's scripts are still building and may register the type: the pass is left out
    // and listed as a PendingScriptPass issue, and the rest of the graph compiles.
    PendingScripts,
};

struct PipelineIssue
{
    PipelineIssueSeverity severity = PipelineIssueSeverity::Error;
    std::string message;
    std::string nodeId; // optional
    PipelineIssueCode code = PipelineIssueCode::General;
};

// Parsed, validated pipeline description used at runtime.
// This is intentionally light: node-specific schema is handled by node implementations.
struct RenderPipelineBlueprint
{
    uint32_t schemaVersion = 0;
    std::string pipelineName;
    std::string sourcePath; // for diagnostics

    // Schema v2: passes-only. Internally these map 1:1 to "pipeline nodes" (types registered in RenderPipelineNodeRegistry).
    struct Pass
    {
        std::string id;
        std::string type;
        bool enabled = true;
        bool perView = false;
        // Compact JSON of the pass object for pass-specific parsing.
        std::string passJson;

        int32_t phase = Rendering::PassPhase::kDefault;
        std::vector<GameEngine::StringId> tags;
        std::vector<GameEngine::StringId> afterTags;
    };

    struct Output
    {
        std::string name;        // e.g. "FinalColor"
        std::string resourceRef; // e.g. "View.Resolve" or "SceneColor"
    };

    // Resource descriptions are kept as compact JSON and interpreted by the pipeline instance.
    // This allows schema evolution without rewriting the asset layer repeatedly.
    struct Resource
    {
        std::string name;
        std::string resourceJson;
    };

    std::vector<Pass> passes;
    std::vector<Resource> resources;
    std::vector<Output> outputs;

    // Top-level numeric constants seeded into the buffer-size expression
    // evaluator scope BEFORE per-resource `variables`, so a size expression can
    // reference e.g. clusterTileSize once instead of repeating the literal across
    // every cluster-grid buffer. Reserved names (renderWidth/renderHeight) are
    // rejected at compile; a resource-local variable of the same name shadows the
    // constant (compile Warning). Scoped to buffer-size expressions this slice —
    // dispatch and uniformWrites stay blueprint-ignorant until Compile bakes
    // resolved values into passJson (a later slice).
    std::unordered_map<std::string, double> constants;

    // Resource a pass declared as its "colorResolveTarget" (the post-MSAA scene
    // color the FX chain samples, e.g. "SceneColor"). When MSAA is off there is
    // nothing to resolve, so the scene must render straight into this target.
    std::string worldColorResolveTargetRef;

    // Hash of the blueprint inputs (used for change detection).
    uint64_t contentHash = 0;

    std::vector<PipelineIssue> issues;
    bool HasErrors() const
    {
        for (const auto& i : issues)
        {
            if (i.severity == PipelineIssueSeverity::Error)
                return true;
        }
        return false;
    }
};

class RenderPipelineCompiler
{
  public:
    // A pass may merge a shaderSourceOverrides.spirv/wgsl object into its
    // declaration; a null override omits that pass. Resolve before validation
    // so bindings and passes follow the cooked shader representation.
    // `unknownPassTypes` decides whether a pass of an unregistered type refuses the blueprint or
    // waits for the project's scripts; a blueprint with waiting passes hashes differently from the
    // same source compiled with them, so the full graph replaces it once its types register.
    RenderPipelineBlueprint Compile(const GameEngine::RenderPipelineAsset& asset,
                                    const RenderPipelineNodeRegistry& registry,
                                    Rendering::ShaderSourceKind sourceKind = Rendering::ShaderSourceKind::SpirV,
                                    UnknownPassTypes unknownPassTypes = UnknownPassTypes::Refused) const;
};

// Runtime instance bound to a RenderServices. Owns node instances; drives
// them through the RenderGraph declaration path (Declare) each frame.
class RenderPipelineInstance
{
  public:
    RenderPipelineInstance(Engine::Renderer::RenderServices& rs,
                           const RenderPipelineNodeRegistry& registry);
    ~RenderPipelineInstance();

    RenderPipelineInstance(const RenderPipelineInstance&) = delete;
    RenderPipelineInstance& operator=(const RenderPipelineInstance&) = delete;

    // Replace the active blueprint. Node instances are rebuilt on the next
    // Declare() call (m_BlueprintDirty drives EnsureNodeInstances).
    void SetBlueprint(RenderPipelineBlueprint blueprint);
    const RenderPipelineBlueprint& GetBlueprint() const { return m_Blueprint; }

    // Force a full node re-instantiation on the next Declare. Node instances
    // lazily cache device-derived handles (e.g. AONode's GTAO sampler, compute
    // nodes' interned pipeline state, node-owned buffers/textures) outside the
    // per-frame render graph; a device rebuild leaves those dangling. Rebuilding
    // the nodes drops the stale handles so they rebind against the new device.
    void OnDeviceRebuilt() { m_BlueprintDirty = true; }

    // ── RenderGraph declaration path. Declare walks the blueprint passes in array
    // order (declaration order IS the ordering — no tags, no RunAfterTag) and
    // rebuilds the per-frame blackboard from scratch; per-view nodes run for
    // each entry in `targets` whose view has a nonzero pipeline mask. ──
    void Declare(Rendering::RenderGraph::RGFrame& frame, std::span<const ViewTargetsRG> targets,
                 std::span<const Rendering::ViewDesc> views, float deltaTimeSeconds = 0.0f);
    // Resolve a pipeline output (e.g. "FinalColor") against the CURRENT
    // frame's blackboard. Invalid when the producing node didn't declare.
    Rendering::RenderGraph::RGTexture GetOutputRG(Rendering::ViewId viewId,
                                          const std::string& outputName) const;
    // The frame blackboard, identity-guarded: null unless `frame` is the one
    // Declare last ran against (a dead frame's ids must never be consumed).
    const PipelineFrameResources* FrameResourcesFor(const Rendering::RenderGraph::RGFrame* frame) const;
    // Frame-boundary ABA hardening: a recycled RGFrame at the same address
    // must not satisfy the identity guard before this epoch's Declare runs.
    // Called by RenderServices::BeginWorldDrawFrame.
    void ResetFrameBlackboard() { m_FrameResources.Frame = nullptr; }

    // Late declares: queued by nodes during the node loop, drained AFTER it
    // (before the spine marks outputs). For terminal overlays that must
    // attach the chain's FINAL output — declared in-node their write would
    // order BEFORE the world/post-FX writes, since declaration order IS the
    // ordering and phase is only a tiebreak.
    using DeferredDeclareFn =
        std::function<void(Rendering::RenderGraph::RGFrame&, RenderPipelineInstance&)>;
    void DeferDeclare(DeferredDeclareFn fn) { m_DeferredDeclares.push_back(std::move(fn)); }

    Engine::Renderer::RenderServices& GetRenderServices() const { return m_Rs; }

    // Collect interned graphics-pipeline ids from all nodes that have a base
    // pipeline. Used for PSO pre-warming.
    std::vector<Rendering::GraphicsPipelineId> CollectBaseGraphicsPipelineIds(Rendering::IDevice& device) const;

  private:
    friend struct ViewDeclare;

    // RenderGraph path: rg-free node (re)instantiation on blueprint dirty (the
    // EnsureBuilt branch is old-graph-coupled via node->Disable(rg); a
    // Declare-driven instance owns no old-graph passes to disable).
    void EnsureNodeInstances();
    // Blackboard lookups (per-view first, then frame scope).
    Rendering::RenderGraph::RGTexture TableTexture(Rendering::ViewId viewId, const std::string& name) const;
    PipelineBufferBindingRG TableBuffer(Rendering::ViewId viewId, const std::string& name) const;
    // Lazy base-name materialization for the Declare path (the EnsureBuilt
    // path prebuilds names on blueprint apply; Declare grows them on demand).
    const std::string& DeclarePassName(uint32_t nodeIdx, Rendering::ViewId viewId,
                                       const char* suffix);
    // RenderGraph lazy materialization from cached blueprint descriptions: pool-backed
    // imports (physicals exist at declaration — the binding-table contract),
    // cached in the blackboard so repeats return the same id (never two
    // descs for one pool name in a frame). Size expressions evaluate against
    // the view's render extent — the SAME numbers dispatch sizing uses.
    // Upload-memory blueprint buffers are NOT materialized (they dissolve
    // into AllocUpload by their owning node, which publishes the binding).
    // outputW/outputH: the display extent for "extent.basis": "output"
    // resources (the post-crossing chain); 0 = no separate display extent
    // (basis falls back to the render extent).
    Rendering::RenderGraph::RGTexture MaterializeTexture(Rendering::RenderGraph::RGFrame& frame,
                                                 Rendering::ViewId viewId,
                                                 const std::string& name, uint32_t renderW,
                                                 uint32_t renderH, uint32_t outputW = 0,
                                                 uint32_t outputH = 0);
    PipelineBufferBindingRG MaterializeBuffer(Rendering::RenderGraph::RGFrame& frame,
                                              Rendering::ViewId viewId, const std::string& name,
                                              uint32_t renderW, uint32_t renderH);
    struct ResourceDescription;
    const ResourceDescription* FindResourceDescription(const std::string& name) const;

    // Dynamic resolution: feed the last resolved frame's GPU cost to the
    // controller and let it move the render scale. No-op outside Dynamic mode.
    void TickDynamicResolution(Rendering::RenderGraph::RGFrame& frame, float deltaTimeSeconds);

    Engine::Renderer::RenderServices& m_Rs;
    const RenderPipelineNodeRegistry& m_Registry;

    RenderPipelineBlueprint m_Blueprint{};
    // Descriptions belong to this blueprint generation, keyed by its resource names.
    // SetBlueprint rebuilds them, including cached refusals of invalid descriptions.
    std::unordered_map<std::string, std::unique_ptr<ResourceDescription>> m_ResourceDescriptions;
    bool m_BlueprintDirty = false;
    bool m_WarnedDrsArmedProfiler = false;
    // Applied render/output extent ratio from the last Declare, and whether any
    // view is TAA-eligible at all (i.e. the render-scale lever is connected).
    float m_DrsLastAppliedScale = 1.0f;
    bool m_DrsAnyViewEligible = false;

    // Node instances keyed by node id
    std::unordered_map<std::string, std::unique_ptr<IRenderPipelineNode>> m_Nodes;

    // Outputs cached as name -> resourceRef string
    std::unordered_map<std::string, std::string> m_Outputs;

    // RenderGraph path: the per-frame blackboard (reset at the top of every Declare).
    PipelineFrameResources m_FrameResources;

    // Late declares queued during the current Declare's node loop (cleared
    // with the blackboard at the top of every Declare).
    std::vector<DeferredDeclareFn> m_DeferredDeclares;

    // Framework-level pass name cache for BuildForView().
    // Keyed by (nodeIndex << 48 | suffixHash << 32 | viewId) to remain stable
    // regardless of the order or subset of views passed to EnsureNodes.
    // suffixHash is 0 for unsuffixed (single-pass) nodes.
    // Rebuilt when the blueprint changes; grown lazily for new views/suffixes.
    std::unordered_map<uint64_t, std::string> m_PassNameCache;

    // Helper: build a cache key for (nodeIdx, viewId, suffix).
    static uint64_t MakePassNameKey(uint32_t nodeIdx, uint32_t viewId, uint16_t suffixHash = 0);

    // Resolve or create a suffixed pass name in the cache.
    const std::string& ResolveSuffixedPassName(uint32_t nodeIdx, uint32_t viewId,
                                                const std::string& baseName, const char* suffix);
};

} // namespace GameEngine::Engine::Renderer::Pipeline
