#pragma once

#include "ECS/ModuleRegistration.h"
#include "Engine/Rendering/Pipeline/PipelineFrameResources.h"
#include "Rendering/Core/RenderGraph/RGFrame.h"

#include <functional>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace GameEngine::Rendering
{
struct ViewDesc;
using ViewId = uint32_t;
} // namespace GameEngine::Rendering

// Explicit imports from the Rendering module (CodingStyle: no namespace-scope using-directives in headers).
namespace GameEngine::Engine::Renderer
{
namespace RenderGraph = ::GameEngine::Rendering::RenderGraph;
using ::GameEngine::Rendering::GraphicsPipelineId;
using ::GameEngine::Rendering::IDevice;
using ::GameEngine::Rendering::ViewDesc;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer
{
class RenderServices;
} // namespace GameEngine::Engine::Renderer

namespace GameEngine::Engine::Renderer::Pipeline
{

class RenderPipelineInstance;

// ── RenderGraph declaration path ──

// Frame-scope context for IRenderPipelineNode::Declare.
struct PipelineDeclareContext
{
    Engine::Renderer::RenderServices& Services;
    Rendering::RenderGraph::RGFrame& Frame;
    std::span<const Rendering::ViewDesc> Views;
    float DeltaTimeSeconds = 0.0f;
};

// Per-view declaration scope. Targets arrive as frame-local RGTexture VALUES
// with the pre-pass redirects already applied (MSAA-off color collapse onto
// the pipeline's resolve target); resource refs resolve through the frame
// blackboard. Built fresh per (node, view) each frame — nothing here may be
// stored across frames.
struct ViewDeclare
{
    ViewDeclare(Rendering::RenderGraph::RGFrame& frame, const Rendering::ViewDesc& view,
                Engine::Renderer::RenderServices& services, const char* nodeId)
        : Frame(frame), View(view), Services(services), NodeId(nodeId)
    {
    }

    Rendering::RenderGraph::RGFrame& Frame;
    const Rendering::ViewDesc& View; // clear config, camera, letterbox, mask
    Engine::Renderer::RenderServices& Services;
    const char* NodeId = "";

    // Pre-resolved view-target VALUES (redirects applied by the pre-pass).
    Rendering::RenderGraph::RGTexture ViewColor{};
    Rendering::RenderGraph::RGTexture ViewDepth{};         // EFFECTIVE (prepass override published)
    Rendering::RenderGraph::RGTexture ViewResolve{};
    Rendering::RenderGraph::RGTexture ViewDepthResolved{}; // aliases ViewDepth until DepthResolve publishes
    // Names::View::OccluderDepthResolved, or ViewDepthResolved where nothing published it (no
    // DepthResolve yet, so no non-occluding heads are in the view depth either).
    Rendering::RenderGraph::RGTexture ViewOccluderDepthResolved{};
    bool ResolveFromPipeline = false;
    // THE extent for buffer-size expressions AND dispatch sizing — both must
    // derive from the same numbers (cluster-grid agreement invariant). Under
    // an active internal-resolution split this is the INTERNAL (reduced)
    // extent the world half rasters at.
    uint32_t RenderWidth = 0;
    uint32_t RenderHeight = 0;
    // Display extent of the caller's targets. Equals Render* except under an
    // active split; the crossing (TemporalAA or RenderScaleUpscale) upscales
    // from Render* to Output*.
    uint32_t OutputWidth = 0;
    uint32_t OutputHeight = 0;
    // Split-active only: the caller's display-resolution color/depth (invalid
    // at scale 1.0). See Names::View::OutputColor/OutputDepth.
    Rendering::RenderGraph::RGTexture ViewOutputColor{};
    Rendering::RenderGraph::RGTexture ViewOutputDepth{};
    // True if this pipeline contains a TransmissiveRender node. The world pass only peels glass into
    // the dedicated transmissive pass when one exists; otherwise glass renders inline (env-cube
    // fallback) so it never vanishes on pipelines/previews that lack the node.
    bool PipelineHasTransmissivePass = false;
    // True if this pipeline contains a SortedTransparent node (the relocated drain). The world pass
    // only BUILDS the sorted set — and the opaque peel only drops order-dependent Blend batches —
    // when one exists; otherwise the build is skipped, the set stays inactive, and Blend rides the
    // batched path so it never vanishes on pipelines/previews that lack the node.
    bool PipelineHasSortedTransparentPass = false;

    // Blackboard access. Resolvers return invalid entries for unknown refs;
    // publication overwrites (a node republishing "View.Depth" redirects every
    // LATER node in blueprint order — declaration order is the only ordering).
    Rendering::RenderGraph::RGTexture ResolveTexture(const std::string& ref) const;
    PipelineBufferBindingRG ResolveBuffer(const std::string& ref) const;
    void PublishTexture(const std::string& name, Rendering::RenderGraph::RGTexture t);
    void PublishBuffer(const std::string& name, const PipelineBufferBindingRG& b);

    // Queue a declaration to run AFTER the node loop (terminal overlays that
    // must attach the chain's FINAL output). Drained by Declare before the
    // spine marks outputs.
    void DeferDeclare(
        std::function<void(Rendering::RenderGraph::RGFrame&, RenderPipelineInstance&)> fn) const;

    // Stable framework pass name (same cache the old BuildForView used — MCP
    // and perf-CSV name contracts depend on identical names across the cutover).
    const std::string& PassName(const char* suffix = "") const;

  private:
    friend class RenderPipelineInstance;
    RenderPipelineInstance* m_Instance = nullptr;
    uint32_t m_NodeIdx = 0;
};

// Base interface for pipeline nodes (pass generators).
// Nodes should create persistent RenderGraph passes via RenderPipelineInstance helpers
// and avoid per-frame pass recreation.
class IRenderPipelineNode
{
  public:
    virtual ~IRenderPipelineNode() = default;

    virtual const char* GetTypeName() const = 0;

    // Called once when the node is instantiated from a blueprint.
    // `nodeJson` is the full node JSON object serialized as a compact string.
    virtual bool Initialize(std::string nodeId, std::string nodeJson, std::string* outError) = 0;

    // RenderGraph declaration path: declare this frame's passes/resources. The
    // frame-scope hook runs once per frame in blueprint order; DeclareForView
    // runs per active view.
    virtual void Declare(RenderPipelineInstance& /*instance*/, const PipelineDeclareContext& /*ctx*/)
    {
    }
    virtual void DeclareForView(ViewDeclare& /*d*/) {}

    // Return the base PipelineDesc for PSO pre-warming, or nullptr if this node
    // Returns the interned base pipeline id for nodes that drive a graphics
    // pipeline (e.g. FullscreenShaderNode). Default: invalid id (no node-owned
    // pipeline). RenderServices uses this for PSO pre-warming on a worker
    // thread during startup.
    virtual Rendering::GraphicsPipelineId GetBaseGraphicsPipelineId(Rendering::IDevice& /*device*/) const
    {
        return {};
    }

    // Whether this node wants PostProcessVolume settings applied to its push
    // constants via shader reflection. Set by "ppOverrides": true in the node JSON.
    bool WantsPPOverrides() const { return m_PPOverrides; }

  protected:
    bool m_PPOverrides = false;
};

using RenderPipelineNodeFactory = std::function<std::unique_ptr<IRenderPipelineNode>()>;

// The JSON keys through which a node type references and publishes pipeline
// resources. Blueprint validation resolves every reference against the builtin
// view names, the declared resources and the names passes publish, so a type
// that registers no fields gets no reference checks and publishes nothing a
// downstream pass could name.
struct RenderPipelineNodeResourceFields
{
    // Keys whose string value names one resource.
    std::vector<std::string> RefKeys;
    // Keys whose object value maps binding names to resources.
    std::vector<std::string> RefMapKeys;
    // Key whose value must name one of the pass's own "inputs" keys.
    std::string InputKey;
    // {jsonKey, defaultName}: the key's value is published, or defaultName when
    // the key is absent (an empty default publishes only when the key is set).
    std::vector<std::pair<std::string, std::string>> PublishKeys;
    // Names the node publishes regardless of its JSON.
    std::vector<std::string> StaticPublishNames;
};

struct RenderPipelineNodeTypeInfo
{
    std::string type;
    RenderPipelineNodeFactory factory;
    bool perView = false;
    RenderPipelineNodeResourceFields ResourceFields;
    // The node declares work the camera depth prepass consumes: a forward
    // producer's prepass head and the buffers it reads, or the upload that
    // producer's compute samples. The pipeline compiler declares such a pass
    // ahead of the first DepthPrepass wherever the blueprint lists it (a
    // Warning when it moves one), since the prepass declares its reads of
    // those buffers when it is declared.
    bool FeedsDepthPrepass = false;
    // Owning module + load generation (C12, ECS/ModuleRegistration.h): empty
    // for engine/static registrations. The factory callable's code lives in the
    // owning module's image, so the reload reconcile / abort purge / unload
    // quiesce ledger select entries by this stamp like the other stamped
    // registries (components, field tables, schemas, plugins, schedule).
    ECS::ModuleRegistrationStamp Module;
};

class RenderPipelineNodeRegistry
{
  public:
    // First registration of a type wins — except a re-registration from a
    // NEWER load generation of the owning module (a reload replay), which
    // replaces the entry in place so the factory comes from the newest mapped
    // image.
    bool Register(const std::string& type, RenderPipelineNodeFactory factory, bool perView,
                  RenderPipelineNodeResourceFields resourceFields = {}, bool feedsDepthPrepass = false);
    const RenderPipelineNodeTypeInfo* Find(const std::string& type) const;
    std::vector<std::string> GetRegisteredTypes() const;

    // C12 reload reconcile: drop entries still stamped with an OLDER
    // generation of `moduleId` — types the newest load's replay stopped
    // registering (re-declared types were already re-owned in place by
    // Register). Returns the number retired.
    std::size_t RetireSupersededModuleNodes(std::string_view moduleId, std::uint64_t currentGeneration);

    // C12 load-abort purge: drop entries stamped with exactly
    // {moduleId, generation} before the aborted image is unmapped. An entry a
    // failed load re-owned from an older generation is dropped too (drop over
    // dangle — same trade-off as the component registry's abort purge).
    std::size_t PurgeModuleNodes(std::string_view moduleId, std::uint64_t generation);

    // C12 unload quiesce ledger: entries still attributed to an OLDER
    // generation of `moduleId`. Non-zero blocks unmapping the superseded image
    // (the factory would dispatch into dead code).
    std::size_t CountSupersededModuleNodes(std::string_view moduleId,
                                           std::uint64_t currentGeneration) const;

    // Whether any entry is attributed to `moduleId` (any generation). The
    // reload reconcile uses this to decide if live pipeline instances hold
    // node objects built by this module's factories and must be rebuilt.
    bool HasModuleNodes(std::string_view moduleId) const;

  private:
    std::unordered_map<std::string, RenderPipelineNodeTypeInfo> m_Types;
};

} // namespace GameEngine::Engine::Renderer::Pipeline

