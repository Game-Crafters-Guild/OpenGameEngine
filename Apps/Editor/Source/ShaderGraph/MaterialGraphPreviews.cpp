#include "ShaderGraph/MaterialGraphPreviews.h"

#include "EditorContext.h"
#include "Graph/GraphCanvas.h"
#include "UI/UIManager.h"

#include <utility>
#include <vector>

namespace GameEngine {
namespace Editor {

void MaterialGraphPreviews::Initialize(const EditorContext* ctx, bool iblEnabled)
{
    if (ctx != m_Context)
        Shutdown();
    m_Context = ctx;
    if (!ctx || !ctx->RenderServices || !ctx->RenderGraphTicks || m_Initialized)
        return;
    m_Atlas.Initialize(ctx->RenderServices);
    m_Atlas.SetIblEnabled(iblEnabled);
    m_Tick = ctx->RenderGraphTicks->Register(
        [this](uint64_t windowId, UIManager* ui, Rendering::RenderGraph::RGFrame& frame)
        { m_Atlas.TickRG(windowId, ui, frame); },
        [this] { Shutdown(); });
    m_Initialized = true;
}

void MaterialGraphPreviews::Shutdown()
{
    /* Order matters: the frame loop must stop calling the tick before the atlas
       it captures is destroyed. */
    if (m_Tick != RenderGraphTickRegistry::kInvalidHandle && m_Context &&
        m_Context->RenderGraphTicks)
        m_Context->RenderGraphTicks->Unregister(m_Tick);
    m_Tick = RenderGraphTickRegistry::kInvalidHandle;
    if (m_Initialized)
        m_Atlas.Shutdown();
    m_Initialized = false;
    m_NodeMaterials.Clear();
    m_MainPreviewMaterial = GUID{};
}

void MaterialGraphPreviews::Sync(const EditorContext* ctx, const Graph::Model& model,
                                 bool expandedView, GraphCanvas* canvas, bool gestureActive)
{
    if (!m_Initialized || !ctx)
        return;

    if (model.KindId != Graph::kKindIdMaterial)
    {
        if (m_Atlas.ClearRequests() && canvas)
            canvas->RefreshBoundNodeValues();
        return;
    }

    /* Collapsing releases the node cells but not their materials — a collapse is
       a view state, and the compiled pipelines answer for the same nodes when it
       comes back. The main preview keeps its cell either way: it is not part of
       the expanded view. */
    if (!expandedView)
    {
        SyncRequests(ctx, model, canvas, /*includeNodes=*/false);
        return;
    }

    if (gestureActive)
    {
        /* Values are safe mid-gesture — a property push rebinds nothing — but a
           regenerated set moves cells, and rebinding the canvas under a live
           pointer destroys the element the drag is holding. The atlas replays
           that on release, and caps how long it waits. */
        if (m_NodeMaterials.NeedsSync(model))
            m_Atlas.DeferRebuild();
        if (m_NodeMaterials.RefreshProperties(ctx, model))
            m_Atlas.MarkDirty();
        return;
    }

    const MaterialGraphNodePreviewMaterials::SyncResult result = m_NodeMaterials.Sync(ctx, model);
    if (m_NodeMaterials.RefreshProperties(ctx, model) || result.SourcesChanged)
        m_Atlas.MarkDirty();
    SyncRequests(ctx, model, canvas, /*includeNodes=*/true);
}

void MaterialGraphPreviews::SetMainPreviewMaterial(const GUID& materialGuid)
{
    m_MainPreviewMaterial = materialGuid;
}

bool MaterialGraphPreviews::TryGetMainPreviewBinding(GraphNodePreviewBinding& out) const
{
    return m_Atlas.TryGetBinding(kMainPreviewKey, out);
}

void MaterialGraphPreviews::SetMainPreviewYaw(float yawRadians)
{
    m_Atlas.SetRequestYaw(kMainPreviewKey, yawRadians);
}

void MaterialGraphPreviews::SyncValues(const EditorContext* ctx, const Graph::Model& model)
{
    if (!m_Initialized || !ctx)
        return;
    if (m_NodeMaterials.RefreshProperties(ctx, model))
        m_Atlas.MarkDirty();
}

void MaterialGraphPreviews::SyncRequests(const EditorContext* ctx, const Graph::Model& model,
                                         GraphCanvas* canvas, bool includeNodes)
{
    const auto& materials = m_NodeMaterials.Materials();
    std::vector<GraphPreviewAtlas::NodeRequest> requests;
    requests.reserve(model.Nodes.size() + 1);
    if (!m_MainPreviewMaterial.IsNull())
        requests.push_back({kMainPreviewKey, m_MainPreviewMaterial, 0.f,
                            GraphPreviewAtlas::kMainPreviewFill});
    if (includeNodes)
    {
        for (const Graph::Node& node : model.Nodes)
        {
            const auto it = materials.find(node.Id);
            if (it == materials.end() || it->second.IsNull())
                continue;
            requests.push_back({node.Id, it->second});
        }
    }

    /* A moved cell assignment invalidates every bound sprite rect, so the
       widgets have to rebind — otherwise a node keeps painting a cell that now
       belongs to a different node (the stale-plate class).
       The atlas is bound to one window. A torn-off graph panel in a secondary
       window would need a per-panel window id; the whole preview path shares
       this limitation today. */
    const uint64_t windowId = ctx ? ctx->ThumbnailHostWindowId : 0;
    if (m_Atlas.SetRequests(windowId, std::move(requests)) && canvas)
        canvas->RefreshBoundNodeValues();
}

} // namespace Editor
} // namespace GameEngine
